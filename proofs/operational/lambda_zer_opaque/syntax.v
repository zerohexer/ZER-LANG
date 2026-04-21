(* ================================================================
   λZER-Opaque : Syntax — minimal subset for provenance proofs.

   Models ZER's `*opaque` pointer round-tripping. A pointer carries
   its original type as a ghost tag; casting through `*opaque` and
   back to the wrong type is an operational-level error.

   This mirrors zercheck's 4-layer provenance tracking + runtime
   `_zer_opaque { ptr, type_id }` representation.

   Scope (minimal for proofs):
     - type_id : unique identifier per struct type
     - PtrTyped : a pointer with a known type tag
     - EAlloc t : allocate a typed pointer (produces PtrTyped _ t)
     - EOpaqueCast : erase the static type (dynamic tag preserved)
     - ETypedCast target : check tag == target, produce *target
     - ELet / ESeq / EVal / EVar for control flow

   What the operational model enforces:
     - Casting *A → *B directly has NO STEP RULE (banned at type level)
     - Only via *opaque round-trip, and cast-back checks the tag
     - Mismatched tag = STUCK (safety violation)
   ================================================================ *)

From stdpp Require Import base strings list gmap.

(* Unique identifier for each struct type in the program *)
Definition type_id := nat.

(* Pointer values carry an instance id + a type tag.
   The tag is what lets `@ptrcast *opaque -> *T` check safety. *)
Inductive ptr : Type :=
  | PtrNull : ptr
  | PtrTyped : nat (* instance id *) -> type_id -> ptr.

#[global] Instance ptr_eq_dec : EqDecision ptr.
Proof. solve_decision. Defined.

(* Variable names *)
Definition var := string.

(* Values *)
Inductive val : Type :=
  | VUnit : val
  | VInt  : Z -> val
  | VPtr  : ptr -> val.

#[global] Instance val_eq_dec : EqDecision val.
Proof. solve_decision. Defined.

(* Expressions.

   EAlloc t            — produce a fresh PtrTyped _ t
   EOpaqueCast e       — cast *T → *opaque (tag preserved in ghost)
   ETypedCast t e      — cast *opaque → *t (checks tag)
   EDeref e            — dereference, requires typed pointer *)
Inductive expr : Type :=
  | EVal  : val -> expr
  | EVar  : var -> expr
  | EAlloc : type_id -> expr
  | EOpaqueCast : expr -> expr
  | ETypedCast : type_id -> expr -> expr
  | EDeref : expr -> expr
  | ELet : var -> expr -> expr -> expr
  | ESeq : expr -> expr -> expr.
