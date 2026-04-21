(* ================================================================
   λZER-Move : Syntax — minimal subset for move-struct verification.

   Operationalizes ZER's `move struct` feature. A move-struct
   instance is a value with a unique identity; passing or assigning
   it to another variable TRANSFERS ownership. The source becomes
   invalid (HS_TRANSFERRED in zercheck terms).

   This is the linear-types story. Models:
     - RustBelt's treatment of `move` semantics
     - C++'s std::move (but enforced by the type system)
     - ZER's `move struct Name { ... }`

   Scope — minimal language for proofs:
     - MoveId : unique identifier for each move-struct instance
     - VMove : value form (a move-struct handle)
     - EAllocMove : produces a fresh move-struct
     - EConsume : takes ownership (simulates fn call that frees)
     - EDrop : ends the move-struct's life (cleanup)
     - Let + Seq + Val for basic control flow

   No arithmetic, no Pools, no concurrency — just move-struct
   linearity.
   ================================================================ *)

From stdpp Require Import base strings list gmap.

(* ---- Move-struct identifier ----

   Unique nat per move-struct instance. Allocation is fresh: each
   EAllocMove produces a new MoveId never used before. *)
Definition move_id := nat.

(* ---- Variable names ---- *)
Definition var := string.

(* ---- Values ---- *)
Inductive val : Type :=
  | VUnit : val
  | VMove : move_id -> val.   (* a move-struct handle *)

(* ---- Expressions ---- *)
Inductive expr : Type :=
  | EVal  : val -> expr
  | EVar  : var -> expr
  | EAllocMove : expr                     (* produce a fresh move-struct *)
  | EConsume   : expr -> expr             (* consume arg, return unit *)
  | EDrop      : expr -> expr             (* like consume, distinct form *)
  | ELet   : var -> expr -> expr -> expr
  | ESeq   : expr -> expr -> expr.

#[global] Instance val_eq_dec : EqDecision val.
Proof. solve_decision. Defined.
