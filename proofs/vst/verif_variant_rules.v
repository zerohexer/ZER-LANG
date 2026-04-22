(* ================================================================
   Level-3 Phase 1 Batch 4 — verif_variant_rules.v

   Union variant safety predicates verified against typing.v Section P.

   Functions verified:
     zer_union_read_mode_safe(mode)
       → 1 iff mode == ZER_URM_SWITCH (DirectRead rejected)
     zer_union_arm_op_safe(self_access)
       → 1 iff self_access == 0 (no self-mutation in own switch arm)

   Callers: checker.c union field access outside switch context (P01);
   union switch-arm mutation check via check_union_switch_mutation (P02).
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.variant_rules.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

(* ---- Constants matching variant_rules.h ---- *)
Definition ZER_URM_SWITCH : Z := 1.

(* ---- Coq specifications ---- *)

Definition zer_union_read_mode_safe_coq (mode : Z) : Z :=
  if Z.eq_dec mode ZER_URM_SWITCH then 1 else 0.

Definition zer_union_arm_op_safe_coq (self_access : Z) : Z :=
  if Z.eq_dec self_access 0 then 1 else 0.

(* ---- VST funspecs ---- *)

Definition zer_union_read_mode_safe_spec : ident * funspec :=
 DECLARE _zer_union_read_mode_safe
  WITH mode : Z
  PRE [ tint ]
    PROP (Int.min_signed <= mode <= Int.max_signed)
    PARAMS (Vint (Int.repr mode))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_union_read_mode_safe_coq mode)))
    SEP ().

Definition zer_union_arm_op_safe_spec : ident * funspec :=
 DECLARE _zer_union_arm_op_safe
  WITH self_access : Z
  PRE [ tint ]
    PROP (Int.min_signed <= self_access <= Int.max_signed)
    PARAMS (Vint (Int.repr self_access))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_union_arm_op_safe_coq self_access)))
    SEP ().

Definition Gprog : funspecs :=
  [ zer_union_read_mode_safe_spec;
    zer_union_arm_op_safe_spec ].

(* ---- Proofs ---- *)

Lemma body_zer_union_read_mode_safe:
  semax_body Vprog Gprog f_zer_union_read_mode_safe
             zer_union_read_mode_safe_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_union_read_mode_safe_coq, ZER_URM_SWITCH;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_union_arm_op_safe:
  semax_body Vprog Gprog f_zer_union_arm_op_safe
             zer_union_arm_op_safe_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_union_arm_op_safe_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

(* ================================================================
   2 variant-safety predicates VST-verified. Phase 1 Batch 4.
   Total Level-3 verified compiler functions: 59
   (Phase 1: 57 + Batch 4: 2).
   ================================================================ *)
