(* ================================================================
   Level-3 VST proofs — src/safety/container_rules.c

   Container/field well-formedness predicates verified against
   typing.v Section T (container validity) + Section K (container
   monomorphization depth).

   Functions verified (all linked into zerc):
     zer_container_depth_valid(depth)
       → 1 iff 0 <= depth < 32 (container_depth_limit)
     zer_field_type_valid(is_void)
       → 1 iff is_void == 0 (non-void fields only)
     zer_type_has_size(is_void)
       → 1 iff is_void == 0 (types have size only if non-void)
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.container_rules.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

(* ---- Constants ---- *)
Definition ZER_CONTAINER_DEPTH_LIMIT : Z := 32.

(* ---- Coq specifications ---- *)

Definition zer_container_depth_valid_coq (depth : Z) : Z :=
  if Z_lt_dec depth 0 then 0
  else if Z_ge_dec depth ZER_CONTAINER_DEPTH_LIMIT then 0
  else 1.

Definition zer_field_type_valid_coq (is_void : Z) : Z :=
  if Z.eq_dec is_void 0 then 1 else 0.

Definition zer_type_has_size_coq (is_void : Z) : Z :=
  if Z.eq_dec is_void 0 then 1 else 0.

(* ---- VST funspecs ---- *)

Definition zer_container_depth_valid_spec : ident * funspec :=
 DECLARE _zer_container_depth_valid
  WITH depth : Z
  PRE [ tint ]
    PROP (Int.min_signed <= depth <= Int.max_signed)
    PARAMS (Vint (Int.repr depth))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_container_depth_valid_coq depth)))
    SEP ().

Definition zer_field_type_valid_spec : ident * funspec :=
 DECLARE _zer_field_type_valid
  WITH is_void : Z
  PRE [ tint ]
    PROP (Int.min_signed <= is_void <= Int.max_signed)
    PARAMS (Vint (Int.repr is_void))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_field_type_valid_coq is_void)))
    SEP ().

Definition zer_type_has_size_spec : ident * funspec :=
 DECLARE _zer_type_has_size
  WITH is_void : Z
  PRE [ tint ]
    PROP (Int.min_signed <= is_void <= Int.max_signed)
    PARAMS (Vint (Int.repr is_void))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_type_has_size_coq is_void)))
    SEP ().

Definition Gprog : funspecs :=
  [ zer_container_depth_valid_spec;
    zer_field_type_valid_spec;
    zer_type_has_size_spec ].

(* ---- Proofs ---- *)

Lemma body_zer_container_depth_valid:
  semax_body Vprog Gprog f_zer_container_depth_valid
             zer_container_depth_valid_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_container_depth_valid_coq;
    repeat (first [ destruct (Z_lt_dec _ _)
                  | destruct (Z_ge_dec _ _) ]; try lia);
    try entailer!.
Qed.

Lemma body_zer_field_type_valid:
  semax_body Vprog Gprog f_zer_field_type_valid
             zer_field_type_valid_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_field_type_valid_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_type_has_size:
  semax_body Vprog Gprog f_zer_type_has_size
             zer_type_has_size_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_type_has_size_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

(* ================================================================
   3 container-rule predicates VST-verified against typing.v T/K.
   Total Level-3 verified compiler functions: 42
   (prev 39 + 3 container).
   ================================================================ *)
