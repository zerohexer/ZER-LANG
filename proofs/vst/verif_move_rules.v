(* ================================================================
   Level-3 VST proofs — src/safety/move_rules.c

   Move-struct safety predicates verified against the λZER-move
   operational oracle (lambda_zer_move/iris_move_specs.v).

   Oracle rules (Section B):
     B01: use-after-move — transferred struct cannot be used
     B02: consume on pass/assign — move struct value transferred
     step_spec_consume: transferred resource can't be used again
     alive_move_exclusive: two transfers of same struct contradict

   Functions verified (all linked into zerc):
     zer_type_kind_is_move_struct(type_kind, is_move_flag)
       → 1 iff kind == STRUCT AND is_move_flag != 0
     zer_move_should_track(is_direct, has_field)
       → 1 iff either bool is set (direct move or contains move field)

   Callers: zercheck.c is_move_struct_type + should_track_move.
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.move_rules.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

(* ---- Constants ---- *)
Definition ZER_TK_STRUCT : Z := 17.

(* ---- Coq specifications ---- *)

Definition zer_type_kind_is_move_struct_coq (kind is_move_flag : Z) : Z :=
  if Z.eq_dec kind ZER_TK_STRUCT then
    (if Z.eq_dec is_move_flag 0 then 0 else 1)
  else 0.

Definition zer_move_should_track_coq (direct has_field : Z) : Z :=
  if Z.eq_dec direct 0 then
    (if Z.eq_dec has_field 0 then 0 else 1)
  else 1.

(* ---- VST funspecs ---- *)

Definition zer_type_kind_is_move_struct_spec : ident * funspec :=
 DECLARE _zer_type_kind_is_move_struct
  WITH kind : Z, is_move_flag : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= kind <= Int.max_signed;
          Int.min_signed <= is_move_flag <= Int.max_signed)
    PARAMS (Vint (Int.repr kind); Vint (Int.repr is_move_flag))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_type_kind_is_move_struct_coq kind is_move_flag)))
    SEP ().

Definition zer_move_should_track_spec : ident * funspec :=
 DECLARE _zer_move_should_track
  WITH direct : Z, has_field : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= direct <= Int.max_signed;
          Int.min_signed <= has_field <= Int.max_signed)
    PARAMS (Vint (Int.repr direct); Vint (Int.repr has_field))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_move_should_track_coq direct has_field)))
    SEP ().

Definition Gprog : funspecs :=
  [ zer_type_kind_is_move_struct_spec;
    zer_move_should_track_spec ].

(* ---- Proofs ---- *)

Lemma body_zer_type_kind_is_move_struct:
  semax_body Vprog Gprog f_zer_type_kind_is_move_struct
             zer_type_kind_is_move_struct_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_type_kind_is_move_struct_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_move_should_track:
  semax_body Vprog Gprog f_zer_move_should_track
             zer_move_should_track_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_move_should_track_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

(* ================================================================
   2 move-struct predicates VST-verified against λZER-move oracle.
   Total Level-3 verified compiler functions: 37
   (4 handle state + 3 range + 7 type kind + 5 coerce + 6 context +
    3 escape + 3 provenance + 2 mmio + 2 optional + 2 move).
   ================================================================ *)
