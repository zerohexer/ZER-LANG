(* ================================================================
   Level-3 VST proofs — src/safety/provenance_rules.c

   @ptrcast provenance predicates verified against the λZER-opaque
   operational oracle (lambda_zer_opaque/iris_opaque_specs.v).

   Oracle rules:
     - EOpaqueCast is identity on tag (step_spec_opaque_cast)
     - ETypedCast t' requires typed_ptr γ id t' (typed_ptr_agree)
     - Unknown provenance (type_id == 0) accepted as documented
       C-interop boundary

   Functions verified (all linked into zerc):
     zer_provenance_check_required(src_unknown, dst_opaque)
       → 1 iff check needed (both concrete + known)
     zer_provenance_type_ids_compatible(actual, expected)
       → 1 iff actual == 0 OR actual == expected
     zer_provenance_opaque_upcast_allowed()
       → always 1 (upcast is step_spec_opaque_cast)

   Callers: checker.c @ptrcast handler (delegates the check-required
   decision to the VST-verified predicate before doing Type* match).
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.provenance_rules.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

(* ---- Coq specifications ---- *)

Definition zer_provenance_check_required_coq (src_unknown dst_opaque : Z) : Z :=
  if Z.eq_dec src_unknown 0 then
    (if Z.eq_dec dst_opaque 0 then 1 else 0)
  else 0.

Definition zer_provenance_type_ids_compatible_coq (actual expected : Z) : Z :=
  if Z.eq_dec actual 0 then 1
  else if Z.eq_dec actual expected then 1
  else 0.

Definition zer_provenance_opaque_upcast_allowed_coq : Z := 1.

(* ---- VST funspecs ---- *)

Definition zer_provenance_check_required_spec : ident * funspec :=
 DECLARE _zer_provenance_check_required
  WITH src_unknown : Z, dst_opaque : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= src_unknown <= Int.max_signed;
          Int.min_signed <= dst_opaque <= Int.max_signed)
    PARAMS (Vint (Int.repr src_unknown); Vint (Int.repr dst_opaque))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr
      (zer_provenance_check_required_coq src_unknown dst_opaque)))
    SEP ().

Definition zer_provenance_type_ids_compatible_spec : ident * funspec :=
 DECLARE _zer_provenance_type_ids_compatible
  WITH actual : Z, expected : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= actual <= Int.max_signed;
          Int.min_signed <= expected <= Int.max_signed)
    PARAMS (Vint (Int.repr actual); Vint (Int.repr expected))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr
      (zer_provenance_type_ids_compatible_coq actual expected)))
    SEP ().

Definition zer_provenance_opaque_upcast_allowed_spec : ident * funspec :=
 DECLARE _zer_provenance_opaque_upcast_allowed
  WITH u : unit
  PRE [ ]
    PROP ()
    PARAMS ()
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr zer_provenance_opaque_upcast_allowed_coq))
    SEP ().

Definition Gprog : funspecs :=
  [ zer_provenance_check_required_spec;
    zer_provenance_type_ids_compatible_spec;
    zer_provenance_opaque_upcast_allowed_spec ].

(* ---- Proofs ---- *)

Lemma body_zer_provenance_check_required:
  semax_body Vprog Gprog f_zer_provenance_check_required
             zer_provenance_check_required_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_provenance_check_required_coq;
    repeat (destruct (Z.eq_dec _ _); try lia); simpl;
    try entailer!.
Qed.

Lemma body_zer_provenance_type_ids_compatible:
  semax_body Vprog Gprog f_zer_provenance_type_ids_compatible
             zer_provenance_type_ids_compatible_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_provenance_type_ids_compatible_coq;
    repeat (destruct (Z.eq_dec _ _); try lia); try entailer!.
Qed.

Lemma body_zer_provenance_opaque_upcast_allowed:
  semax_body Vprog Gprog f_zer_provenance_opaque_upcast_allowed
             zer_provenance_opaque_upcast_allowed_spec.
Proof.
  start_function.
  forward.
Qed.

(* ================================================================
   3 provenance predicates VST-verified against λZER-opaque oracle.
   Total Level-3 verified compiler functions: 31
   (4 handle state + 3 range + 7 type kind + 5 coerce + 6 context +
    3 escape + 3 provenance).
   ================================================================ *)
