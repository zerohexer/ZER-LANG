(* ================================================================
   λZER-Typing : Syntax + types for typing-level proofs.

   Covers the typing-enforced rules (sections I, K, N, P, Q, R, S,
   T) that were previously just `True. Qed.` schematic closures.
   This subset provides REAL Coq proofs about the typing relation
   instead of trivial placeholders.

   Scope of types modeled:
     - Primitive: UnitT, BoolT, IntT (width)
     - Qualifiers: const, volatile (Section I)
     - Optional: ?T (Section N)
     - Enum/Bool switches (Section Q)
     - Pool/Ring/Slab container declarations (Section T)
     - Union variants (Section P)

   Scope of expressions:
     - Values and variables
     - Switch on enum/bool/int/float
     - Optional unwrap (if-unwrap, orelse)
     - Pointer casts
     - Pool operations (for T)
   ================================================================ *)

From stdpp Require Import base strings list gmap.

(* ---- Type qualifiers ----

   A pointer/slice type carries (kind, qualifiers) where qualifiers
   is a bitmask encoding const + volatile. ZER's rule: qualifiers
   can be ADDED through coercions, never REMOVED. *)

Record qualifiers : Type := mkQuals {
  is_const : bool;
  is_volatile : bool;
}.

Definition qual_none : qualifiers := mkQuals false false.
Definition qual_const : qualifiers := mkQuals true false.
Definition qual_volatile : qualifiers := mkQuals false true.
Definition qual_const_volatile : qualifiers := mkQuals true true.

#[global] Instance qualifiers_eq_dec : EqDecision qualifiers.
Proof. solve_decision. Defined.

(* Qualifier subtyping: `q1 <:q q2` means q1 is AT LEAST as
   restrictive as q2 (q1 can be used where q2 is expected, but
   not vice versa — UNLESS adding qualifiers).

   Concretely: const → non-const is FORBIDDEN, but non-const →
   const is ALLOWED (you can always add const, never strip it). *)
Definition qual_le (q1 q2 : qualifiers) : bool :=
  (* q1 can be ADDED to yield q2 iff q2 has every qualifier q1 has *)
  (implb q1.(is_const) q2.(is_const)) &&
  (implb q1.(is_volatile) q2.(is_volatile)).

(* Alternative reading: q2 can be coerced FROM q1 iff q2 has
   every qualifier q1 has. So `coercion q1 → q2` is safe iff
   `qual_le q1 q2 = true`. *)

(* ---- Types ---- *)

(* Enum type: an enum with N variants, identified by their indices
   in 0..N-1. (We don't care about the names here — switch
   exhaustiveness only depends on the index count.) *)
Definition enum_type := nat.   (* cardinality *)

Inductive ty : Type :=
  | UnitT   : ty
  | BoolT   : ty
  | IntT    : nat -> ty             (* width in bits *)
  | FloatT  : ty
  | PtrT    : ty -> qualifiers -> ty  (* *T with qualifiers *)
  | OptT    : ty -> ty              (* ?T *)
  | EnumT   : enum_type -> ty       (* enum with N variants *)
  | UnionT  : list ty -> ty.        (* union with these variant types *)

(* No EqDecision for `ty` — the recursive UnionT case would need
   manual induction. The theorems below don't require decidable
   equality on types. *)

(* Switch patterns: either a specific value (bool true/false,
   enum index, int literal) or default. *)
Inductive switch_pat : Type :=
  | PatBoolT   : switch_pat          (* matches true *)
  | PatBoolF   : switch_pat          (* matches false *)
  | PatEnum    : nat -> switch_pat   (* matches enum variant index *)
  | PatInt     : nat -> switch_pat   (* matches int literal *)
  | PatDefault : switch_pat.

#[global] Instance switch_pat_eq_dec : EqDecision switch_pat.
Proof. solve_decision. Defined.

Definition switch_is_default (p : switch_pat) : bool :=
  match p with PatDefault => true | _ => false end.
