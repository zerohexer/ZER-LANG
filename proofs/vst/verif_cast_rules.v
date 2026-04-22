(* ================================================================
   Level-3 Phase 1 Batch 7 — verif_cast_rules.v

   Cast intrinsic shape predicates verified against typing.v J-extended.

   Functions verified:
     zer_conversion_safe(kind)                    — J02/J03
     zer_bitcast_width_valid(src, dst)            — J05
     zer_bitcast_operand_valid(is_primitive)      — J06
     zer_cast_distinct_valid(src_d, dst_d)        — J07
     zer_saturate_operand_valid(is_numeric)       — J08
     zer_ptrtoint_source_valid(is_pointer)        — J09
     zer_cast_types_compatible(src_tag, dst_tag)  — J10

   This batch completes the STRICT Phase 1 milestone (71/85).
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.cast_rules.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

(* Constants from cast_rules.h *)
Definition ZER_CONV_CSTYLE : Z := 2.

(* ---- Coq specifications ---- *)

Definition zer_conversion_safe_coq (kind : Z) : Z :=
  if Z.eq_dec kind ZER_CONV_CSTYLE then 0 else 1.

Definition zer_bitcast_width_valid_coq (src dst : Z) : Z :=
  if Z.eq_dec src dst then 1 else 0.

Definition zer_bitcast_operand_valid_coq (is_primitive : Z) : Z :=
  if Z.eq_dec is_primitive 0 then 0 else 1.

Definition zer_cast_distinct_valid_coq (src_d dst_d : Z) : Z :=
  if Z.eq_dec src_d 0 then
    (if Z.eq_dec dst_d 0 then 0 else 1)
  else 1.

Definition zer_saturate_operand_valid_coq (is_numeric : Z) : Z :=
  if Z.eq_dec is_numeric 0 then 0 else 1.

Definition zer_ptrtoint_source_valid_coq (is_pointer : Z) : Z :=
  if Z.eq_dec is_pointer 0 then 0 else 1.

Definition zer_cast_types_compatible_coq (src_tag dst_tag : Z) : Z :=
  if Z.eq_dec src_tag dst_tag then 1 else 0.

(* ---- VST funspecs ---- *)

Definition zer_conversion_safe_spec : ident * funspec :=
 DECLARE _zer_conversion_safe
  WITH kind : Z
  PRE [ tint ]
    PROP (Int.min_signed <= kind <= Int.max_signed)
    PARAMS (Vint (Int.repr kind)) SEP ()
  POST [ tint ]
    PROP () RETURN (Vint (Int.repr (zer_conversion_safe_coq kind))) SEP ().

Definition zer_bitcast_width_valid_spec : ident * funspec :=
 DECLARE _zer_bitcast_width_valid
  WITH src : Z, dst : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= src <= Int.max_signed;
          Int.min_signed <= dst <= Int.max_signed)
    PARAMS (Vint (Int.repr src); Vint (Int.repr dst)) SEP ()
  POST [ tint ]
    PROP () RETURN (Vint (Int.repr (zer_bitcast_width_valid_coq src dst))) SEP ().

Definition zer_bitcast_operand_valid_spec : ident * funspec :=
 DECLARE _zer_bitcast_operand_valid
  WITH is_primitive : Z
  PRE [ tint ]
    PROP (Int.min_signed <= is_primitive <= Int.max_signed)
    PARAMS (Vint (Int.repr is_primitive)) SEP ()
  POST [ tint ]
    PROP () RETURN (Vint (Int.repr (zer_bitcast_operand_valid_coq is_primitive))) SEP ().

Definition zer_cast_distinct_valid_spec : ident * funspec :=
 DECLARE _zer_cast_distinct_valid
  WITH src_d : Z, dst_d : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= src_d <= Int.max_signed;
          Int.min_signed <= dst_d <= Int.max_signed)
    PARAMS (Vint (Int.repr src_d); Vint (Int.repr dst_d)) SEP ()
  POST [ tint ]
    PROP () RETURN (Vint (Int.repr (zer_cast_distinct_valid_coq src_d dst_d))) SEP ().

Definition zer_saturate_operand_valid_spec : ident * funspec :=
 DECLARE _zer_saturate_operand_valid
  WITH is_numeric : Z
  PRE [ tint ]
    PROP (Int.min_signed <= is_numeric <= Int.max_signed)
    PARAMS (Vint (Int.repr is_numeric)) SEP ()
  POST [ tint ]
    PROP () RETURN (Vint (Int.repr (zer_saturate_operand_valid_coq is_numeric))) SEP ().

Definition zer_ptrtoint_source_valid_spec : ident * funspec :=
 DECLARE _zer_ptrtoint_source_valid
  WITH is_pointer : Z
  PRE [ tint ]
    PROP (Int.min_signed <= is_pointer <= Int.max_signed)
    PARAMS (Vint (Int.repr is_pointer)) SEP ()
  POST [ tint ]
    PROP () RETURN (Vint (Int.repr (zer_ptrtoint_source_valid_coq is_pointer))) SEP ().

Definition zer_cast_types_compatible_spec : ident * funspec :=
 DECLARE _zer_cast_types_compatible
  WITH src_tag : Z, dst_tag : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= src_tag <= Int.max_signed;
          Int.min_signed <= dst_tag <= Int.max_signed)
    PARAMS (Vint (Int.repr src_tag); Vint (Int.repr dst_tag)) SEP ()
  POST [ tint ]
    PROP () RETURN (Vint (Int.repr (zer_cast_types_compatible_coq src_tag dst_tag))) SEP ().

Definition Gprog : funspecs :=
  [ zer_conversion_safe_spec;
    zer_bitcast_width_valid_spec;
    zer_bitcast_operand_valid_spec;
    zer_cast_distinct_valid_spec;
    zer_saturate_operand_valid_spec;
    zer_ptrtoint_source_valid_spec;
    zer_cast_types_compatible_spec ].

(* ---- Proofs (all use Pattern C — flat cascade with Z.eq_dec) ---- *)

Lemma body_zer_conversion_safe:
  semax_body Vprog Gprog f_zer_conversion_safe zer_conversion_safe_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_conversion_safe_coq, ZER_CONV_CSTYLE;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_bitcast_width_valid:
  semax_body Vprog Gprog f_zer_bitcast_width_valid zer_bitcast_width_valid_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_bitcast_width_valid_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_bitcast_operand_valid:
  semax_body Vprog Gprog f_zer_bitcast_operand_valid zer_bitcast_operand_valid_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_bitcast_operand_valid_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_cast_distinct_valid:
  semax_body Vprog Gprog f_zer_cast_distinct_valid zer_cast_distinct_valid_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_cast_distinct_valid_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_saturate_operand_valid:
  semax_body Vprog Gprog f_zer_saturate_operand_valid zer_saturate_operand_valid_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_saturate_operand_valid_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_ptrtoint_source_valid:
  semax_body Vprog Gprog f_zer_ptrtoint_source_valid zer_ptrtoint_source_valid_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_ptrtoint_source_valid_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_cast_types_compatible:
  semax_body Vprog Gprog f_zer_cast_types_compatible zer_cast_types_compatible_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_cast_types_compatible_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

(* ================================================================
   7 cast predicates VST-verified. Phase 1 Batch 7.
   Total: 71 verified compiler functions.
   STRICT PHASE 1 MILESTONE COMPLETE.
   ================================================================ *)
