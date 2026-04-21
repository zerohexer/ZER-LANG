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
