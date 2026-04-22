(* ================================================================
   Level-3 Phase 1 Batch 6 — verif_comptime_rules.v

   Comptime-soundness predicates verified against typing.v Section R.

   Functions verified:
     zer_comptime_arg_valid(is_constant)         — R02
     zer_static_assert_holds(is_constant, value) — R04
     zer_comptime_ops_valid(ops_count)           — R06
     zer_expr_nesting_valid(depth)               — R07
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.comptime_rules.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

(* ---- Constants ---- *)
Definition ZER_COMPTIME_OPS_BUDGET : Z := 1000000.
Definition ZER_EXPR_NESTING_LIMIT  : Z := 1000.

(* ---- Coq specifications ---- *)

Definition zer_comptime_arg_valid_coq (is_constant : Z) : Z :=
  if Z.eq_dec is_constant 0 then 0 else 1.

Definition zer_static_assert_holds_coq (is_constant value : Z) : Z :=
  if Z.eq_dec is_constant 0 then 0
  else if Z.eq_dec value 0 then 0
  else 1.

Definition zer_comptime_ops_valid_coq (ops_count : Z) : Z :=
  if Z_ge_dec ops_count ZER_COMPTIME_OPS_BUDGET then 0 else 1.

Definition zer_expr_nesting_valid_coq (depth : Z) : Z :=
  if Z_ge_dec depth ZER_EXPR_NESTING_LIMIT then 0 else 1.

(* ---- VST funspecs ---- *)

Definition zer_comptime_arg_valid_spec : ident * funspec :=
 DECLARE _zer_comptime_arg_valid
  WITH is_constant : Z
  PRE [ tint ]
    PROP (Int.min_signed <= is_constant <= Int.max_signed)
    PARAMS (Vint (Int.repr is_constant))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_comptime_arg_valid_coq is_constant)))
    SEP ().

Definition zer_static_assert_holds_spec : ident * funspec :=
 DECLARE _zer_static_assert_holds
  WITH is_constant : Z, value : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= is_constant <= Int.max_signed;
          Int.min_signed <= value <= Int.max_signed)
    PARAMS (Vint (Int.repr is_constant); Vint (Int.repr value))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_static_assert_holds_coq is_constant value)))
    SEP ().

Definition zer_comptime_ops_valid_spec : ident * funspec :=
 DECLARE _zer_comptime_ops_valid
  WITH ops_count : Z
  PRE [ tint ]
    PROP (Int.min_signed <= ops_count <= Int.max_signed)
    PARAMS (Vint (Int.repr ops_count))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_comptime_ops_valid_coq ops_count)))
    SEP ().

Definition zer_expr_nesting_valid_spec : ident * funspec :=
 DECLARE _zer_expr_nesting_valid
  WITH depth : Z
  PRE [ tint ]
    PROP (Int.min_signed <= depth <= Int.max_signed)
    PARAMS (Vint (Int.repr depth))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_expr_nesting_valid_coq depth)))
    SEP ().

Definition Gprog : funspecs :=
  [ zer_comptime_arg_valid_spec;
    zer_static_assert_holds_spec;
    zer_comptime_ops_valid_spec;
    zer_expr_nesting_valid_spec ].

(* ---- Proofs ---- *)

Lemma body_zer_comptime_arg_valid:
  semax_body Vprog Gprog f_zer_comptime_arg_valid
             zer_comptime_arg_valid_spec.
Proof.
  start_function.
  forward_if;
    forward;
    unfold zer_comptime_arg_valid_coq;
    destruct (Z.eq_dec is_constant 0); try lia;
    try entailer!.
Qed.

Lemma body_zer_static_assert_holds:
  semax_body Vprog Gprog f_zer_static_assert_holds
             zer_static_assert_holds_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_static_assert_holds_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_comptime_ops_valid:
  semax_body Vprog Gprog f_zer_comptime_ops_valid
             zer_comptime_ops_valid_spec.
Proof.
  start_function.
  forward_if;
    forward;
    unfold zer_comptime_ops_valid_coq, ZER_COMPTIME_OPS_BUDGET;
    destruct (Z_ge_dec ops_count 1000000); try lia;
    try entailer!.
Qed.

Lemma body_zer_expr_nesting_valid:
  semax_body Vprog Gprog f_zer_expr_nesting_valid
             zer_expr_nesting_valid_spec.
Proof.
  start_function.
  forward_if;
    forward;
    unfold zer_expr_nesting_valid_coq, ZER_EXPR_NESTING_LIMIT;
    destruct (Z_ge_dec depth 1000); try lia;
    try entailer!.
Qed.

(* ================================================================
   4 comptime predicates VST-verified. Phase 1 Batch 6.
   Total: 64 verified compiler functions.
   ================================================================ *)
