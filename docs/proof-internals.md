# Proof Internals — Coq + Iris for ZER-LANG

**Required reading before modifying any `proofs/operational/**/*.v` file.** Captures tactic recipes, name-collision workarounds, Iris idioms, and every non-obvious thing learned while building λZER-Handle. Same role as `compiler-internals.md` but for the proof side.

## When to read this

- Starting a fresh session on proofs
- Modifying existing `.v` files in `proofs/operational/`
- Adding a new subset (`lambda_zer_move`, `lambda_zer_mmio`, etc.)
- Debugging a Coq build failure

## The build

```bash
# One-time: build the Docker image (Coq 8.18 + Iris 4.2 + stdpp 1.10)
docker build -t zer-proofs -f proofs/operational/Dockerfile .

# Compile all proofs:
cd proofs/operational
MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W):/work" -w /work zer-proofs \
    bash -c 'eval $(opam env) && make'
```

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

**Zero admits across all Iris files.** Every `.v` file ends with `Qed.`, never `Admitted.`. If this changes, something regressed.

Verify:
```bash
grep -c "Admitted\|admit\." proofs/operational/lambda_zer_handle/iris_*.v
```
Should be all zeros.

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

## Reading order for fresh sessions

1. `docs/formal_verification_plan.md` — big picture (Iris-from-start, no timeline)
2. `proofs/operational/README.md` — directory architecture
3. `docs/safety_list.md` — what's proven and what isn't (203-row coverage matrix)
4. This file (`proof-internals.md`) — tactics, gotchas, patterns
5. The `.v` files in dependency order: `syntax.v` → `semantics.v` → `typing.v` → `iris_lang.v` → `iris_resources.v` → `iris_state.v` → `iris_specs.v` → `iris_step_specs.v` → `iris_leak.v` → `iris_adequacy.v` → `iris_demo.v`

Each `.v` file has a header comment describing its Phase (0, 1a, 1b, etc.) and what it delivers.

## What's next (when continuing)

Per `docs/safety_list.md`, section A has 6 remaining ◐ rows. Priorities:

1. **A03, A04** — resource fractions. Need to thread alloc_id from parent pointer to interior pointer. Iris's fractional ghost ownership (using `q : Qp` fractions) is the tool.
2. **A05, A07** — `FuncSpec` iProp. RustBelt's approach: specify each function's resource pre/post via `□ ∀ args, P args -∗ WP body {{ Q }}`. Parameters threaded by caller.
3. **A15, A16** — loop fixpoints. Iris has `wp_while` / `wp_for` combinators; need Löb induction for convergence.

Sections B (move), C-D (concurrency), H (MMIO), etc. are separate subsets, each in their own directory. Don't start them until A is further closed.

## Invariants maintained by this doc

- **Every `.v` file ends `Qed.`, never `Admitted.`** — verified by grep at every commit.
- **Every `.v` file has a header comment** explaining Phase + what it delivers.
- **Section A row count in `safety_list.md` matches proven lemmas** — update when closing rows.
- **No modification to `model*.v`** — design-level proofs are frozen at commit `000064d`.
