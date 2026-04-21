(* ================================================================
   λZER-Escape : State interpretation.
   ================================================================ *)

From iris.base_logic.lib Require Import ghost_map.
From iris.proofmode Require Import proofmode.
From stdpp Require Import gmap.
From Coq Require Import Lia.
From zer_escape Require Import syntax semantics iris_escape_resources.

Section state_interp.
  Context `{!escapeG Σ}.

  Definition escape_state_interp (γ : gname) (σ : state) : iProp Σ :=
    ∃ gs : gmap nat region,
      ghost_map_auth γ 1 gs ∗
      ⌜gs = σ.(st_regions)⌝ ∗
      ⌜∀ id r, gs !! id = Some r → id < σ.(st_next)⌝.

  Lemma escape_state_interp_init γ :
    ghost_map_auth γ 1 (∅ : gmap nat region) ⊢
      escape_state_interp γ initial_state.
  Proof.
    iIntros "Hauth".
    iExists ∅. iFrame.
    iPureIntro. split; [reflexivity|].
    intros id r Hlook. rewrite lookup_empty in Hlook. discriminate.
  Qed.

  Lemma region_ptr_from_interp γ σ id r :
    escape_state_interp γ σ -∗ region_ptr γ id r -∗
      ⌜σ.(st_regions) !! id = Some r⌝.
  Proof.
    iIntros "Hinterp Hfrag".
    iDestruct "Hinterp" as (gs) "(Hauth & %Heq & _)".
    iDestruct (region_ptr_lookup with "Hauth Hfrag") as %Hlook.
    iPureIntro. rewrite -Heq. exact Hlook.
  Qed.

  Lemma state_interp_next_fresh γ σ :
    escape_state_interp γ σ -∗
      ⌜σ.(st_regions) !! σ.(st_next) = None⌝.
  Proof.
    iIntros "Hinterp".
    iDestruct "Hinterp" as (gs) "(_ & %Heq & %Hlt)".
    iPureIntro.
    destruct (σ.(st_regions) !! σ.(st_next)) as [r|] eqn:Heqn; [|reflexivity].
    exfalso.
    rewrite <- Heq in Heqn.
    specialize (Hlt σ.(st_next) r Heqn). lia.
  Qed.

End state_interp.
