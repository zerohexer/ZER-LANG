(* ================================================================
   Level-3 VST proofs — src/safety/atomic_rules.c

   Atomic intrinsic safety predicates verified against
   lambda_zer_typing/typing.v Section E (E01 + E02).

   Functions verified (all linked into zerc):
     zer_atomic_width_valid(bytes) → 1 iff bytes ∈ {1, 2, 4, 8}
     zer_atomic_arg_is_ptr_to_int(flag) → 1 iff flag != 0

   Callers: checker.c @atomic_* validation.
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.atomic_rules.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

(* ---- Coq specifications ---- *)

Definition zer_atomic_width_valid_coq (bytes : Z) : Z :=
  if Z.eq_dec bytes 1 then 1
  else if Z.eq_dec bytes 2 then 1
  else if Z.eq_dec bytes 4 then 1
  else if Z.eq_dec bytes 8 then 1
  else 0.

Definition zer_atomic_arg_is_ptr_to_int_coq (flag : Z) : Z :=
  if Z.eq_dec flag 0 then 0 else 1.

(* ---- VST funspecs ---- *)

Definition zer_atomic_width_valid_spec : ident * funspec :=
 DECLARE _zer_atomic_width_valid
  WITH bytes : Z
  PRE [ tint ]
    PROP (Int.min_signed <= bytes <= Int.max_signed)
    PARAMS (Vint (Int.repr bytes))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_atomic_width_valid_coq bytes)))
    SEP ().

Definition zer_atomic_arg_is_ptr_to_int_spec : ident * funspec :=
 DECLARE _zer_atomic_arg_is_ptr_to_int
  WITH flag : Z
  PRE [ tint ]
    PROP (Int.min_signed <= flag <= Int.max_signed)
    PARAMS (Vint (Int.repr flag))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_atomic_arg_is_ptr_to_int_coq flag)))
    SEP ().

Definition Gprog : funspecs :=
  [ zer_atomic_width_valid_spec;
    zer_atomic_arg_is_ptr_to_int_spec ].

(* ---- Proofs ---- *)

Lemma body_zer_atomic_width_valid:
  semax_body Vprog Gprog f_zer_atomic_width_valid
             zer_atomic_width_valid_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_atomic_width_valid_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_atomic_arg_is_ptr_to_int:
  semax_body Vprog Gprog f_zer_atomic_arg_is_ptr_to_int
             zer_atomic_arg_is_ptr_to_int_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_atomic_arg_is_ptr_to_int_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

(* ================================================================
   2 atomic-rule predicates VST-verified against typing.v Section E.
   Total Level-3 verified compiler functions: 39
   (prev 37 + 2 atomic).
   ================================================================ *)
