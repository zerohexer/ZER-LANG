# Operational-Semantics Proofs for ZER-LANG

**Status:** Year 1 of Path 1 (see `docs/formal_verification_plan.md`).

Proves ZER's safety claims at the **implementation level**, not just
the abstract-model level (which is in the parent `proofs/` directory).

## Relationship to the top-level `proofs/` directory

- `../proofs/*.v` — abstract 4-model soundness (design-level,
  2,401 lines, plain Coq).
- `proofs/operational/` (this directory) — connects the abstract
  model to actual ZER program execution (implementation-level,
  Coq + Iris).

Abstract model: *"If the rules are X, then properties Y follow."*

Operational proofs: *"ZER's concrete language rules match X, so Y
holds for every ZER program."*

Both are formal verification in Coq. They answer different questions.

## Architecture

```
operational/
  Dockerfile        Coq 8.18 + Iris 4.2 + stdpp 1.9 (pinned)
  Makefile          wraps CoqMakefile
  _CoqProject       file list + compile flags
  README.md         this file
  lambda_zer_handle/    # Year 1: sequential handle safety
    syntax.v          # AST: values, expressions, programs
    semantics.v       # small-step operational semantics
    typing.v          # type system
    adequacy.v        # preservation + progress
    handle_safety.v   # main theorem
```

## Building

```bash
# Build the Docker image (once):
docker build -t zer-proofs -f proofs/operational/Dockerfile .

# Compile all proofs:
docker run --rm -v "$PWD/proofs/operational:/work" zer-proofs \
    bash -c 'eval $(opam env) && make'
```

Every `.v` file must compile green. A PR that breaks the build is
not merged.

## Why sequential first?

- Sequential ZER is fully formalizable without Iris's concurrency
  primitives — no ghost state, no invariants, no step-indexing in
  the first year's proofs.
- Iris is still used (Hoare triples, separation logic for heap
  reasoning) to match the framework we'll need later.
- Year 4 adds concurrency. Until then, "sequential Iris" = regular
  separation logic in a framework that CAN do concurrency later.

## λZER-Handle subset (Year 1)

Scope defined in `docs/formal_verification_plan.md`:

- `Pool(T, N)` / `Slab(T)` declarations
- `pool.alloc()` → `?Handle(T)`
- `pool.free(h)`
- `pool.get(h)` + field read/write
- `?Handle` unwrap via `orelse return`
- Sequential control flow (`if`, `while`, `let`/`seq`)
- Primitives: `u32`, `bool`, `void`
- Single-threaded

## Main theorem (Year 1)

```coq
Theorem lambda_zer_handle_safety :
  forall (p : program) (s s' : state),
    well_typed p ->
    exec p s s' ->
    ~ is_uaf s' /\
    ~ is_double_free s' /\
    (terminates p s s' -> all_handles_released s').
```

Every well-typed λZER-Handle program executes without
use-after-free, double-free, or leak.

## Deferred to later years

| Year | Adds | Key technique |
|---|---|---|
| 2 | defer + move struct + goto | Continuation-style cleanup |
| 3 | MMIO + volatile + `*opaque` | Hardware semantics |
| 4 | shared struct + spawn + condvar | Iris proper (ghost state, invariants) |
| 5 | async + IR + C emission | Full compiler correctness |

Each year's subset is a new directory. Earlier subsets are imported
as dependencies; their soundness theorems stay valid.

## For fresh sessions

- Read `docs/formal_verification_plan.md` first — the long-term
  roadmap and scope decisions.
- Read this file — the current structure.
- Read `lambda_zer_handle/syntax.v` — smallest self-contained piece,
  gives you the AST to reason about.
- Don't add files to earlier subsets — add new subsets.
