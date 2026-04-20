(* ================================================================
   λZER-Handle : Type System

   Defines the type-checking judgement that determines which
   λZER-Handle programs are "well-typed." The safety theorem proves
   that every well-typed program either reduces to a value or
   continues stepping — never getting stuck on a UAF / double-free.

   Types model ZER's handle safety intuition:
     - `TyUnit` — no value (void-like)
     - `TyBool` — boolean
     - `TyInt`  — integer (stored in pool slots)
     - `TyHandle p`     — live Handle(T) for pool p (guaranteed
                          non-null, guaranteed not-yet-freed in a
                          static sense)
     - `TyOptHandle p`  — ?Handle(T) for pool p (might be null)

   The type system enforces:
     - Can't pass ?Handle where Handle is expected (must unwrap)
     - Can't pass Handle from pool A to pool B's free/get
     - Variables must be bound

   What the type system does NOT enforce (that's zercheck's job
   operationally, and will be our safety theorem's job formally):
     - Linearity of handles (use-after-free prevention)
     - Leak freedom

   Those are semantic properties proven via adequacy.v, not
   syntactic properties of typing.
   ================================================================ *)

From stdpp Require Import base strings list gmap.
From zer_handle Require Import syntax semantics.

(* ---- Types ---- *)

Inductive ty : Type :=
  | TyUnit   : ty
  | TyBool   : ty
  | TyInt    : ty
  | TyHandle : pool_id -> ty
  | TyOptHandle : pool_id -> ty.

#[global] Instance ty_eq_dec : EqDecision ty.
Proof. solve_decision. Defined.

(* ---- Typing context ----

   Maps variables to their types. Pool capacities are tracked
   separately in `program.prog_pools` (we need them for typing
   allocation calls — though Year 1's simple model doesn't use
   capacity in typing). *)
Definition tctx := gmap var ty.

(* ---- Value typing ----

   The runtime type of a value. Handles carry their pool_id in
   both the operational `val` and the static `ty`, so value typing
   is straightforward. *)
Inductive has_val_ty : val -> ty -> Prop :=
  | vty_unit : has_val_ty VUnit TyUnit
  | vty_bool : forall b, has_val_ty (VBool b) TyBool
  | vty_int  : forall n, has_val_ty (VInt n) TyInt
  | vty_handle : forall p i g,
      has_val_ty (VHandle p i g) (TyHandle p)
  (* A non-null handle can also be used where an optional is expected. *)
  | vty_handle_opt : forall p i g,
      has_val_ty (VHandle p i g) (TyOptHandle p)
  (* Null only types as optional. *)
  | vty_null_opt : forall p,
      has_val_ty (VNullHandle p) (TyOptHandle p).

(* ---- Expression typing ----

   `typed Γ e τ` : under context Γ, expression e has type τ. *)
Inductive typed : tctx -> expr -> ty -> Prop :=

  (* Values inherit their value type. *)
  | ty_val : forall Γ v τ,
      has_val_ty v τ ->
      typed Γ (EVal v) τ

  (* Variable lookup. *)
  | ty_var : forall Γ x τ,
      Γ !! x = Some τ ->
      typed Γ (EVar x) τ

  (* Let-binding: e1 has type τ1, e2 is typed under Γ extended
     with x : τ1, and yields τ2. *)
  | ty_let : forall Γ x e1 e2 τ1 τ2,
      typed Γ e1 τ1 ->
      typed (<[ x := τ1 ]> Γ) e2 τ2 ->
      typed Γ (ELet x e1 e2) τ2

  (* Sequencing: e1 typed (anything), e2 yields τ2. *)
  | ty_seq : forall Γ e1 e2 τ1 τ2,
      typed Γ e1 τ1 ->
      typed Γ e2 τ2 ->
      typed Γ (ESeq e1 e2) τ2

  (* If-then-else: condition is Bool, arms have same type. *)
  | ty_if : forall Γ c e1 e2 τ,
      typed Γ c TyBool ->
      typed Γ e1 τ ->
      typed Γ e2 τ ->
      typed Γ (EIf c e1 e2) τ

  (* Pool allocation: `pool.alloc()` produces ?Handle(p). *)
  | ty_alloc : forall Γ p,
      typed Γ (EAlloc p) (TyOptHandle p)

  (* Pool free: takes a Handle(p) (non-null), returns Unit.
     Calling `pool.free(?Handle)` is a type error — must unwrap
     first via orelse. *)
  | ty_free : forall Γ p e,
      typed Γ e (TyHandle p) ->
      typed Γ (EFree p e) TyUnit

  (* Pool get: takes Handle(p), returns Int (slot's value).
     (In future extensions with structs, return type will be the
     slot's struct type.) *)
  | ty_get : forall Γ p e,
      typed Γ e (TyHandle p) ->
      typed Γ (EGet p e) TyInt

  (* Field read / write: same simplification as semantics — for
     Year 1, slots are scalar, field access returns the slot's int.
     Added as stubs so the AST's `EFieldRead/Write` forms are
     typable; semantics currently has no rules for them, so
     field-read typed terms can't step until Tier 2 adds them.
     Once added, this typing rule stays. *)
  | ty_field_read : forall Γ p e f,
      typed Γ e (TyHandle p) ->
      typed Γ (EFieldRead p e f) TyInt

  | ty_field_write : forall Γ p e f v,
      typed Γ e (TyHandle p) ->
      typed Γ v TyInt ->
      typed Γ (EFieldWrite p e f v) TyUnit

  (* Orelse return: `e orelse return r`
       - e has type ?Handle(p)
       - r has the return type of the enclosing function — we model
         this by requiring r to have some type, and the whole form
         has type Handle(p) (the unwrapped non-null).
     The "enclosing function's return type" is tracked implicitly:
     when orelse-return fires at runtime, it sets st_returned to r's
     value, and subsequent evaluation can't happen. For typing
     purposes, we require r to have a sensible type (any type is
     allowed since it only shows up as the program's final return). *)
  | ty_orelse : forall Γ p e r τr,
      typed Γ e (TyOptHandle p) ->
      typed Γ r τr ->
      typed Γ (EOrelseReturn e r) (TyHandle p)

  (* While-loop: body yields Unit, whole yields Unit.
     (Operational rules for this in Tier 2.) *)
  | ty_while : forall Γ c body,
      typed Γ c TyBool ->
      typed Γ body TyUnit ->
      typed Γ (EWhile c body) TyUnit

  (* Return: e has any type, return yields that same type as the
     "result of the never-continues tail." *)
  | ty_return : forall Γ e τ,
      typed Γ e τ ->
      typed Γ (EReturn e) τ.

(* ---- Well-typed program ----

   A program is well-typed if its `main` expression is typable
   in the empty context (no pre-bound variables). We intentionally
   don't type the pool declarations — `EAlloc/EFree/EGet` reference
   pools by ID, and typing just trusts those references. A later
   extension can add "pool exists in program" as a side condition.
   For Year 1 safety, the operational semantics will get stuck on
   references to non-existent pools anyway, which the theorem rules
   out for well-typed programs only when we add the side condition.

   The simpler approach: define `well_typed` to additionally require
   the context to know every referenced pool, and just have the
   safety theorem thread that through. *)

Definition well_typed (p : program) : Prop :=
  exists τ, typed empty (prog_main p) τ.

(* ---- Canonical forms lemmas ----

   Progress proofs need to know that a value of a given type has
   one of a few specific shapes. E.g., a value of type TyBool is
   VBool true or VBool false. These lemmas are small but critical
   for the progress proof in adequacy.v. *)

Lemma canonical_bool : forall v,
  has_val_ty v TyBool ->
  exists b, v = VBool b.
Proof.
  intros v Hty. inversion Hty. eexists; reflexivity.
Qed.

Lemma canonical_int : forall v,
  has_val_ty v TyInt ->
  exists n, v = VInt n.
Proof.
  intros v Hty. inversion Hty. eexists; reflexivity.
Qed.

Lemma canonical_unit : forall v,
  has_val_ty v TyUnit ->
  v = VUnit.
Proof.
  intros v Hty. inversion Hty. reflexivity.
Qed.

Lemma canonical_handle : forall v p,
  has_val_ty v (TyHandle p) ->
  exists i g, v = VHandle p i g.
Proof.
  intros v p Hty. inversion Hty; subst. eexists; eexists; reflexivity.
Qed.

Lemma canonical_opt_handle : forall v p,
  has_val_ty v (TyOptHandle p) ->
  (exists i g, v = VHandle p i g) \/ v = VNullHandle p.
Proof.
  intros v p Hty. inversion Hty; subst.
  - left. eexists; eexists; reflexivity.
  - right. reflexivity.
Qed.

(* ---- Weakening (small, helpful for progress proof) ----

   If Γ ⊢ e : τ and Γ' extends Γ, then Γ' ⊢ e : τ.
   This is the expected structural property — we don't need full
   exchange/contraction because typing ignores irrelevant bindings. *)

Definition extends (Γ Γ' : tctx) : Prop :=
  forall x τ, Γ !! x = Some τ -> Γ' !! x = Some τ.

(* Helper: extending a context with the same binding preserves `extends`. *)
Lemma extends_insert : forall Γ Γ' x τ,
  extends Γ Γ' ->
  extends (<[ x := τ ]> Γ) (<[ x := τ ]> Γ').
Proof.
  intros Γ Γ' x τ Hext y τ' Hy.
  apply lookup_insert_Some in Hy as [[-> ->] | [Hne Hy]].
  - apply lookup_insert.
  - apply lookup_insert_Some. right. split; [exact Hne|]. apply Hext. exact Hy.
Qed.

Lemma weakening : forall Γ Γ' e τ,
  typed Γ e τ ->
  extends Γ Γ' ->
  typed Γ' e τ.
Proof.
  intros Γ Γ' e τ Hty.
  revert Γ'.
  induction Hty; intros Γ' Hext.
  - constructor; assumption.
  - constructor. apply Hext. assumption.
  - econstructor.
    + apply IHHty1. assumption.
    + apply IHHty2. apply extends_insert. assumption.
  - econstructor; auto.
  - constructor; auto.
  - constructor.
  - constructor; auto.
  - constructor; auto.
  - constructor; auto.
  - econstructor; auto.
  - econstructor; eauto.
  - constructor; auto.
  - constructor; auto.
Qed.

(* ================================================================
   What's next:

   - adequacy.v : preservation + progress.
     Preservation: if Γ ⊢ e : τ and step (st, e) (st', e'), then
     Γ ⊢ e' : τ. Typing is preserved across reduction.
     Progress:     if Γ ⊢ e : τ and e is not a value, then e can
     step (unless we're in the "returned" sub-state).

   - handle_safety.v : the main theorem.
     For any well-typed program, any reachable state satisfies
     the handle-safety invariants (no UAF, no double-free, no
     leak at termination).
   ================================================================ *)
