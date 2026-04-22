(* ================================================================
   Level-3 Phase 1 Batch 1 — verif_arith_rules.v

   Arithmetic safety predicates verified against typing.v Section M.

   Functions verified:
     zer_div_valid(divisor)
       → 1 iff divisor != 0
     zer_divisor_proven_nonzero(has_proof)
       → 1 iff has_proof != 0
     zer_narrowing_valid(src_width, dst_width, has_truncate)
       → 1 iff src_width <= dst_width OR has_truncate != 0

   NOTE: zer_literal_fits (M08) deferred to Batch 1b due to tlong
   forward_if goal structure differences — needs split predicate design.

   Callers: checker.c NODE_BINARY SLASH/PERCENT, compound assign.
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.arith_rules.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

(* ---- Coq specifications ---- *)

Definition zer_div_valid_coq (divisor : Z) : Z :=
  if Z.eq_dec divisor 0 then 0 else 1.

Definition zer_divisor_proven_nonzero_coq (has_proof : Z) : Z :=
  if Z.eq_dec has_proof 0 then 0 else 1.

Definition zer_narrowing_valid_coq (src_width dst_width has_truncate : Z) : Z :=
  if Z_le_dec src_width dst_width then 1
  else if Z.eq_dec has_truncate 0 then 0
  else 1.

(* ---- VST funspecs ---- *)

Definition zer_div_valid_spec : ident * funspec :=
 DECLARE _zer_div_valid
  WITH divisor : Z
  PRE [ tint ]
    PROP (Int.min_signed <= divisor <= Int.max_signed)
    PARAMS (Vint (Int.repr divisor))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_div_valid_coq divisor)))
    SEP ().

Definition zer_divisor_proven_nonzero_spec : ident * funspec :=
 DECLARE _zer_divisor_proven_nonzero
  WITH has_proof : Z
  PRE [ tint ]
    PROP (Int.min_signed <= has_proof <= Int.max_signed)
    PARAMS (Vint (Int.repr has_proof))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_divisor_proven_nonzero_coq has_proof)))
    SEP ().

Definition zer_narrowing_valid_spec : ident * funspec :=
 DECLARE _zer_narrowing_valid
  WITH src_width : Z, dst_width : Z, has_truncate : Z
  PRE [ tint, tint, tint ]
    PROP (Int.min_signed <= src_width <= Int.max_signed;
          Int.min_signed <= dst_width <= Int.max_signed;
          Int.min_signed <= has_truncate <= Int.max_signed)
    PARAMS (Vint (Int.repr src_width);
            Vint (Int.repr dst_width);
            Vint (Int.repr has_truncate))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr
              (zer_narrowing_valid_coq src_width dst_width has_truncate)))
    SEP ().

Definition Gprog : funspecs :=
  [ zer_div_valid_spec;
    zer_divisor_proven_nonzero_spec;
    zer_narrowing_valid_spec ].

(* ---- Proofs ---- *)

Lemma body_zer_div_valid:
  semax_body Vprog Gprog f_zer_div_valid zer_div_valid_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_div_valid_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_divisor_proven_nonzero:
  semax_body Vprog Gprog f_zer_divisor_proven_nonzero
             zer_divisor_proven_nonzero_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_divisor_proven_nonzero_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_narrowing_valid:
  semax_body Vprog Gprog f_zer_narrowing_valid zer_narrowing_valid_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_narrowing_valid_coq;
    repeat (destruct (Z_le_dec _ _); try lia);
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

(* ================================================================
   3 arithmetic predicates VST-verified. Phase 1 Batch 1.
   Total Level-3 verified compiler functions: 51
   (Phase 1: 48 + Batch 1: 3).

   M08 zer_literal_fits deferred to Batch 1b.
   ================================================================ *)
