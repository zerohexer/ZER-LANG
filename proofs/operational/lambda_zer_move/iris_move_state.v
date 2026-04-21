(* ================================================================
   λZER-Move : State interpretation — Iris ghost state ↔ runtime.

   The invariant: the ghost map's keys = the runtime state's
   st_live set. Owning `alive_move γ id` certifies `id ∈ st_live`.

   Key bridge lemma `move_alive_from_interp` turns resource
   ownership into the operational fact needed to fire a consume
   step.
   ================================================================ *)

From iris.base_logic.lib Require Import ghost_map.
From iris.proofmode Require Import proofmode.
From stdpp Require Import gmap sets.
From Coq Require Import Lia.
From zer_move Require Import syntax semantics iris_move_resources.

Section state_interp.
  Context `{!moveG Σ}.

  (* The invariant:
     - ghost-map domain = st_live set.
     - every live id is < st_next (counter well-formedness).

     The second clause ensures fresh allocations (using st_next as
     the new id) don't collide with existing ghost entries. *)
  Definition move_state_interp (γ : gname) (σ : state) : iProp Σ :=
    ∃ gmap_state : gmap move_id unit,
      ghost_map_auth γ 1 gmap_state ∗
      ⌜dom gmap_state = σ.(st_live)⌝ ∗
      ⌜∀ id, id ∈ σ.(st_live) → id < σ.(st_next)⌝.

  (* Initial state: empty ghost map + empty live + next=0.
     ∀ id ∈ ∅, anything = vacuously true. *)
  Lemma move_state_interp_init γ :
    ghost_map_auth γ 1 (∅ : gmap move_id unit) ⊢
      move_state_interp γ initial_state.
  Proof.
    iIntros "Hauth".
    iExists ∅. iFrame.
    iPureIntro. split.
    - simpl. rewrite dom_empty_L. reflexivity.
    - intros id Hin. simpl in Hin.
      exfalso. apply (not_elem_of_empty (C := gset move_id) id Hin).
  Qed.

  (* Bridge: owning `alive_move γ id` implies `id ∈ σ.(st_live)`. *)
  Lemma move_alive_from_interp γ σ id :
    move_state_interp γ σ -∗ alive_move γ id -∗
      ⌜id ∈ σ.(st_live)⌝.
  Proof.
    iIntros "Hinterp Hfrag".
    iDestruct "Hinterp" as (gs) "(Hauth & %Hdom & _)".
    iDestruct (alive_move_lookup with "Hauth Hfrag") as %Hlook.
    iPureIntro.
    rewrite -Hdom.
    apply elem_of_dom. exists tt. exact Hlook.
  Qed.

  (* Derived: st_next is fresh — not in any alive_move. *)
  Lemma state_interp_next_fresh γ σ :
    move_state_interp γ σ -∗
      ⌜σ.(st_next) ∉ σ.(st_live)⌝.
  Proof.
    iIntros "Hinterp".
    iDestruct "Hinterp" as (gs) "(_ & _ & %Hlt)".
    iPureIntro. intros Hin.
    apply Hlt in Hin. lia.
  Qed.

End state_interp.
