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

**Progress tracking:**
- `src/safety/handle_state.c` — 4 predicates extracted + verified (2026-04-21):
  - `zer_handle_state_is_invalid(int)` — {FREED, MAYBE_FREED, TRANSFERRED}
  - `zer_handle_state_is_alive(int)` — == ALIVE
  - `zer_handle_state_is_freed(int)` — == FREED
  - `zer_handle_state_is_transferred(int)` — == TRANSFERRED
- Delegation: zercheck.c `is_handle_invalid` + `is_handle_consumed` (consolidated, both now go through the same VST-verified predicate). zercheck_ir.c `ir_is_invalid`.
- Plus 21 demonstrator proofs in `proofs/vst/` (standalone `.c` files, NOT extracted from compiler — pre-extraction scaffolding that established the VST pattern).

**Next extractions** (by estimated value + effort):
1. Type predicates: `zer_type_is_move_struct(int kind, int is_move_flag)` — requires exposing a minimal type-kind enum (no Type* in extracted function signature — primitive args only)
2. Range predicates: bounds check, variant in range, pool count valid
3. Cascade the 3 single-value predicates (is_alive/is_freed/is_transferred) to replace inline `h->state == IR_HS_X` sites throughout zercheck_ir.c (~30+ sites)
4. ... (see checklist in `docs/compiler-internals.md` when authored)

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
| **Level 3 — VST on extracted predicates** | 4 real extractions (all 4 handle-state predicates in `src/safety/handle_state.c`) + 21 pre-extraction demonstrators. Pattern established. Continuing. |
| λZER-concurrency (Iris concurrency primitives) | Not started. |
| λZER-async | Not started. |

## For fresh sessions

The directory `proofs/operational/` is the operational-semantics work. The top-level `proofs/*.v` files are the abstract-model proofs (design-level). Keep them separate — they operate at different levels of abstraction and shouldn't share files.

**Start reading:** `proofs/operational/README.md` for the architecture, then `lambda_zer_handle/syntax.v` for the smallest self-contained piece.

**Framework:** Coq + Iris + stdpp. If a new proof file doesn't `From iris ... Require Import ...` something, question why. Exception: `syntax.v` and `typing.v` may be framework-agnostic.

**Don't start writing:** without a plan. Each new subset should add an entry to the current-status table above with a clear scope statement. No "incremental additions to λZER-Handle" — instead, new subsets that embed it.
