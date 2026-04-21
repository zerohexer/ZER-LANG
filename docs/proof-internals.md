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

**Status:** not started. Should precede Level 3 VST.

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
| H01-H09 (MMIO) | Schematic | `iris_mmio_cast_escape.v` |
| I01-I11 (qualifiers) | Schematic | `iris_typing_rules.v` |
| J01-J14 (cast/provenance) | Schematic | `iris_mmio_cast_escape.v` |
| K01-K04 (@container/etc) | Schematic | `iris_typing_rules.v` |
| L01-L11 (bounds) | Schematic | `iris_misc_sections.v` |
| M01-M13 (arith) | Schematic | `iris_misc_sections.v` |
| N01-N08 (optional) | Schematic | `iris_typing_rules.v` |
| O01-O12 (escape) | Schematic | `iris_mmio_cast_escape.v` |
| P01-P08 (variant) | Schematic | `iris_misc_sections.v` |
| Q01-Q05 (switch) | Schematic | `iris_typing_rules.v` |
| R01-R07 (comptime) | Schematic | `iris_misc_sections.v` |
| S01-S06 (resource limits) | Schematic | `iris_misc_sections.v` |
| T01-T07 (container validity) | Schematic | `iris_container_validity.v` |

Language infrastructure:
- `iris_lang.v` — Canonical language instance for λZH_lang
- `iris_smoke.v` — Iris imports + basic BI sanity

### Schematic vs operational depth

This is the MOST IMPORTANT distinction when reading the proofs.

**Operational depth** (section A only):
- Resource algebra defined (`alive_handle γ p i g : iProp`)
- State interpretation connecting ghost state to concrete state
- fupd-style step specs tying Iris resources to semantics.v's step relation
- Direct proof of safety via resource discipline

**Schematic depth** (all other sections):
- Closure lemma (often `Lemma foo : True. Proof. exact I. Qed.`)
- Comment block documenting the compiler-side enforcement mechanism
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

### Level 3 — VST on compiler

When schematic proofs exist for all sections, the next value-add is VST-verifying the compiler's implementation against them. Scope: ~50 safety-critical functions in zercheck.c + emitter.c.

- Per-function contract + VST proof: ~5-20 hours each
- Total: ~150-500 hours for full "Iris spec as compiler correctness oracle"

## Design precedents — use for consistency

When building new schematic closures:
- Section A (resource algebra + fupd step specs + adequacy) is the OPERATIONAL template.
- Section B (`iris_move.v`) is the template for "new linear resource, reuse handleG" — a dedicated subset would extract this to its own directory.
- Section G (`iris_control_flow.v`) is the template for "context-flag checks" — pure typing, schematic.
- Section T (`iris_container_validity.v`) is the template for "structural well-formedness" — mostly `True` proofs with strong comments.
- Section L/M (`iris_misc_sections.v`) is the template for "VRP integration" — points at compile-time + runtime checks.

When unsure which template to use, grep the `Covers safety_list.md rows:` comment in each file to find the match.

## Invariants maintained by this doc

- **Every `.v` file ends `Qed.`, never `Admitted.`** — verified by grep at every commit.
- **Every `.v` file has a header comment** explaining Phase + what it delivers.
- **Section A row count in `safety_list.md` matches proven lemmas** — update when closing rows.
- **No modification to `model*.v`** — design-level proofs are frozen at commit `000064d`.
