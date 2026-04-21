(* ================================================================
   λZER-Escape : Step specs in fupd form.

   Key spec: step_spec_store_global and step_spec_return ONLY fire
   when the pointer's region is RegStatic. The resource required
   in the precondition pins the region, which the compiler must
   match.
   ================================================================ *)

From iris.base_logic.lib Require Import ghost_map.
From iris.proofmode Require Import proofmode.
From stdpp Require Import gmap.
From Coq Require Import Lia.
From zer_escape Require Import syntax semantics
                              iris_escape_resources iris_escape_state.

Section step_specs.
  Context `{!escapeG Σ}.

  (* Generic alloc helper — creates a fresh region_ptr with any region. *)
  Lemma step_spec_alloc_gen γ σ (alloc_e : expr) (r : region)
        (step_rule : forall st,
           let new_id := st.(st_next) in
           let st' := mkState (S st.(st_next)) st.(st_env)
                              (<[ new_id := r ]> st.(st_regions))
                              st.(st_globals_stored) st.(st_returned)
           in step st alloc_e st' (EVal (VPtr (PtrTagged new_id r)))) :
    escape_state_interp γ σ ==∗
      ∃ σ' id,
        ⌜step σ alloc_e σ' (EVal (VPtr (PtrTagged id r)))⌝ ∗
        escape_state_interp γ σ' ∗
        region_ptr γ id r.
  Proof.
    iIntros "Hinterp".
    iDestruct (state_interp_next_fresh with "Hinterp") as %Hfresh.
    iDestruct "Hinterp" as (gs) "(Hauth & %Heq & %Hlt)".
    set (new_id := σ.(st_next)).
    assert (gs !! new_id = None) as Hfresh_gs.
    { rewrite Heq. exact Hfresh. }
    iMod (region_ptr_new γ gs new_id r Hfresh_gs with "Hauth")
         as "[Hauth' Hfrag]".
    set (σ' := mkState (S σ.(st_next)) σ.(st_env)
                       (<[ new_id := r ]> σ.(st_regions))
                       σ.(st_globals_stored) σ.(st_returned)).
    iModIntro. iExists σ', new_id. iFrame "Hfrag". iSplit.
    - iPureIntro. subst σ' new_id. apply step_rule.
    - iExists (<[ new_id := r ]> gs). iFrame "Hauth'".
      iPureIntro. split.
      + subst σ' new_id. simpl. rewrite Heq. reflexivity.
      + subst σ' new_id. simpl.
        intros id' r' Hlook.
        apply lookup_insert_Some in Hlook as [[<- _] | [Hne Hlook']].
        * lia.
        * apply Hlt in Hlook'. lia.
  Qed.

  Lemma step_spec_alloc_local γ σ :
    escape_state_interp γ σ ==∗
      ∃ σ' id,
        ⌜step σ EAllocLocal σ' (EVal (VPtr (PtrTagged id RegLocal)))⌝ ∗
        escape_state_interp γ σ' ∗
        region_ptr γ id RegLocal.
  Proof.
    iApply step_spec_alloc_gen.
    intros st. apply step_alloc_local.
  Qed.

  Lemma step_spec_alloc_arena γ σ :
    escape_state_interp γ σ ==∗
      ∃ σ' id,
        ⌜step σ EAllocArena σ' (EVal (VPtr (PtrTagged id RegArena)))⌝ ∗
        escape_state_interp γ σ' ∗
        region_ptr γ id RegArena.
  Proof.
    iApply step_spec_alloc_gen.
    intros st. apply step_alloc_arena.
  Qed.

  Lemma step_spec_alloc_static γ σ :
    escape_state_interp γ σ ==∗
      ∃ σ' id,
        ⌜step σ EAllocStatic σ' (EVal (VPtr (PtrTagged id RegStatic)))⌝ ∗
        escape_state_interp γ σ' ∗
        region_ptr γ id RegStatic.
  Proof.
    iApply step_spec_alloc_gen.
    intros st. apply step_alloc_static.
  Qed.

  (* ---- Store-global: requires RegStatic ----

     With `region_ptr γ id RegStatic` and state_interp, the step
     fires. For RegLocal/RegArena, the required `region_ptr γ id
     RegStatic` would conflict with the actual RegLocal/RegArena
     resource — Iris exclusivity forbids owning both. *)
  Lemma step_spec_store_global_static γ σ id :
    escape_state_interp γ σ -∗
    region_ptr γ id RegStatic ==∗
      ∃ σ',
        ⌜step σ (EStoreGlobal (EVal (VPtr (PtrTagged id RegStatic))))
             σ' (EVal VUnit)⌝ ∗
        escape_state_interp γ σ' ∗
        region_ptr γ id RegStatic.
  Proof.
    iIntros "Hinterp Hh".
    iDestruct (region_ptr_from_interp with "Hinterp Hh") as %Hlook.
    iDestruct "Hinterp" as (gs) "(Hauth & %Heq & %Hlt)".
    set (σ' := mkState σ.(st_next) σ.(st_env) σ.(st_regions)
                       (S σ.(st_globals_stored)) σ.(st_returned)).
    iModIntro. iExists σ'. iFrame "Hh". iSplit.
    - iPureIntro. subst σ'. apply step_store_global. exact Hlook.
    - iExists gs. iFrame "Hauth". iPureIntro. split.
      + subst σ'. simpl. exact Heq.
      + subst σ'. simpl. exact Hlt.
  Qed.

  (* ---- Return: requires RegStatic ---- *)
  Lemma step_spec_return_static γ σ id :
    escape_state_interp γ σ -∗
    region_ptr γ id RegStatic ==∗
      ∃ σ',
        ⌜step σ (EReturn (EVal (VPtr (PtrTagged id RegStatic))))
             σ' (EVal VUnit)⌝ ∗
        escape_state_interp γ σ' ∗
        region_ptr γ id RegStatic.
  Proof.
    iIntros "Hinterp Hh".
    iDestruct (region_ptr_from_interp with "Hinterp Hh") as %Hlook.
    iDestruct "Hinterp" as (gs) "(Hauth & %Heq & %Hlt)".
    set (σ' := mkState σ.(st_next) σ.(st_env) σ.(st_regions)
                       σ.(st_globals_stored) (S σ.(st_returned))).
    iModIntro. iExists σ'. iFrame "Hh". iSplit.
    - iPureIntro. subst σ'. apply step_return. exact Hlook.
    - iExists gs. iFrame "Hauth". iPureIntro. split.
      + subst σ'. simpl. exact Heq.
      + subst σ'. simpl. exact Hlt.
  Qed.

End step_specs.
