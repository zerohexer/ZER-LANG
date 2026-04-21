(* ================================================================
   λZER-Move : Step specs in fupd form.

   For each move-struct operation, prove that resource ownership
   is sufficient to fire the operational step, with appropriate
   ghost-state updates preserving state_interp.

   This is the OPERATIONAL depth that section B lacked at schematic
   level. Every B-row theorem now has a concrete step-rule backing.
   ================================================================ *)

From iris.base_logic.lib Require Import ghost_map.
From iris.proofmode Require Import proofmode.
From stdpp Require Import gmap sets.
From Coq Require Import Lia.
From zer_move Require Import syntax semantics
                             iris_move_resources iris_move_state.

Section step_specs.
  Context `{!moveG Σ}.

  (* ---- Consume: operational step + resource consumption ----

     Pre:  state_interp + alive_move γ id
     Post: state_interp for σ' (id removed from live),
           resource CONSUMED (not in post)
     Step: step σ (EConsume (EVal (VMove id))) σ' (EVal VUnit) *)

  Lemma step_spec_consume γ σ id :
    move_state_interp γ σ -∗
    alive_move γ id ==∗
      ∃ σ',
        ⌜step σ (EConsume (EVal (VMove id))) σ' (EVal VUnit)⌝ ∗
        move_state_interp γ σ'.
  Proof.
    iIntros "Hinterp Hh".
    iDestruct (move_alive_from_interp with "Hinterp Hh") as %Hlive.
    iDestruct "Hinterp" as (gs) "(Hauth & %Hdom & %Hlt)".
    iMod (alive_move_consume γ gs id with "Hauth Hh") as "Hauth'".
    set (σ' := mkState (σ.(st_live) ∖ {[ id ]})
                       σ.(st_env) σ.(st_next)).
    iModIntro. iExists σ'. iSplit.
    - iPureIntro. subst σ'. apply step_consume. exact Hlive.
    - iExists (delete id gs). iFrame "Hauth'".
      iPureIntro. split.
      + subst σ'. simpl. rewrite dom_delete_L. rewrite Hdom. reflexivity.
      + subst σ'. simpl. intros id' Hin'.
        apply elem_of_difference in Hin' as [Hin_old _].
        apply Hlt. exact Hin_old.
  Qed.

  (* ---- Drop: same shape as consume ---- *)

  Lemma step_spec_drop γ σ id :
    move_state_interp γ σ -∗
    alive_move γ id ==∗
      ∃ σ',
        ⌜step σ (EDrop (EVal (VMove id))) σ' (EVal VUnit)⌝ ∗
        move_state_interp γ σ'.
  Proof.
    iIntros "Hinterp Hh".
    iDestruct (move_alive_from_interp with "Hinterp Hh") as %Hlive.
    iDestruct "Hinterp" as (gs) "(Hauth & %Hdom & %Hlt)".
    iMod (alive_move_consume γ gs id with "Hauth Hh") as "Hauth'".
    set (σ' := mkState (σ.(st_live) ∖ {[ id ]})
                       σ.(st_env) σ.(st_next)).
    iModIntro. iExists σ'. iSplit.
    - iPureIntro. subst σ'. apply step_drop. exact Hlive.
    - iExists (delete id gs). iFrame "Hauth'".
      iPureIntro. split.
      + subst σ'. simpl. rewrite dom_delete_L. rewrite Hdom. reflexivity.
      + subst σ'. simpl. intros id' Hin'.
        apply elem_of_difference in Hin' as [Hin_old _].
        apply Hlt. exact Hin_old.
  Qed.

  (* ---- Allocation: fresh resource ----

     Pre:  state_interp
     Post: fresh alive_move for a NEW id = σ.(st_next).
     Step: step σ EAllocMove σ' (EVal (VMove id)) *)

  Lemma step_spec_alloc γ σ :
    move_state_interp γ σ ==∗
      ∃ σ' id,
        ⌜step σ EAllocMove σ' (EVal (VMove id))⌝ ∗
        move_state_interp γ σ' ∗
        alive_move γ id.
  Proof.
    iIntros "Hinterp".
    iDestruct "Hinterp" as (gs) "(Hauth & %Hdom & %Hlt)".
    set (new_id := σ.(st_next)).
    (* new_id ∉ dom gs because dom gs = st_live, and every element
       of st_live is < st_next = new_id, so new_id itself cannot be
       a member (< is irreflexive). *)
    assert (Hfresh : gs !! new_id = None).
    { destruct (gs !! new_id) as [v|] eqn:Heq; [|reflexivity].
      exfalso.
      assert (new_id ∈ dom gs) as Hin
        by (apply elem_of_dom; eexists; exact Heq).
      rewrite Hdom in Hin.
      apply Hlt in Hin.
      subst new_id. lia. }
    iMod (alive_move_new γ gs new_id Hfresh with "Hauth")
         as "[Hauth' Hfrag]".
    set (σ' := mkState ({[ new_id ]} ∪ σ.(st_live))
                       σ.(st_env) (S σ.(st_next))).
    iModIntro. iExists σ', new_id. iFrame "Hfrag". iSplit.
    - iPureIntro. subst σ' new_id. apply step_alloc_move.
    - iExists (<[ new_id := tt ]> gs). iFrame "Hauth'".
      iPureIntro. split.
      + subst σ' new_id. simpl.
        rewrite dom_insert_L. rewrite Hdom. reflexivity.
      + subst σ' new_id. simpl.
        intros id' Hin'.
        apply elem_of_union in Hin' as [Hin_new | Hin_old].
        * apply elem_of_singleton in Hin_new. subst. lia.
        * apply Hlt in Hin_old. lia.
  Qed.

End step_specs.
