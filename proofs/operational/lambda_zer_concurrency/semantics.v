(* ================================================================
   λZER-Concurrency : Small-step semantics with FORK  (DESIGN.md §7 step 1)

   The step relation `cstep e σ e' σ' efs` carries a list of forked
   threads `efs` (Iris's threadpool shape). Every rule has efs = []
   except `cs_spawn`, which forks: `ESpawn e` steps to unit and emits
   [e] as a new thread. Congruence rules thread `efs` through, so a
   spawn nested in an evaluation position still forks correctly
   (e.g. `let x = spawn body in e2`).

   State is JUST the shared heap `gmap loc val` (no variable env — see
   syntax.v: substitution-based). This is the substrate that §4.2's
   shared invariant will guard; here load/store are unguarded (the
   discipline is added by the ghost state in a later file, not the
   bare semantics).
   ================================================================ *)

From stdpp Require Import base strings list gmap.
From zer_conc Require Import syntax.

(* ---- Program state: the shared heap ---- *)

Definition state := gmap loc val.

(* ---- Step relation with fork output ----

   `cstep e σ e' σ' efs` : expression `e` in heap `σ` steps to `e'` in
   heap `σ'`, forking the threads in `efs`. A stuck configuration (no
   step, non-value) is a runtime error; the safety theorem (later file)
   rules out the data-race / UAF stuck states for typed programs. *)

Inductive cstep : expr -> state -> expr -> state -> list expr -> Prop :=

  (* -- head reductions (efs = [] except spawn) -- *)
  | cs_let : forall x v e2 σ,
      cstep (ELet x (EVal v) e2) σ (subst x v e2) σ []
  | cs_seq : forall v e2 σ,
      cstep (ESeq (EVal v) e2) σ e2 σ []
  | cs_if_true : forall e1 e2 σ,
      cstep (EIf (EVal (VBool true)) e1 e2) σ e1 σ []
  | cs_if_false : forall e1 e2 σ,
      cstep (EIf (EVal (VBool false)) e1 e2) σ e2 σ []
  | cs_alloc : forall v σ l,
      σ !! l = None ->
      cstep (EAlloc (EVal v)) σ (EVal (VLoc l)) (<[ l := v ]> σ) []
  | cs_load : forall l v σ,
      σ !! l = Some v ->
      cstep (ELoad (EVal (VLoc l))) σ (EVal v) σ []
  | cs_store : forall l v w σ,
      σ !! l = Some w ->
      cstep (EStore (EVal (VLoc l)) (EVal v)) σ (EVal VUnit) (<[ l := v ]> σ) []
  | cs_spawn : forall e σ,
      cstep (ESpawn e) σ (EVal VUnit) σ [e]

  (* -- congruence (efs threaded from the sub-step) -- *)
  | cs_let_ctx : forall x e1 e1' e2 σ σ' efs,
      cstep e1 σ e1' σ' efs ->
      cstep (ELet x e1 e2) σ (ELet x e1' e2) σ' efs
  | cs_seq_ctx : forall e1 e1' e2 σ σ' efs,
      cstep e1 σ e1' σ' efs ->
      cstep (ESeq e1 e2) σ (ESeq e1' e2) σ' efs
  | cs_if_ctx : forall c c' e1 e2 σ σ' efs,
      cstep c σ c' σ' efs ->
      cstep (EIf c e1 e2) σ (EIf c' e1 e2) σ' efs
  | cs_alloc_ctx : forall e e' σ σ' efs,
      cstep e σ e' σ' efs ->
      cstep (EAlloc e) σ (EAlloc e') σ' efs
  | cs_load_ctx : forall e e' σ σ' efs,
      cstep e σ e' σ' efs ->
      cstep (ELoad e) σ (ELoad e') σ' efs
  | cs_store_ctx1 : forall e1 e1' e2 σ σ' efs,
      cstep e1 σ e1' σ' efs ->
      cstep (EStore e1 e2) σ (EStore e1' e2) σ' efs
  | cs_store_ctx2 : forall v e2 e2' σ σ' efs,
      cstep e2 σ e2' σ' efs ->
      cstep (EStore (EVal v) e2) σ (EStore (EVal v) e2') σ' efs.

(* ---- Multi-step (single-thread; the threadpool relation comes from
   Iris's adequacy machinery once this is a `language` instance) ---- *)

Inductive csteps : expr -> state -> expr -> state -> Prop :=
  | csteps_refl : forall e σ, csteps e σ e σ
  | csteps_trans : forall e σ e' σ' e'' σ'' efs,
      cstep e σ e' σ' efs ->
      csteps e' σ' e'' σ'' ->
      csteps e σ e'' σ''.

(* ---- A stepping expression is never a value ---- *)

Lemma cstep_not_val e σ e' σ' efs :
  cstep e σ e' σ' efs -> is_value e = false.
Proof. intros Hstep. inversion Hstep; subst; reflexivity. Qed.
