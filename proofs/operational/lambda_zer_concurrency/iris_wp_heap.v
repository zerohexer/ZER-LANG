(* ================================================================
   λZER-Concurrency : WP heap specs  (DESIGN.md §7 step 3b — the WP
   atomic lifting, the genuinely hard part)

   The FIRST real WP triple in the ZER proofs (the handle/escape/etc.
   subsets stay at the resource/step-spec level and never build WP).
   Concurrency needs it because the shared-invariant access yields
   `▷ contents` that only a program STEP can strip — and the WP atomic
   rule is what provides that step.

   Keystone: the LOCAL load spec, via `wp_lift_atomic_step_fupd`.
   "Local" = the caller exclusively owns `l ↦ v` (no invariant), so
   there is no ▷ obstacle — this isolates and solves the WP-lifting
   mechanics for the flat λZC_lang. The SHARED (invariant-guarded)
   load/store reuse this with `iInv`; the store spec mirrors load with
   a heap update.

   The head-step inversion is factored into the pure lemma `load_inv`
   so the WP proof doesn't depend on inversion's auto-generated names.
   ================================================================ *)

From iris.program_logic Require Import weakestpre lifting.
From iris.base_logic.lib Require Import gen_heap invariants.
From iris.proofmode Require Import proofmode.
From zer_conc Require Import syntax semantics iris_lang iris_state.

(* Pure inversion for a load head-step: the only rule that fires on
   `ELoad (EVal (VLoc l))` is cs_load (cs_load_ctx needs the value to
   step, impossible). Isolates inversion naming from the WP proof. *)
Lemma load_inv l σ e2 σ2 efs :
  cstep (ELoad (EVal (VLoc l))) σ e2 σ2 efs →
  ∃ w, σ !! l = Some w ∧ e2 = EVal w ∧ σ2 = σ ∧ efs = [].
Proof.
  intros H. inversion H; subst.
  - eauto.
  - match goal with
    | Hc : cstep (EVal _) _ _ _ _ |- _ => apply cstep_not_val in Hc; discriminate
    end.
Qed.

Section wp_heap.
  Context `{!concGS Σ}.

  Local Notation "l '↦' v" := (pointsto l (DfracOwn 1) v)
    (at level 20, format "l  '↦'  v") : bi_scope.

  (* to_val on a value, by computation — reduces the atomic-step
     postcondition `from_option Φ False (to_val (EVal v))` to `Φ v`
     (simpl alone won't reduce the language projection). *)
  Lemma to_val_EVal (v : val) : to_val (EVal v) = Some v.
  Proof. reflexivity. Qed.

  (* LOCAL LOAD: owning `l ↦ v` exclusively, a load returns v and keeps
     the points-to. Exclusivity (local_excl) means no other thread can
     concurrently access l — the race-free path for thread-private data.
     This is the first WP triple proven for λZC_lang: the WP atomic
     lifting works for our flat (non-ectx) language. *)
  Lemma wp_load l v :
    l ↦ v -∗ WP (ELoad (EVal (VLoc l))) {{ w, ⌜w = v⌝ ∗ l ↦ v }}.
  Proof.
    iIntros "Hl".
    iApply wp_lift_atomic_step_fupd; first done.
    iIntros (σ1 ns κ κs nt) "Hσ".
    iDestruct (gen_heap_valid with "Hσ Hl") as %Hlook.
    iModIntro. iSplitR.
    { (* reducible: cs_load fires *)
      iPureIntro. exists [], (EVal v), σ1, []. split; [reflexivity|]. by apply cs_load. }
    iIntros (e2 σ2 efs Hps) "_".
    destruct Hps as [_ Hcs].
    apply load_inv in Hcs as (w & Hw & -> & -> & ->).
    rewrite Hw in Hlook. injection Hlook as ->.
    iModIntro. iNext. iModIntro.
    iFrame "Hσ". rewrite to_val_EVal /=.
    iSplitL "Hl"; last done.
    iSplitR "Hl"; [ iPureIntro; reflexivity | iFrame "Hl" ].
  Qed.

End wp_heap.
