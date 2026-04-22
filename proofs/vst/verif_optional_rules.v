(* ================================================================
   Level-3 VST proofs — src/safety/optional_rules.c

   Optional-type safety predicates verified against
   lambda_zer_typing/typing.v Section N:
     N01: non-null *T cannot accept null
     N02: null can only be assigned to OPTIONAL types
     N03: if-unwrap requires optional source
     N05: no nested ??T

   Functions verified (all linked into zerc):
     zer_type_permits_null(type_kind)
       → 1 iff type_kind == TYPE_OPTIONAL (covers N01/N02/N03)
     zer_type_is_nested_optional(outer_kind, inner_kind)
       → 1 iff both == TYPE_OPTIONAL (N05)

   Callers: checker.c TYNODE_OPTIONAL resolution delegates to
   zer_type_is_nested_optional when rejecting ??T.
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.optional_rules.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

(* ---- Constants (must match optional_rules.h and types.h) ---- *)
Definition ZER_TK_OPTIONAL : Z := 14.

(* ---- Coq specifications ---- *)

Definition zer_type_permits_null_coq (k : Z) : Z :=
  if Z.eq_dec k ZER_TK_OPTIONAL then 1 else 0.

Definition zer_type_is_nested_optional_coq (outer inner : Z) : Z :=
  if Z.eq_dec outer ZER_TK_OPTIONAL then
    (if Z.eq_dec inner ZER_TK_OPTIONAL then 1 else 0)
  else 0.

(* ---- VST funspecs ---- *)

Definition zer_type_permits_null_spec : ident * funspec :=
 DECLARE _zer_type_permits_null
  WITH k : Z
  PRE [ tint ]
    PROP (Int.min_signed <= k <= Int.max_signed)
    PARAMS (Vint (Int.repr k))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_type_permits_null_coq k)))
    SEP ().

Definition zer_type_is_nested_optional_spec : ident * funspec :=
 DECLARE _zer_type_is_nested_optional
  WITH outer : Z, inner : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= outer <= Int.max_signed;
          Int.min_signed <= inner <= Int.max_signed)
    PARAMS (Vint (Int.repr outer); Vint (Int.repr inner))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_type_is_nested_optional_coq outer inner)))
    SEP ().

Definition Gprog : funspecs :=
  [ zer_type_permits_null_spec;
    zer_type_is_nested_optional_spec ].

(* ---- Proofs ---- *)

Lemma body_zer_type_permits_null:
  semax_body Vprog Gprog f_zer_type_permits_null
             zer_type_permits_null_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_type_permits_null_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_type_is_nested_optional:
  semax_body Vprog Gprog f_zer_type_is_nested_optional
             zer_type_is_nested_optional_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_type_is_nested_optional_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

(* ================================================================
   2 optional predicates VST-verified against typing.v Section N.
   Total Level-3 verified compiler functions: 35
   (4 handle state + 3 range + 7 type kind + 5 coerce + 6 context +
    3 escape + 3 provenance + 2 mmio + 2 optional).
   ================================================================ *)
