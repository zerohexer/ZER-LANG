# ZER Formal Verification Plan — Path 1 (Coq + Iris)

**Status (2026-04-20):** Path 1 committed. Sequential λZER-Handle
proofs begin here. Concurrency verification deferred to Year 4+.

## Context and decision

After the 2026-04-20 ir_validate gap audit, the question of "how
formal can ZER's safety claims be?" was revisited. The abstract
4-model proofs in `proofs/*.v` (2,401 lines, 129 theorems) verify the
**design** of the safety system — but not the **implementation**.

Path 1 (full operational-semantics formal verification) was chosen
over Path 2 (empirical-only) and Path 3 (bounded λZER-Handle subset).
This matches the RustBelt methodology for Rust: formalize a language
subset in Iris, prove soundness, then extend.

### Why Iris?

- **Iris is standard for this kind of work.** RustBelt (Rust subset),
  RefinedC (C verification), Perennial (file systems) all use Iris.
  Prior art + proof techniques + community reuse.
- **Iris scales to concurrency.** When Year 4+ arrives, no tool
  switch needed.
- **Iris is a Coq library.** Sequential proofs don't need Iris's
  concurrency primitives — but starting in the framework means no
  migration later.
- **Sequential proofs are still tractable.** We use Iris's Hoare
  triples + separation logic even for sequential reasoning; it's
  overkill for simple loops but aligns with longer-term plans.

### Why not plain Coq?

Plain Coq is sufficient for sequential work. But starting there and
migrating to Iris later costs 20-40% of the original proof effort.
Full-Iris-from-start eats a 6-12 month ramp upfront in exchange for
no migration.

### Why not VST / Lean / F*?

- **VST** (Coq + C verification) is for object-level C program
  verification, not language-level soundness. Wrong tool for Path 1.
- **Lean 4** is capable but has less PL verification prior art than
  Coq. Existing `proofs/*.v` are in Coq — migrating costs everything.
- **F*** has smaller community, narrower scope.

## Structure

```
proofs/
  # Existing abstract-model proofs (Plain Coq, 2026-04-20)
  model1_handle_states.v        # handle lifecycle
  model2_point_properties.v     # VRP, provenance, escape flags, context
  model3_function_summaries.v   # cross-function analysis
  model4_static_annotations.v   # non-storable, MMIO, qualifiers
  composition.v                 # models compose without interference

  # NEW: operational-semantics proofs (Coq + Iris, this plan)
  operational/
    Dockerfile                  # Coq + Iris build environment
    _CoqProject                 # Coq build config
    Makefile                    # per-subdir build
    README.md                   # architecture notes
    lambda_zer_handle/
      syntax.v                  # AST: values, expressions, programs
      semantics.v               # small-step operational semantics
      typing.v                  # type system
      adequacy.v                # safety = preservation + progress
      handle_safety.v           # the main theorem (UAF/DF/leak-free)
    # Future expansions (each a new directory):
    # lambda_zer_defer/         # adds defer + orelse blocks
    # lambda_zer_move/          # adds move struct
    # lambda_zer_mmio/          # adds volatile + MMIO
    # lambda_zer_concurrency/   # adds shared struct + spawn (Iris proper)
    # lambda_zer_async/         # adds yield/await state machines
```

## λZER-Handle scope (Year 1 target)

This is the SUBSET of ZER that Year 1 formally verifies:

**Included:**
- `Pool(T, N)` / `Slab(T)` declarations
- `pool.alloc()` → `?Handle(T)`
- `pool.free(h)`
- `pool.get(h)` + field read/write
- `?Handle` unwrap via `orelse return`
- Sequential control flow: `if`/`else`, `while`, `let`/`seq`
- Primitives: `u32`, `bool`, `void`
- Single-threaded execution (no spawn, no shared struct)

**Explicitly NOT included in Year 1:**
- `defer` (Year 2)
- `move struct` (Year 2)
- `goto` (Year 2)
- MMIO / volatile / `@inttoptr` (Year 3)
- `shared struct` / `spawn` / concurrency (Year 4)
- `async` / `yield` / `await` (Year 5)
- Full IR + C emission correctness (Year 5+)

Each future subset is a separate directory with its own soundness
theorem. Extensions build on earlier subsets via refinement or
semantic embedding.

## Main soundness theorem (Year 1 deliverable)

```coq
Theorem lambda_zer_handle_safety :
  forall (p : program) (s s' : state),
    well_typed p ->
    exec p s s' ->
    ~ is_uaf s' /\
    ~ is_double_free s' /\
    (terminates p s s' -> all_handles_released s').
```

Informally: *"Every well-typed λZER-Handle program executes without
use-after-free, without double-free, and all handles are released at
termination (no leaks)."*

This is the **implementation-level** formal guarantee for ZER's
keystone handle-safety claim. Matches RustBelt's λRust safety
theorem for scope.

## Realistic timeline

| Year | Subset | Lines of Coq (est) | Key milestone |
|---|---|---|---|
| 1 | λZER-Handle | ~3,000 | Sequential handle safety proven |
| 2 | + defer + move struct + goto | +2,000 | Ownership semantics proven |
| 3 | + MMIO + volatile + `*opaque` | +1,500 | Embedded safety proven |
| 4 | + shared struct + spawn + condvar | +3,000 (Iris heavy) | Concurrency safety proven |
| 5 | + async + IR + emitter correctness | +5,000 | Full compiler verified |

Total estimate: ~15,000 lines of Coq + Iris over 5 years. This
matches the scope of multi-person research efforts (RustBelt, CakeML)
and is ambitious for solo + LLM. Milestones are independently
valuable — stopping at any year still leaves a meaningful formal
result.

## Milestone gates

At each year's end, evaluate:

- Are the proofs reusable as ZER evolves?
- Has proof maintenance slowed feature development?
- Is LLM assistance keeping up?

Stop / scope-limit if any answer is "no." Shipping incomplete Path 1
at any year is fine — each subset is self-contained. Partial formal
verification > no formal verification.

## Verification workflow

Build in Docker for reproducibility (`proofs/operational/Dockerfile`
pins Coq + Iris versions). Every proof file must compile under
`coqc`. Every commit to `proofs/operational/` must leave the build
green.

CI (not yet implemented — future work) should:
- Build the Docker image
- Run `make` in `proofs/operational/`
- Fail the PR if any `.v` file fails to compile

## For fresh sessions

The directory `proofs/operational/` is the operational-semantics
work. The top-level `proofs/*.v` files are the abstract-model proofs
(design-level). Keep them separate — they operate at different
levels of abstraction and shouldn't share files.

**Start reading:** `proofs/operational/README.md` for the
architecture, then `lambda_zer_handle/syntax.v` for the smallest
self-contained piece.

**Don't start writing:** without a plan. Each new subset should add
a theorem to the table above with a clear scope statement. No
"incremental additions to λZER-Handle" — instead, new subsets that
embed it.
