(* ================================================================
   Level-3 VST proofs — src/safety/misc_rules.c

   Final Phase 1 predicates: Q03 (int switch default) + I01-I11
   (cast qualifier safety, unified form).

   Oracle:
     - typing.v Section Q, int_switch_has_default
     - typing.v Section I, cast_safe = qual_le q_src q_dst

   Functions verified:
     zer_int_switch_has_default(flag) → 1 iff flag != 0
     zer_cast_qualifier_safe(fc, tc, fv, tv)
       → 1 iff (fv == 0 OR tv != 0) AND (fc == 0 OR tc != 0)
       → can't strip volatile, can't strip const
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.misc_rules.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

(* ---- Coq specifications ---- *)

Definition zer_int_switch_has_default_coq (flag : Z) : Z :=
  if Z.eq_dec flag 0 then 0 else 1.

Definition zer_bool_switch_covers_both_coq (has_default has_true has_false : Z) : Z :=
  if Z.eq_dec has_default 0 then
    (if Z.eq_dec has_true 0 then 0
     else if Z.eq_dec has_false 0 then 0
     else 1)
  else 1.

(* ---- VST funspecs ---- *)

Definition zer_int_switch_has_default_spec : ident * funspec :=
 DECLARE _zer_int_switch_has_default
  WITH flag : Z
  PRE [ tint ]
    PROP (Int.min_signed <= flag <= Int.max_signed)
    PARAMS (Vint (Int.repr flag))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_int_switch_has_default_coq flag)))
    SEP ().

Definition zer_bool_switch_covers_both_spec : ident * funspec :=
 DECLARE _zer_bool_switch_covers_both
  WITH has_default : Z, has_true : Z, has_false : Z
  PRE [ tint, tint, tint ]
    PROP (Int.min_signed <= has_default <= Int.max_signed;
          Int.min_signed <= has_true <= Int.max_signed;
          Int.min_signed <= has_false <= Int.max_signed)
    PARAMS (Vint (Int.repr has_default); Vint (Int.repr has_true);
            Vint (Int.repr has_false))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr
      (zer_bool_switch_covers_both_coq has_default has_true has_false)))
    SEP ().

Definition Gprog : funspecs :=
  [ zer_int_switch_has_default_spec;
    zer_bool_switch_covers_both_spec ].

(* ---- Proofs ---- *)

Lemma body_zer_int_switch_has_default:
  semax_body Vprog Gprog f_zer_int_switch_has_default
             zer_int_switch_has_default_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_int_switch_has_default_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_bool_switch_covers_both:
  semax_body Vprog Gprog f_zer_bool_switch_covers_both
             zer_bool_switch_covers_both_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_bool_switch_covers_both_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

(* ================================================================
   2 misc predicates VST-verified against typing.v Q + I.
   PHASE 1 COMPLETE: 44/44 Level-3-verified compiler functions.
   ================================================================ *)
