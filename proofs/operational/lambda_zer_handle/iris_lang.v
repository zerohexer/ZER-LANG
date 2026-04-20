(* ================================================================
   Phase 0b — λZER-Handle as an Iris language instance.

   Bridges our small-step `step` relation (semantics.v) to Iris's
   `language` typeclass. Once this is in place, Iris's weakest-
   precondition machinery (`WP e {{ v, P v }}`) becomes available
   for reasoning about λZER-Handle programs.

   The language typeclass needs:
     - expr / val / state / observation — our types
     - of_val / to_val — embed vals into expressions
     - prim_step — the step relation lifted to Iris's shape
         (adds observation list + forked threads; both empty for us)
     - mixin lemmas — to_of/of_to inverses + values-can't-step

   Sequential-only for now: observation = unit, efs = []. Concurrency
   subsets will extend this.

   What this file does NOT do yet:
     - No Iris resources yet (no `alive_handle p i g : iProp`).
     - No wp specs for alloc/free/get.
     - No adequacy corollary.
   Those are Phase 1 work, built on top of this instance.
   ================================================================ *)

From iris.program_logic Require Import language.
From zer_handle Require Import syntax semantics.

(* ---- Val/expr embedding ---- *)

Definition λZH_of_val (v : val) : expr := EVal v.

Definition λZH_to_val (e : expr) : option val :=
  match e with
  | EVal v => Some v
  | _ => None
  end.

(* ---- prim_step : lift our step relation to Iris shape ----

   Iris's step relation is
     prim_step : expr → state → list observation → expr → state → list expr → Prop
   The observation list models IO effects (we have none for Year-1
   handle safety — `observation := unit` and list is always `[]`).
   The expr list models forked threads from spawn (always `[]`
   until λZER-concurrency). *)

Definition λZH_prim_step (e : expr) (σ : state) (κs : list unit)
                         (e' : expr) (σ' : state) (efs : list expr) : Prop :=
  κs = [] /\ efs = [] /\ step σ e σ' e'.

(* ---- Mixin lemmas ---- *)

Lemma λZH_to_of_val (v : val) : λZH_to_val (λZH_of_val v) = Some v.
Proof. reflexivity. Qed.

Lemma λZH_of_to_val (e : expr) (v : val) :
  λZH_to_val e = Some v -> λZH_of_val v = e.
Proof.
  destruct e; simpl; intro H; try discriminate.
  injection H as ->. reflexivity.
Qed.

Lemma λZH_val_stuck (e : expr) (σ : state) (κs : list unit)
                    (e' : expr) (σ' : state) (efs : list expr) :
  λZH_prim_step e σ κs e' σ' efs -> λZH_to_val e = None.
Proof.
  intros [_ [_ Hstep]].
  (* By inversion on our step relation: no rule has EVal _ on the
     LHS, so a stepping expression cannot be a value. *)
  inversion Hstep; subst; simpl; reflexivity.
Qed.

(* ---- The language instance ----

   `language` is a Structure (record), `Language` its constructor.
   Using the explicit constructor avoids a name collision between
   our `expr`/`val`/`state` types and the structure's field names. *)

Definition λZH_mixin : LanguageMixin λZH_of_val λZH_to_val λZH_prim_step :=
  Build_LanguageMixin _ _ _ λZH_to_of_val λZH_of_to_val λZH_val_stuck.

Canonical Structure λZH_lang : language :=
  Language λZH_mixin.

(* ---- Typeclass instances for Iris wp tactics ----

   `IntoVal e v` lets `wp_value` recognize our `EVal v` as a value
   without requiring explicit unfolding. `AsVal e` is similar but
   exists-quantifies over v (used by other wp tactics). *)

Global Instance into_val_EVal (v : val) : IntoVal (EVal v) v.
Proof. reflexivity. Qed.

Global Instance as_val_EVal (v : val) : AsVal (EVal v).
Proof. exists v. reflexivity. Qed.

(* ---- Sanity check: values are exactly EVal _ ---- *)

Lemma λZH_is_value_iff (e : expr) :
  (exists v, λZH_to_val e = Some v) <-> is_value e = true.
Proof.
  split.
  - intros [v Hv]. destruct e; simpl in *; try discriminate. reflexivity.
  - intros H. destruct e; simpl in *; try discriminate. eexists; reflexivity.
Qed.
