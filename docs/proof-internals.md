# Proof Internals — Coq + Iris for ZER-LANG

**Required reading before modifying any `proofs/operational/**/*.v` file.** Captures tactic recipes, name-collision workarounds, Iris idioms, and every non-obvious thing learned while building λZER-Handle. Same role as `compiler-internals.md` but for the proof side.

## When to read this

- Starting a fresh session on proofs
- Modifying existing `.v` files in `proofs/operational/`
- Adding a new subset (`lambda_zer_move`, `lambda_zer_mmio`, etc.)
- Debugging a Coq build failure

## Fresh-session reading order (do this first)

If you're picking up proof work cold, read these in order. Total ~65 minutes. After this, you have enough context to safely modify any `.v` file.

| # | File | Time | What you learn |
|---|---|---|---|
| 1 | `CLAUDE.md` | (auto-injected) | ZER language summary + pointer to this file |
| 2 | `docs/formal_verification_plan.md` | 5 min | Big picture — Coq + Iris, no year timeline, subset-per-feature |
| 3 | `docs/safety_list.md` | 15 min | **What's proven + at what depth** (203-row matrix) |
| 4 | **This file** (`docs/proof-internals.md`) | 25 min | **MANDATORY** — patterns, gotchas, build quirks, tactic recipes |
| 5 | `BUGS-FIXED.md` (search "Iris" / "Coq") | 10 min | Past session pitfalls |
| 6 | `proofs/operational/README.md` | 3 min | Directory layout |
| 7 | Your target `.v` file header | 5 min | Each file documents its phase/purpose |

**After reading:** you know what's proven, how to extend the proofs, and what errors to expect. Without this reading, fresh sessions typically waste 2-5 hours on rediscovered pitfalls.

## What's next — priority queue (2026-04-21)

Next subsets to deepen from schematic to operational, ranked by ratio (rows closed / effort hours):

| Priority | Subset | Rows | Est. hours | Reason |
|---|---|---|---|---|
| **1** | **λZER-opaque** (J) | 14 | 20-40 | Provenance ghost state — well-understood RustBelt pattern. 14 rows / 30 hrs ≈ high ratio. Unlocks `*opaque` C interop proofs. |
| **2** | **λZER-escape** (O) | 12 | 30-60 | Region invariants (dangling pointers). Clean RustBelt-lifetime analog. Also unblocks cross-function escape reasoning. |
| **3** | **λZER-mmio** (H) | 9 | 20-40 | MMIO region invariants. Hardware safety. Smaller scope, clean invariant. |
| **4** | **λZER-concurrency** (C+D+E) | 25 | 100-200 | Real Iris concurrency. Hardest. Do after we have more easy subsets for confidence. |
| **5** | **λZER-async** (F) | 5 | 40-80 | Builds on concurrency. Do after (4). |

**Orthogonal priority: Layer 2 tests.** See "Layer 2 — wiring proofs to tests" section below. ~20-30 hours to add one `tests/zer_proof/*.zer` per proven theorem. Closes the "Iris spec = correctness oracle" loop.

**Long-term: Level 3 VST on zercheck.c.** ~150-500 hours. Start after at least 3-4 subsets at operational depth.

## Layer 2 — wiring proofs to tests (not yet done)

The correctness-oracle workflow requires a test for each proven theorem. If the compiler silently regresses, these tests catch it.

**Proposed structure:**
```
tests/zer_proof/
  A01_no_uaf_simple.zer              — exercises spec_get
  A01_no_uaf_simple_bad.zer          — violation (must FAIL to compile)
  A06_no_double_free.zer             — alive_handle_exclusive
  A06_no_double_free_bad.zer         — violation
  A12_no_ghost_handle.zer            — step_spec_alloc_succ
  A12_no_ghost_handle_bad.zer        — violation
  B01_no_use_after_move.zer          — B01_use_after_move_operational
  B01_no_use_after_move_bad.zer      — violation
  ... one pair per proven theorem
```

**Mechanism:** `make check` already runs positive + negative `.zer` tests. Adding `tests/zer_proof/` means each theorem has a compile-time enforcement check. If zercheck changes and starts accepting the `*_bad.zer` program, the test fails — **pointing at which Iris theorem the compiler just violated**.

**Cost:** ~30 min per theorem × ~55 theorems = 20-30 hours total. High value.

**Status (2026-04-21):** **106 tests landed**, one per major theorem. All 105 `_bad.zer` correctly rejected by the compiler; 1 `_no_uaf.zer` positive test verifies the safe pattern compiles. Coverage spans all sections A-T. See `tests/zer_proof/README.md` for the full list.

The correctness-oracle loop is now CLOSED for all major theorems: any compiler regression that accepts a violating program fails the matching test, and the test name (e.g., `A06_no_double_free_bad`) tells you which Iris theorem was violated.

## The build

```bash
# One-time: build the Docker image (Coq 8.18 + Iris 4.2 + stdpp 1.10)
docker build -t zer-proofs -f proofs/operational/Dockerfile .

# Compile all proofs:
cd proofs/operational
MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W):/work" -w /work zer-proofs \
    bash -c 'eval $(opam env) && make'
```

`make check-proofs` from the repo root is the shortcut.

**Subset directories and bindings:** The `_CoqProject` file declares both subsets:
```
-Q lambda_zer_handle zer_handle
-Q lambda_zer_move   zer_move
```
Use `From zer_handle Require Import ...` for lambda_zer_handle/ files; `From zer_move Require Import ...` for lambda_zer_move/. Each subset has its own namespace.

**Why `MSYS_NO_PATHCONV=1`**: Git Bash on Windows auto-converts `:` in volume mounts, breaking Docker. Must disable for mount to work.

**Why `$(pwd -W)`**: Git Bash's POSIX path (`/c/...`) isn't what Docker wants; `pwd -W` gives the Windows-style path (`C:/...`) that Docker accepts. Linux hosts can use plain `$(pwd)`.

**Regenerate CoqMakefile after editing `_CoqProject`:**
```bash
rm -f CoqMakefile CoqMakefile.conf
```
The next `make` rebuilds it. Required when adding new `.v` files.

## File layout — what lives where

```
proofs/
  model1_handle_states.v          # Abstract models (2,401 lines, 0 admits,
  model2_point_properties.v       # 129 theorems). Design-level proofs.
  model3_function_summaries.v     # Don't modify unless safety design changes.
  model4_static_annotations.v
  composition.v

  operational/                    # Operational proofs (connects model → compiler)
    Dockerfile                    # zer-proofs image
    Makefile, _CoqProject         # Build config
    lambda_zer_handle/            # First subset (sequential handle safety)
      syntax.v                    # AST (keep framework-agnostic)
      semantics.v                 # Small-step relation (keep framework-agnostic)
      typing.v                    # Type system + canonical-forms lemmas
      adequacy.v                  # Plain-Coq preservation/progress (partial,
                                  # superseded by iris_*.v chain)
      handle_safety.v             # Top-level theorem scaffold
      iris_smoke.v                # Iris infra smoke test
      iris_lang.v                 # Our step relation as Iris language instance
      iris_resources.v            # alive_handle resource algebra
      iris_state.v                # State interpretation + irisGS_gen instance
      iris_specs.v                # Safety specs at resource level
      iris_step_specs.v           # Step-level specs (alloc/free/get in fupd form)
      iris_adequacy.v             # Iris proof → operational safety bridges
      iris_leak.v                 # Leak detection (A09/A10/A11/A14/A18)
      iris_demo.v                 # End-to-end demos
```

## Current state — what's proven

**Zero admits across all Iris files.** 19 `.v` files, 80+ axiom-free lemmas. Every file ends with `Qed.`, never `Admitted.`. If this changes, something regressed.

Verify:
```bash
grep -c "Admitted\|admit\." proofs/operational/lambda_zer_handle/iris_*.v
```
Should be all zeros.

### File → section mapping

When adding a new safety row, find the matching file:

| Rows | Depth | File |
|---|---|---|
| A01-A02 (UAF) | Full operational | `iris_specs.v`, `iris_adequacy.v` |
| A03-A04 (interior pointer, cast UAF) | Schematic | `iris_derived.v` |
| A05, A07 (cross-function) | Schematic | `iris_func_spec.v` |
| A06, A08 (DF) | Full operational | `iris_specs.v`, `iris_resources.v` |
| A09-A11, A14, A18 (leaks, freed returns) | Full operational | `iris_leak.v` |
| A12 (ghost handle) | Full operational | `iris_step_specs.v` |
| A13 (wrong pool) | Foundation (structural) | `iris_resources.v` |
| A15-A16 (loops) | Schematic | `iris_loop.v` |
| A17 (runtime gen check) | Operational | `iris_specs.v` |
| Step specs (alloc/free/get) | Full operational | `iris_step_specs.v` |
| B01-B08 (move struct) | Schematic (in lambda_zer_handle, reuses handleG) | `iris_move.v` |
| B01-B08 (move struct) | **FULL operational** (dedicated subset) | `lambda_zer_move/syntax.v`, `semantics.v`, `iris_move_resources.v`, `iris_move_state.v`, `iris_move_specs.v`, `iris_move_theorems.v` |
| C01-C12 (thread/spawn) | Schematic | `iris_concurrency.v` |
| D01-D05 (shared/deadlock) | Schematic | `iris_concurrency.v` |
| E01-E08 (atomic/condvar/etc) | Schematic | `iris_concurrency.v` |
| F01-F05 (async) | Schematic | `iris_concurrency.v` |
| G01-G12 (control flow) | Schematic | `iris_control_flow.v` |
| H01-H09 (MMIO) | Schematic (in lambda_zer_handle) | `iris_mmio_cast_escape.v` |
| H01-H04, H06 (core MMIO) | **FULL operational** (dedicated subset) | `lambda_zer_mmio/syntax.v`, `semantics.v`, `iris_mmio_theorems.v` |
| I01-I11 (qualifiers) | Schematic | `iris_typing_rules.v` |
| J01-J14 (cast/provenance) | Schematic (in lambda_zer_handle) | `iris_mmio_cast_escape.v` |
| J01, J04, J11, J12, J13, J14 (core provenance) | **FULL operational** (dedicated subset) | `lambda_zer_opaque/syntax.v`, `semantics.v`, `iris_opaque_resources.v`, `iris_opaque_state.v`, `iris_opaque_specs.v`, `iris_opaque_theorems.v` |
| K01-K04 (@container/etc) | Schematic | `iris_typing_rules.v` |
| L01-L11 (bounds) | Schematic | `iris_misc_sections.v` |
| M01-M13 (arith) | Schematic | `iris_misc_sections.v` |
| N01-N08 (optional) | Schematic | `iris_typing_rules.v` |
| O01-O12 (escape) | Schematic (in lambda_zer_handle) | `iris_mmio_cast_escape.v` |
| O01-O12 (escape) | **FULL operational** (dedicated subset) | `lambda_zer_escape/syntax.v`, `semantics.v`, `iris_escape_resources.v`, `iris_escape_state.v`, `iris_escape_specs.v`, `iris_escape_theorems.v` |
| P01-P08 (variant) | Schematic | `iris_misc_sections.v` |
| Q01-Q05 (switch) | Schematic | `iris_typing_rules.v` |
| R01-R07 (comptime) | Schematic | `iris_misc_sections.v` |
| S01-S06 (resource limits) | Schematic | `iris_misc_sections.v` |
| T01-T07 (container validity) | Schematic | `iris_container_validity.v` |

Language infrastructure:
- `iris_lang.v` — Canonical language instance for λZH_lang
- `iris_smoke.v` — Iris imports + basic BI sanity

## Level 3 — VST verification of C implementations

**Location:** `proofs/vst/` — separate from `proofs/operational/`.

**Docker image:** `zer-vst` — coqorg/coq:8.18 + coq-iris 4.2 + coq-stdpp 1.10 + **coq-vst 3.0beta2** + coq-vst-zlist + CompCert `clightgen` 3.13.

**Build:**
```bash
make check-vst-image    # one-time, ~5 min
make check-vst          # compile + verify all VST proofs
```

**File structure per function verified:**
```
proofs/vst/
  <func>_src.c           # C source (extracted from zercheck/emitter or standalone)
  <func>_src.v           # clightgen-generated Coq Clight AST (GENERATED — in .gitignore)
  verif_<func>.v         # VST spec + proof
```

### VST 3.0 patterns

**VST 3.0 is Iris-based.** `funspec` takes an implicit `Σ : gFunctors` argument. For simple proofs without custom ghost state, use the precompiled `VST.floyd.compat`:

```coq
Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.    (* precompiled in zer-vst image *)
Require Import zer_vst.simple_check.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.
```

`VST.floyd.compat` provides `Notation funspec := (@funspec (VSTΣ unit))` — specializes to no ghost state or external calls. Not compiled by default in the opam install — the Docker `RUN coqc` step precompiles it.

### Standard spec pattern

```coq
Definition foo_spec : ident * funspec :=
 DECLARE _foo
  WITH arg : Z
  PRE [ tint ]
    PROP (Int.min_signed <= arg <= Int.max_signed)   (* range check *)
    PARAMS (Vint (Int.repr arg))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (foo_coq arg)))           (* matches Coq spec *)
    SEP ().
```

`Int.min_signed <= arg <= Int.max_signed` is the C-int range (needed for VST's int-representation lemmas).

### Standard proof pattern

For simple if-else functions:
```coq
Lemma body_foo: semax_body Vprog Gprog f_foo foo_spec.
Proof.
  start_function.
  forward_if;                                         (* handle if-branch *)
    forward;                                           (* emit return *)
    unfold foo_coq;                                    (* unfold Coq spec *)
    destruct (Z.eq_dec arg VALUE); try contradiction;  (* case-split *)
    try subst; try contradiction;
    entailer!.                                         (* discharge goal *)
Qed.
```

The combined `forward_if; forward; ...; entailer!` closes both branches at once — VST's goal structure for if-else is often confusing when handled separately.

### Common VST errors and fixes

| Error | Cause | Fix |
|---|---|---|
| `Cannot infer the implicit parameter Σ of funspec` | VST 3.0 Iris-based; `funspec` needs Σ | Import `VST.floyd.compat` for the `(VSTΣ unit)`-specialized version |
| `Cannot find a physical path bound to logical path VST.floyd.compat` | compat.v not precompiled | Dockerfile precompiles it: `coqc -Q ... VST compat.v` |
| `No such goal. Focus next goal with bullet -` | forward_if produced different goal count than expected | Don't use bullets/braces — use `forward_if; forward; ...` one-liner |
| `No such contradiction` | `destruct` case where `contradiction` hypothesis isn't visible yet | Rearrange: `destruct first; try subst; try contradiction` |
| `Custom entry dfrac has been overridden` | Harmless Iris warning on every file | Ignore |
| `The following notations have been disabled: Notation 'True'` | Iris overrides Coq's `True`/`False` | Ignore — inside proofs use `True%I` / `False%I` for BI |
| `The variable X was not found in the current environment` after nested `forward_if` with `if (x != 0) return 0;` pattern | VST auto-`subst`s the WITH-bound variable to 0 in the else-branch of a `!=0` early-return if. The variable literally disappears. | After first `forward_if`'s else-branch, don't reference the subst'd variable. Use `simpl` to evaluate `Z.eq_dec 0 0 = left`, then `destruct` only on the OTHER variables. `<` / `>=` comparisons don't trigger subst — only `==` / `!=` with a constant do. |
| `No such goal` after nested `forward_if. { ... }` closes the then-branch | Second nested `forward_if` merges its post with the outer continuation → one goal left, not two | Use cascade pattern: `forward_if; [<then-tactic> \| ]. forward_if; <unified-tactic>;...` — the `[tac \| ]` discharges the then-branch and leaves ONE else-goal. The nested forward_if's post is already the final return. |

### What Level 3 proves (vs Level 1, Level 2)

**Level 1** (Coq/Iris predicates in `lambda_zer_*/` and `lambda_zer_typing/`): proves the safety ARGUMENT is sound. Abstract math.

**Level 2** (tests in `tests/zer_proof/`): empirically verifies the compiler rejects specific known violations. Fast, covers known patterns.

**Level 3** (VST in `proofs/vst/`): proves the C IMPLEMENTATION of a check matches its Coq predicate for EVERY input. Mechanical certainty over the entire input space.

A bug that Level 1 can't catch but Level 3 does: zercheck's C source has a typo `if (state = 1)` (assignment, not comparison). Level 1's spec is correct; Level 2 might miss this if tests don't cover the right pattern; Level 3 fails the proof because the C control flow doesn't match the predicate.

### Verification strategy — extract-and-link (2026-04-21)

**The honest form of Level 3 is NOT writing standalone `.c` files for VST.** That's what `proofs/vst/zer_checks.c` / `zer_checks2.c` / `simple_check.c` do — they demonstrate the VST pattern but verify code that doesn't exist in zerc. It's a scaffolding step, not real verification.

**Real Level 3 is extract-and-link:**
1. Identify a pure predicate in `zercheck.c` or `zercheck_ir.c` (primitive args, no mutation, no AST dependencies).
2. Move it to `src/safety/<name>.c` + `<name>.h`.
3. Call the extracted function from BOTH zercheck.c and zercheck_ir.c (dual-run still sees the same logic).
4. Makefile `CORE_SRCS` and `LIB_SRCS` include `src/safety/*.c` — they're linked into `zerc`.
5. `make check-vst` clightgens the SAME `src/safety/*.c` files and runs VST on them.
6. If the C implementation diverges from the Coq spec, `make check-vst` fails → PR blocked.

**File layout:**
```
src/safety/
  <name>.c                          # Pure function, linked into zerc
  <name>.h                          # Declarations + constants
  .gitignore                        # Excludes handle_state.v (clightgen output)
proofs/vst/
  verif_<name>.v                    # VST spec + proof
```

**Makefile wiring:** `check-vst` mounts the WHOLE repo (not just proofs/vst/) so clightgen can see `src/safety/`. The `.v` output lands in `src/safety/`, imported by `verif_<name>.v` via `-Q src/safety zer_safety` and `From zer_safety Require Import <name>.`

**First real extraction (2026-04-21):** `zer_handle_state_is_invalid(int state)` — checks if state ∈ {FREED, MAYBE_FREED, TRANSFERRED}. Replaces inline logic in zercheck.c:is_handle_invalid + zercheck_ir.c:ir_is_invalid. VST-verified in `proofs/vst/verif_handle_state.v`.

**What counts as Level 3 verified:**
- `src/safety/handle_state.c:zer_handle_state_is_invalid` — 1 function verified
- The 21 proofs in `proofs/vst/verif_simple_check.v` / `verif_zer_checks*.v` are SCAFFOLDING — don't count them as compiler verification. The standalone .c files in proofs/vst/ can eventually be deleted (or kept as demonstrator examples).

**Scope estimate (honest):** 15-25 pure predicates extractable. ~1-3 hrs per extraction (smaller than the earlier "5-20 hrs" estimate — simple predicates don't need struct separation logic). Complex functions (loops, recursion, scope walks) don't fit this pattern and are separate work.

Progress tracking: each verified real-code function counts once in `src/safety/*.c`. `make check-vst` should print the count.

### Spec-writing discipline (the single most important Level 3 habit)

**Write Coq specs against the Level 1 ORACLE, not against the current C behavior.**

This is THE critical habit. VST proves "C matches the spec I wrote." It does NOT prove the spec is correct. The spec is human-authored and can be wrong.

**Two ways to write an extraction spec:**

1. **Code-driven (AVOID):** "What does this C function do? Let me write Coq to match."
   - VST passes trivially (tautology: C matches its own transliteration)
   - Bugs in C are frozen into the spec
   - Future maintainers think "the spec says X, so X is correct"

2. **Oracle-driven (USE):** "What should this predicate compute per the safety rule in typing.v / λZER-subset?"
   - VST may FAIL on first run, because the C doesn't match the rule — bug exposed
   - When VST passes, the guarantee is STRONG (C implements the rule correctly)
   - The proof answers "is the implementation correct?" not "does it self-match?"

**Oracles in increasing precedence:**
- Operational proof (e.g., `λZER-escape/iris_escape_specs.v`) — gold standard
- Predicate proof (e.g., `typing.v` section G predicates) — decidable, reliable
- Schematic claim in `safety_list.md` with code comment — weak, but usable
- English text in `ZER-LANG.md` / `CLAUDE.md` — weakest, translate to math carefully

**Retroactive check of 2026-04-21 Gemini findings:** all 3 real bugs (F5 shift, F3 escape, F7 iter-limit) were catchable by oracle-driven Level 3 VST. The audit was a SECOND-BEST defense that would have been unnecessary with disciplined spec-writing. Keep audits as a safety net, not a blocking prerequisite.

### When the audit IS still valuable

- **No oracle exists:** the subsystem's Level 1 proof is schematic/missing (e.g., some concurrency rows). No precise spec to write against.
- **Extraction won't happen:** the function is too complex to extract as a pure predicate (e.g., recursive AST walkers). Level 3 can't help; only tests + audits can.
- **Multi-subsystem interactions:** bugs that cross subsystem boundaries (e.g., move + async + shared) can't be captured in a single predicate's spec.

For extractable predicates WITH Level 1 oracles, skip the audit step — write oracle-driven specs and let VST do the work.

### Audit-before-extraction (spec-implementation gap) — SECONDARY

The previous section's rule ("always audit before extract") was over-defensive. Keep it as a fallback for un-oraclable subsystems. For oracle-backed predicates, oracle-driven specs are the primary defense.

**THE dominant risk in Level 3 is freezing existing bugs into specs.**

Level 3 extract-and-link VST pins the C behavior to the Coq spec. If the C has a bug at extraction time AND the spec is code-driven, the spec captures the bug. CI then enforces the bug as "correct." Future sessions may even write Coq proofs about code that mathematically describes wrong behavior.

This risk is mitigated by:
1. **Primary defense: oracle-driven spec writing** (cross-check against Level 1)
2. **Secondary defense: audit-before-extraction** (when no oracle exists)
3. **Tertiary defense: regression tests in tests/zer/ and tests/zer_fail/**

**The 2026-04-21 Gemini audit proved this risk concrete.** 4 real bugs in compiler source, present before audit:

| Bug | Rule it violated | What Level 1 Coq proved |
|---|---|---|
| F5: `1 << 40` for u32 → 2^40 | "Shift by >= width = 0" | `typing.v` proves the rule (not its application) |
| F3: struct field escape | "No local refs in returned struct fields" | `λZER-escape` operational proof covers returned pointers; schematic for struct-field case |
| F7: iteration limit fails-open | Lattice converges → UAF detected | Coq induction proved convergence mathematically; C had arbitrary limit |
| F1-2-4-6: NOT bugs | Design decisions | Already documented |

**Pattern:** Level 1 proves "the rule is correct" in Coq. Level 3 proves "the C implementation applies the rule" via VST. The 4 bugs lived in the GAP between these layers — the rule was fine, the compiler's implementation of the rule was wrong.

**Implication:** before extracting any predicate, the current C behavior must MATCH the intended spec. If not, either (a) fix the C first, or (b) write the spec to match the current (buggy) behavior and document it as intentional.

**Audit cadence — required before each Phase 1 batch:**
1. Identify the subsystem about to be extracted (e.g., "escape rules")
2. Red-team the implementation: write adversarial programs that probe edge cases
3. Fix bugs found, add regression tests to `tests/zer/` or `tests/zer_fail/`
4. THEN extract — the spec now matches verified-correct behavior

**Why this matters more than it seems:** without audits, you're doing "Level 3 VST" but accidentally encoding bugs as truth. The math is valid; the claim ("this compiler is correct") becomes false. A future audit finds the bug, now documented as a spec — you have to update spec, proof, implementation, AND tests. Much more expensive than auditing first.

**Audit quick-reference ladder:**
- Tier 1: run existing `make docker-check` (baseline)
- Tier 2: write 5-10 adversarial `.zer` files probing the subsystem's claims
- Tier 3: Gemini/Claude red-team audit with the subsystem's source as context
- Tier 4: full fuzzer run on a known-small space

Use Tier 2 minimum before every batch. Tier 3 every 2-3 batches. Tier 4 periodically.

### Phase 1 COMPLETE (2026-04-22, 44/44 predicates)

13 `src/safety/*.c` files, 44 VST-verified predicates, 16 VST `.v` files,
zero admits. Every major safety subsystem in `safety_list.md` has at
least one oracle-driven predicate. `make check-vst` enforces the whole
thing.

Subsystem roll-call: handle_state, range_checks, type_kind, coerce_rules,
context_bans, escape_rules, provenance_rules, mmio_rules, optional_rules,
move_rules, atomic_rules, container_rules, misc_rules.

Next milestone: **Phase 2 — decision extraction** (~150 hrs, ~60 decisions).

### Phase 2 pattern — decision extraction (different from Phase 1)

Phase 1 predicates answered "is X valid?" — one bool output per check.

Phase 2 decisions answer "given X, what should happen?" — at mutation sites.
Two options for extraction:

**Option 2a: Multiple bool predicates per mutation site.**
Keep each predicate a pure bool-returning check. Mutation site calls
several predicates and branches on results.

```c
// BEFORE (mutation tangled with decision)
if (h->state == HS_ALIVE) {
    h->state = HS_FREED;
    h->free_line = line;
} else if (h->state == HS_FREED) {
    error("double free");
}

// AFTER (Phase 1 predicates already cover most cases)
if (zer_handle_state_is_alive(h->state) != 0) {
    h->state = HS_FREED;   // trivial assignment, not verifiable
    h->free_line = line;
} else if (zer_handle_state_is_freed(h->state) != 0) {
    error("double free");
}
```

Much of Phase 2 is actually **sweeping inline state checks to delegate
to Phase 1 predicates** — no new extractions needed, just wider
coverage of the existing VST guarantees.

**Option 2b: Single decision function returning action code.**

```c
// Extracted pure function
int zer_handle_merge_states(int state_a, int state_b);
// Returns: merged state per if/else path-merge rules

// Mutation site becomes a dispatch
h->state = zer_handle_merge_states(then_state, else_state);
```

Works when the decision itself is complex enough to be worth extracting
(state merge rules, coercion dispatch, etc.) but the action at the
call site is trivial assignment.

**Phase 2 priorities (in order of value per hour):**
1. **Sweep inline checks** to delegate to existing Phase 1 predicates
   — cheap, big coverage gains, no new VST proofs
2. **State merge decisions** (path-merge at if/else/switch/loop)
   — 4 pure functions
3. **Coercion dispatch** (which coercion to apply given source/dest
   type kinds) — ~10 functions, builds on Phase 1 coerce_rules
4. **Escape flag propagation** (when is a flag set/cleared?)
   — ~6 functions

See `docs/formal_verification_plan.md` Phase 2 section for full target list.

### Oracle catalog — which Level 1 proof covers which subsystem

When extracting a predicate, the Coq spec MUST be written against a Level 1
oracle, not against the current C. This table lets a fresh session find
the oracle quickly.

| Subsystem | Oracle file | Key Iris lemma | Phase 1 batch |
|---|---|---|---|
| Handle states (A) | `proofs/operational/lambda_zer_handle/iris_specs.v` | `alive_handle_exclusive`, `spec_get` | handle_state.c |
| Move struct (B) | `proofs/operational/lambda_zer_move/iris_move_specs.v` | `step_spec_consume`, `alive_move_exclusive` | TODO move_rules.c |
| Opaque cast / provenance (J) | `proofs/operational/lambda_zer_opaque/iris_opaque_specs.v` | `step_spec_typed_cast`, `typed_ptr_agree` | provenance_rules.c |
| Escape analysis (O) | `proofs/operational/lambda_zer_escape/iris_escape_specs.v` | `step_spec_store_global_static` | escape_rules.c |
| MMIO / volatile (H) | `proofs/operational/lambda_zer_mmio/iris_mmio_theorems.v` | `H01_H02_out_of_range_stuck`, `H03_unaligned_stuck` | mmio_rules.c |
| Typing-level rules (G, I, K, N, Q, T) | `proofs/operational/lambda_zer_typing/typing.v` | predicate definitions + `X_ok` / `X_bad_rejected` theorems | context_bans.c (partial), TODO optional_rules.c |
| VRP / arith (L, M, R, S) | `proofs/operational/lambda_zer_typing/typing.v` | same pattern | range_checks.c (partial), TODO more |
| Concurrency (C, D, E) | Schematic only — no operational oracle yet | — | Audit-first required |
| Async (F) | Schematic only | — | Audit-first required |

**How to read an oracle:**
1. Open the file in the table
2. Search for the lemma name (column 3)
3. The lemma's STATEMENT is the safety claim
4. Translate the statement into a C predicate + Coq spec
5. VST proves C matches the translation — if C diverges from the Iris lemma, proof fails

**When no oracle exists (concurrency, async, or schematic rows):**
- Don't extract yet — no way to write oracle-driven specs
- Either: (a) upgrade the Iris proof to operational depth first, or (b) audit the C
  implementation, document current behavior as the spec, and extract defensively

### VST cheat sheet — types

When writing VST specs for extracted C, the parameter types in C → Coq mapping:

| C type | VST C type | Coq value | Works for `repeat forward_if; destruct (Z.eq_dec _ _)` |
|---|---|---|---|
| `int` | `tint` | `Z` in range `[Int.min_signed, Int.max_signed]` | ✓ (primary target — all current extractions use this) |
| `unsigned int` | `tuint` | `Z` in `[0, Int.max_unsigned]` | ✓, but use `Z_ge_dec/Z_lt_dec` not `Z.eq_dec` for comparisons |
| `long` | `tlong` | `Z` in `[Int64.min_signed, Int64.max_signed]` | Needs `Int64.repr` in spec |
| `unsigned long` | `tulong` | `Z` in `[0, Int64.max_unsigned]` | Same |
| `char` (signed) | `tschar` | small range; tricky sign extension | Avoid — use int |
| `void` return | `tvoid` | N/A | Avoid — extract predicates always return int |

**Rule:** prefer `int` parameters for extracted predicates. If the caller has
wider types (like `uint64_t` addresses in MMIO), either:
1. Narrow at call site (accept truncation for very-large values)
2. Split extraction: inline the wide-type logic in checker, delegate only the
   final int-to-int gate combination (what we did for MMIO)
3. Widen the predicate to `long`/`tlong` (more VST setup)

### VST gotcha — modulus / division deferred

`forward` on `addr % align` needs additional lemma setup in VST 3.0 for the
`Int.repr / Z.mod` interaction. Not blocking but adds ~30 min per proof.

**Workaround:** extract the combination predicate (which takes an already-
computed bool), keep the modulus inline in the caller. Example:
`zer_mmio_addr_aligned(addr, align)` was deferred; `zer_mmio_inttoptr_allowed(
in_range, aligned)` was verified instead, with the alignment-bool computed
inline in checker.c using `(uint64_t)addr % (uint64_t)align`.

### VST gotcha — tlong (int64) forward_if goal structure (Batch 1 finding, 2026-04-22)

**Pattern that works for tint but FAILS for tlong:**

```c
/* tint version — proof passes with standard cascade */
int zer_X(int a, int b, int c) {
    if (a == 0) return 0;
    if (b == 0) return 0;
    return 1;
}
```

```coq
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_X_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.
```

**tlong version of the same cascade FAILS:**

```c
/* tlong version — "Attempt to save an incomplete proof" */
int zer_Y(long long a, long long b, long long c) {
    if (a < b) return 0;
    if (a > c) return 0;
    return 1;
}
```

Standard pattern with `destruct (Z_lt_dec _ _)` does NOT close all goals.
Bullet/brace variants (`{ ... }`) produce `This proof is focused, but
cannot be unfocused this way`.

**Root cause (hypothesis):** VST's `forward_if` on Int64 comparisons
produces goals with different structure than Int32 — likely due to
`Int64.lt` / `Int64.ltu` resolution vs `Int.eq_dec` / `Int.lt` used
in tint. The `repeat destruct` tactics don't align with the tlong
goal form.

**Workarounds (in order of preference):**

1. **Split into width-specific predicates** (preferred). Instead of
   one `int zer_literal_fits(long long min, long long max, long long lit)`,
   use `int zer_literal_fits_u32(unsigned int max, unsigned int lit)` +
   `int zer_literal_fits_i32(int min, int max, int lit)` +
   `int zer_literal_fits_u64(unsigned long long max, unsigned long long lit)`.
   Each uses tint/tuint/tulong with proofs matching their VST pattern.
   Caller dispatches by type at the call site.

2. **Extract via combination pattern** (same as MMIO modulus workaround).
   Caller precomputes `int is_in_range = (lit >= min && lit <= max) ? 1 : 0;`
   and passes the bool to `int zer_range_check(int is_in_range)`. Trivial
   VST proof; range arithmetic stays inline in checker.c.

3. **Defer the predicate** to a follow-up batch. Mark with clear NOTE
   in the header file; leave inline check in checker.c. Document in
   `docs/phase1_catalog.md` as "deferred — needs split redesign."

**Applied (Batch 1, M08):** Option 3 (deferred). `zer_literal_fits`
was originally designed with `long long` args covering all ZER literal
widths uniformly. VST failed. Inline check kept in `is_literal_compatible`
(checker.c). Batch 1b will apply Option 1 (split into u32/i32/u64).

**Error signatures:**

| Error | Interpretation |
|---|---|
| `Attempt to save an incomplete proof` at `Qed` | `repeat forward_if; ... entailer!` pattern produced goals that the destruct cascade didn't close. tlong's `forward_if` likely has subgoals requiring `Int64.signed_repr` rewriting before `entailer!`. |
| `This proof is focused, but cannot be unfocused this way` | Using `{ ... }` brackets to focus sub-goals, but the inner block closed the wrong goal count. Happens when you assume 2 sub-goals but VST produced 1 (for a return statement) or 3 (for nested control flow). |

**Do NOT:**
- Use `tlong` in extraction signatures unless there's no alternative.
- Mix `Vint` and `Vlong` params in one predicate (the range constraints become tangled).
- Assume `repeat forward_if; ...; destruct (Z_lt_dec _ _)` works for 64-bit the same as 32-bit. It doesn't.

### Standard proof patterns that WORK (Batch 1-6 findings, 2026-04-22)

After extracting 16 predicates in batches 1-6, these patterns are proven to work:

**Pattern A — single-branch predicate:**
```c
int zer_X(int arg) {
    if (arg == 0) return 0;
    return 1;
}
```
```coq
Lemma body_zer_X:
  semax_body Vprog Gprog f_zer_X zer_X_spec.
Proof.
  start_function.
  forward_if;
    forward;
    unfold zer_X_coq;
    destruct (Z.eq_dec arg 0); try lia;
    try entailer!.
Qed.
```

**Pattern B — N-branch sequential cascade (2-5 branches):**
```c
int zer_Y(int a, int b) {
    if (a < 0) return 0;
    if (a >= b) return 0;
    return 1;
}
```
```coq
Proof.
  start_function.
  forward_if.
  { forward. unfold zer_Y_coq.
    destruct (Z_ge_dec a 0); try lia. simpl. entailer!. }
  forward_if.
  { forward. unfold zer_Y_coq.
    destruct (Z_ge_dec a 0); try lia.
    simpl. destruct (Z_lt_dec a b); try lia. entailer!. }
  forward. unfold zer_Y_coq.
  destruct (Z_ge_dec a 0); try lia.
  simpl. destruct (Z_lt_dec a b); try lia. entailer!.
Qed.
```

**Pattern C — all-equality cascade (2-3 branches with `==` comparisons):**
```c
int zer_Z(int a, int b) {
    if (a == 0) return 0;
    if (b == 0) return 0;
    return 1;
}
```
```coq
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_Z_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.
```

Pattern C is the shortest. Use it when the cascade is pure-equality.
Pattern B when comparisons mix `<`/`>=`/`==`.
Pattern A for single branches.

**Common failure with brackets `{ ... }` on cascade:** if you write
Pattern B-style brackets for a Pattern-C-style cascade, VST produces
"No such goal. Try unfocusing with '}'" because `repeat forward_if`
unified multiple subgoals into one. Solution: use Pattern C when the
C has no complex branch-specific state.

### Makefile + .gitignore checklist for new src/safety/*.c file

When adding a brand-new file (not extending an existing one), these MUST be updated:

1. **`Makefile` CORE_SRCS line** — append `src/safety/<name>.c` (preserves link into zerc)
2. **`Makefile` LIB_SRCS line** — append same (for test_* builds)
3. **`Makefile` check-vst target** — add TWO lines (clightgen + coqc pairs):
   ```
   coqc -Q . zer_vst -Q ../../src/safety zer_safety ../../src/safety/<name>.v && \
   coqc -Q . zer_vst -Q ../../src/safety zer_safety verif_<name>.v && \
   ```
   Also append `<name>.c` to the `clightgen -normalize src/safety/...` line.
4. **`src/safety/.gitignore`** — append `<name>.v` (clightgen auto-generates, don't commit)

**Skip if extending existing file** (Batches 2 + 3 extended range_checks.c and container_rules.c — no Makefile changes needed, just added Lemmas to the existing verif_*.v).

### Delegation pattern checklist for Phase 6 readiness

Every call-site delegation must include:

1. **`/* SAFETY: zer_X in src/safety/<file>.c (ruleID) */` comment** — Phase 6 `check-no-inline-safety` will grep for this pattern. Put on the line IMMEDIATELY BEFORE the predicate call or condition.
2. **Actual predicate call in condition** — `if (zer_X(args) == 0)` or `if (zer_X(args) != 0)`. NOT just the comment — the predicate must execute.
3. **The condition semantic must match** — replace the ENTIRE condition, not just annotate. E.g., replace `if (val == 0)` with `if (zer_div_valid(val == 0 ? 0 : 1) == 0)`.

**Bad delegation (Phase 6 will reject):**
```c
/* SAFETY: zer_div_valid */
if (val == 0) checker_error(...);  // predicate not called
```

**Good delegation:**
```c
/* SAFETY: zer_div_valid in src/safety/arith_rules.c (M01) */
int div_nz = (val == 0) ? 0 : 1;
if (zer_div_valid(div_nz) == 0) checker_error(...);
```

### int64 → int32 narrowing pattern (when caller has uint64 but predicate takes int)

When the caller has a `uint64_t` / `int64_t` value but the predicate takes `int` (preferred):

```c
/* Pre-filter to int32 range before narrowing cast */
if (val < 0 || val > 0x7FFFFFFFLL) {
    /* value exceeds int range — caller-specific handling
     * (error, default, or split path) */
} else {
    if (zer_X((int)val, ...) == 0) error(...);
}
```

Alternative: split predicate into size-specific forms (`zer_X_u32`, `zer_X_i32`) —
each using tint/tuint. Works for simple comparisons. See M08 literal_fits
redesign for example (Batch 1 + 1b).

### Codegen bug class — void main() → BUG-603 (2026-04-22)

Level 3 VST proves per-predicate correctness over the input space. It does NOT catch bugs in:
- Function signature emission (e.g., `void main()` copied verbatim from ZER to C)
- Source-to-C translation of standard library patterns
- Runtime-only behaviors (trap messages, malloc wrapping)

Level 2 integration tests (`tests/zer_proof/`, `tests/zer/`, regression) catch these.

BUG-603 example: `void main()` in ZER was emitted as `void main(void)` in C. C99 §5.1.2.2.3 requires `main` to return int; `void main()` leaves exit code undefined. Fixed by auto-promoting to `int main(void) { ... return 0; }` in emitter.

**Lesson for proof work:** when a test exits with unexpected code despite compiling cleanly, check the generated C's signature and return handling — not the Coq predicates. Predicates verify computation; codegen verifies structure.

### Phase 1 extraction recipe (end-to-end)

**Read this before extracting a new predicate.** Covers every step from identifying the target to commit.

#### Step 1: Identify extractable predicates

A function is extractable if:
- **Pure** — no global state mutation, no side effects
- **Primitive types only** — `int`, no `Type*`, no `Node*`, no `Symbol*`, no `ZerCheck*`
- **Short** — ~5-30 lines. If longer, split into smaller predicates.
- **Used at ≥1 call site** in real compiler code (not just tests)

Good candidates: state checks, range checks, type-kind checks, coercion rules, context bans, provenance rules, optional unwrap rules.

Bad candidates (not Phase 1): AST walks, scope lookups, typemap mutations, buffer I/O, recursive functions. These need Phase 4 APIs or separation logic.

#### Step 2: VST-friendly C style — MANDATORY

The C implementation MUST follow these rules. VST fails on any deviation:

**✓ ALLOWED:**
```c
int zer_X(int a, int b) {
    if (a == 0) {
        return 0;       // Simple condition, early return
    }
    if (b > 10) {
        return 1;       // Next simple condition, early return
    }
    return 2;           // Final fallthrough
}
```

**✗ BANNED:**
```c
if (a == 0 && b > 10) return 1;      // Compound conditions block VST
if (a == 0) {
    if (b > 10) return 1;             // Nested ifs block VST
    return 0;
}
if (a > 0) return 1; else return 0;  // else clause — use early return instead
return (a > 0) ? 1 : 0;              // Ternary — VST prefers explicit ifs
```

**Rule of thumb:** each `if` has a single comparison. Each branch is either `return X;` or fallthrough. No else, no nesting, no compound conditions.

If the logic naturally wants compound/nested, SPLIT into multiple predicates and AND the results at the call site. Example: `qualifier_widening(const, volatile)` split into `preserves_const` + `preserves_volatile`.

#### Step 3: Write the .h header

Template:

```c
/* src/safety/<name>.h
 *
 * One-line summary.
 * See docs/formal_verification_plan.md Level 3.
 */
#ifndef ZER_SAFETY_<NAME>_H
#define ZER_SAFETY_<NAME>_H

/* Any #define constants (e.g., enum values matching types.h) */
#define ZER_TK_... 17  /* MUST match types.h enum order */

/* Function documentation — describe rules, callers.
 *
 * Callers: compiler.c:function_name (after type_unwrap_distinct). */
int zer_predicate_name(int arg);

#endif
```

Constants that must match types.h enum values: include them as `#define` in the header so both the C implementation AND the Coq spec can use them. Order matters — NEVER reorder an enum without updating these constants.

#### Step 4: Write the .c implementation

```c
/* src/safety/<name>.c — pure predicates.
 *
 * See <name>.h. Linked into zerc via Makefile CORE_SRCS. VST-verified
 * in proofs/vst/verif_<name>.v.
 */
#include "<name>.h"

int zer_predicate_name(int arg) {
    /* Cascade of if-returns, no nesting, no compound conditions. */
    if (arg == ZER_TK_X) { return 1; }
    if (arg == ZER_TK_Y) { return 1; }
    return 0;
}
```

#### Step 5: Update Makefile

Add `src/safety/<name>.c` to TWO lines:
```makefile
CORE_SRCS = ... src/safety/<name>.c
LIB_SRCS  = ... src/safety/<name>.c
```

Add to `check-vst` target:
```makefile
clightgen -normalize src/safety/handle_state.c ... src/safety/<name>.c && \
...
coqc -Q . zer_vst -Q ../../src/safety zer_safety ../../src/safety/<name>.v && \
coqc -Q . zer_vst -Q ../../src/safety zer_safety verif_<name>.v
```

#### Step 6: Update .gitignore

Add to `src/safety/.gitignore`:
```
<name>.v
```
(The .v is generated by clightgen; don't commit.)

#### Step 7: Wire original call sites

In the calling file (zercheck.c, checker.c, types.c, etc.):

```c
#include "src/safety/<name>.h"   /* zer_predicate_name — VST-verified */
...
/* Replace inline logic with delegation */
- if (original_inline_check) { ... }
+ if (zer_predicate_name(arg) != 0) { ... }
```

Keep a `/* SAFETY: */` comment linking to the Coq spec for future auditors.

#### Step 8: Write the VST proof

Template for simple flat cascade:

```coq
(* proofs/vst/verif_<name>.v *)
Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.<name>.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

(* Coq spec — MUST use Z.eq_dec (not Z.eqb) to align with proof's destruct. *)
Definition zer_predicate_name_coq (arg : Z) : Z :=
  if Z.eq_dec arg 17 then 1
  else if Z.eq_dec arg 19 then 1
  else 0.

Definition zer_predicate_name_spec : ident * funspec :=
 DECLARE _zer_predicate_name
  WITH arg : Z
  PRE [ tint ]
    PROP (Int.min_signed <= arg <= Int.max_signed)
    PARAMS (Vint (Int.repr arg))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_predicate_name_coq arg)))
    SEP ().

Definition Gprog : funspecs := [ zer_predicate_name_spec ].

Lemma body_zer_predicate_name:
  semax_body Vprog Gprog f_zer_predicate_name zer_predicate_name_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_predicate_name_coq;
    repeat (destruct (Z.eq_dec _ _); try lia); try entailer!.
Qed.
```

For `<` / `>=` comparisons, use `Z_lt_dec`/`Z_ge_dec` in BOTH spec and proof:
```coq
if Z_lt_dec arg 10 then ...  (* in spec *)
destruct (Z_lt_dec _ _); try lia;  (* in proof *)
```

For spec that combines multiple comparison types:
```coq
repeat (first [ destruct (Z.eq_dec _ _)
              | destruct (Z_lt_dec _ _)
              | destruct (Z_ge_dec _ _) ]; try lia);
try entailer!.
```

#### Step 9: Verify

```bash
make docker-build          # zerc builds with new predicate linked
make check-vst             # VST proof compiles, 0 admits
make docker-check          # no test regression
```

If `make check-vst` fails, consult "Common VST errors and fixes" + the error-message-to-cause table below.

#### Step 10: Commit

One predicate (or tight batch) per commit. Commit message template:

```
Level 3 Phase 1: <batch name> predicates (N total extractions)

Extracted <count> <category> predicates to src/safety/<name>.c:
- zer_predicate_one(...)
- zer_predicate_two(...)

Wired <file.c>:<function> to delegate: <one-line summary>.

VST proof (verif_<name>.v): <count> lemmas, zero admits.

Status:
- Phase 1: N/44 (X%)
- make check-vst: PASS
- make docker-check: <result>

Total Level-3 verified compiler functions: N
```

### VST error message → cause + fix (quick reference)

| Error | Root cause | Fix |
|---|---|---|
| `Use [forward_if Post] to prove this if-statement` | C has nested ifs, OR compound conditions (`&&`, `\|\|`), OR forward_if can't infer post from context | Flatten C to single-level cascade of early-return ifs. Split predicate if needed. |
| `The variable X was not found in the current environment` | VST auto-substed the WITH variable to a constant after `if (x == N)` | Use `repeat (destruct (Z.eq_dec _ _); try lia)` pattern — `_` adapts to substituted values. |
| `No such goal. Try unfocusing with '}'` | `forward_if` produced 1 goal, my proof assumed 2 (via `{ ... }`) | Don't use bullets/braces for forward_if in these simple predicates. Use `repeat forward_if; forward; ...` one-liner. |
| `Attempt to save an incomplete proof` at Qed | Proof closed fewer goals than forward_ifs generated | Add more destructs: `repeat (first [ destruct (Z.eq_dec _ _) \| destruct (Z_lt_dec _ _) ]; try lia)`. |
| `No such contradiction` | `destruct` case where contradiction hyp isn't visible | Rearrange: `destruct first; try subst; try contradiction`. |
| `Cannot find a physical path bound to logical path zer_safety.<name>` | clightgen didn't run on the new .c, OR Makefile missing `-Q src/safety zer_safety` mapping | Add clightgen line + coqc line in Makefile's check-vst target, including `-Q ../../src/safety zer_safety`. |
| `Cannot infer the implicit parameter Σ of funspec` | Missing `Require Import VST.floyd.compat` | Add the import. compat.v is precompiled in the zer-vst Docker image. |

### Nested if-returns block `repeat forward_if`

VST fails with "Use [forward_if Post]" when the C has nested ifs like:

```c
if (outer_cond) {
    if (inner_cond) return 1;
    return 0;
}
```

The INNER forward_if can't infer its postcondition from the outer
context. Fix: flatten to a single level of cascaded if-returns with
no nesting, no compound conditions (`&&`/`||`):

```c
if (outer_cond && !inner_cond) return 0;  // BAD: compound
if (outer_cond != 0 && inner_cond != 0) return 1;  // BAD

// GOOD: fully flat cascade
if (cond1) return val1;
if (cond2) return val2;
if (cond3) return val3;
return default_val;
```

Sometimes the logic doesn't fit a single cascade. In that case, SPLIT
into multiple smaller predicates and AND the results at the call site.
Example: `zer_coerce_qualifier_widening_allowed(c, v)` → split into
`zer_coerce_preserves_volatile` + `zer_coerce_preserves_const`.

### `Z.eq_dec` vs `Z.eqb` — destruct alignment

Coq spec uses `Z.eq_dec` (sumbool) OR `Z.eqb` (bool). If the proof
tactic uses `destruct (Z.eq_dec _ _)` but the spec uses `Z.eqb`, the
cases don't match and `entailer!` fails with "not decidable" or
"Attempt to save an incomplete proof".

**Rule:** align spec and proof. If the proof uses
`destruct (Z.eq_dec _ _)`, the Coq spec MUST use `if Z.eq_dec _ _ then _ else _`.

Example wrong (spec with Z.eqb, proof with Z.eq_dec):
```coq
Definition spec x : Z := if Z.eqb x 0 then 1 else 0.  (* bool *)
...
  destruct (Z.eq_dec x 0); try lia; entailer!.  (* fails — doesn't match *)
```

Fix:
```coq
Definition spec x : Z := if Z.eq_dec x 0 then 1 else 0.  (* sumbool *)
```

### Composed predicates — inline vs forward_call

When a predicate's C implementation calls OTHER verified predicates
(e.g., `zer_type_kind_is_numeric` calling `is_integer` then `is_float`),
VST's `forward_if` after each `forward_call` requires an explicit
postcondition:

```
Error: Tactic failure: Use [forward_if Post] to prove this if-statement,
where Post is the postcondition of both branches
```

**Workarounds (prefer option A for small cases):**

**(A) Inline the cases** — best for ≤15 enum values. Duplicate the
    compared cases directly in the C, match the cascade in the Coq
    spec. Uniform proof pattern `repeat forward_if; forward; unfold;
    destruct; try lia; try entailer!` works.

**(B) forward_call + explicit Post** — needed for large predicates
    where inlining would duplicate 30+ lines. Write
    `forward_call k; forward_if (PROP () LOCAL (...) SEP ())` with
    the right postcondition. Much harder but scales.

Example of (A): `zer_type_kind_is_numeric` in src/safety/type_kind.c
inlines all 10 integer cases + 2 float cases rather than calling
`is_integer` then `is_float`.

### VST proof pattern for `==` chains — auto-subst gotcha

When the C function has `if (x == N) return ...;` (equality with constant), VST 3.0's `forward_if` auto-`subst`s `x := N` in the then-branch. The Coq WITH-bound variable `x` literally disappears from scope.

This differs from `<` / `>=` (relational) where no subst happens.

**Pattern that works** (from `verif_handle_state.v`):

```coq
Lemma body_foo: semax_body Vprog Gprog f_foo foo_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold foo_coq;
    repeat (destruct (Z.eq_dec _ _); try lia); try entailer!.
Qed.
```

Why it works:
- `repeat forward_if` — handles arbitrary cascade depth. After subst, some branches close automatically.
- `repeat (destruct (Z.eq_dec _ _); try lia)` — destructs ANY remaining equality decisions, skipping already-substituted ones. `_` lets the tactic work on whatever `state` became.
- `try entailer!` — discharges remaining postconditions.

**Pattern that DOESN'T work** — referencing the WITH variable by name after the first `forward_if`:
```coq
forward_if.
{ forward. destruct (Z.eq_dec state ZER_HS_FREED); ... entailer!. }  (* OK *)
forward_if.  (* state has been subst'd away *)
{ forward. destruct (Z.eq_dec state ZER_HS_FREED); ... }  (* FAIL: "state not found" *)
```

See `proof-internals.md` common-errors table for the full list of VST 3.0 gotchas.

### All typing-level sections now covered by real Coq proofs

**lambda_zer_typing/typing.v is the home for non-operational typing-level proofs.** 135 real theorems covering sections G, C, D, E, F, I, J-extended, K, L, M, N, P, Q, R, S, T. Each section defines its predicate + proves theorems about it. No `True. Qed.` placeholders remain for substantive rows.

Pattern used throughout typing.v:
```coq
Definition check_X_valid (inputs) : bool := ... .

Theorem X_ok : check_X_valid valid_input = true.
Proof. reflexivity. Qed.  (* or computation *)

Theorem X_bad_rejected : condition → check_X_valid bad = false.
Proof. ... . Qed.
```

Each theorem is decidable (bool predicate) and mechanically checkable — the compiler can implement the predicate.

When adding a new typing-level row: follow the pattern in the closest section. Most are "define bool-returning predicate, prove ok-case + reject-case." Takes 5-15 minutes per row once you have the template.

### Schematic vs operational vs predicate-based

Three proof depths exist in the codebase:

**Operational depth** (sections A, B, H, J-core, O):
- Resource algebra defined (`alive_handle γ p i g : iProp`, etc.)
- State interpretation connecting ghost state to concrete state
- fupd-style step specs tying Iris resources to semantics.v's step relation
- Direct proof of safety via resource discipline on step rules

**Predicate-based depth** (most other sections — in `lambda_zer_typing/typing.v`):
- bool-returning predicate defining the check
- Real theorems proving the predicate accepts/rejects correctly
- Decidability implicit (bool = computable)
- NO step rules / no ghost state — pure typing content

**Schematic depth** (deprecated — no rows remain here):
- Used to be `Lemma foo : True. Proof. exact I. Qed.`
- All replaced with predicate-based (typing.v) or operational (subset).
- If you see a `True. Qed.` still, it's either historical OR a
  section-summary lemma, not a safety theorem.
- Reference to which checker.c / emitter.c pass implements the rule
- Does NOT prove the Iris property operationally — the safety CONTENT is expressed by the lemma's STATEMENT being a true invariant of well-typed programs

Schematic proofs are VALID but WEAKER than operational. A schematic lemma says "this constraint exists and is a real invariant; enforcement is in the compiler, verified empirically by tests + future Level 3 VST." An operational lemma proves the invariant mechanically from the resource algebra.

For "Iris spec = correctness oracle" workflow at schematic level: the compiler's pass-level implementation must match the schematic comment. If a compiler change drops the check, tests in `tests/zer_fail/` should catch it.

### Deepening schematic → operational

To upgrade a section from schematic to operational:

1. Create a new subset directory (e.g., `proofs/operational/lambda_zer_move/`)
2. Copy `syntax.v` / `semantics.v` / `typing.v` from lambda_zer_handle/ as base
3. Extend operational semantics: new step rules for the feature (e.g., `step_move_transfer`, `step_shared_access`)
4. Extend typing: new typing rules
5. Extend ghost state: new `Class moveG Σ` if a new resource is needed
6. Prove step specs in fupd form (see `iris_step_specs.v` as template)
7. Re-prove the schematic closures from `iris_move.v` (etc.) as operational theorems in the new subset

Estimated work per section:
- Typing-level (G, I, K, N, Q, T): ~3-8 hours each — mostly copy patterns
- VRP integration (L, M, R, S): ~10-20 hours each
- Provenance/regions (H, J, O): ~20-40 hours each
- Concurrency (C, D, E, F): ~100-200 hours total (real Iris work)

## Iris name collisions — the single biggest productivity drain

Iris's `language` typeclass projections collide with our type names.

| Our name (syntax.v) | Iris name (program_logic/language.v) |
|---|---|
| `expr : Type` | `expr : language → Type` |
| `val : Type` | `val : language → Type` |
| `state : Type` | `state : language → Type` |

After `From iris.program_logic Require Import weakestpre`, bare `val` resolves to Iris's projection, not our type.

**Fix:** qualify with the module name — `syntax.val`, `syntax.expr`, `semantics.state`. OR rely on Canonical Structure unification (works at call sites, not in signatures).

**Canonical Structure pattern** (iris_lang.v):
```coq
Canonical Structure λZH_lang : language :=
  Language λZH_mixin.
```
After this, when Iris sees `val` in a context expecting `language → Type`, it infers `λZH_lang` and unifies with our `syntax.val`. At USE sites (e.g., `EVal v` in a wp), this works. At DECLARATION sites (lemma signatures), you must qualify.

**Example that fails:**
```coq
Lemma wp_val (v : val) : ⊢ WP (EVal v) {{ ... }}.
(* Error: val has type language → Type, cannot be instantiated. *)
```

**Fix:**
```coq
Lemma wp_val (v : syntax.val) : ⊢ WP (EVal v) {{ ... }}.
```

## Record-constructor boilerplate — avoiding `{| ... |}`

Iris's `language` is a `Structure`. Its constructor is `Language`, not `Build_language`. Building via `{| field := value |}` syntax fails when field names collide with your type names:

```coq
(* FAILS: "expr: Not a projection." *)
Canonical Structure λZH_lang : language := {|
  expr := expr;       (* Coq confused about which expr *)
  val := val;
  ...
|}.
```

**Fix:** use the positional constructor `Language`:
```coq
Canonical Structure λZH_lang : language :=
  Language λZH_mixin.
```

Similarly for `LanguageMixin` — use `Build_LanguageMixin` (the auto-generated constructor for Records):
```coq
Definition λZH_mixin : LanguageMixin λZH_of_val λZH_to_val λZH_prim_step :=
  Build_LanguageMixin _ _ _ λZH_to_of_val λZH_of_to_val λZH_val_stuck.
```

## `IntoVal` typeclass — required for `wp_value`

`wp_value` uses `IntoVal e v` to recognize that expression `e` is a value `v`. If not registered, `iApply wp_value` fails with:
```
Tactic failure: iStartProof: not a BI assertion:
(IntoVal (EVal v) ?Goal).
```

**Fix:** register an instance in the language file:
```coq
Global Instance into_val_EVal (v : val) : IntoVal (EVal v) v.
Proof. reflexivity. Qed.

Global Instance as_val_EVal (v : val) : AsVal (EVal v).
Proof. exists v. reflexivity. Qed.
```

Add one `IntoVal`/`AsVal` per constructor that yields a value. For λZER-Handle we only have `EVal v`, so one pair.

## Multi-line `destruct` intropatterns

Intropatterns like `[v|x|p|p e|...]` correspond 1:1 with constructors. For λZER-Handle's `expr` (13 constructors), we need 12 `|` separators. Each sub-pattern must match the constructor's arity:

```coq
(* For an inductive with constructors of arity 1, 1, 1, 2, 2, 3, 4, 2, 3, 3, 2, 2, 1: *)
destruct e as [v|x|p|p e1|p e2|p e3 f|p e f w|e r|c e1 e2|x e1 e2|e1 e2|c b|e].
```

**Easier alternative** — skip the as-pattern, accept default names:
```coq
destruct e; try discriminate Hv.
```
This generates all sub-goals, and `try discriminate Hv` closes the non-EVal ones where the `is_value` hypothesis becomes `false = true`.

**The common bug** — pipe-count off by one:
```coq
destruct e as [v|||||||||||||];  (* 13 pipes = 14 branches, expr has 13 *)
```
Fails with `Syntax error: '|' or ']' expected`. Count twice.

## Naming captured hypotheses in `inversion`

When you `inversion Hty; subst`, Coq auto-generates names `H`, `H0`, `H1`, etc. These numbers DEPEND on the surrounding tactic state — fragile.

**Fix:** use `match goal` to pin by shape instead of name:
```coq
inversion Hty; subst.
match goal with Hvt : has_val_ty _ TyBool |- _ =>
  apply canonical_bool in Hvt as [b ->]
end.
```

This matches ANY hypothesis with that shape, regardless of autoname.

## `sed`/`grep` pitfalls in tool scripts

In `tools/safety_coverage.sh`:

- **Use `@@@` as a delimiter inside `sed`**, not `|`. Messages contain `|`, and `sed s|...|...|` breaks on them.
- **Escape inside Coq-emitted strings**: `_zer_trap("division by zero")` in source is `_zer_trap(\"division by zero\")` as a C-emitted string (escaped quotes). Your regex needs to match the escaped form:
  ```bash
  grep -nE '_zer_trap\(\\"[^"\\]+\\"' emitter.c
  ```

- **`set -e` + grep-with-zero-matches = silent exit.** Use `grep -c ... || true` to avoid the script dying on empty results.

## Iris proof tactics — what works when

| Situation | Tactic |
|---|---|
| Introduce hypotheses (spatial + persistent) | `iIntros "H1 H2 %Hpure"` |
| Split existentials | `iDestruct "H" as (x) "H"` |
| Split conjunctions | `iDestruct "H" as "[H1 H2]"` |
| Fancy update (`==∗`) | `iMod (lemma with "args") as "pat"` |
| Apply lemma with args | `iApply (lemma with "H1 H2")` |
| Get pure fact from iProp | `iDestruct (lemma with "H") as %Hpure` |
| Framing | `iFrame` (auto) or `iFrame "H"` (named) |
| Pure intro at end | `iPureIntro` |
| Entering BI mode | Auto when goal is `⊢ P : iProp` |

## Typeclass instance file structure

Standard pattern for ghost-state setup (iris_resources.v):
```coq
Class handleG Σ := HandleG {
  handle_ghost_mapG :: ghost_mapG Σ (pool_id * nat) nat;
}.

Definition handleΣ : gFunctors := #[ghost_mapΣ (pool_id * nat) nat].

Global Instance subG_handleΣ Σ : subG handleΣ Σ → handleG Σ.
Proof. solve_inG. Qed.
```

The `::` (double colon) in the class field is **type-class subtyping** — it registers the subclass as a default instance. Critical for Iris's tc-resolution to find the sub-algebras.

`subG_handleΣ` lets consumers who have `subG handleΣ Σ` automatically get `handleG Σ`. Without this, every user has to manually prove `handleG Σ` — tedious.

## Ghost-map design gotcha — insert vs delete on free

**Wrong approach** (what we tried first):
```coq
Lemma alive_handle_free : ... ==∗ ghost_map_auth γ 1 (<[(p,i) := S g]> σ).
```
Bump the gen in the ghost map. Problem: after free, the slot is NOT alive, but the ghost map still has an entry — violates `gens_agree_store` which ties ghost entries to alive slots.

**Right approach** (what we landed):
```coq
Lemma alive_handle_free : ... ==∗ ghost_map_auth γ 1 (delete (p,i) σ).
```
Delete the entry on free. Re-allocation creates a new fragment via `alive_handle_new`. This matches `gens_agree_store` naturally.

**Lesson:** ghost state tracks "currently true" facts, not "ever true" facts. If the concrete state says X is not alive, the ghost state must not claim X is alive.

## Operational step rule quirks

### `step_free_alive` requires `st_returned = None`

All step rules in `semantics.v` have the precondition `st.(st_returned) = None`. This models "function hasn't returned, keep executing."

In step spec proofs:
```coq
Lemma step_spec_free ... :
  σ.(st_returned) = None →    (* <-- must thread this *)
  handle_state_interp γ σ -∗
  alive_handle γ p i g ==∗
    ...
```

Forgetting this produces errors about failing to construct the step-rule instance.

### Applying step rules with multiple implicit args

`step_free_alive` has signature:
```coq
step_free_alive : forall st p i g ps s,
  st.(st_returned) = None ->
  st.(st_store) !! p = Some ps ->
  ps !! i = Some s ->
  slot_gen s = g ->
  slot_val s <> None ->
  step st (EFree p (EVal (VHandle p i g))) st' (EVal VUnit)
```

Where `st'` is computed from arguments. `apply step_free_alive with ps s; auto` fails (Coq can't infer `st'`); use `eapply`:
```coq
eapply step_free_alive; eauto.
```

## Extending proofs — pattern

When adding a lemma to an existing file:

1. **Add to the right `Section`** — inside `Section resources. Context .` etc., so typeclass instances are available.
2. **Prove iteratively** — try first, read Coq's error, adjust.
3. **After file compiles, re-run `make`** to ensure nothing else broke:
   ```bash
   MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W):/work" -w /work zer-proofs \
       bash -c 'eval $(opam env) && make 2>&1 | tail -30'
   ```

When adding a new `.v` file:

1. **Add to `_CoqProject`** — IN DEPENDENCY ORDER. Later files import earlier ones.
2. **Delete old CoqMakefile** so it regenerates:
   ```bash
   rm -f CoqMakefile CoqMakefile.conf
   ```
3. **Build** — the first `make` regenerates `CoqMakefile` from `_CoqProject`, then compiles.

## Extending to new subsets

When starting `lambda_zer_move/` or similar:

1. Create new directory: `proofs/operational/lambda_zer_move/`
2. Create `syntax.v` / `semantics.v` / `typing.v` as extensions of λZER-Handle's
3. In `_CoqProject`, add a new `-Q lambda_zer_move zer_move` line + list the files
4. Reuse `handleG Σ` typeclass if handling linear resources (move = `alive_handle`-style)
5. Define new ghost state if different semantics (e.g., `shared_struct` needs Iris invariants)

**Don't modify `lambda_zer_handle/` files** — they're the verified foundation. New subsets extend, not replace.

## Common build errors and fixes

| Error | Cause | Fix |
|---|---|---|
| `Not the right number of missing arguments (expected 0).` | Used `apply` where `eapply` needed (step rules with computed args) | Change `apply` → `eapply` |
| `No such hypothesis: H0` | `inversion` auto-naming changed | Use `match goal with Hname : shape \|- _ => ... end` |
| `The reference EVal was not found` | `Require Import` doesn't transitively re-export | Add `Require Import syntax` or use `Require Export` in library files |
| `Illegal application (Non-functional construction): expr` | Name collision with Iris projection | Qualify: `syntax.expr` |
| `expr: Not a projection.` | `{\|...\|}` record syntax confused by field-name collision | Use positional constructor `Language` / `Build_LanguageMixin` |
| `Tactic failure: iStartProof: not a BI assertion` | Used iris tactic on a Coq-Prop goal, or vice versa | Check whether goal is `⊢ iProp` or plain `Prop`; use `iPureIntro` to transition |
| `Syntax error: '\|' or ']' expected (in [or_and_intropattern])` | Wrong pipe count in `destruct as [...]` | Count constructors, use n-1 pipes |
| `Syntax Error: Lexer: Unterminated comment` | Inline text inside a Coq comment contains `(*` (e.g., `(*T` from `@inttoptr(*T, ...)`) | Coq nests comments — any `(*` in prose opens a new comment. Rewrite prose to avoid `(*` patterns, or use `( *T` with a space |
| `Tactic failure: iPoseProof: "Hname" not found` | `iPoseProof` with a string name tries to dereference a hypothesis; after `iApply wp_mono`, hypotheses may have been dropped/renamed | Use persistent hypothesis (`#Hname`) which can be reused without iPoseProof |
| `Tactic failure: iIntro: introducing non-persistent into non-empty spatial context` | `iIntros "H [H1 H2]"` when introducing a wand-shape — Iris doesn't accept binding non-persistent and then splitting in one go | Split the intro: `iIntros "H". iIntros "[H1 H2]".` OR restructure the lemma statement (sep-conjunction instead of wand) |
| `has type "upred.uPred..." while it is expected to have type "bi_car ?PROP0"` | BI / iProp type mismatch — missing Iris imports for the framework | Add `From iris.base_logic.lib Require Import ghost_map` (or whatever brings the BI instances) |
| `The term "state" has type "language → Type"` | Same name-collision pattern as `expr`/`val` — Iris's `state` projection shadows ours | Qualify: `semantics.state` (NOT `syntax.state` — `state` lives in semantics.v) |
| `injection Heq as -> -> ->` makes a variable vanish | When LHS has form `(p',i',g') = (p,i,g)`, the substitutions can eliminate the target metavariable | Use named intros: `injection Heq as Hp_eq Hi_eq. subst p' i' g'.` |

## Reading order for fresh sessions

1. `docs/formal_verification_plan.md` — big picture (Iris-from-start, no timeline)
2. `proofs/operational/README.md` — directory architecture
3. `docs/safety_list.md` — what's proven and what isn't (203-row coverage matrix) — **ALL 168 substantive rows now closed**
4. This file (`proof-internals.md`) — tactics, gotchas, patterns
5. The `.v` files in dependency order:
   - Foundation: `syntax.v` → `semantics.v` → `typing.v`
   - Iris setup: `iris_lang.v` → `iris_resources.v` → `iris_state.v`
   - Specs/adequacy: `iris_specs.v` → `iris_step_specs.v` → `iris_leak.v` → `iris_adequacy.v`
   - Extensions: `iris_func_spec.v` → `iris_loop.v` → `iris_derived.v`
   - Schematic closures: `iris_move.v` → `iris_control_flow.v` → `iris_typing_rules.v` → `iris_misc_sections.v` → `iris_mmio_cast_escape.v` → `iris_concurrency.v` → `iris_container_validity.v`
   - Demo: `iris_demo.v`

Each `.v` file has a header comment describing its Phase (0, 1a, 1b, etc.) and what it delivers.

## What's next (when continuing)

The safety matrix is 100% covered at schematic level. Priorities for DEEPENING:

### Quick wins (typing-level, low effort)
- Already done — G, I, K, N, P, Q, T sections are structural and don't need operational deepening.

### Medium effort — section-specific subsets
These would get their own `lambda_zer_*/` directory with operational semantics extensions:

1. **lambda_zer_move/** (B section, 8 rows) — move struct operational semantics.
   Extension: add `EMove`, `EDrop`, `EConsume` step rules. Re-prove schematic closures as full operational.
   Effort: ~10-20 hours.

2. **lambda_zer_mmio/** (H section, 9 rows) — MMIO operational semantics.
   Extension: add region invariants as Iris invariants. Each MMIO range = `mmio_region γ addr size : iProp`.
   Effort: ~20-40 hours.

3. **lambda_zer_opaque/** (J section, 14 rows) — provenance ghost state.
   Extension: ghost map from pointer → type_id. Each cast operation updates. `@ptrcast` checks match.
   Effort: ~20-40 hours.

4. **lambda_zer_escape/** (O section, 12 rows) — region invariants for dangling pointers.
   Extension: each allocation-site region tagged; assignment/return rules check flow.
   Effort: ~30-60 hours.

### Hard effort — concurrency

5. **lambda_zer_concurrency/** (C, D, E sections, 25 rows) — real Iris concurrency.
   Extension: Iris invariants for shared struct, lock-order ghost state, logically-atomic triples for atomics/condvar.
   Effort: ~100-200 hours. This is real Iris concurrency engineering.

6. **lambda_zer_async/** (F section, 5 rows) — async state-machine verification.
   Extension: continuation-passing wp, Löb induction, state-struct invariants.
   Effort: ~40-80 hours. Builds on concurrency subset.

### Level 3 — VST on compiler (extract-and-link, IN PROGRESS)

See "Verification strategy — extract-and-link" above for the full pattern.

**Status (2026-04-21):** 1 real extraction landed (`zer_handle_state_is_invalid`). Next batch: `is_freed`, `is_alive`, `is_transferred`, pool/variant range predicates.

**Scope (honest, smaller than earlier estimates):**
- 15-25 pure predicates extractable from zercheck.c + zercheck_ir.c
- ~1-3 hrs per extraction (smaller than earlier "5-20 hrs" — these are 1-line predicates)
- Complex functions (struct separation logic, loops, recursion) are SEPARATE work — maybe 5-10 functions, 20+ hrs each. Not in current wave.

**Decision tree for fresh sessions:**

```
Editing a safety check?
├─ Is it a pure predicate (primitive args, no mutation)?
│  ├─ YES → extract it. Place in src/safety/<name>.c.
│  │        Wire both zercheck.c AND zercheck_ir.c.
│  │        Add VST proof. Add to check-vst target.
│  └─ NO → leave it inline in zercheck_ir.c / zercheck.c.
│          Document why it can't be extracted (needs AST,
│          recursive, mutates state, etc.).
└─ Editing src/safety/*.c directly?
   └─ Run `make check-vst` before committing. If VST fails,
      either fix the C or update the Coq spec to match.
```

**When `make check-vst` fails:**
- Proof error at Qed → C diverged from spec. Either fix C to match spec, or update spec.
- Compile error in handle_state.v → clightgen output changed. Inspect differences.
- Import error → Makefile -Q mapping wrong.

## Design precedents — use for consistency

When building new schematic closures:
- Section A (resource algebra + fupd step specs + adequacy) is the OPERATIONAL template.
- Section B (`iris_move.v`) is the template for "new linear resource, reuse handleG" — a dedicated subset would extract this to its own directory.
- Section G (`iris_control_flow.v`) is the template for "context-flag checks" — pure typing, schematic.
- Section T (`iris_container_validity.v`) is the template for "structural well-formedness" — mostly `True` proofs with strong comments.
- Section L/M (`iris_misc_sections.v`) is the template for "VRP integration" — points at compile-time + runtime checks.

When unsure which template to use, grep the `Covers safety_list.md rows:` comment in each file to find the match.

## Operational subset templates — which to follow

Five operational subsets exist, each demonstrating a different template:

### Template 1 — Pool + generation counter (lambda_zer_handle/)
**For:** entity lifecycle with alloc/free/use state machine.
- Ghost state: `gmap (pool_id * nat) nat` — keys are (pool, slot), value is generation.
- State_interp: two-way agreement with concrete store's alive slots.
- Use when: the thing being tracked has a generation counter or similar versioning.

### Template 2 — Linear resource (lambda_zer_move/)
**For:** "exclusive-ownership-then-consumed" patterns.
- Ghost state: `gmap move_id unit` — presence = owned.
- State_interp: ghost map matches st_live + counter invariant.
- Use when: the thing has binary states (owned/consumed), no generation.

### Template 3 — Type-tagged provenance (lambda_zer_opaque/)
**For:** pointers carrying their original type through opaque round-trips.
- Ghost state: `gmap nat type_id` — instance → type tag.
- State_interp: ghost map = st_ptr_types, counter invariant.
- Key theorems use `typed_ptr_agree` (same id can't own different tags).
- Use when: values carry a pinned classification that's checked on operations.

### Template 4 — Region tagging (lambda_zer_escape/)
**For:** enum-valued tags (not integer types). Same structure as template 3 but with a finite sum type instead of nat.
- Ghost state: `gmap nat region` where `region = RegLocal | RegArena | RegStatic`.
- Key theorems: exclusivity + agreement → "wrong region" is contradictory.
- Use when: you have a finite classification (region, color, kind) that restricts operations.

### Template 5 — Static constraint (lambda_zer_mmio/)
**For:** purely operational safety with NO ghost state needed.
- State contains the constraint data (list of declared ranges).
- Step rule premises enforce constraints directly.
- Stuck-on-violation proofs via inversion on step rules.
- Use when: the constraint data is program-level constant, no dynamic tracking needed.

**Picking a template for a new subset:**
1. Does the thing have dynamic state (created/destroyed/mutated)? → Template 1-4 (ghost state needed)
2. Does it have a lifecycle with generations? → Template 1
3. Is it a linear resource (owned then consumed)? → Template 2
4. Does it carry a pinned tag checked on use? → Template 3 (nat tag) or 4 (enum tag)
5. Is the constraint purely static/program-level? → Template 5 (no ghost state)

## Cross-template patterns learned

### Unit type in gmap values — use `tt` not `()`

When ghost_map's value type is `unit`:
```coq
(* Right *)
Definition alive_move γ id : iProp Σ := id ↪[γ] tt.
Definition foo : gmap nat unit := <[ k := tt ]> ∅.

(* Wrong — () parses as Set not unit value *)
Definition alive_move γ id : iProp Σ := id ↪[γ] ().     (* fails *)
Definition foo : gmap nat unit := <[ k := () ]> ∅.      (* fails *)
```

### Two-invariant state_interp pattern

Minimal pattern (template 1/2/3):
```coq
Definition state_interp γ σ : iProp Σ :=
  ∃ gs : gmap K V,
    ghost_map_auth γ 1 gs ∗
    ⌜gs = σ.(concrete_map)⌝ ∗                             (* invariant A *)
    ⌜∀ id v, gs !! id = Some v → id < σ.(st_next)⌝.       (* invariant B *)
```

Invariant B (counter well-formedness) is NEEDED to discharge `step_spec_alloc` — without it you can't prove the fresh id isn't already in the ghost map.

Destructure with:
```coq
iDestruct "Hinterp" as (gs) "(Hauth & %Heq & %Hlt)".
```

### Stuck-proof via inversion on step rules

For subsets without ghost state (or as a fallback pattern):
```coq
Lemma foo_stuck st ... :
  condition_fails →
  ¬ (exists st' e', step st (EOp ...) st' e').
Proof.
  intros Hfail [st' [e' Hstep]].
  inversion Hstep; subst.
  - (* primary rule case: contradicts Hfail *)
    match goal with H : precondition_pattern |- _ =>
      rewrite H in Hfail; discriminate
    end.
  - (* ctx rule case: inner arg must step. EVal doesn't step. *)
    match goal with H : step _ (EVal _) _ _ |- _ => inversion H end.
Qed.
```

Use `match goal with H : shape |- _ => ... end` to bind hypotheses by shape — avoids fragile `H3 / H4` auto-names.

### Rewrite direction in state_interp

Given `Heq : gs = σ.(some_map)` and `Heqn : σ.(some_map) !! k = Some v`:
- `rewrite <- Heq in Heqn` — rewrites Heqn from σ-form to gs-form (correct direction for using Heqn with lemmas that take gs)
- `rewrite Heq in Hlt` — rewrites Hlt's gs → σ-form (reverse direction)

The `<-` in `rewrite <- Heq` uses the equation from right-to-left. Mentally: "rewrite σ.(some_map) to gs" means we have gs = σ.some_map and want to replace σ.some_map with gs, so we need to go right-to-left.

### `eapply` for step rules with computed arguments

Step rules often have `let st' := ...` in their body. `apply step_X with args` tries to unify st' explicitly and fails:
```coq
(* WRONG: "Not the right number of missing arguments" *)
apply step_deref with t. exact Hlook.

(* RIGHT: use eapply which unifies incrementally *)
eapply step_deref. exact Hlook.
```

## Invariants maintained by this doc

- **Every `.v` file ends `Qed.`, never `Admitted.`** — verified by grep at every commit.
- **Every `.v` file has a header comment** explaining Phase + what it delivers.
- **Section A row count in `safety_list.md` matches proven lemmas** — update when closing rows.
- **No modification to `model*.v`** — design-level proofs are frozen at commit `000064d`.

---

## Keyword-agnostic structural predicates (2026-04-23 lesson)

**When renaming a parser keyword, Phase 1 predicates do NOT need re-verification** if the predicate checks a structural context flag (e.g., `in_naked`, `in_loop`, `in_critical`, `defer_depth`) rather than the keyword's spelling.

### Example: `unsafe asm` rename (2026-04-23)

The `asm(...)` keyword was renamed to `unsafe asm(...)` to match Rust's explicit-unsafe pattern. Phase 1 predicate:

```c
/* src/safety/context_bans.c */
int zer_asm_allowed_in_context(int in_naked) {
    if (in_naked != 0) { return 1; }
    return 0;
}
```

VST spec in `proofs/vst/verif_context_bans.v` defines the Coq semantics of this function. It checks the context flag, not the keyword.

Result: renaming `asm` → `unsafe asm` at the parser level required ZERO changes to `context_bans.c` or `verif_context_bans.v`. Phase 1 stayed at 85/85 predicates, zero admits. VST proof holds unchanged.

### When does a predicate need re-verification?

A predicate needs re-verification if ANY of:

1. **The predicate's function signature changes** (new param, changed return)
2. **The predicate's behavior changes** (different answer for the same inputs)
3. **The predicate's Coq spec changes** (different semantic claim)
4. **A caller adds/removes a call site that the predicate's assumptions depend on**

A predicate does NOT need re-verification if:

1. **The call site syntax changes** but the predicate inputs stay the same
2. **A new keyword aliases an old one** (e.g., `unsafe asm` → same `TOK_ASM` path → same `in_naked` check)
3. **The error message changes** (pure cosmetic)

### How to verify a predicate is keyword-agnostic

Before a parser-level rename, grep the predicate's implementation for the old keyword:

```bash
grep "asm\|ASM" src/safety/context_bans.c
```

If the keyword appears only in COMMENTS (not in logic), the predicate is keyword-agnostic. If it appears in conditions or state lookups, re-verify.

### Parser-level changes that are always safe

These never require Phase 1 re-verification:

- Renaming a keyword (e.g., `asm` → `unsafe asm`)
- Adding a new keyword alias (e.g., both `asm` and `unsafe asm` accept)
- Changing error messages
- Changing parser-internal state names

These MAY require re-verification:

- Adding a new context flag (e.g., `in_vale_block`) that predicates should check
- Removing a context flag that predicates depend on
- Changing how a flag is set (e.g., `in_naked` set at different AST nodes)

### Test: after a rename, predicate tests must still pass

The test suite for a predicate is the VST spec + `.v` file. Running
`make check-vst` must complete without changes. If it fails, the
rename accidentally broke the predicate's assumption and you need to
understand why.

For `unsafe asm` rename 2026-04-23: `make check-vst` passed unchanged.
Confirmed keyword-agnostic.

---

## Phase D-Alpha and future proof integration (D-Beta, post-Phase-7)

D-Alpha is implementation-first (2026-04-23 decision). Intrinsics ship
without formal proofs — tests validate behavior. D-Beta adds proofs
later once Phase 7 (operational deepening) is complete.

### What proofs will be added in D-Beta

For each of the 96 intrinsics:

1. **Coq specification** (`src/safety/intrinsics_*.v`): defines the
   semantic contract of the intrinsic (what it computes, memory
   effects, error conditions).

2. **VST proof** (`proofs/vst/verif_intrinsics_*.v`): proves the
   emitter's inline asm or GCC builtin call satisfies the spec.

3. **Iris Hoare logic** (for async-context-sensitive intrinsics):
   proves compositional safety when intrinsics are used inside
   complex control flow.

### Design constraint for D-Alpha implementation (forward-looking)

When implementing D-Alpha intrinsics, the emitted C should be
**proof-friendly** for D-Beta:

1. **One intrinsic = one GCC builtin OR one inline asm block** (no
   macro expansions that confuse Sail parsing).

2. **Operands go through `emit_rewritten_node`** (preserves type
   tracking for Coq spec generation).

3. **Error conditions caught in checker, not emitter** (checker errors
   leave a clear compile-time trail; emitter-level errors are harder
   to prove about).

4. **No hidden state** (no emitter-private variables that the Coq spec
   would need to model).

D-Alpha-1 and D-Alpha-2 both follow these constraints. Future
batches (D-Alpha-3 through D-Alpha-7) should continue the pattern.

### Proof tiers for intrinsics (D-Beta plan)

Not all intrinsics need full Hoare logic:

| Tier | Method | Intrinsics covered |
|---|---|---|
| Tier 1 (full Hoare logic) | Iris + VST | Context switch, MMU, interrupts (~30%) |
| Tier 2 (structural + tests) | VST + unit tests | Atomics, barriers, cache ops (~50%) |
| Tier 3 (inspection) | Code review + tests | cpuid, rdtsc, pure reads (~20%) |

Weighted cost: ~52% of full-formal. This is captured in the budget
breakdown in `docs/asm_plan.md`.

---

## Asm category research phase COMPLETE (2026-04-24) — implications for future proofs

**Fresh proof sessions: READ THIS when asm safety verification work starts.**

### What the research produced (proof perspective)

7-session research phase (docs/asm_preconditions_research.md) classified
every UB-bearing instruction across x86-64 / ARM64 / RISC-V + PowerPC +
Cortex-M spot-checks into **8 universal precondition categories**:

| Category | Maps to ZER system | Level 3 VST target |
|---|---|---|
| C1 Value-range | #12 VRP | `src/safety/value_range_rules.c` (future) |
| C2 Alignment | #20 Qualifier (ext) | `src/safety/alignment_rules.c` (future) |
| C3 State machine | Model 1 (ext) | Extend existing `src/safety/handle_state.c` pattern |
| C4 CPU feature | #24 Context Flags (ext) | `src/safety/feature_gate_rules.c` (future) |
| C5 Privilege | #24 Context Flags (ext) | `src/safety/privilege_rules.c` (future) |
| C6 Memory addressability | #19 MMIO (minor ext) | Extend existing `src/safety/mmio_rules.c` |
| C7 Provenance/aliasing | #3 + #11 | Already-extracted `src/safety/provenance_rules.c` |
| C8 Memory ordering | **NEW System #30** | `src/safety/ordering_rules.c` (NEW, Phase 3) |

Category C9 was MERGED into C3 during Session 3 research. C10 was DELETED in Session 7 after verification found it collapsed into existing structural rules (O1/O2/E1/I4).

### Current state (2026-04-24)

- Research: **DESIGN COMPLETE.** 8 categories formally defined with ISA citations.
- Implementation: **NOT STARTED.** Zero compiler code changes during research phase.
- Framework verified universal across **5 architectures** (3 full + 2 spot-checks).

### When someone starts asm safety implementation

Implementation order (per plan roadmap):
1. Complete D-Alpha intrinsic batches (D-Alpha-10 through D-Alpha-14)
2. D-Alpha-7.5 Phase 1: Hardened unsafe asm infrastructure (H1-H7)
3. D-Alpha-7.5 Phase 2: Strict mode implementation
   - 18 structural rules (S/O/I/E)
   - 13 Z-rules (extend existing ZER safety systems through asm boundaries)
   - 8-category framework + per-arch data files
   - **New System #30 (Atomic Ordering)** — the only new ZER safety system added by research

Level 3 VST extraction for these checks comes AFTER the implementation ships. Standard extract-and-link pattern applies (see "Verification strategy — extract-and-link" section earlier in this doc).

### System #30 (NEW — Atomic Ordering) — spec for future proof work

**Design spec in `docs/asm_preconditions_research.md` C8 section.**

Tracks happens-before edges between memory operations via CFG traversal.
Similar pattern to existing Model 1 state-machine tracking (Handle states)
but tracks program-point state rather than per-entity state.

**State representation:**
```
OrderingState {
    barriers_encountered: Bitmap<BarrierKind>,
    pending_requirements: Set<(MemOpID, RequiredBarrierKind)>
}

BarrierKind ∈ {FullMemory, StoreStore, LoadLoad, LoadStore, StoreLoad,
               Release, Acquire, AcquireRelease, InstructionSync,
               IoMemory, DMASync}
```

**File placement when implemented:**
- Primary: `zercheck_ir.c` (CFG-based tracking, ~400-600 lines)
- AST-level hooks: `checker.c` (recognize barrier intrinsics, ~50-100 lines)
- Level 3 VST: `src/safety/ordering_rules.c` + `proofs/vst/verif_ordering_rules.v`
- **NOT in:** `zercheck.c` (being deleted, CFG migration Phase G)

### Scope distinction (important for proof oracles)

**System #30 proves language-level ordering facts:**
- "Each acquire has a matching release" — structural
- "CLWB followed by SFENCE before next write to same cache line" — pattern-based
- "FENCE.I before self-modifying code execution" — ordering constraint

**System #30 does NOT prove:**
- Full memory-model consistency across hart interleavings (Iris/RelaxedMem territory)
- Persistence atomicity on power failure (hardware concern)
- Weak memory model reorderings allowed by hardware

These deeper proofs remain in Tier C `@verified_spec` territory for users who opt in per-block.

### Research artifacts (reference locations)

- `docs/asm_preconditions_research.md` — master research doc (session-by-session)
- `research/asm_generics/C1..C8/` — 24 POC `.zer` files specifying expected check behavior
- `research/asm_generics/README.md` — structure + POC → test migration path

**POCs are NOT in tests/ directory.** They use `unsafe asm` structured syntax
not yet implemented. When D-Alpha-7.5 Phase 2 lands, POCs get promoted to
`tests/zer_fail/` (reject) and `tests/zer/` (accept) as real regression tests.

### Proof-session onboarding for asm work

If a fresh proof session starts on asm safety verification:

1. Read `docs/asm_preconditions_research.md` fully — 8 categories + System #30 design + ISA citations
2. Read this section + "Verification strategy — extract-and-link" earlier in this doc
3. Decide scope: Level 1 (Coq predicate spec) OR Level 3 (VST on extracted predicates)
4. For Level 1 category specs: follow typing.v pattern — bool-returning predicates with theorems
5. For Level 3 extraction: follow existing `handle_state.c` / `mmio_rules.c` pattern

**Oracle source for asm safety proofs:** the ISA manuals cited in `docs/asm_preconditions_research.md`. Direct quotes from Intel SDM, ARM ARM, RISC-V Manual establish the semantics being proved correct.
