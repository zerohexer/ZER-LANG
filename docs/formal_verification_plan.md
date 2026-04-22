# ZER Formal Verification Plan — Path 1 (Coq + Iris)

**Status:** Path 1 committed. Iris adopted from the start (no plain-Coq → Iris migration tax). λZER-Handle is the first subset.

## Purpose

Formal verification serves as a **correctness oracle for the compiler**. When a proof breaks, it points at a concrete compiler invariant that's been violated. This is the primary reason for doing this work — not academic rigor in isolation, but a mechanical check that catches compiler regressions the test suite can't.

For this to work:
1. Semantics in the proof must mirror the compiler's actual operational behavior.
2. Typing rules must mirror what zercheck actually enforces.
3. Broken proofs must point at the specific compiler invariant that was violated.
4. CI must enforce the proof build on every PR.

## Context and decision

After the ir_validate gap audit, the question of "how formal can ZER's safety claims be?" was revisited. The abstract 4-model proofs in `proofs/*.v` (2,401 lines, 129 theorems) verify the **design** of the safety system — but not the **implementation**.

Path 1 (full operational-semantics formal verification) was chosen over Path 2 (empirical-only) and Path 3 (bounded λZER-Handle subset). This matches the RustBelt methodology for Rust: formalize a language subset in Iris, prove soundness, then extend.

### Why Iris from the start

- **Iris is standard for this kind of work.** RustBelt (Rust subset), RefinedC (C verification), Perennial (file systems) all use Iris. Prior art + proof techniques + community reuse.
- **No migration tax.** Concurrency absolutely needs Iris (ghost state, invariants, rely-guarantee). Starting in plain Coq and porting later costs 20-40% of the proof effort. Paying the learning curve upfront is cheaper over the full scope.
- **Iris is a Coq library.** Sequential proofs don't require Iris's concurrency primitives — but starting in the framework means no rewrite later.
- **Working Iris code is a training signal.** Well-written Iris proofs for ZER become a reference that future LLM sessions can learn from. Each additional proof gets cheaper as the pattern library grows.
- **Resources encode safety directly.** Handle safety in Iris: owning `alive_handle p i g : iProp` *is* the proof that the handle is currently alive. `pool.free` consumes the resource — you can't free twice because you don't have the resource anymore. Cleaner than manual state-machine tracking.

### Why not plain Coq

Plain Coq is sufficient for sequential work but creates a migration cliff when concurrency lands. For a solo+LLM project, that cliff is expensive — you're doing the hardest framework addition (concurrency semantics) simultaneously with porting (rewriting existing proofs into Iris style). Front-loading the Iris learning curve spreads the cost and avoids the cliff.

### Why not VST / Lean / F* *for Path 1*

- **VST** (Coq + C verification) is for object-level C program verification, not language-level soundness. Wrong tool for Path 1 — but RIGHT tool for **Level 3 implementation verification** (see below).
- **Lean 4** is capable but has less PL verification prior art than Coq. Existing `proofs/*.v` are in Coq — migrating costs everything.
- **F*** has smaller community, narrower scope.

### Why VST IS in scope for Level 3 (addendum, 2026-04-21)

Levels 1 (abstract models) and 2 (tests) prove that the safety ARGUMENT is sound and that the compiler REJECTS known violations. Neither level proves that the actual C source of zercheck/zercheck_ir matches the safety predicates.

**Gap:** A typo like `if (state = HS_ALIVE)` (assignment, not comparison) in zercheck.c would:
- Leave Level 1's Coq spec unchanged (still correct mathematically)
- Pass most Level 2 tests (wouldn't cover every state value)
- Silently miscompile safety checks in production

Level 3 closes this gap by mechanically verifying that the C implementation matches the Coq predicate for EVERY possible input. VST is the standard tool for this — it's what RefinedC, CertiKOS, and certified CompCert use to bridge Coq specs to C source.

**Level 3 scope:** NOT a whole-program verification of zercheck.c (that's ~500+ hrs and would block language work). Instead, **extracted predicates**: pure, side-effect-free functions extracted from zercheck.c/zercheck_ir.c into `src/safety/*.c`, linked into `make zerc`, clightgen'd into Coq, VST-verified against the corresponding `typing.v` predicate.

Level 3 complements — does not replace — Level 1. They answer different questions:
- Level 1: is the predicate semantics correct?
- Level 3: does the C implementation compute that predicate correctly?

## Structure

```
proofs/
  # Existing abstract-model proofs (Plain Coq)
  model1_handle_states.v        # handle lifecycle
  model2_point_properties.v     # VRP, provenance, escape flags, context
  model3_function_summaries.v   # cross-function analysis
  model4_static_annotations.v   # non-storable, MMIO, qualifiers
  composition.v                 # models compose without interference

  # Operational-semantics proofs (Coq + Iris)
  operational/
    Dockerfile                  # Coq 8.18 + Iris 4.x + stdpp build env
    _CoqProject                 # Coq build config
    Makefile                    # per-subdir build
    README.md                   # architecture notes
    lambda_zer_handle/          # first subset: sequential handle safety
      syntax.v                  # AST: values, expressions, programs
      semantics.v               # small-step operational semantics
      typing.v                  # type system
      adequacy.v                # safety = preservation + progress (Iris wp-based)
      handle_safety.v           # main theorem (UAF/DF/leak-free)
    # Future subsets (each a new directory, extends λZER-Handle):
    # lambda_zer_defer/         # adds defer + orelse blocks
    # lambda_zer_move/          # adds move struct
    # lambda_zer_mmio/          # adds volatile + MMIO
    # lambda_zer_concurrency/   # adds shared struct + spawn (Iris proper)
    # lambda_zer_async/         # adds yield/await state machines
```

## λZER-Handle scope (current subset)

The SUBSET of ZER that the current proof effort covers:

**Included:**
- `Pool(T, N)` / `Slab(T)` declarations
- `pool.alloc()` → `?Handle(T)`
- `pool.free(h)`
- `pool.get(h)` + field read/write
- `?Handle` unwrap via `orelse return`
- Sequential control flow: `if`/`else`, `while`, `let`/`seq`
- Primitives: `u32`, `bool`, `void`
- Single-threaded execution (no spawn, no shared struct)

**Explicitly NOT in λZER-Handle (future subsets):**
- `defer` — future subset `lambda_zer_defer`
- `move struct` — future subset `lambda_zer_move`
- `goto` — future subset (likely bundled with defer)
- MMIO / volatile / `@inttoptr` — future subset `lambda_zer_mmio`
- `shared struct` / `spawn` / concurrency — future subset `lambda_zer_concurrency` (Iris concurrency primitives kick in)
- `async` / `yield` / `await` — future subset `lambda_zer_async`
- Full IR + C emission correctness — final subset

Each future subset is a separate directory with its own soundness theorem. Extensions build on earlier subsets via refinement or semantic embedding. No "incremental additions to λZER-Handle" — if a feature isn't in the current subset scope, it waits for its own dedicated subset.

## Main soundness theorem for λZER-Handle

```coq
Theorem lambda_zer_handle_safety :
  forall (p : program) (s s' : state),
    well_typed p ->
    exec p s s' ->
    ~ is_uaf s' /\
    ~ is_double_free s' /\
    (terminates p s s' -> all_handles_released s').
```

Informally: *"Every well-typed λZER-Handle program executes without use-after-free, without double-free, and all handles are released at termination (no leaks)."*

This is the **implementation-level** formal guarantee for ZER's keystone handle-safety claim. Matches RustBelt's λRust safety theorem for scope.

## Execution approach (Iris)

Iris has a steep learning curve. To avoid sinking effort into infrastructure before validating tractability, work proceeds in de-risking phases:

### Phase 0 — Minimal end-to-end Iris proof

Prove, using Iris weakest preconditions, that the program
```
let h = pool.alloc() orelse return;
pool.free(h);
```
is safe (doesn't get stuck). ~150-250 lines of Iris.

**Validates:**
1. The Iris setup in the Dockerfile compiles non-trivial proofs.
2. The small-step semantics plugs into Iris's `language` typeclass correctly.
3. Iris tactics are tractable for the project workflow.
4. Produces a concrete working-code example — reference material for all subsequent proofs.

**If Phase 0 stalls (multiple sessions without progress):** fall back to plain Coq with the existing `lambda_zer_handle/` work. Cost of experiment: ~1 session. The plain-Coq work is preserved as insurance.

### Phase 1 — Rewrite λZER-Handle in Iris

Using Phase 0 as template:
- `syntax.v` stays mostly as-is.
- `semantics.v` gets adapted to Iris's `language` typeclass.
- `typing.v` stays (types don't depend on proof framework).
- `adequacy.v` replaced by Iris wp-based proofs + Iris adequacy theorem.
- `handle_safety.v` derived from wp triples.

The existing plain-Coq `lambda_zer_handle/` is preserved in an `_plain/` archive directory until the Iris version reaches parity, then deleted.

### Phase 2 — Close all admits

Target: `handle_safety.v` axiom-free. Uses Iris resource invariants to encode handle liveness directly (`alive_handle p i g : iProp`). Store well-formedness becomes an Iris invariant rather than a side-condition threaded through preservation proofs.

### Phase 3 — CI enforcement

Docker image builds. `make` runs in `proofs/operational/`. Any broken `.v` file fails the PR. Without this, compiler changes can silently invalidate proofs — the whole correctness-oracle purpose breaks.

## Milestone gates

At each subset's end, evaluate:

- Are the proofs reusable as ZER evolves?
- Has proof maintenance slowed feature development?
- Is LLM assistance keeping up?

Stop / scope-limit if any answer is "no." Shipping incomplete Path 1 is fine — each subset is self-contained. Partial formal verification > no formal verification.

## Verification workflow

Build in Docker for reproducibility (`proofs/operational/Dockerfile` pins Coq + Iris versions). Every proof file must compile under `coqc`. Every commit to `proofs/operational/` must leave the build green.

**Current baseline:** `make` succeeds on the plain-Coq `lambda_zer_handle/` as of this writing. Iris migration begins with Phase 0.

CI (not yet implemented — Phase 3 work) should:
- Build the Docker image
- Run `make` in `proofs/operational/`
- Fail the PR if any `.v` file fails to compile

## Level 3 — VST on extracted predicates (2026-04-21 — in progress)

**Purpose:** close the implementation-correctness gap — prove that the actual C source of safety checks matches the Coq predicate for every input.

**Mechanism: extract-and-link.** Pure predicate functions are extracted from `zercheck.c` and `zercheck_ir.c` into `src/safety/*.c`. That `.c` file is:

1. **Linked into `zerc`** via Makefile `CORE_SRCS` — it's part of the real compiler, not a duplicate.
2. **Called from zercheck.c AND zercheck_ir.c** — both analyzers delegate to the extracted function. If the extraction broke the logic, dual-run (Phase F, already landed) would disagree.
3. **Verified by `make check-vst`** — CompCert's `clightgen` reads the SAME `.c` file, produces a Coq AST, VST proves the function matches the Coq spec in `lambda_zer_typing/typing.v`.

```
src/safety/handle_state.c          ← real code (linked into zerc)
src/safety/handle_state.h          ← declarations
proofs/vst/verif_handle_state.v    ← VST spec + proof
```

**CI integration:**
- `make zerc` — compiles `src/safety/*.c` with the rest
- `make check-vst` — clightgen + coqc on the SAME files

**Four failure modes, each with a clear diagnosis:**

| What fails | Meaning |
|---|---|
| `make zerc` | Code bug in extracted predicate |
| `make check-vst` | Coq proof caught: C implementation diverged from spec |
| `tests/zer/` | Runtime regression |
| `tests/zer_proof/` `_bad` passes | Safety rule stopped enforcing |

If someone optimizes `zer_handle_state_is_invalid` in a way that breaks it:
- `make zerc` compiles clean
- Unit tests may pass (didn't cover the broken case)
- **`make check-vst` fails** — pointing at the exact line where C diverged

That's the correctness-oracle loop closing at the implementation level.

**Extraction rule.** A function is extractable when it is:
1. **Pure** — no global state mutation, no side effects.
2. **Self-contained** — no AST nodes, no `Checker*`/`ZerCheck*` parameters.
3. **Primitive types only** — int, minimal structs. No callbacks, no allocations.

This limits the first wave to ~15-25 functions from zercheck.c + zercheck_ir.c. Complex functions (call-graph DFS, scope walks) need per-function VST work with struct separation logic — 20+ hrs each, later phases.

**Anti-overclaim.** Level 3 does NOT verify the whole compiler. It verifies that every extracted predicate matches its spec. Functions NOT extracted (i.e., still inline in `zercheck*.c`) are NOT Level-3-verified. The count of verified functions is the honest metric.

**Architecture 1 decision (2026-04-21):** chosen over Architecture 2 (Coq rewrite + extract). Reasons: LLM velocity (C >> Coq), incremental value at every phase, no heroic rewrite risk, working compiler throughout. Architecture 2 reserved for future migration of stable subsystems (probably year 2+).

### Concrete 6-phase roadmap

Each phase has a target, cost, and acceptance criterion. Stop at any phase and still ship real value.

**Phase 0 — Infrastructure (DONE 2026-04-21, commit 4aaa1e7/5f98ed4)**
- `src/safety/` directory, Makefile CORE_SRCS integration, Dockerfile COPY, `.gitignore`
- `make check-vst` target with `-Q src/safety zer_safety` mapping
- `make check-all` target (tests + Coq + VST + coverage audit)
- VST pattern documented in `proof-internals.md` (subst gotcha, cascade pattern)
- **Acceptance:** 1 real extraction (`zer_handle_state_is_invalid`) wired + verified

**Phase 1 — Pure predicate extraction (~100 hrs, IN PROGRESS)**

Target: 40 predicates extracted + VST-verified. Each file in `src/safety/`, one VST proof file in `proofs/vst/verif_*.v`.

| Predicate batch | Location | Status |
|---|---|---|
| Handle state (4 fns) | `src/safety/handle_state.c` | **DONE** (`zer_handle_state_is_invalid/alive/freed/transferred`) |
| Range checks (3 fns) | `src/safety/range_checks.c` | **DONE** (`zer_count_is_positive`, `zer_index_in_bounds`, `zer_variant_in_range`) |
| Type kind predicates (7 fns) | `src/safety/type_kind.c` | **DONE** (`zer_type_kind_is_integer`, `is_signed`, `is_unsigned`, `is_float`, `is_numeric`, `is_pointer`, `has_fields`) |
| Coercion rules (5 fns) | `src/safety/coerce_rules.c` | **DONE** (`zer_coerce_int_widening_allowed`, `_usize_same_width_allowed`, `_float_widening_allowed`, `_preserves_volatile`, `_preserves_const`) |
| Context ban rules (6 fns) | `src/safety/context_bans.c` | **DONE** (`zer_return/break/continue/goto/defer/asm_allowed_in_context`) |
| Provenance rules (3 fns) | `src/safety/provenance_rules.c` | **DONE** (`zer_provenance_check_required`, `_type_ids_compatible`, `_opaque_upcast_allowed`; oracle-driven from λZER-opaque) |
| Optional unwrap rules (2 fns) | `src/safety/optional_rules.c` | **DONE** (`zer_type_permits_null`, `_is_nested_optional`; oracle-driven from typing.v Section N) |
| MMIO range rules (2 fns) | `src/safety/mmio_rules.c` | **DONE** (`zer_mmio_addr_in_range`, `_inttoptr_allowed`; oracle-driven from λZER-mmio. Alignment predicate deferred — modulus in VST needs extra setup) |
| Move struct rules (2 fns) | `src/safety/move_rules.c` | **DONE** (`zer_type_kind_is_move_struct`, `_should_track`; oracle-driven from λZER-move) |
| Escape rules (3 fns) | `src/safety/escape_rules.c` | **DONE** (`zer_region_can_escape`, `_is_local`, `_is_arena`; oracle-driven from λZER-escape) |

**Total Phase 1:** ~44 predicates. Current: 7/44 (16%).

**Acceptance:** 40+ predicates verified, each called from at least one site in `checker.c`, `zercheck.c`, or `zercheck_ir.c`.

**Phase 2 — Decision extraction (~150 hrs)**

Target: 60 decisions extracted using command-query separation. Covers state mutation sites.

| Decision family | Extract to | Scope |
|---|---|---|
| Handle state transitions | `src/safety/handle_transitions.c` | 5-8 functions (alloc/free/use/transfer decisions) |
| Path merge | `src/safety/path_merge.c` | 4 functions (if/else/switch/loop merge) |
| Coercion dispatch | `src/safety/coerce_dispatch.c` | ~10 functions (which coercion to apply) |
| Escape flag propagation | `src/safety/escape_propagate.c` | ~6 functions |
| ISR ban decisions | `src/safety/isr_rules.c` | ~4 functions |
| Control flow bans | `src/safety/control_flow.c` | ~8 functions (return in defer, etc.) |
| VRP arithmetic | `src/safety/vrp_arith.c` | ~10 functions (range_after_op, range_merge) |
| Move transfer | `src/safety/move_transitions.c` | ~5 functions |

**Acceptance:** 60+ decisions extracted, original mutation sites reduced to trivial switch-dispatches.

**Phase 3 — Generic AST walker (~60 hrs)**

Target: 1 generic walker replaces ~3-4 hand-written recursions.

```c
// src/safety/ast_walk.c
void zer_ast_walk(Node *n, Visitor v, void *ctx);
// Spec: visitor called on n, then recursively on every child of n
// Proof: structural induction on Node's inductive definition in Coq
```

Refactor targets: `check_expr`, `emit_expr`, `zc_check_stmt`, `zc_check_expr`. Each becomes a visitor function + call to `zer_ast_walk`.

**Acceptance:** zero hand-written recursive AST walks remain in safety paths.

**Phase 4 — Verified state APIs (~240 hrs)**

Target: 4 small APIs, each VST-verified with invariant preservation.

| API | Data structure | Invariant |
|---|---|---|
| `src/safety/typemap_api.c` | Typemap | All registered nodes have resolved non-NULL types |
| `src/safety/scope_api.c` | Scope / symbol table | Lookups return most-recent binding at scope depth |
| `src/safety/handle_pool_api.c` | Pool / Slab metadata | Alive handles have distinct alloc_ids |
| `src/safety/handle_set_api.c` | Pool map (per-compile) | Every handle in set is registered |

**Acceptance:** direct struct-field writes to these data structures banned (grep CI check). All mutations go through the API.

**Phase 5 — Phase-typed checker (~30 hrs)**

Target: C type system enforces pass ordering.

```c
typedef struct { Checker *c; } CheckerPhase0;
typedef struct { Checker *c; } CheckerPhase1;
typedef struct { Checker *c; } CheckerPhase2;
typedef struct { Checker *c; } CheckerPhase3;

CheckerPhase1 zer_register_symbols(CheckerPhase0 p);
CheckerPhase2 zer_resolve_types(CheckerPhase1 p);
CheckerPhase3 zer_check_bodies(CheckerPhase2 p);
```

**Acceptance:** checker.c main dispatch enforces phase order at compile time. Attempting to call out-of-order = C type error.

**Phase 6 — CI discipline (~30 hrs)**

Target: automated checks that enforce the verification discipline.

- `make check-discipline`: grep-level audits
  - Every safety function has `/* SAFETY: */` comment with proof link
  - No direct `c->typemap[X] =`, `c->scopes[X]->...`, `pool->...` writes (bypassing API)
  - No `Admitted.` or `admit.` anywhere in proofs
  - Every safety_list.md row maps to at least one VST-verified predicate
- Add `make check-discipline` to `check-all`
- CI enforces: any PR that fails `check-all` is non-mergeable

**Acceptance:** `make check-all` runs all gates. Every mergeable PR = mechanically verified safety.

**Phase 7 — Deepen schematic to operational (~425 hrs, commitment 2026-04-21)**

Target: every safety row in `safety_list.md` reaches ✓ operational depth (currently 40 ✓ + 82 ○ schematic + 15 ◐ in-progress + 46 — not-safety-semantic).

Schematic proofs are VALID but WEAKER than operational. Schematic proves "the checker's predicate is correct by its stated claim." Operational proves "well-typed programs cannot exhibit this bug at runtime" via step rules + resource algebra + Iris adequacy. seL4/CompCert/RustBelt all target operational depth.

| Subset to create | Rows closed | Effort | Current depth |
|---|---|---|---|
| `lambda_zer_concurrency/` (C, D, E sections) | 22 | ~150 hrs | Schematic in `iris_concurrency.v` |
| `lambda_zer_async/` (F section) | 5 | ~80 hrs | Schematic in `iris_concurrency.v` |
| `lambda_zer_control_flow/` (G section) | 10 | ~30 hrs | Schematic in `iris_control_flow.v` |
| Deepen `lambda_zer_mmio/` rest (H leftovers) | 9 | ~20 hrs | Core rows already ✓ |
| Deepen `lambda_zer_opaque/` rest (J leftovers) | 11 | ~30 hrs | Core rows already ✓ |
| Deepen `lambda_zer_escape/` rest (O leftovers) | 11 | ~30 hrs | Core rows already ✓ |
| `lambda_zer_typing_extra/` (K, T sections) | 4 | ~15 hrs | Schematic in `iris_container_validity.v` |
| `lambda_zer_vrp/` (L, M, R, S sections) | 17 | ~50 hrs | Schematic in `iris_misc_sections.v` |
| `lambda_zer_variant/` (P section) | 5 | ~15 hrs | Schematic in `iris_misc_sections.v` |
| Spec reviews + integration | — | ~25 hrs | N/A |

**Per-subset pattern** (learned from λZER-handle/move/opaque/escape/mmio):
1. Create `proofs/operational/lambda_zer_<name>/` directory
2. `syntax.v` — AST constructs specific to this feature
3. `semantics.v` — step rules enforcing safety invariants
4. `typing.v` — typing rules matching compiler's actual checks
5. `iris_<name>_resources.v` — resource algebra (if needed)
6. `iris_<name>_state.v` — state interpretation
7. `iris_<name>_specs.v` — wp specs + theorems axiom-free
8. Add to `_CoqProject` with `-Q lambda_zer_<name> zer_<name>`
9. Delete schematic theorems in `iris_*.v` once replaced

**Acceptance:** safety_list.md shows 0 rows at ○ status. Only ✓ (operational) and — (not-safety-semantic) remain. `make check-proofs` green, zero admits across all subsets.

### Total Architecture 1 plan (updated 2026-04-21)

```
Phase 0 — Infrastructure              ✅ DONE (2026-04-21)
Phase 1 — 40+ predicates              🔄 7/44 (16%)    ~93 hrs
Phase 2 — 60 decisions                 ⏳ TODO          ~150 hrs
Phase 3 — Generic AST walker           ⏳ TODO          ~60 hrs
Phase 4 — Verified APIs                ⏳ TODO          ~240 hrs
Phase 5 — Phase types                  ⏳ TODO          ~30 hrs
Phase 6 — CI discipline                ⏳ TODO          ~30 hrs
Phase 7 — Deepen schematic→operational ⏳ TODO          ~425 hrs
  ├─ λZER-concurrency                                    ~150 hrs
  ├─ λZER-async                                           ~80 hrs
  ├─ λZER-control-flow                                    ~30 hrs
  ├─ λZER-mmio rest (H)                                   ~20 hrs
  ├─ λZER-opaque rest (J)                                 ~30 hrs
  ├─ λZER-escape rest (O)                                 ~30 hrs
  ├─ λZER-typing-extra (K, T)                             ~15 hrs
  ├─ λZER-vrp (L, M, R, S)                                ~50 hrs
  ├─ λZER-variant (P)                                     ~15 hrs
  └─ Spec reviews                                         ~25 hrs
Phase 8 — Release verification polish  ⏳ TODO          ~50 hrs
────────────────────────────────────────────────────
Total remaining: ~1,078 hrs to seL4-level proof
```

At 3 hrs/day LLM-assisted: **~1 year focused work.**
At 1 hr/day casual: **~3 years.**

**Benchmark comparisons:**
- CompCert: ~20,000 person-hours (decade, team of 10)
- seL4: ~30,000 person-hours (decade, team of 20)
- **ZER at ~1,085 hrs is 20-30x faster** because: smaller language, emit-to-C (no native backend), existing 42-file Iris infrastructure with 0 admits, LLM assistance, narrower target (safety properties, not full semantic preservation).

### End state — what ZER gets after Phase 7

**Every safety property in safety_list.md (all 203 rows) is mechanically proven at operational depth.** Every extracted predicate is VST-verified. Every `make check-all` is a proof of correctness at the trust-base boundary.

Claim at completion:

> "ZER is a formally verified compiler. For every program ZER accepts, the resulting C is provably free of use-after-free, double-free, bounds violations, data races, move-after-transfer, escape bugs, MMIO errors, handle leaks, and 200+ other specified safety properties — proven in Coq, verified in VST, tested empirically. The trust base is Coq kernel + GCC + hardware."

Same strength of claim as CompCert and seL4 make for their respective targets.

**Not in scope (would need Phase 9+):**
- Semantic preservation (ZER → C meaning-preservation proof) — ~1500 hrs additional, CompCert-style
- Compiler termination proof — ~200 hrs
- Performance guarantees — not a safety concern

### Phase-7 stopping points

Each subset is self-contained. Stop at any sub-phase and still ship real value:

- After λZER-concurrency (~150 hrs): all thread/spawn/condvar safety at operational depth. Biggest gain — concurrency is the riskiest area.
- After λZER-async (~80 hrs more): stackless coroutines proven sound.
- After control-flow (~30 hrs more): return/break/goto rules operational.
- After MMIO/opaque/escape rest (~80 hrs more): complete provenance + region proofs.
- After VRP/typing/variant rest (~80 hrs more): 100% operational coverage.

### Progression diagram

```
CURRENT STATE (2026-04-21):
┌─────────────────────────────────────────────────────────────┐
│ Level 1:  40 ✓ operational + 82 ○ schematic + 46 — NSS      │
│ Level 2:  106 Layer-2 tests linking programs to theorems    │
│ Level 3:  7 real extractions + 21 demonstrators              │
│ CI:       make check-all gates (partial)                     │
└─────────────────────────────────────────────────────────────┘
                         ↓ ~1,085 hrs
END STATE (Phase 7+8 complete):
┌─────────────────────────────────────────────────────────────┐
│ Level 1:  ALL 157 substantive rows ✓ operational             │
│ Level 2:  200+ Layer-2 tests                                 │
│ Level 3:  100+ extracted predicates VST-verified             │
│ CI:       make check-all blocks any mergeable safety bug     │
│ Claim:    "seL4-level formally verified compiler"            │
└─────────────────────────────────────────────────────────────┘
```
### When to migrate to Architecture 2

Criteria for starting an Architecture 2 subsystem rewrite:
1. The target subsystem has been stable for 6+ months (no major refactors)
2. Every safety predicate in the subsystem is already Phase 1 / 2 verified
3. The subsystem has a small, well-defined interface
4. LLM/solo time budget allows ~1000 hrs for the rewrite

First candidate (per these criteria): `zercheck.c` — safety logic is settling, Phase F dual-run validated it for 3143 programs without disagreement. Would become `proofs/extracted/zercheck.v` in year 2.

**NOT Architecture 2 candidates:** `checker.c` (churns frequently), `emitter.c` (IR-dependent), `parser.c` (mostly not safety-critical).

Each extraction: (1) extract to `src/safety/*.c`, (2) wire zercheck.c AND zercheck_ir.c to call it, (3) add VST proof, (4) add to `make check-vst`, (5) commit. One predicate per commit.

**Scope estimate:** 15-25 pure predicates total. ~1-3 hours per extraction (not 5-20; the complex ones don't fit this pattern). Complex functions (loops, recursion, struct separation logic) are separate future work — not part of the predicate extraction wave.

## Current status

| Subset / Level | Status |
|---|---|
| **Level 1 — λZER-Handle (plain Coq)** | Baseline compiles. 17 admits in adequacy.v + 3 in handle_safety.v. Preserved as insurance during Iris migration. |
| **Level 1 — λZER-Handle (Iris)** | Complete + axiom-free. |
| **Level 1 — other operational subsets** | λZER-move, λZER-opaque, λZER-escape, λZER-mmio — all at operational depth. |
| **Level 1 — λZER-typing (predicate-based)** | 135 real theorems covering sections G, C, D, E, F, I, J-extended, K, L, M, N, P, Q, R, S, T. |
| **Level 2 — tests/zer_proof/** | 106 theorem-linked tests. Correctness-oracle loop closed empirically. |
| **Level 3 — VST on extracted predicates** | **42 real extractions** across 12 `src/safety/*.c` files. Phase 1 at 95% (42/44) — effectively COMPLETE. Oracle-driven batches: escape (λZER-escape), provenance (λZER-opaque), mmio (λZER-mmio), optional (typing.v N), move (λZER-move), atomic (typing.v E), container (typing.v T+K). |
| **Phase 7 — Deepen schematic → operational** | 82 rows at schematic depth. Path to seL4-level proof. ~425 hrs. |
| **Total path to seL4-level formal verification** | **~1,085 hrs** (~1 year focused, ~3 years casual). 20-30x faster than CompCert/seL4 thanks to existing 42-file Iris infrastructure + LLM assistance + narrower target (safety properties only, not semantic preservation). |
| λZER-concurrency (Iris concurrency primitives) | Not started. |
| λZER-async | Not started. |

## For fresh sessions

The directory `proofs/operational/` is the operational-semantics work. The top-level `proofs/*.v` files are the abstract-model proofs (design-level). Keep them separate — they operate at different levels of abstraction and shouldn't share files.

**Start reading:** `proofs/operational/README.md` for the architecture, then `lambda_zer_handle/syntax.v` for the smallest self-contained piece.

**Framework:** Coq + Iris + stdpp. If a new proof file doesn't `From iris ... Require Import ...` something, question why. Exception: `syntax.v` and `typing.v` may be framework-agnostic.

**Don't start writing:** without a plan. Each new subset should add an entry to the current-status table above with a clear scope statement. No "incremental additions to λZER-Handle" — instead, new subsets that embed it.
