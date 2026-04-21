(* ================================================================
   Level-3 VST proofs — src/safety/type_kind.c

   Type-kind category predicates. Each takes an int kind (matching
   TypeKind enum in types.h) and returns 1 iff kind belongs to the
   named category.

   Functions verified (all linked into zerc):
     zer_type_kind_is_integer(int kind)
       returns 1 iff kind ∈ {U8..U64, USIZE, I8..I64, ENUM}
     zer_type_kind_is_signed(int kind)
       returns 1 iff kind ∈ {I8..I64, ENUM}
     zer_type_kind_is_unsigned(int kind)
       returns 1 iff kind ∈ {U8..U64, USIZE}
     zer_type_kind_is_float(int kind)
       returns 1 iff kind ∈ {F32, F64}
     zer_type_kind_is_numeric(int kind)
       returns 1 iff kind is integer or float
     zer_type_kind_is_pointer(int kind)
       returns 1 iff kind ∈ {POINTER, OPAQUE}
     zer_type_kind_has_fields(int kind)
       returns 1 iff kind ∈ {STRUCT, UNION}

   Callers: types.c:type_is_integer/signed/unsigned/float/numeric.
   The Type* wrappers in types.c delegate after type_unwrap_distinct.
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.type_kind.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

(* ---- TypeKind constants — must match handle_state.h ---- *)
Definition ZER_TK_VOID       : Z := 0.
Definition ZER_TK_BOOL       : Z := 1.
Definition ZER_TK_U8         : Z := 2.
Definition ZER_TK_U16        : Z := 3.
Definition ZER_TK_U32        : Z := 4.
Definition ZER_TK_U64        : Z := 5.
Definition ZER_TK_USIZE      : Z := 6.
Definition ZER_TK_I8         : Z := 7.
Definition ZER_TK_I16        : Z := 8.
Definition ZER_TK_I32        : Z := 9.
Definition ZER_TK_I64        : Z := 10.
Definition ZER_TK_F32        : Z := 11.
Definition ZER_TK_F64        : Z := 12.
Definition ZER_TK_POINTER    : Z := 13.
Definition ZER_TK_OPAQUE     : Z := 21.
Definition ZER_TK_STRUCT     : Z := 17.
Definition ZER_TK_UNION      : Z := 19.
Definition ZER_TK_ENUM       : Z := 18.

(* ---- Coq specifications ---- *)

Definition zer_type_kind_is_integer_coq (k : Z) : Z :=
  if Z.eq_dec k ZER_TK_U8    then 1
  else if Z.eq_dec k ZER_TK_U16   then 1
  else if Z.eq_dec k ZER_TK_U32   then 1
  else if Z.eq_dec k ZER_TK_U64   then 1
  else if Z.eq_dec k ZER_TK_USIZE then 1
  else if Z.eq_dec k ZER_TK_I8    then 1
  else if Z.eq_dec k ZER_TK_I16   then 1
  else if Z.eq_dec k ZER_TK_I32   then 1
  else if Z.eq_dec k ZER_TK_I64   then 1
  else if Z.eq_dec k ZER_TK_ENUM  then 1
  else 0.

Definition zer_type_kind_is_signed_coq (k : Z) : Z :=
  if Z.eq_dec k ZER_TK_I8   then 1
  else if Z.eq_dec k ZER_TK_I16  then 1
  else if Z.eq_dec k ZER_TK_I32  then 1
  else if Z.eq_dec k ZER_TK_I64  then 1
  else if Z.eq_dec k ZER_TK_ENUM then 1
  else 0.

Definition zer_type_kind_is_unsigned_coq (k : Z) : Z :=
  if Z.eq_dec k ZER_TK_U8    then 1
  else if Z.eq_dec k ZER_TK_U16   then 1
  else if Z.eq_dec k ZER_TK_U32   then 1
  else if Z.eq_dec k ZER_TK_U64   then 1
  else if Z.eq_dec k ZER_TK_USIZE then 1
  else 0.

Definition zer_type_kind_is_float_coq (k : Z) : Z :=
  if Z.eq_dec k ZER_TK_F32 then 1
  else if Z.eq_dec k ZER_TK_F64 then 1
  else 0.

(* Numeric inlined — matches the C implementation's inlined cascade.
   Equivalent in meaning to is_integer OR is_float, but expressed as
   a direct cascade for VST-proof simplicity. *)
Definition zer_type_kind_is_numeric_coq (k : Z) : Z :=
  if Z.eq_dec k ZER_TK_U8    then 1
  else if Z.eq_dec k ZER_TK_U16   then 1
  else if Z.eq_dec k ZER_TK_U32   then 1
  else if Z.eq_dec k ZER_TK_U64   then 1
  else if Z.eq_dec k ZER_TK_USIZE then 1
  else if Z.eq_dec k ZER_TK_I8    then 1
  else if Z.eq_dec k ZER_TK_I16   then 1
  else if Z.eq_dec k ZER_TK_I32   then 1
  else if Z.eq_dec k ZER_TK_I64   then 1
  else if Z.eq_dec k ZER_TK_ENUM  then 1
  else if Z.eq_dec k ZER_TK_F32   then 1
  else if Z.eq_dec k ZER_TK_F64   then 1
  else 0.

Definition zer_type_kind_is_pointer_coq (k : Z) : Z :=
  if Z.eq_dec k ZER_TK_POINTER then 1
  else if Z.eq_dec k ZER_TK_OPAQUE  then 1
  else 0.

Definition zer_type_kind_has_fields_coq (k : Z) : Z :=
  if Z.eq_dec k ZER_TK_STRUCT then 1
  else if Z.eq_dec k ZER_TK_UNION  then 1
  else 0.

(* ---- VST funspecs ---- *)

Definition mk_int_spec (name : ident) (spec_fn : Z -> Z) : ident * funspec :=
 DECLARE name
  WITH k : Z
  PRE [ tint ]
    PROP (Int.min_signed <= k <= Int.max_signed)
    PARAMS (Vint (Int.repr k))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (spec_fn k)))
    SEP ().

Definition zer_type_kind_is_integer_spec  := mk_int_spec _zer_type_kind_is_integer  zer_type_kind_is_integer_coq.
Definition zer_type_kind_is_signed_spec   := mk_int_spec _zer_type_kind_is_signed   zer_type_kind_is_signed_coq.
Definition zer_type_kind_is_unsigned_spec := mk_int_spec _zer_type_kind_is_unsigned zer_type_kind_is_unsigned_coq.
Definition zer_type_kind_is_float_spec    := mk_int_spec _zer_type_kind_is_float    zer_type_kind_is_float_coq.
Definition zer_type_kind_is_numeric_spec  := mk_int_spec _zer_type_kind_is_numeric  zer_type_kind_is_numeric_coq.
Definition zer_type_kind_is_pointer_spec  := mk_int_spec _zer_type_kind_is_pointer  zer_type_kind_is_pointer_coq.
Definition zer_type_kind_has_fields_spec  := mk_int_spec _zer_type_kind_has_fields  zer_type_kind_has_fields_coq.

Definition Gprog : funspecs :=
  [ zer_type_kind_is_integer_spec;
    zer_type_kind_is_signed_spec;
    zer_type_kind_is_unsigned_spec;
    zer_type_kind_is_float_spec;
    zer_type_kind_is_numeric_spec;
    zer_type_kind_is_pointer_spec;
    zer_type_kind_has_fields_spec ].

(* ---- Proofs ---- *)

(* All 7 predicates are cascaded if-equality chains. VST's forward_if
   auto-substs k := N in the then-branch, so the proof simplifies to
   the same `repeat forward_if; forward; unfold; destruct; try lia;
   try entailer!` pattern documented in verif_handle_state.v. *)

Lemma body_zer_type_kind_is_integer:
  semax_body Vprog Gprog f_zer_type_kind_is_integer
             zer_type_kind_is_integer_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_type_kind_is_integer_coq;
    repeat (destruct (Z.eq_dec _ _); try lia); try entailer!.
Qed.

Lemma body_zer_type_kind_is_signed:
  semax_body Vprog Gprog f_zer_type_kind_is_signed
             zer_type_kind_is_signed_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_type_kind_is_signed_coq;
    repeat (destruct (Z.eq_dec _ _); try lia); try entailer!.
Qed.

Lemma body_zer_type_kind_is_unsigned:
  semax_body Vprog Gprog f_zer_type_kind_is_unsigned
             zer_type_kind_is_unsigned_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_type_kind_is_unsigned_coq;
    repeat (destruct (Z.eq_dec _ _); try lia); try entailer!.
Qed.

Lemma body_zer_type_kind_is_float:
  semax_body Vprog Gprog f_zer_type_kind_is_float
             zer_type_kind_is_float_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_type_kind_is_float_coq;
    repeat (destruct (Z.eq_dec _ _); try lia); try entailer!.
Qed.

Lemma body_zer_type_kind_is_pointer:
  semax_body Vprog Gprog f_zer_type_kind_is_pointer
             zer_type_kind_is_pointer_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_type_kind_is_pointer_coq;
    repeat (destruct (Z.eq_dec _ _); try lia); try entailer!.
Qed.

Lemma body_zer_type_kind_has_fields:
  semax_body Vprog Gprog f_zer_type_kind_has_fields
             zer_type_kind_has_fields_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_type_kind_has_fields_coq;
    repeat (destruct (Z.eq_dec _ _); try lia); try entailer!.
Qed.

Lemma body_zer_type_kind_is_numeric:
  semax_body Vprog Gprog f_zer_type_kind_is_numeric
             zer_type_kind_is_numeric_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_type_kind_is_numeric_coq;
    repeat (destruct (Z.eq_dec _ _); try lia); try entailer!.
Qed.

(* ================================================================
   7 type-kind predicates mechanically verified against
   src/safety/type_kind.c. Total Level-3 verified compiler functions:
   14 (4 handle state + 3 range + 7 type kind).
   ================================================================ *)
