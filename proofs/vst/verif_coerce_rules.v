(* ================================================================
   Level-3 VST proofs — src/safety/coerce_rules.c

   Four coercion-rule predicates. Each takes int arguments (0/1 bools
   + widths) and returns 0/1.

   Functions verified:
     zer_coerce_int_widening_allowed(from_signed, to_signed, from_w, to_w)
       Integer widening rule: same sign + strict widening, or
       unsigned → larger signed.
     zer_coerce_usize_same_width_allowed(from_is_usize, to_is_usize,
                                          from_signed, to_signed)
       Same-width int coercion via USIZE.
     zer_coerce_float_widening_allowed(from_is_f32, to_is_f64)
       Only f32 → f64 allowed.
     zer_coerce_qualifier_widening_allowed(from_const, to_const,
                                             from_volatile, to_volatile)
       Cannot strip const or volatile.

   Callers: types.c:can_implicit_coerce for integer/float/qualifier
   widening cases. The Type*-taking wrapper extracts primitives and
   delegates.
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.coerce_rules.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

(* ---- Coq specifications ---- *)

(* Integer widening — matches the flattened C flow.
   Must be strictly widening. If widths don't allow it → 0.
   If widening AND (same sign OR unsigned source) → 1. Else 0. *)
Definition zer_coerce_int_widening_allowed_coq
  (from_s to_s from_w to_w : Z) : Z :=
  if Z_ge_dec from_w to_w then 0
  else if Z.eq_dec from_s to_s then 1
  else if Z.eq_dec from_s 0 then 1
  else 0.

(* USIZE same-width — flat early-return chain (using Z.eq_dec to
   align with destruct tactic). *)
Definition zer_coerce_usize_same_width_allowed_coq
  (from_u to_u from_s to_s : Z) : Z :=
  if Z.eq_dec from_s to_s then
    (if Z.eq_dec from_u 0 then
       (if Z.eq_dec to_u 0 then 0 else 1)
     else 1)
  else 0.

(* Float widening — flattened form. *)
Definition zer_coerce_float_widening_allowed_coq
  (from_f32 to_f64 : Z) : Z :=
  if Z.eq_dec from_f32 0 then 0
  else if Z.eq_dec to_f64 0 then 0
  else 1.

(* Preserve volatile — simple 3-case cascade. *)
Definition zer_coerce_preserves_volatile_coq (from_v to_v : Z) : Z :=
  if Z.eq_dec from_v 0 then 1
  else if Z.eq_dec to_v 0 then 0
  else 1.

(* Preserve const — identical shape. *)
Definition zer_coerce_preserves_const_coq (from_c to_c : Z) : Z :=
  if Z.eq_dec from_c 0 then 1
  else if Z.eq_dec to_c 0 then 0
  else 1.

(* ---- VST funspecs ---- *)

Definition zer_coerce_int_widening_allowed_spec : ident * funspec :=
 DECLARE _zer_coerce_int_widening_allowed
  WITH from_s : Z, to_s : Z, from_w : Z, to_w : Z
  PRE [ tint, tint, tint, tint ]
    PROP (Int.min_signed <= from_s <= Int.max_signed;
          Int.min_signed <= to_s <= Int.max_signed;
          Int.min_signed <= from_w <= Int.max_signed;
          Int.min_signed <= to_w <= Int.max_signed)
    PARAMS (Vint (Int.repr from_s); Vint (Int.repr to_s);
            Vint (Int.repr from_w); Vint (Int.repr to_w))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr
      (zer_coerce_int_widening_allowed_coq from_s to_s from_w to_w)))
    SEP ().

Definition zer_coerce_usize_same_width_allowed_spec : ident * funspec :=
 DECLARE _zer_coerce_usize_same_width_allowed
  WITH from_u : Z, to_u : Z, from_s : Z, to_s : Z
  PRE [ tint, tint, tint, tint ]
    PROP (Int.min_signed <= from_u <= Int.max_signed;
          Int.min_signed <= to_u <= Int.max_signed;
          Int.min_signed <= from_s <= Int.max_signed;
          Int.min_signed <= to_s <= Int.max_signed)
    PARAMS (Vint (Int.repr from_u); Vint (Int.repr to_u);
            Vint (Int.repr from_s); Vint (Int.repr to_s))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr
      (zer_coerce_usize_same_width_allowed_coq from_u to_u from_s to_s)))
    SEP ().

Definition zer_coerce_float_widening_allowed_spec : ident * funspec :=
 DECLARE _zer_coerce_float_widening_allowed
  WITH from_f32 : Z, to_f64 : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= from_f32 <= Int.max_signed;
          Int.min_signed <= to_f64 <= Int.max_signed)
    PARAMS (Vint (Int.repr from_f32); Vint (Int.repr to_f64))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr
      (zer_coerce_float_widening_allowed_coq from_f32 to_f64)))
    SEP ().

Definition zer_coerce_preserves_volatile_spec : ident * funspec :=
 DECLARE _zer_coerce_preserves_volatile
  WITH from_v : Z, to_v : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= from_v <= Int.max_signed;
          Int.min_signed <= to_v <= Int.max_signed)
    PARAMS (Vint (Int.repr from_v); Vint (Int.repr to_v))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_coerce_preserves_volatile_coq from_v to_v)))
    SEP ().

Definition zer_coerce_preserves_const_spec : ident * funspec :=
 DECLARE _zer_coerce_preserves_const
  WITH from_c : Z, to_c : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= from_c <= Int.max_signed;
          Int.min_signed <= to_c <= Int.max_signed)
    PARAMS (Vint (Int.repr from_c); Vint (Int.repr to_c))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_coerce_preserves_const_coq from_c to_c)))
    SEP ().

Definition Gprog : funspecs :=
  [ zer_coerce_int_widening_allowed_spec;
    zer_coerce_usize_same_width_allowed_spec;
    zer_coerce_float_widening_allowed_spec;
    zer_coerce_preserves_volatile_spec;
    zer_coerce_preserves_const_spec ].

(* ---- Proofs — cascade pattern with auto-subst handling ---- *)

Lemma body_zer_coerce_int_widening_allowed:
  semax_body Vprog Gprog f_zer_coerce_int_widening_allowed
             zer_coerce_int_widening_allowed_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_coerce_int_widening_allowed_coq;
    repeat (first [ destruct (Z_ge_dec _ _)
                  | destruct (Z_lt_dec _ _)
                  | destruct (Z.eq_dec _ _) ]; try lia);
    try entailer!.
Qed.

Lemma body_zer_coerce_usize_same_width_allowed:
  semax_body Vprog Gprog f_zer_coerce_usize_same_width_allowed
             zer_coerce_usize_same_width_allowed_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_coerce_usize_same_width_allowed_coq;
    repeat (destruct (Z.eq_dec _ _); try lia); simpl;
    try entailer!.
Qed.

Lemma body_zer_coerce_float_widening_allowed:
  semax_body Vprog Gprog f_zer_coerce_float_widening_allowed
             zer_coerce_float_widening_allowed_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_coerce_float_widening_allowed_coq;
    repeat (destruct (Z.eq_dec _ _); try lia); simpl;
    try entailer!.
Qed.

Lemma body_zer_coerce_preserves_volatile:
  semax_body Vprog Gprog f_zer_coerce_preserves_volatile
             zer_coerce_preserves_volatile_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_coerce_preserves_volatile_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_coerce_preserves_const:
  semax_body Vprog Gprog f_zer_coerce_preserves_const
             zer_coerce_preserves_const_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_coerce_preserves_const_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

(* ================================================================
   4 coercion predicates VST-verified.
   Total Level-3 verified compiler functions: 18
   (4 handle state + 3 range + 7 type kind + 4 coerce).
   ================================================================ *)
