(* ================================================================
   Level-3 Phase 2 Batch 1 — verif_isr_rules.v

   Hardware-context ban decisions verified against CLAUDE.md
   "Ban Decision Framework" + typing.v C03/C04/S04/S05.

   Functions verified:
     zer_alloc_allowed_in_isr(in_interrupt)
       → 1 iff in_interrupt == 0
     zer_alloc_allowed_in_critical(critical_depth)
       → 1 iff critical_depth <= 0
     zer_spawn_allowed_in_isr(in_interrupt)
       → 1 iff in_interrupt == 0
     zer_spawn_allowed_in_critical(critical_depth)
       → 1 iff critical_depth <= 0

   Callers: checker.c check_isr_ban (delegates to
   zer_alloc_allowed_in_isr).
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.isr_rules.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

(* ---- Coq specifications ---- *)

Definition zer_alloc_allowed_in_isr_coq (in_interrupt : Z) : Z :=
  if Z.eq_dec in_interrupt 0 then 1 else 0.

Definition zer_alloc_allowed_in_critical_coq (critical_depth : Z) : Z :=
  if Z_gt_dec critical_depth 0 then 0 else 1.

Definition zer_spawn_allowed_in_isr_coq := zer_alloc_allowed_in_isr_coq.
Definition zer_spawn_allowed_in_critical_coq := zer_alloc_allowed_in_critical_coq.

(* ---- VST funspecs ---- *)

Definition zer_alloc_allowed_in_isr_spec : ident * funspec :=
 DECLARE _zer_alloc_allowed_in_isr
  WITH in_interrupt : Z
  PRE [ tint ]
    PROP (Int.min_signed <= in_interrupt <= Int.max_signed)
    PARAMS (Vint (Int.repr in_interrupt))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_alloc_allowed_in_isr_coq in_interrupt)))
    SEP ().

Definition zer_alloc_allowed_in_critical_spec : ident * funspec :=
 DECLARE _zer_alloc_allowed_in_critical
  WITH critical_depth : Z
  PRE [ tint ]
    PROP (Int.min_signed <= critical_depth <= Int.max_signed)
    PARAMS (Vint (Int.repr critical_depth))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_alloc_allowed_in_critical_coq critical_depth)))
    SEP ().

Definition zer_spawn_allowed_in_isr_spec : ident * funspec :=
 DECLARE _zer_spawn_allowed_in_isr
  WITH in_interrupt : Z
  PRE [ tint ]
    PROP (Int.min_signed <= in_interrupt <= Int.max_signed)
    PARAMS (Vint (Int.repr in_interrupt))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_spawn_allowed_in_isr_coq in_interrupt)))
    SEP ().

Definition zer_spawn_allowed_in_critical_spec : ident * funspec :=
 DECLARE _zer_spawn_allowed_in_critical
  WITH critical_depth : Z
  PRE [ tint ]
    PROP (Int.min_signed <= critical_depth <= Int.max_signed)
    PARAMS (Vint (Int.repr critical_depth))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_spawn_allowed_in_critical_coq critical_depth)))
    SEP ().

Definition Gprog : funspecs :=
  [ zer_alloc_allowed_in_isr_spec;
    zer_alloc_allowed_in_critical_spec;
    zer_spawn_allowed_in_isr_spec;
    zer_spawn_allowed_in_critical_spec ].

(* ---- Proofs ---- *)

Lemma body_zer_alloc_allowed_in_isr:
  semax_body Vprog Gprog f_zer_alloc_allowed_in_isr
             zer_alloc_allowed_in_isr_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_alloc_allowed_in_isr_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_alloc_allowed_in_critical:
  semax_body Vprog Gprog f_zer_alloc_allowed_in_critical
             zer_alloc_allowed_in_critical_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_alloc_allowed_in_critical_coq;
    repeat (destruct (Z_gt_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_spawn_allowed_in_isr:
  semax_body Vprog Gprog f_zer_spawn_allowed_in_isr
             zer_spawn_allowed_in_isr_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_spawn_allowed_in_isr_coq, zer_alloc_allowed_in_isr_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_spawn_allowed_in_critical:
  semax_body Vprog Gprog f_zer_spawn_allowed_in_critical
             zer_spawn_allowed_in_critical_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_spawn_allowed_in_critical_coq,
           zer_alloc_allowed_in_critical_coq;
    repeat (destruct (Z_gt_dec _ _); try lia);
    try entailer!.
Qed.

(* ================================================================
   4 ISR/@critical predicates VST-verified. Phase 2 Batch 1.
   Total Level-3 verified compiler functions: 48
   (Phase 1: 44 + Phase 2: 4).
   ================================================================ *)
