(* ================================================================
   λZER-Escape : Syntax — region tags for dangling-pointer safety.

   Models ZER's compile-time escape analysis: pointers carry a
   region tag (local/arena/static) that determines where they can
   be stored or returned. A local pointer cannot be stored in a
   global slot or returned, because the local memory dies at
   scope exit.

   Scope:
     - Region : enum { RegLocal | RegArena | RegStatic }
     - Pointer values carry an instance id + region tag
     - EAllocLocal / EAllocArena / EAllocStatic — produce region-tagged ptrs
     - EStoreGlobal — tries to store a ptr into a global slot
     - EReturn — tries to return a ptr from a function
     - Let / Seq for control flow
   ================================================================ *)

From stdpp Require Import base strings list gmap.

Definition var := string.

(* Where a pointer was born. *)
Inductive region : Type :=
  | RegLocal  : region     (* stack-allocated, dies at scope exit *)
  | RegArena  : region     (* arena-allocated, dies at arena reset *)
  | RegStatic : region.    (* global/static, lives forever *)

#[global] Instance region_eq_dec : EqDecision region.
Proof. solve_decision. Defined.

Inductive ptr : Type :=
  | PtrNull   : ptr
  | PtrTagged : nat -> region -> ptr.

#[global] Instance ptr_eq_dec : EqDecision ptr.
Proof. solve_decision. Defined.

Inductive val : Type :=
  | VUnit : val
  | VPtr  : ptr -> val.

#[global] Instance val_eq_dec : EqDecision val.
Proof. solve_decision. Defined.

(* Expressions.

   EAllocLocal / EAllocArena / EAllocStatic
     — produce a fresh pointer tagged with the corresponding region.

   EStoreGlobal e
     — take a pointer value (the result of e) and try to store it
       in a global slot. Only RegStatic pointers may succeed;
       RegLocal / RegArena pointers have NO STEP RULE → stuck.

   EReturn e
     — take a pointer value (the result of e) and try to return
       it from the current function. Same rule: only RegStatic
       allowed (RegArena pointers might also survive if the arena
       outlives the caller, but for this minimal model we stick
       with static-only returns). *)
Inductive expr : Type :=
  | EVal   : val -> expr
  | EVar   : var -> expr
  | EAllocLocal  : expr
  | EAllocArena  : expr
  | EAllocStatic : expr
  | EStoreGlobal : expr -> expr
  | EReturn      : expr -> expr
  | ELet   : var -> expr -> expr -> expr
  | ESeq   : expr -> expr -> expr.
