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

### Why not VST / Lean / F*

- **VST** (Coq + C verification) is for object-level C program verification, not language-level soundness. Wrong tool for Path 1.
- **Lean 4** is capable but has less PL verification prior art than Coq. Existing `proofs/*.v` are in Coq — migrating costs everything.
- **F*** has smaller community, narrower scope.

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

## Current status

| Subset | Status |
|---|---|
| λZER-Handle (plain Coq) | Baseline compiles. 17 admits in adequacy.v + 3 in handle_safety.v. Preserved as insurance during Iris migration. |
| λZER-Handle (Iris) | **Next: Phase 0** — minimal end-to-end Iris proof of alloc→free. |
| λZER-defer / move / goto | Not started. Starts after λZER-Handle Iris version is axiom-free. |
| λZER-mmio | Not started. |
| λZER-concurrency | Not started. Iris concurrency primitives kick in here. |
| λZER-async | Not started. |

## For fresh sessions

The directory `proofs/operational/` is the operational-semantics work. The top-level `proofs/*.v` files are the abstract-model proofs (design-level). Keep them separate — they operate at different levels of abstraction and shouldn't share files.

**Start reading:** `proofs/operational/README.md` for the architecture, then `lambda_zer_handle/syntax.v` for the smallest self-contained piece.

**Framework:** Coq + Iris + stdpp. If a new proof file doesn't `From iris ... Require Import ...` something, question why. Exception: `syntax.v` and `typing.v` may be framework-agnostic.

**Don't start writing:** without a plan. Each new subset should add an entry to the current-status table above with a clear scope statement. No "incremental additions to λZER-Handle" — instead, new subsets that embed it.
