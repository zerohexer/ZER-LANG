(* ================================================================
   λZER-Handle : Adequacy — Preservation + Progress

   The two workhorse theorems that combine to give type safety:

     Preservation : well-typed terms stay well-typed under reduction.
     Progress     : well-typed non-value terms can step (or reach
                    a designated returned-value state).

   Together they mean: well-typed programs never get stuck. Since
   stuck-at-a-handle-op is what UAF / double-free manifest as in
   our operational model, well-typed-ness implies handle safety.

   **Proof status (as of commit):** statements in Coq, proofs
   `Admitted`. The statements are type-checked — this file compiles
   because the theorems are well-formed, but the proofs themselves
   are the body of Year-1 work. Follow-up commits will discharge
   each admit incrementally.

   This file exists to:
     1. Commit the theorem statements so dependencies are explicit.
     2. Provide the scaffolding that handle_safety.v will use.
     3. Make the proof obligations concrete and inspectable.

   It is *not* a finished soundness proof. Honest labels on every
   admit — no hidden assumptions.
   ================================================================ *)

From stdpp Require Import base strings list gmap.
From zer_handle Require Import syntax semantics typing.

(* ================================================================
   Runtime-environment typing

   The semantic state contains a runtime environment `st_env :
   venv = gmap var val`. The typing context Γ : tctx maps variables
   to types. For preservation to make sense, these must agree:
   every variable bound at runtime has a value of the type the
   static context expects.
   ================================================================ *)

Definition env_typed (E : venv) (Γ : tctx) : Prop :=
  forall x τ, Γ !! x = Some τ -> exists v, E !! x = Some v /\ has_val_ty v τ.

(* ---- Runtime-typing properties ---- *)

Lemma env_typed_insert : forall E Γ x v τ,
  env_typed E Γ ->
  has_val_ty v τ ->
  env_typed (<[ x := v ]> E) (<[ x := τ ]> Γ).
Proof.
  intros E Γ x v τ HE Hvt y τ' Hy.
  apply lookup_insert_Some in Hy as [[-> ->] | [Hne Hy]].
  - exists v. split.
    + apply lookup_insert.
    + exact Hvt.
  - destruct (HE y τ' Hy) as [v' [HEv' Hv't]].
    exists v'. split.
    + apply lookup_insert_Some. right. split; auto.
    + exact Hv't.
Qed.

Lemma env_typed_empty : env_typed empty empty.
Proof.
  intros x τ Hx.
  apply lookup_empty_Some in Hx. contradiction.
Qed.

(* ================================================================
   Preservation

   Statement: if `env_typed E Γ`, `typed Γ e τ`, and
   `step ⟨E, e⟩ ⟨E', e'⟩`, then there exists Γ' extending Γ with
   `env_typed E' Γ'` and `typed Γ' e' τ`.

   The Γ' is necessary because `step_let_val` extends the runtime
   env; the static context must extend in lockstep.

   Proof sketch (to be discharged):
     Case step_var: trivially, env_typed gives us a typed value.
     Case step_let_ctx: induction via IH.
     Case step_let_val: use env_typed_insert.
     Case step_seq_ctx/val: direct.
     Case step_if_*: values are typed, branches already typed.
     Case step_alloc_*: allocation produces a well-typed handle or
       null; both satisfy TyOptHandle.
     Case step_free_*: free returns Unit, which is typed.
     Case step_get_*: get returns an Int (slot value), typed.
     Case step_orelse_*: unwrap gives Handle, return fires leave
       the expression as EVal of the returned value.
     Case step_return_*: return expression reduces to its inner
       value.

   Roughly ~60 sub-cases (one per step_* constructor × congruence
   handling). Doable but tedious. Deferred to follow-up.
   ================================================================ *)

Theorem preservation : forall st e st' e' τ,
  env_typed st.(st_env) empty ->
  typed empty e τ ->
  step st e st' e' ->
  env_typed st'.(st_env) empty /\ typed empty e' τ.
Proof.
  (* TODO(Year-1, Week-2): discharge all ~60 cases of step.
     Each case follows the pattern:
       inversion Hty; subst.    (* decompose typing *)
       inversion Hstep; subst.  (* decompose step    *)
       split; [handle env|handle typed].
     Cases involving ELet/EOrelseReturn need env_typed_insert.
     Cases involving EAlloc/EFree/EGet use vty_* constructors. *)
Admitted.

(* ================================================================
   Progress

   Statement: if `env_typed E Γ`, `typed Γ e τ`, and `e` is not a
   value, and `st_returned = None`, then `e` can step.

   (The st_returned = None side-condition is because once a
   function has returned, no further evaluation is expected.)

   Proof sketch:
     Induction on typing.
       ty_val → e = EVal, contradiction with not-value.
       ty_var → env_typed gives us the value; step_var fires.
       ty_let → IH on e1. If e1 is a value, step_let_val fires.
                Otherwise step_let_ctx via IH.
       ty_seq → similar.
       ty_if  → IH on condition. If value, canonical_bool gives
                step_if_true/false. Otherwise step_if_ctx.
       ty_alloc → unconditional: step_alloc_init, then succ/fail.
       ty_free/ty_get → IH on argument. If value, canonical_handle
                gives the concrete handle, and the alive case
                always fires (this is the handle-safety argument —
                progress breaks without it, and that's what the
                main theorem is about).
       etc.

   ~12 cases. Shorter than preservation but uses preservation's
   canonical-forms lemmas.
   ================================================================ *)

Theorem progress : forall st e τ,
  env_typed st.(st_env) empty ->
  typed empty e τ ->
  st.(st_returned) = None ->
  is_value e = true \/ exists st' e', step st e st' e'.
Proof.
  (* TODO(Year-1, Week-2): induction on typing. *)
Admitted.

(* ================================================================
   Multi-step preservation (corollary)

   For n-step execution, typing is preserved throughout.
   Immediate consequence of preservation + induction on steps.
   ================================================================ *)

Theorem steps_preservation : forall st e st' e' τ,
  env_typed st.(st_env) empty ->
  typed empty e τ ->
  steps st e st' e' ->
  env_typed st'.(st_env) empty /\ typed empty e' τ.
Proof.
  intros st e st' e' τ Henv Hty Hsteps.
  revert τ Henv Hty.
  induction Hsteps; intros τ Henv Hty.
  - split; assumption.
  - eapply preservation in H as [Henv' Hty']; eauto.
Qed.

(* ================================================================
   Safety corollary: no stuck configurations

   If a well-typed program reduces to some state + expression, the
   result is either a value, a state with st_returned = Some _, or
   a term that can still step.

   This rules out "stuck at a handle op" — the operational
   manifestation of UAF / double-free.
   ================================================================ *)

Theorem no_stuck : forall st e st' e' τ,
  env_typed st.(st_env) empty ->
  typed empty e τ ->
  steps st e st' e' ->
  is_value e' = true \/
  (exists v, st'.(st_returned) = Some v) \/
  (exists st'' e'', step st' e' st'' e'').
Proof.
  intros st e st' e' τ Henv Hty Hsteps.
  destruct (steps_preservation _ _ _ _ _ Henv Hty Hsteps) as [Henv' Hty'].
  destruct st'.(st_returned) as [v|] eqn:Hret.
  - right. left. exists v. reflexivity.
  - destruct (progress _ _ _ Henv' Hty' Hret) as [Hval | Hstep].
    + left. exact Hval.
    + right. right. exact Hstep.
Qed.

(* ================================================================
   Proof-status tracking (for Year-1 bookkeeping)

   Lemmas currently Admitted:
     - preservation
     - progress

   Lemmas proven in this file:
     - env_typed_insert
     - env_typed_empty
     - steps_preservation (uses preservation)
     - no_stuck (uses preservation + progress)

   The "proven" ones are correct modulo the Admitted ones. When
   preservation + progress are discharged, the whole file becomes
   axiom-free. Until then, `no_stuck` is a conditional theorem.
   ================================================================ *)
