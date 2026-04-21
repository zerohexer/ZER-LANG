(* ================================================================
   λZER-MMIO : Syntax — MMIO region declarations + @inttoptr safety.

   Models ZER's "mmio start..end;" declarations + the @inttoptr
   intrinsic. Any @inttoptr must target an address within a
   declared MMIO range AND be properly aligned for the target
   type.

   Scope:
     - addr : nat (address)
     - align : nat (alignment requirement for the target type)
     - MMIO range: pair (start, end)
     - EInttoPtr align addr — tries to convert an integer to a
       typed pointer. Only fires when:
         (a) addr is within a declared MMIO range
         (b) addr mod align = 0 (alignment check)
       Otherwise: STUCK.
   ================================================================ *)

From stdpp Require Import base strings list gmap.

Definition var := string.
Definition addr := nat.
Definition align := nat.

Inductive val : Type :=
  | VUnit : val
  | VAddr : addr -> val     (* raw integer address *)
  | VMMIOPtr : addr -> align -> val.  (* validated MMIO pointer *)

Inductive expr : Type :=
  | EVal : val -> expr
  | EVar : var -> expr
  | EInttoPtr : align -> expr -> expr
     (* try to convert integer addr (from sub-expr) to typed ptr
        with given alignment requirement *)
  | ELet : var -> expr -> expr -> expr
  | ESeq : expr -> expr -> expr.
