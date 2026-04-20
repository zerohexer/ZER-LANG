# Operational-Semantics Proofs for ZER-LANG

**Status:** λZER-Handle baseline compiles (plain Coq). Iris migration beginning with Phase 0 — see `docs/formal_verification_plan.md`.

Proves ZER's safety claims at the **implementation level**, not just the abstract-model level (which is in the parent `proofs/` directory). The proofs serve as a correctness oracle for the compiler: when a proof breaks, it points at a compiler invariant that was violated.

## Relationship to the top-level `proofs/` directory

- `../proofs/*.v` — abstract 4-model soundness (design-level, 2,401 lines, plain Coq).
- `proofs/operational/` (this directory) — connects the abstract model to actual ZER program execution (implementation-level, Coq + Iris).

Abstract model: *"If the rules are X, then properties Y follow."*

Operational proofs: *"ZER's concrete language rules match X, so Y holds for every ZER program."*

Both are formal verification in Coq. They answer different questions.

## Architecture

```
operational/
  Dockerfile        Coq 8.18 + Iris 4.x + stdpp (opam-resolved)
  Makefile          wraps CoqMakefile
  _CoqProject       file list + compile flags
  README.md         this file
  lambda_zer_handle/    # first subset: sequential handle safety
    syntax.v          # AST: values, expressions, programs
    semantics.v       # small-step operational semantics
    typing.v          # type system
    adequacy.v        # preservation + progress (being ported to Iris wp)
    handle_safety.v   # main theorem
```

Future subsets (e.g., `lambda_zer_defer/`, `lambda_zer_mmio/`, `lambda_zer_concurrency/`) land as new directories. See `docs/formal_verification_plan.md` for the full subset roadmap.

## Building

```bash
# Build the Docker image (once):
docker build -t zer-proofs -f proofs/operational/Dockerfile .

# Compile all proofs:
docker run --rm -v "$PWD/proofs/operational:/work" zer-proofs \
    bash -c 'eval $(opam env) && make'
```

Every `.v` file must compile green. A PR that breaks the build is not merged (CI enforcement is Phase 3 work — see the plan doc).

## Why Iris

Sequential proofs don't *require* Iris — plain Coq is sufficient for handle safety in a single-threaded setting. But concurrency (a future subset) genuinely needs Iris's ghost state, invariants, and step-indexing. Starting in Iris from the beginning avoids a painful port later.

Sequential Iris proofs use:
- Weakest preconditions (`wp`) for reasoning about program execution
- Iris's `language` typeclass to plug in our small-step semantics
- Resources (`iProp`) to encode handle liveness directly — owning `alive_handle p i g` *is* the proof that the handle is alive
- Iris's adequacy theorem to connect `wp` to operational safety

When concurrency lands, the same infrastructure extends — no rewrite.

## λZER-Handle subset

Scope defined in `docs/formal_verification_plan.md`:

- `Pool(T, N)` / `Slab(T)` declarations
- `pool.alloc()` → `?Handle(T)`
- `pool.free(h)`
- `pool.get(h)` + field read/write
- `?Handle` unwrap via `orelse return`
- Sequential control flow (`if`, `while`, `let`/`seq`)
- Primitives: `u32`, `bool`, `void`
- Single-threaded

## Main theorem

```coq
Theorem lambda_zer_handle_safety :
  forall (p : program) (s s' : state),
    well_typed p ->
    exec p s s' ->
    ~ is_uaf s' /\
    ~ is_double_free s' /\
    (terminates p s s' -> all_handles_released s').
```

Every well-typed λZER-Handle program executes without use-after-free, double-free, or leak.

## Current status

| File | State |
|---|---|
| `syntax.v` | ✓ complete |
| `semantics.v` | ✓ complete (Tier 1 step rules) |
| `typing.v` | ✓ complete + canonical-forms lemmas |
| `adequacy.v` | ⚠ partial — 17 admits (10 preservation + 7 progress). Being ported to Iris wp. |
| `handle_safety.v` | ⚠ scaffolding — 3 admits for main theorems. Depends on adequacy. |

## Iris migration — Phase 0 in progress

Before rewriting the full adequacy proof in Iris, Phase 0 validates tractability with a minimal end-to-end proof:

**Target:** Prove, using Iris weakest preconditions, that
```
let h = pool.alloc() orelse return;
pool.free(h);
```
is safe.

**Phase 0 location:** new file `lambda_zer_handle/iris_demo.v` (pending).

If Phase 0 works, Phase 1 rewrites the full subset in Iris. If it stalls, plain Coq continuation is the fallback — existing work is preserved as insurance.

## Deferred to future subsets

Each subset is a new directory. Earlier subsets are imported as dependencies; their soundness theorems stay valid.

| Subset | Adds | Key technique |
|---|---|---|
| `lambda_zer_defer` | defer + move struct + goto | Continuation-style cleanup |
| `lambda_zer_mmio` | MMIO + volatile + `*opaque` | Hardware semantics |
| `lambda_zer_concurrency` | shared struct + spawn + condvar | Iris concurrency primitives (ghost state, invariants) |
| `lambda_zer_async` | async + IR + C emission | State-machine semantics + compiler correctness |

## For fresh sessions

- Read `docs/formal_verification_plan.md` first — the long-term roadmap and scope decisions.
- Read this file — the current structure and status.
- Read `lambda_zer_handle/syntax.v` — smallest self-contained piece, gives you the AST to reason about.
- Don't add files to earlier subsets — add new subsets.
- Don't write a new `.v` file without a `From iris ... Require Import ...` — with rare exceptions (`syntax.v`, `typing.v`) the framework is Iris.
