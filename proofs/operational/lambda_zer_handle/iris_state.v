(* ================================================================
   Phase 1b — State interpretation for λZER-Handle.

   Connects Iris ghost state (ghost_map_auth γ 1 gens) to ZER's
   concrete runtime state (semantics.state).

   The core invariant:
     For every (p, i), gens !! (p,i) = Some g  iff
       st_store !! p = Some ps ∧ ps !! i = Some s ∧
       slot_gen s = g ∧ is_Some (slot_val s).

   In words: "the ghost map tracks exactly the alive handles in
   the concrete store, with matching generations."

   This is what ties the Iris resource `alive_handle γ p i g`
   (ownership of fragment (p,i) ↪ g) to an operational fact
   (slot (p,i) is currently alive with gen g).

   Phase 1c will use this to prove wp specs for pool.alloc/free/get.
   ================================================================ *)

From iris.program_logic Require Import weakestpre.
From iris.base_logic.lib Require Import ghost_map invariants.
From iris.proofmode Require Import proofmode.
From zer_handle Require Import syntax semantics iris_lang iris_resources.

(* ---- State-interpretation: the main invariant ----

   We hold the authoritative ghost map together with a pure
   predicate saying "ghost map agrees with the concrete store on
   aliveness." Putting the pure predicate as part of state_interp
   means wp rules can extract it when they need to reason about
   concrete state from resource ownership. *)

Definition slot_is_alive (σ : state) (p : pool_id) (i : nat) (g : nat) : Prop :=
  ∃ ps s, σ.(st_store) !! p = Some ps
       ∧ ps !! i = Some s
       ∧ slot_gen s = g
       ∧ is_Some (slot_val s).

Definition gens_agree_store (σ : state) (gens : gmap (pool_id * nat) nat) : Prop :=
  ∀ p i g, gens !! (p, i) = Some g ↔ slot_is_alive σ p i g.

Section state_interp.
  Context `{!handleG Σ}.

  (* The handle invariant, parameterized by a ghost name γ. *)
  Definition handle_state_interp (γ : gname) (σ : state) : iProp Σ :=
    ∃ gens : gmap (pool_id * nat) nat,
      ghost_map_auth γ 1 gens ∗
      ⌜gens_agree_store σ gens⌝.

  (* Sanity: the initial state (empty store) pairs with an empty
     ghost map. Used when setting up the irisGS instance for the
     adequacy proof. *)
  Lemma handle_state_interp_init γ :
    ghost_map_auth γ 1 (∅ : gmap (pool_id * nat) nat) ⊢
      handle_state_interp γ (mkState ∅ ∅ ∅ None).
  Proof.
    iIntros "Hauth".
    iExists ∅. iFrame.
    iPureIntro. intros p i g. split.
    - intros Hlook. rewrite lookup_empty in Hlook. discriminate.
    - intros [ps [s [Hps _]]]. simpl in Hps. rewrite lookup_empty in Hps. discriminate.
  Qed.

  (* Key deduction: if we own `alive_handle γ p i g` AND the state
     interp for σ, then slot (p,i) in σ is operationally alive with
     gen g. This bridges the Iris world to the operational world —
     the wp spec for pool.get / pool.free will use this to show
     the step rule fires. *)
  Lemma handle_alive_from_interp γ σ p i g :
    handle_state_interp γ σ -∗ alive_handle γ p i g -∗
      ⌜slot_is_alive σ p i g⌝.
  Proof.
    iIntros "Hinterp Hfrag".
    iDestruct "Hinterp" as (gens) "[Hauth %Hagree]".
    iDestruct (alive_handle_lookup with "Hauth Hfrag") as %Hlook.
    iPureIntro. apply Hagree. exact Hlook.
  Qed.

End state_interp.

(* ---- IrisGS-gen instance for λZH_lang ----

   Wraps the handle state interp into Iris's general program-logic
   interface. This is what makes `WP e {{ Φ }}` meaningful on
   λZER-Handle expressions.

   We parameterize by the ghost name for the handle map (stored
   in handleGS — a dedicated typeclass so wp specs can access it).

   Fields:
     - state_interp: connects runtime state to ghost state.
     - fork_post: post-condition for forked threads (no threads
       in λZER-Handle → trivial `True`).
     - num_laters_per_step: step-indexing granularity (0 = no
       step-index, sequential only).
     - state_interp_mono: state_interp is monotone in step count
       (trivial since we don't use step count). *)

Class handleGS Σ := HandleGS {
  handleGS_invG :: invGS_gen HasNoLc Σ;
  handleGS_handleG :: handleG Σ;
  handle_gname : gname;
}.

Global Program Instance handleGS_irisGS `{!handleGS Σ} :
    irisGS_gen HasNoLc λZH_lang Σ := {
  iris_invGS := handleGS_invG;
  state_interp σ _ _ _ := handle_state_interp handle_gname σ;
  fork_post _ := True%I;
  num_laters_per_step _ := 0;
  state_interp_mono _ _ _ _ := fupd_intro _ _;
}.
