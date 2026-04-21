(* ================================================================
   Level-3 VST proofs — zer_checks2.c safety predicates.

   Batch-2 continuation of the Level-3 coverage push. Each function
   in zer_checks2.c is verified against its Coq specification, which
   mirrors the corresponding predicate from
   lambda_zer_typing/typing.v.

   Functions verified here:
     comptime_arg_ok          — R02 comptime-arg must be constant
     static_assert_ok         — R04 static_assert condition truthy
     stack_frame_ok           — S01 per-function frame within limit
     slab_alloc_allowed       — S04 slab.alloc banned in ISR
     slab_in_critical_allowed — S05 slab.alloc banned in @critical
     return_safe              — G01-G03 return allowed unless
                                 @critical / defer
     defer_safe               — G04 nested defer banned
     asm_safe                 — G10 asm only in naked functions
     yield_safe               — F01/F02 yield/await only in async
     spawn_in_isr_allowed     — C03 spawn banned in ISR
     thread_join_safe         — C02 double-join detection
     address_of_safe          — D01 &shared_struct.field banned
     atomic_arg_ok            — E02 @atomic_* arg must be ptr-to-int

   Pattern is the same one-liner used in verif_zer_checks.v:
     forward_if; forward; unfold spec; destruct; entailer!
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_vst.zer_checks2.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

(* ================================================================
   Coq specifications.
   ================================================================ *)

Definition comptime_arg_ok_coq (is_constant : Z) : Z :=
  if Z.eq_dec is_constant 0 then 0 else 1.

Definition static_assert_ok_coq (condition : Z) : Z :=
  if Z.eq_dec condition 0 then 0 else 1.

Definition stack_frame_ok_coq (limit frame_size : Z) : Z :=
  if Z_le_dec frame_size limit then 1 else 0.

Definition slab_alloc_allowed_coq (in_isr : Z) : Z :=
  if Z.eq_dec in_isr 0 then 1 else 0.

Definition slab_in_critical_allowed_coq (in_critical : Z) : Z :=
  if Z.eq_dec in_critical 0 then 1 else 0.

Definition return_safe_coq (crit_flag defer_flag : Z) : Z :=
  if Z.eq_dec crit_flag 0 then
    (if Z.eq_dec defer_flag 0 then 1 else 0)
  else 0.

Definition defer_safe_coq (in_defer : Z) : Z :=
  if Z.eq_dec in_defer 0 then 1 else 0.

Definition asm_safe_coq (in_naked : Z) : Z :=
  if Z.eq_dec in_naked 0 then 0 else 1.

Definition yield_safe_coq (in_async : Z) : Z :=
  if Z.eq_dec in_async 0 then 0 else 1.

Definition spawn_in_isr_allowed_coq (in_isr : Z) : Z :=
  if Z.eq_dec in_isr 0 then 1 else 0.

Definition thread_join_safe_coq (thread_state : Z) : Z :=
  if Z.eq_dec thread_state 0 then 1 else 0.

Definition address_of_safe_coq (is_shared_field : Z) : Z :=
  if Z.eq_dec is_shared_field 0 then 1 else 0.

Definition atomic_arg_ok_coq (is_ptr_to_int : Z) : Z :=
  if Z.eq_dec is_ptr_to_int 0 then 0 else 1.

(* ================================================================
   VST specifications.
   ================================================================ *)

Definition comptime_arg_ok_spec : ident * funspec :=
 DECLARE _comptime_arg_ok
  WITH is_constant : Z
  PRE [ tint ]
    PROP (Int.min_signed <= is_constant <= Int.max_signed)
    PARAMS (Vint (Int.repr is_constant))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (comptime_arg_ok_coq is_constant)))
    SEP ().

Definition static_assert_ok_spec : ident * funspec :=
 DECLARE _static_assert_ok
  WITH condition : Z
  PRE [ tint ]
    PROP (Int.min_signed <= condition <= Int.max_signed)
    PARAMS (Vint (Int.repr condition))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (static_assert_ok_coq condition)))
    SEP ().

Definition stack_frame_ok_spec : ident * funspec :=
 DECLARE _stack_frame_ok
  WITH limit : Z, frame_size : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= limit <= Int.max_signed;
          Int.min_signed <= frame_size <= Int.max_signed)
    PARAMS (Vint (Int.repr limit); Vint (Int.repr frame_size))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (stack_frame_ok_coq limit frame_size)))
    SEP ().

Definition slab_alloc_allowed_spec : ident * funspec :=
 DECLARE _slab_alloc_allowed
  WITH in_isr : Z
  PRE [ tint ]
    PROP (Int.min_signed <= in_isr <= Int.max_signed)
    PARAMS (Vint (Int.repr in_isr))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (slab_alloc_allowed_coq in_isr)))
    SEP ().

Definition slab_in_critical_allowed_spec : ident * funspec :=
 DECLARE _slab_in_critical_allowed
  WITH in_critical : Z
  PRE [ tint ]
    PROP (Int.min_signed <= in_critical <= Int.max_signed)
    PARAMS (Vint (Int.repr in_critical))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (slab_in_critical_allowed_coq in_critical)))
    SEP ().

Definition return_safe_spec : ident * funspec :=
 DECLARE _return_safe
  WITH crit_flag : Z, defer_flag : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= crit_flag <= Int.max_signed;
          Int.min_signed <= defer_flag <= Int.max_signed)
    PARAMS (Vint (Int.repr crit_flag); Vint (Int.repr defer_flag))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (return_safe_coq crit_flag defer_flag)))
    SEP ().

Definition defer_safe_spec : ident * funspec :=
 DECLARE _defer_safe
  WITH in_defer : Z
  PRE [ tint ]
    PROP (Int.min_signed <= in_defer <= Int.max_signed)
    PARAMS (Vint (Int.repr in_defer))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (defer_safe_coq in_defer)))
    SEP ().

Definition asm_safe_spec : ident * funspec :=
 DECLARE _asm_safe
  WITH in_naked : Z
  PRE [ tint ]
    PROP (Int.min_signed <= in_naked <= Int.max_signed)
    PARAMS (Vint (Int.repr in_naked))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (asm_safe_coq in_naked)))
    SEP ().

Definition yield_safe_spec : ident * funspec :=
 DECLARE _yield_safe
  WITH in_async : Z
  PRE [ tint ]
    PROP (Int.min_signed <= in_async <= Int.max_signed)
    PARAMS (Vint (Int.repr in_async))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (yield_safe_coq in_async)))
    SEP ().

Definition spawn_in_isr_allowed_spec : ident * funspec :=
 DECLARE _spawn_in_isr_allowed
  WITH in_isr : Z
  PRE [ tint ]
    PROP (Int.min_signed <= in_isr <= Int.max_signed)
    PARAMS (Vint (Int.repr in_isr))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (spawn_in_isr_allowed_coq in_isr)))
    SEP ().

Definition thread_join_safe_spec : ident * funspec :=
 DECLARE _thread_join_safe
  WITH thread_state : Z
  PRE [ tint ]
    PROP (Int.min_signed <= thread_state <= Int.max_signed)
    PARAMS (Vint (Int.repr thread_state))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (thread_join_safe_coq thread_state)))
    SEP ().

Definition address_of_safe_spec : ident * funspec :=
 DECLARE _address_of_safe
  WITH is_shared_field : Z
  PRE [ tint ]
    PROP (Int.min_signed <= is_shared_field <= Int.max_signed)
    PARAMS (Vint (Int.repr is_shared_field))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (address_of_safe_coq is_shared_field)))
    SEP ().

Definition atomic_arg_ok_spec : ident * funspec :=
 DECLARE _atomic_arg_ok
  WITH is_ptr_to_int : Z
  PRE [ tint ]
    PROP (Int.min_signed <= is_ptr_to_int <= Int.max_signed)
    PARAMS (Vint (Int.repr is_ptr_to_int))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (atomic_arg_ok_coq is_ptr_to_int)))
    SEP ().

Definition Gprog : funspecs :=
  [ comptime_arg_ok_spec; static_assert_ok_spec;
    stack_frame_ok_spec;
    slab_alloc_allowed_spec; slab_in_critical_allowed_spec;
    return_safe_spec; defer_safe_spec;
    asm_safe_spec; yield_safe_spec;
    spawn_in_isr_allowed_spec; thread_join_safe_spec;
    address_of_safe_spec; atomic_arg_ok_spec ].

(* ================================================================
   Proofs.
   ================================================================ *)

Lemma body_comptime_arg_ok:
  semax_body Vprog Gprog f_comptime_arg_ok comptime_arg_ok_spec.
Proof.
  start_function.
  forward_if;
    forward;
    unfold comptime_arg_ok_coq;
    destruct (Z.eq_dec is_constant 0); try contradiction; try subst; try contradiction;
    entailer!.
Qed.

Lemma body_static_assert_ok:
  semax_body Vprog Gprog f_static_assert_ok static_assert_ok_spec.
Proof.
  start_function.
  forward_if;
    forward;
    unfold static_assert_ok_coq;
    destruct (Z.eq_dec condition 0); try contradiction; try subst; try contradiction;
    entailer!.
Qed.

Lemma body_stack_frame_ok:
  semax_body Vprog Gprog f_stack_frame_ok stack_frame_ok_spec.
Proof.
  start_function.
  forward_if;
    forward;
    unfold stack_frame_ok_coq;
    destruct (Z_le_dec frame_size limit); try lia; try contradiction;
    entailer!.
Qed.

Lemma body_slab_alloc_allowed:
  semax_body Vprog Gprog f_slab_alloc_allowed slab_alloc_allowed_spec.
Proof.
  start_function.
  forward_if;
    forward;
    unfold slab_alloc_allowed_coq;
    destruct (Z.eq_dec in_isr 0); try contradiction; try subst; try contradiction;
    entailer!.
Qed.

Lemma body_slab_in_critical_allowed:
  semax_body Vprog Gprog f_slab_in_critical_allowed slab_in_critical_allowed_spec.
Proof.
  start_function.
  forward_if;
    forward;
    unfold slab_in_critical_allowed_coq;
    destruct (Z.eq_dec in_critical 0); try contradiction; try subst; try contradiction;
    entailer!.
Qed.

Lemma body_return_safe:
  semax_body Vprog Gprog f_return_safe return_safe_spec.
Proof.
  start_function.
  forward_if;
    [ forward; unfold return_safe_coq;
      destruct (Z.eq_dec crit_flag 0); try lia; entailer! | ].
  forward_if;
    forward; unfold return_safe_coq; simpl;
    destruct (Z.eq_dec defer_flag 0); try lia; try contradiction;
    entailer!.
Qed.

Lemma body_defer_safe:
  semax_body Vprog Gprog f_defer_safe defer_safe_spec.
Proof.
  start_function.
  forward_if;
    forward;
    unfold defer_safe_coq;
    destruct (Z.eq_dec in_defer 0); try contradiction; try subst; try contradiction;
    entailer!.
Qed.

Lemma body_asm_safe:
  semax_body Vprog Gprog f_asm_safe asm_safe_spec.
Proof.
  start_function.
  forward_if;
    forward;
    unfold asm_safe_coq;
    destruct (Z.eq_dec in_naked 0); try contradiction; try subst; try contradiction;
    entailer!.
Qed.

Lemma body_yield_safe:
  semax_body Vprog Gprog f_yield_safe yield_safe_spec.
Proof.
  start_function.
  forward_if;
    forward;
    unfold yield_safe_coq;
    destruct (Z.eq_dec in_async 0); try contradiction; try subst; try contradiction;
    entailer!.
Qed.

Lemma body_spawn_in_isr_allowed:
  semax_body Vprog Gprog f_spawn_in_isr_allowed spawn_in_isr_allowed_spec.
Proof.
  start_function.
  forward_if;
    forward;
    unfold spawn_in_isr_allowed_coq;
    destruct (Z.eq_dec in_isr 0); try contradiction; try subst; try contradiction;
    entailer!.
Qed.

Lemma body_thread_join_safe:
  semax_body Vprog Gprog f_thread_join_safe thread_join_safe_spec.
Proof.
  start_function.
  forward_if;
    forward;
    unfold thread_join_safe_coq;
    destruct (Z.eq_dec thread_state 0); try contradiction; try subst; try contradiction;
    entailer!.
Qed.

Lemma body_address_of_safe:
  semax_body Vprog Gprog f_address_of_safe address_of_safe_spec.
Proof.
  start_function.
  forward_if;
    forward;
    unfold address_of_safe_coq;
    destruct (Z.eq_dec is_shared_field 0); try contradiction; try subst; try contradiction;
    entailer!.
Qed.

Lemma body_atomic_arg_ok:
  semax_body Vprog Gprog f_atomic_arg_ok atomic_arg_ok_spec.
Proof.
  start_function.
  forward_if;
    forward;
    unfold atomic_arg_ok_coq;
    destruct (Z.eq_dec is_ptr_to_int 0); try contradiction; try subst; try contradiction;
    entailer!.
Qed.

(* ================================================================
   13 proofs discharged, zero admits. Total Level-3 proofs: 22.
   ================================================================ *)
