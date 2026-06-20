(* ================================================================
   λZER-Concurrency : Iris `language` instance  (DESIGN.md §7 step 1)

   Lifts `cstep` (semantics.v) to Iris's `language` typeclass. Unlike
   the sequential subsets, `prim_step`'s forked-thread list `efs` is
   NON-TRIVIAL: `ESpawn e` produces `efs = [e]`. This is what makes
   Iris's THREADPOOL adequacy (multi-thread, all-schedules) available
   for λZER-Concurrency — the foundation the data-race / cross-thread-
   UAF theorem (DESIGN §0) is built on.

   This file gives ONLY the `language` instance (of_val/to_val/
   prim_step + the 3 mixin lemmas). The `irisGS` state-interpretation
   (heap ghost map) + adequacy corollary are the next file
   (iris_state.v), and the ghost state for the four conditions
   (DESIGN §4) follows that.
   ================================================================ *)

From iris.program_logic Require Import language.
From zer_conc Require Import syntax semantics.

(* ---- Val/expr embedding ---- *)

Definition λZC_of_val (v : val) : expr := EVal v.

Definition λZC_to_val (e : expr) : option val :=
  match e with
  | EVal v => Some v
  | _ => None
  end.

(* ---- prim_step : lift cstep to Iris shape ----

   observation = unit (no IO yet; κs always []). The forked-thread
   list efs is taken directly from cstep — [e] for spawn, [] otherwise. *)

Definition λZC_prim_step (e : expr) (σ : state) (κs : list unit)
                         (e' : expr) (σ' : state) (efs : list expr) : Prop :=
  κs = [] /\ cstep e σ e' σ' efs.

(* ---- Mixin lemmas ---- *)

Lemma λZC_to_of_val (v : val) : λZC_to_val (λZC_of_val v) = Some v.
Proof. reflexivity. Qed.

Lemma λZC_of_to_val (e : expr) (v : val) :
  λZC_to_val e = Some v -> λZC_of_val v = e.
Proof.
  destruct e; simpl; intro H; try discriminate.
  injection H as ->. reflexivity.
Qed.

Lemma λZC_val_stuck (e : expr) (σ : state) (κs : list unit)
                    (e' : expr) (σ' : state) (efs : list expr) :
  λZC_prim_step e σ κs e' σ' efs -> λZC_to_val e = None.
Proof.
  intros [_ Hstep].
  inversion Hstep; subst; simpl; reflexivity.
Qed.

(* ---- The language instance ---- *)

Definition λZC_mixin : LanguageMixin λZC_of_val λZC_to_val λZC_prim_step :=
  Build_LanguageMixin _ _ _ λZC_to_of_val λZC_of_to_val λZC_val_stuck.

Canonical Structure λZC_lang : language :=
  Language λZC_mixin.

(* ---- Typeclass instances for Iris wp tactics ---- *)

Global Instance into_val_EVal (v : val) : IntoVal (EVal v) v.
Proof. reflexivity. Qed.

Global Instance as_val_EVal (v : val) : AsVal (EVal v).
Proof. exists v. reflexivity. Qed.

(* ---- Fork sanity: spawn really does emit one forked thread ---- *)

Lemma λZC_spawn_forks (e : expr) (σ : state) :
  λZC_prim_step (ESpawn e) σ [] (EVal VUnit) σ [e].
Proof. split; [reflexivity|]. apply cs_spawn. Qed.
