(* ================================================================
   Level-3 Phase 1 Batch 5 — verif_stack_rules.v

   Stack-frame safety verified against typing.v Section S.

   Functions verified:
     zer_stack_frame_valid(limit, frame) → 1 iff frame <= limit

   Used for both S01 (per-function frame) and S02 (call-chain depth).

   Callers: checker.c check_stack_depth enforcement of --stack-limit.
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.stack_rules.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

Definition zer_stack_frame_valid_coq (limit frame : Z) : Z :=
  if Z_le_dec frame limit then 1 else 0.

Definition zer_stack_frame_valid_spec : ident * funspec :=
 DECLARE _zer_stack_frame_valid
  WITH limit : Z, frame : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= limit <= Int.max_signed;
          Int.min_signed <= frame <= Int.max_signed)
    PARAMS (Vint (Int.repr limit); Vint (Int.repr frame))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_stack_frame_valid_coq limit frame)))
    SEP ().

Definition Gprog : funspecs := [ zer_stack_frame_valid_spec ].

Lemma body_zer_stack_frame_valid:
  semax_body Vprog Gprog f_zer_stack_frame_valid zer_stack_frame_valid_spec.
Proof.
  start_function.
  forward_if;
    forward;
    unfold zer_stack_frame_valid_coq;
    destruct (Z_le_dec frame limit); try lia;
    try entailer!.
Qed.

(* ================================================================
   1 stack predicate VST-verified. Phase 1 Batch 5.
   Total: 60 verified compiler functions.
   ================================================================ *)
