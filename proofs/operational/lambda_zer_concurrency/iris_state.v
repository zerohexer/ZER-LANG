(* ================================================================
   λZER-Concurrency : State interpretation + irisGS  (DESIGN.md §7 step 1b)

   Makes `WP e {{ Φ }}` meaningful on λZER-Concurrency expressions and
   plugs into Iris's THREADPOOL adequacy. The shared heap `gmap loc val`
   is interpreted by Iris's standard `gen_heap` library, which gives the
   points-to `ℓ ↦ v` — the resource that §4.2's shared invariant
   `inv N (∃ v, ℓ ↦ v ∗ payload)` will guard (the discipline condition)
   and that the §4.1 share-tag / §4.3 region+join ghost state layer onto.

   Fields of the irisGS instance (mirroring lambda_zer_handle/iris_state.v):
     - state_interp = gen_heap authority over the heap.
     - fork_post = True (refined to the join obligation in the
       region+linear-join file, DESIGN §4.3 — a forked thread's post is
       what `Join` consumes).
     - num_laters_per_step = 0, HasNoLc (no later credits).

   This is the "adequacy plumbing" of step 1: once this instance exists,
   Iris's generic adequacy applies to the threadpool, and the
   DR-free / UAF-free theorem (DESIGN §0) is stated against it in the
   theorems file (step 6).
   ================================================================ *)

From iris.program_logic Require Import weakestpre.
From iris.base_logic.lib Require Import gen_heap.
From iris.proofmode Require Import proofmode.
From zer_conc Require Import syntax semantics iris_lang.

(* ---- The Iris ghost-state bundle for λZER-Concurrency ----

   - invGS_gen HasNoLc Σ : the invariant ghost state (needed for fupd /
     Iris invariants — the shared-struct invariant lives here).
   - gen_heapGS loc val Σ : the heap authority + `ℓ ↦ v` fragments. *)

Class concGS Σ := ConcGS {
  concGS_invG :: invGS_gen HasNoLc Σ;
  concGS_heapG :: gen_heapGS loc val Σ;
}.

(* ---- The irisGS_gen instance — `WP` becomes available ---- *)

Global Program Instance concGS_irisGS `{!concGS Σ} :
    irisGS_gen HasNoLc λZC_lang Σ := {
  iris_invGS := concGS_invG;
  state_interp σ _ _ _ := gen_heap_interp σ;
  fork_post _ := True%I;
  num_laters_per_step _ := 0;
  state_interp_mono _ _ _ _ := fupd_intro _ _;
}.

Section smoke.
  Context `{!concGS Σ}.

  (* Plumbing smoke test: WP is meaningful — a value satisfies its own
     postcondition. If the irisGS instance were malformed this would not
     typecheck. *)
  Lemma wp_smoke_value (v : val) :
    ⊢ WP (EVal v) {{ w, ⌜w = v⌝ }}.
  Proof. iApply wp_value. done. Qed.

End smoke.
