(* ================================================================
   Level-3 VST proofs — src/safety/escape_rules.c

   Pointer-escape predicates verified against the λZER-escape
   operational proof (lambda_zer_escape/iris_escape_specs.v).

   Oracle: only RegStatic pointers can escape. Local and Arena
   pointers leave the state stuck in the operational semantics.

   Functions verified (all linked into zerc):
     zer_region_can_escape(region)   — 1 iff region == STATIC
     zer_region_is_local(region)     — 1 iff region == LOCAL
     zer_region_is_arena(region)     — 1 iff region == ARENA

   Callers: checker.c NODE_RETURN escape path. Converts
   is_local_derived/is_arena_derived flags → region tag, then
   calls zer_region_can_escape. If denied, emits error using
   zer_region_is_arena for message distinction.
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.escape_rules.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

(* ---- Constants (must match escape_rules.h) ---- *)
Definition ZER_REGION_STATIC : Z := 0.
Definition ZER_REGION_LOCAL  : Z := 1.
Definition ZER_REGION_ARENA  : Z := 2.

(* ---- Coq specifications ---- *)

Definition zer_region_can_escape_coq (region : Z) : Z :=
  if Z.eq_dec region ZER_REGION_STATIC then 1 else 0.

Definition zer_region_is_local_coq (region : Z) : Z :=
  if Z.eq_dec region ZER_REGION_LOCAL then 1 else 0.

Definition zer_region_is_arena_coq (region : Z) : Z :=
  if Z.eq_dec region ZER_REGION_ARENA then 1 else 0.

(* ---- VST funspecs ---- *)

Definition zer_region_can_escape_spec : ident * funspec :=
 DECLARE _zer_region_can_escape
  WITH region : Z
  PRE [ tint ]
    PROP (Int.min_signed <= region <= Int.max_signed)
    PARAMS (Vint (Int.repr region))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_region_can_escape_coq region)))
    SEP ().

Definition zer_region_is_local_spec : ident * funspec :=
 DECLARE _zer_region_is_local
  WITH region : Z
  PRE [ tint ]
    PROP (Int.min_signed <= region <= Int.max_signed)
    PARAMS (Vint (Int.repr region))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_region_is_local_coq region)))
    SEP ().

Definition zer_region_is_arena_spec : ident * funspec :=
 DECLARE _zer_region_is_arena
  WITH region : Z
  PRE [ tint ]
    PROP (Int.min_signed <= region <= Int.max_signed)
    PARAMS (Vint (Int.repr region))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_region_is_arena_coq region)))
    SEP ().

Definition Gprog : funspecs :=
  [ zer_region_can_escape_spec;
    zer_region_is_local_spec;
    zer_region_is_arena_spec ].

(* ---- Proofs ---- *)

Lemma body_zer_region_can_escape:
  semax_body Vprog Gprog f_zer_region_can_escape
             zer_region_can_escape_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_region_can_escape_coq;
    repeat (destruct (Z.eq_dec _ _); try lia); try entailer!.
Qed.

Lemma body_zer_region_is_local:
  semax_body Vprog Gprog f_zer_region_is_local
             zer_region_is_local_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_region_is_local_coq;
    repeat (destruct (Z.eq_dec _ _); try lia); try entailer!.
Qed.

Lemma body_zer_region_is_arena:
  semax_body Vprog Gprog f_zer_region_is_arena
             zer_region_is_arena_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_region_is_arena_coq;
    repeat (destruct (Z.eq_dec _ _); try lia); try entailer!.
Qed.

(* ================================================================
   3 escape predicates VST-verified against lambda_zer_escape oracle.
   Total Level-3 verified compiler functions: 28
   (4 handle state + 3 range + 7 type kind + 5 coerce + 6 context + 3 escape).
   ================================================================ *)
