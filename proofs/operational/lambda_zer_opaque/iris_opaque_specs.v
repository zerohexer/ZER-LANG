(* ================================================================
   λZER-Opaque : Step specs in fupd form.

   For each pointer operation (alloc, opaque-cast, typed-cast,
   deref), prove that resource ownership justifies firing the
   corresponding operational step and preserves state_interp.
   ================================================================ *)

From iris.base_logic.lib Require Import ghost_map.
From iris.proofmode Require Import proofmode.
From stdpp Require Import gmap.
From Coq Require Import Lia.
From zer_opaque Require Import syntax semantics iris_opaque_resources
                               iris_opaque_state.

Section step_specs.
  Context `{!opaqueG Σ}.

  (* ---- Allocation: fresh resource + state update ---- *)
  Lemma step_spec_alloc γ σ t :
    opaque_state_interp γ σ ==∗
      ∃ σ' id,
        ⌜step σ (EAlloc t) σ' (EVal (VPtr (PtrTyped id t)))⌝ ∗
        opaque_state_interp γ σ' ∗
        typed_ptr γ id t.
  Proof.
    iIntros "Hinterp".
    iDestruct (state_interp_next_fresh with "Hinterp") as %Hfresh.
    iDestruct "Hinterp" as (gs) "(Hauth & %Heq & %Hlt)".
    set (new_id := σ.(st_next)).
    (* Fresh in ghost map via state_interp_next_fresh + Heq *)
    assert (gs !! new_id = None) as Hfresh_gs.
    { rewrite Heq. exact Hfresh. }
    iMod (typed_ptr_new γ gs new_id t Hfresh_gs with "Hauth")
         as "[Hauth' Hfrag]".
    set (σ' := mkState (S σ.(st_next))
                       σ.(st_env)
                       (<[ new_id := t ]> σ.(st_ptr_types))).
    iModIntro. iExists σ', new_id. iFrame "Hfrag". iSplit.
    - iPureIntro. subst σ' new_id. apply step_alloc.
    - iExists (<[ new_id := t ]> gs). iFrame "Hauth'".
      iPureIntro. split.
      + subst σ' new_id. simpl. rewrite Heq. reflexivity.
      + subst σ' new_id. simpl.
        intros id' t' Hlook.
        apply lookup_insert_Some in Hlook as [[<- <-] | [Hne Hlook']].
        * lia.
        * apply Hlt in Hlook'. lia.
  Qed.

  (* ---- Opaque cast: no state change, resource retained --

     The step is definitionally trivial (PtrTyped id t goes to itself
     — the static type is erased but the tag is unchanged). *)
  Lemma step_spec_opaque_cast γ σ id t :
    typed_ptr γ id t -∗
      ⌜step σ (EOpaqueCast (EVal (VPtr (PtrTyped id t))))
           σ  (EVal (VPtr (PtrTyped id t)))⌝ ∗
      typed_ptr γ id t.
  Proof.
    iIntros "Hh".
    iFrame. iPureIntro.
    apply step_opaque_cast.
  Qed.

  (* ---- Typed cast: resource + state_interp ⇒ step fires with
     matching tag --

     CORE SAFETY SPEC. Owning typed_ptr γ id t means `st_ptr_types
     !! id = Some t` (from state_interp). Therefore ETypedCast t
     on this pointer can fire step_typed_cast_ok. If you tried
     ETypedCast t' with t' ≠ t, you'd need typed_ptr γ id t' which
     by typed_ptr_agree would force t' = t — contradiction.

     So: having the wrong-type resource is already impossible at
     the Iris level, and the step rule enforces the same at the
     operational level. *)
  Lemma step_spec_typed_cast γ σ id t :
    opaque_state_interp γ σ -∗
    typed_ptr γ id t -∗
      ⌜step σ (ETypedCast t (EVal (VPtr (PtrTyped id t))))
           σ  (EVal (VPtr (PtrTyped id t)))⌝ ∗
      opaque_state_interp γ σ ∗
      typed_ptr γ id t.
  Proof.
    iIntros "Hinterp Hh".
    iDestruct (typed_ptr_from_interp with "Hinterp Hh") as %Hlook.
    iFrame. iPureIntro.
    apply step_typed_cast_ok. exact Hlook.
  Qed.

  (* ---- Deref: resource ⇒ step fires --

     Owning typed_ptr γ id t means the concrete st_ptr_types has
     the matching entry, so step_deref fires. *)
  Lemma step_spec_deref γ σ id t :
    opaque_state_interp γ σ -∗
    typed_ptr γ id t -∗
      ⌜step σ (EDeref (EVal (VPtr (PtrTyped id t))))
           σ  (EVal VUnit)⌝ ∗
      opaque_state_interp γ σ ∗
      typed_ptr γ id t.
  Proof.
    iIntros "Hinterp Hh".
    iDestruct (typed_ptr_from_interp with "Hinterp Hh") as %Hlook.
    iFrame. iPureIntro.
    eapply step_deref. exact Hlook.
  Qed.

End step_specs.
