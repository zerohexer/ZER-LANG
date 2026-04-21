(* ================================================================
   λZER-Opaque : State interpretation.

   Ties Iris ghost state (ghost_map_auth γ 1 G) to the concrete
   st_ptr_types map, plus the monotonic-counter well-formedness
   invariant (every known id < st_next).
   ================================================================ *)

From iris.base_logic.lib Require Import ghost_map.
From iris.proofmode Require Import proofmode.
From stdpp Require Import gmap.
From Coq Require Import Lia.
From zer_opaque Require Import syntax semantics iris_opaque_resources.

Section state_interp.
  Context `{!opaqueG Σ}.

  (* Two-part invariant:
     1. Ghost map exactly matches st_ptr_types
     2. Every tracked id < st_next (fresh-alloc correctness) *)
  Definition opaque_state_interp (γ : gname) (σ : state) : iProp Σ :=
    ∃ gs : gmap nat type_id,
      ghost_map_auth γ 1 gs ∗
      ⌜gs = σ.(st_ptr_types)⌝ ∗
      ⌜∀ id t, gs !! id = Some t → id < σ.(st_next)⌝.

  (* Initial state *)
  Lemma opaque_state_interp_init γ :
    ghost_map_auth γ 1 (∅ : gmap nat type_id) ⊢
      opaque_state_interp γ initial_state.
  Proof.
    iIntros "Hauth".
    iExists ∅. iFrame.
    iPureIntro. split; [reflexivity|].
    intros id t Hlook. rewrite lookup_empty in Hlook. discriminate.
  Qed.

  (* Bridge lemma: owning the Iris resource proves the concrete
     state has the matching tag. This is THE key step for proving
     cast safety — if you own typed_ptr γ id t, then concretely
     st_ptr_types !! id = Some t, so ETypedCast t fires. *)
  Lemma typed_ptr_from_interp γ σ id t :
    opaque_state_interp γ σ -∗ typed_ptr γ id t -∗
      ⌜σ.(st_ptr_types) !! id = Some t⌝.
  Proof.
    iIntros "Hinterp Hfrag".
    iDestruct "Hinterp" as (gs) "(Hauth & %Heq & _)".
    iDestruct (typed_ptr_lookup with "Hauth Hfrag") as %Hlook.
    iPureIntro. rewrite -Heq. exact Hlook.
  Qed.

  (* The counter is fresh: st_next can't be in the ghost map. *)
  Lemma state_interp_next_fresh γ σ :
    opaque_state_interp γ σ -∗
      ⌜σ.(st_ptr_types) !! σ.(st_next) = None⌝.
  Proof.
    iIntros "Hinterp".
    iDestruct "Hinterp" as (gs) "(_ & %Heq & %Hlt)".
    iPureIntro.
    destruct (σ.(st_ptr_types) !! σ.(st_next)) as [t|] eqn:Heqn; [|reflexivity].
    exfalso.
    (* Heqn : st_ptr_types σ !! st_next σ = Some t; rewrite it to match
       Hlt's gs-based form. *)
    rewrite <- Heq in Heqn.
    specialize (Hlt σ.(st_next) t Heqn). lia.
  Qed.

End state_interp.
