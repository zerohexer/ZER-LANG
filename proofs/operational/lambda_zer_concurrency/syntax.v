(* ================================================================
   λZER-Concurrency : Syntax  (DESIGN.md §7 step 1)

   The FIRST ZER subset with real Iris threadpool semantics. All prior
   subsets (handle/move/escape/mmio/opaque) are sequential single-step;
   here `ESpawn e` forks a thread (prim_step returns efs = [e]).

   SUBSTITUTION-BASED (not environment-based like λZER-Handle): each
   forked thread is a closed term, and the ONLY shared mutable state is
   the heap. A global variable environment would wrongly share bindings
   across threads, so we substitute let-bound values directly.

   Minimal step-1 language (just enough to plumb fork + a shared heap):
     - VUnit / VBool / VInt / VLoc  values
     - let / seq / if control flow
     - alloc / load / store  (the shared heap — the carrier substrate
       that §4.2's shared invariant will guard in later files)
     - spawn  (the fork)

   Deferred to later files (DESIGN.md §4): the share tag (reach), the
   shared invariant (discipline), the region tag + linear join token
   (lifetime), the opaque-boundary capability (visibility). This file
   carries NO safety content — it is the bare language the ghost state
   is layered onto.
   ================================================================ *)

From stdpp Require Import base strings list gmap.

(* ---- Identifiers ---- *)

Definition var := string.
Definition loc := nat.

(* ---- Values ---- *)

Inductive val : Type :=
  | VUnit : val
  | VBool : bool -> val
  | VInt  : Z -> val
  | VLoc  : loc -> val.        (* heap location — the shared substrate *)

(* ---- Expressions ---- *)

Inductive expr : Type :=
  | EVal   : val -> expr
  | EVar   : var -> expr
  | ELet   : var -> expr -> expr -> expr      (* let x = e1 in e2 *)
  | ESeq   : expr -> expr -> expr             (* e1 ; e2 *)
  | EIf    : expr -> expr -> expr -> expr
  | EAlloc : expr -> expr                     (* alloc cell holding e's value *)
  | ELoad  : expr -> expr                     (* *e *)
  | EStore : expr -> expr -> expr             (* e1 := e2 *)
  | ESpawn : expr -> expr.                    (* fork a thread evaluating e *)

#[global] Instance val_eq_dec : EqDecision val.
Proof. solve_decision. Defined.

#[global] Instance expr_eq_dec : EqDecision expr.
Proof. solve_decision. Defined.

(* ---- Substitution ----

   Values are closed (no expr inside a val), so plain (non-capture-
   avoiding) substitution is sound: replace free occurrences of `x`
   with `v`, stopping under a binder that rebinds `x` (ELet y with
   y = x). *)

Fixpoint subst (x : var) (v : val) (e : expr) : expr :=
  match e with
  | EVal w => EVal w
  | EVar y => if decide (x = y) then EVal v else EVar y
  | ELet y e1 e2 =>
      ELet y (subst x v e1) (if decide (x = y) then e2 else subst x v e2)
  | ESeq e1 e2 => ESeq (subst x v e1) (subst x v e2)
  | EIf c e1 e2 => EIf (subst x v c) (subst x v e1) (subst x v e2)
  | EAlloc e1 => EAlloc (subst x v e1)
  | ELoad e1 => ELoad (subst x v e1)
  | EStore e1 e2 => EStore (subst x v e1) (subst x v e2)
  | ESpawn e1 => ESpawn (subst x v e1)
  end.

(* ---- Values predicate ---- *)

Definition is_value (e : expr) : bool :=
  match e with EVal _ => true | _ => false end.
