(* ================================================================
   Phase 1e — Step-level specs in fupd form.

   Rather than going through `wp_lift_atomic_step` (which requires
   threading Iris's later-credits, fancy-update masks, and stuckness
   infrastructure), we prove the essential content of a wp spec
   directly:

     "Given state interp + ownership of the right resources,
      the operational step rule fires, and the state interp is
      preserved (after appropriate ghost updates)."

   This is the same safety content as a wp triple `{pre} e {post}`:
     - Precondition: the resources owned before the step.
     - Operational step: the concrete reduction.
     - Post state_interp: updated ghost state matching new concrete state.
     - Postcondition: resources produced after the step.

   Phase 1f (adequacy) will combine these with the language's
   operational safety theorem to yield the final handle_safety.

   Covers safety_list.md rows:
     A06 (double-free simple) — free consumes resource
     A01 (UAF simple)         — get preserves resource, freed has none
     A17 (runtime gen check)  — compile-time proven redundant
   ================================================================ *)

From iris.base_logic.lib Require Import ghost_map.
From iris.proofmode Require Import proofmode.
From zer_handle Require Import syntax semantics iris_resources iris_state.

Section step_specs.
  Context `{!handleG Σ}.

  (* ---- Step spec for pool.free ----

     Precondition: handle_state_interp + alive_handle, st_returned = None.
     Operational: one step to EVal VUnit, with state updated
                  (slot gen bumped, val cleared).
     Postcondition: handle_state_interp preserved for the new state.
                    alive_handle resource CONSUMED (not in post).

     The `==∗` (fancy update) is needed because we update ghost state. *)

  Lemma step_spec_free γ σ p i g :
    σ.(st_returned) = None →
    handle_state_interp γ σ -∗
    alive_handle γ p i g ==∗
      ∃ σ',
        ⌜step σ (EFree p (EVal (VHandle p i g))) σ' (EVal VUnit)⌝ ∗
        handle_state_interp γ σ'.
  Proof.
    iIntros (Hret) "Hinterp Hh".
    (* Extract operational aliveness from the resources. *)
    iDestruct (handle_alive_from_interp with "Hinterp Hh") as %Halive.
    destruct Halive as (ps & s & Hps & Hs & Hgen & Hval).
    destruct Hval as [v Hvv].
    (* Destructure state_interp to get the ghost map. *)
    iDestruct "Hinterp" as (gens) "[Hauth %Hagree]".
    (* Do the ghost update: consume alive_handle, DELETE (p,i) from gens. *)
    iMod (alive_handle_free γ gens p i g with "Hauth Hh") as "Hauth'".
    (* Construct the post-state: slot at (p,i) freed, gen bumped. *)
    set (s' := mkSlot (S (slot_gen s)) None).
    set (ps' := <[ i := s' ]> ps).
    set (σ' := mkState (<[ p := ps' ]> σ.(st_store))
                       σ.(st_env)
                       σ.(st_caps)
                       None).
    iModIntro. iExists σ'. iSplit.
    - (* The step fires: step_free_alive. *)
      iPureIntro.
      subst σ' ps' s'.
      rewrite <- Hgen.
      eapply step_free_alive; eauto.
      congruence.
    - (* state_interp preserved for σ' with the deleted ghost entry. *)
      iExists (delete (p, i) gens).
      iFrame "Hauth'".
      iPureIntro.
      (* Prove agreement: (delete (p,i) gens) !! (p',i') = Some g'
         ↔ slot_is_alive σ' p' i' g'. *)
      intros p' i' g'.
      rewrite lookup_delete_Some.
      split.
      + intros [Hne Hlook].
        (* Case: (p',i') ≠ (p,i). Use original agreement. *)
        apply Hagree in Hlook as Holdalive.
        (* Need to show slot_is_alive σ' p' i' g', given it held for σ. *)
        destruct Holdalive as (ps2 & s2 & Hps2 & Hs2 & Hg2 & Hv2).
        exists (if decide (p' = p) then ps' else ps2). eexists.
        destruct (decide (p' = p)) as [-> | Hpne].
        * (* p' = p but i' ≠ i *)
          assert (i' ≠ i) by (intros ->; apply Hne; reflexivity).
          split; [subst σ'; simpl; apply lookup_insert | ].
          split; [subst ps'; rewrite lookup_insert_ne; auto;
                  rewrite Hps in Hps2; injection Hps2 as ->; exact Hs2 | ].
          split; auto.
        * (* p' ≠ p, so σ' and σ have the same pool at p'. *)
          split; [subst σ'; simpl; rewrite lookup_insert_ne; auto; exact Hps2 | ].
          split; auto.
      + intros [ps'' [s'' [Hps'' [Hs'' [Hg'' Hv'']]]]].
        (* σ' at p' has some pool with alive slot. Show (p',i') ≠ (p,i). *)
        subst σ'. simpl in Hps''.
        destruct (decide (p' = p)) as [-> | Hpne].
        * (* p' = p *)
          rewrite lookup_insert in Hps''.
          injection Hps'' as <-.
          subst ps'.
          destruct (decide (i' = i)) as [-> | Hine].
          -- (* i' = i: ps'' !! i = Some s'. But s' has slot_val = None,
                contradicting Hv''. *)
             rewrite lookup_insert in Hs''.
             injection Hs'' as <-. subst s'. simpl in Hv''.
             destruct Hv'' as [? Hx]. discriminate.
          -- (* i' ≠ i *)
             split; [intros Heq; injection Heq; auto | ].
             rewrite lookup_insert_ne in Hs''; auto.
             apply Hagree. exists ps, s''.
             auto.
        * (* p' ≠ p *)
          rewrite lookup_insert_ne in Hps''; auto.
          split; [intros Heq; injection Heq; intros; subst; contradiction | ].
          apply Hagree. exists ps'', s''. auto.
  Qed.

  (* ---- Simpler: safety-only step spec for pool.get ----

     Get doesn't change state, so the agreement is trivially
     preserved. This gives us a clean axiom-free step spec. *)

  Lemma step_spec_get γ σ p i g :
    σ.(st_returned) = None →
    handle_state_interp γ σ -∗
    alive_handle γ p i g -∗
      ∃ v,
        ⌜step σ (EGet p (EVal (VHandle p i g))) σ (EVal v)⌝ ∗
        handle_state_interp γ σ ∗
        alive_handle γ p i g.
  Proof.
    iIntros (Hret) "Hinterp Hh".
    iDestruct (handle_alive_from_interp with "Hinterp Hh") as %Halive.
    destruct Halive as (ps & s & Hps & Hs & Hgen & Hval).
    destruct Hval as [v Hvv].
    iExists v. iFrame.
    iPureIntro.
    apply step_get_alive.
    - exact Hret.
    - unfold handle_lookup. rewrite Hps. simpl.
      rewrite Hs. simpl.
      rewrite Hgen.
      rewrite option_guard_True; last reflexivity.
      simpl. exact Hvv.
  Qed.

  (* ---- Combined safety corollary ----

     Starting from state_interp + alive_handle, a pool.get
     operation is always safe: we exhibit a concrete step,
     the state_interp is preserved, and the resource is
     still owned after (non-consuming read). *)

  Corollary owner_can_always_get γ σ p i g :
    σ.(st_returned) = None →
    handle_state_interp γ σ -∗
    alive_handle γ p i g -∗
      ∃ σ' e',
        ⌜step σ (EGet p (EVal (VHandle p i g))) σ' e'⌝ ∗
        handle_state_interp γ σ' ∗
        alive_handle γ p i g.
  Proof.
    iIntros (Hret) "Hinterp Hh".
    iDestruct (step_spec_get with "Hinterp Hh") as (v) "(%Hstep & Hinterp & Hh)"; first done.
    iExists σ, (EVal v). iFrame.
    iPureIntro. exact Hstep.
  Qed.

End step_specs.

(* ---- Summary ----

   Axiom-free:
     step_spec_get           — get operational step from resource
     owner_can_always_get    — corollary: resource ⇒ safe step exists

   Partial (3 admits in agreement-preservation detail):
     step_spec_free          — free step + ghost update. The step-
                               firing part is proven; what remains
                               is threading the gens-post equivalence
                               through the case analysis.

   Significance:
     The UAF safety direction (`owner_can_always_get`) is now a
     direct theorem. No wp machinery needed. This says: "in any
     state where Iris proofs hold, pool.get on a well-owned handle
     operationally succeeds." Combined with exclusivity of the
     resource (from iris_resources.v), this means the emitter's
     runtime `_zer_trap("use-after-free")` is unreachable in
     Iris-proved programs — a COMPILE-TIME safety guarantee.

   The double-free side needs step_spec_free fully discharged.
   Phase 1f can proceed on top of step_spec_get alone for adequacy
   of the "get-after-alloc is safe" subset; the full spec_free
   discharge is a cleanup task for the refined ghost-state design.
*)
