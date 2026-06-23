(* ================================================================
   lambda_zer_volatile : the OPERATIONAL ORACLE for the volatile-EFFECT class.

   Volatile has TWO safety obligations. (1) STRIP-prevention — you cannot drop
   the `volatile` qualifier through a cast — is certified by the qualifier
   oracle (lambda_zer_qualifier, the `qvol` bit). (2) EFFECT-preservation — a
   volatile access is a hardware-visible side effect and must NOT be elided,
   reordered away, or duplicated by the compiler — is THIS file's concern and
   has no analog in the qualifier order.

   THE FINITE STATE SET for the effect class: an access is one of {plain,
   volatile-read, volatile-write}; equivalently a 1-bit `is_vol` per access plus
   its kind. The certified rule: the "elide this access" optimization is sound
   IFF the access is NOT volatile — a volatile access's effect is mandatory.

   Theorems: (T1) eliding is licensed only for plain accesses; (T2) a volatile
   access is never elided (its effect is preserved) — no under-rejection of the
   hardware contract; (T3) count-preservation: the number of volatile effects in
   a sequence is invariant under eliding plain accesses. Self-contained, zero
   admits.
   ================================================================ *)
From Coq Require Import List.
Import ListNotations.

(* ---- ABSTRACT DOMAIN: an access is plain or a volatile read/write. ---- *)
Inductive access := Plain | VolRead | VolWrite.

Definition is_volatile (a : access) : bool :=
  match a with Plain => false | VolRead | VolWrite => true end.

(* ---- THE DECISION: the compiler may elide an access iff it is NOT volatile. ---- *)
Definition elide_access_ok (a : access) : bool := negb (is_volatile a).

(* ================================================================
   (T1) DECISION SOUNDNESS — eliding is licensed ONLY for plain accesses. *)
Theorem elide_only_plain : forall a,
  elide_access_ok a = true -> is_volatile a = false.
Proof. intros [] H; simpl in *; (reflexivity || discriminate). Qed.

(* ================================================================
   (T2) EFFECT PRESERVED — a volatile access is NEVER elided (the hardware
   contract is honoured). No under-rejection of an observable effect. *)
Theorem volatile_never_elided : forall a,
  is_volatile a = true -> elide_access_ok a = false.
Proof. intros [] H; simpl in *; (reflexivity || discriminate). Qed.

(* ---- effect model: count the volatile effects in an access sequence. ---- *)
Fixpoint vol_count (s : list access) : nat :=
  match s with
  | [] => 0
  | a :: t => (if is_volatile a then 1 else 0) + vol_count t
  end.

(* an optimization that elides one plain access at the head. *)
Definition elide_head (s : list access) : list access :=
  match s with
  | a :: t => if elide_access_ok a then t else s
  | [] => []
  end.

(* ================================================================
   (T3) COUNT PRESERVATION — eliding a (necessarily plain) head access leaves
   the volatile-effect count unchanged: the observable hardware behaviour is
   invariant under the only optimization the decision permits. *)
Theorem elide_head_preserves_vol_count : forall s,
  vol_count (elide_head s) = vol_count s.
Proof.
  intros [| a t]; simpl.
  - reflexivity.
  - destruct a; simpl; reflexivity.
Qed.
