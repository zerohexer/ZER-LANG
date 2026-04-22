(* ================================================================
   Level-3 Phase 1 Batches 9+10+11 — verif_concurrency_rules.v

   All 11 concurrency predicates verified against typing.v sections
   C (thread/spawn), D (shared/deadlock), F (async).

   Oracle tier: schematic (typing.v has real theorems; no operational
   Iris subset yet — Phase 7 upgrades to operational).

   Functions verified:
     F: zer_yield_context_valid                              — F01-F04
     D: zer_address_of_shared_valid                          — D01
        zer_shared_in_suspend_valid                          — D02
        zer_volatile_compound_valid                          — D04
        zer_isr_main_access_valid                            — D05
     C: zer_thread_op_valid                                  — C01/C02
        zer_thread_cleanup_valid                             — C01 exit
        zer_spawn_context_valid                              — C03/C04/C05
        zer_spawn_return_safe                                — C07
        zer_spawn_arg_valid                                  — C09
        zer_spawn_arg_is_handle_rejected                     — C10

   COMPLETES FULL PHASE 1 MILESTONE — 85/85 (100%).
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.concurrency_rules.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

Definition ZER_THR_JOINED : Z := 1.
Definition ZER_THR_ALIVE  : Z := 0.

(* ---- Coq specifications ---- *)

Definition zer_thread_op_valid_coq (state joining : Z) : Z :=
  if Z.eq_dec joining 0 then 1
  else if Z.eq_dec state ZER_THR_ALIVE then 1
  else 0.

Definition zer_thread_cleanup_valid_coq (state : Z) : Z :=
  if Z.eq_dec state ZER_THR_JOINED then 1 else 0.

Definition zer_spawn_context_valid_coq (in_isr in_crit in_async : Z) : Z :=
  if Z.eq_dec in_isr 0 then
    (if Z.eq_dec in_crit 0 then
      (if Z.eq_dec in_async 0 then 1 else 0)
    else 0)
  else 0.

Definition zer_spawn_return_safe_coq (returns_resource : Z) : Z :=
  if Z.eq_dec returns_resource 0 then 1 else 0.

Definition zer_spawn_arg_valid_coq (is_shared_ptr is_value : Z) : Z :=
  if Z.eq_dec is_shared_ptr 0 then
    (if Z.eq_dec is_value 0 then 0 else 1)
  else 1.

Definition zer_spawn_arg_is_handle_rejected_coq (is_handle : Z) : Z :=
  if Z.eq_dec is_handle 0 then 1 else 0.

Definition zer_address_of_shared_valid_coq (is_shared_field : Z) : Z :=
  if Z.eq_dec is_shared_field 0 then 1 else 0.

Definition zer_shared_in_suspend_valid_coq (accesses has_yield : Z) : Z :=
  if Z.eq_dec accesses 0 then 1
  else if Z.eq_dec has_yield 0 then 1
  else 0.

Definition zer_volatile_compound_valid_coq (is_vol is_comp : Z) : Z :=
  if Z.eq_dec is_vol 0 then 1
  else if Z.eq_dec is_comp 0 then 1
  else 0.

Definition zer_isr_main_access_valid_coq (in_isr in_main is_vol : Z) : Z :=
  if Z.eq_dec is_vol 0 then
    (if Z.eq_dec in_isr 0 then 1
     else (if Z.eq_dec in_main 0 then 1 else 0))
  else 1.

Definition zer_yield_context_valid_coq (in_async in_crit in_defer : Z) : Z :=
  if Z.eq_dec in_async 0 then 0
  else (if Z.eq_dec in_crit 0 then
          (if Z.eq_dec in_defer 0 then 1 else 0)
        else 0).

(* ---- VST funspecs ---- *)

Definition zer_thread_op_valid_spec : ident * funspec :=
 DECLARE _zer_thread_op_valid
  WITH state : Z, joining : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= state <= Int.max_signed;
          Int.min_signed <= joining <= Int.max_signed)
    PARAMS (Vint (Int.repr state); Vint (Int.repr joining)) SEP ()
  POST [ tint ] PROP () RETURN (Vint (Int.repr (zer_thread_op_valid_coq state joining))) SEP ().

Definition zer_thread_cleanup_valid_spec : ident * funspec :=
 DECLARE _zer_thread_cleanup_valid
  WITH state : Z
  PRE [ tint ]
    PROP (Int.min_signed <= state <= Int.max_signed)
    PARAMS (Vint (Int.repr state)) SEP ()
  POST [ tint ] PROP () RETURN (Vint (Int.repr (zer_thread_cleanup_valid_coq state))) SEP ().

Definition zer_spawn_context_valid_spec : ident * funspec :=
 DECLARE _zer_spawn_context_valid
  WITH in_isr : Z, in_crit : Z, in_async : Z
  PRE [ tint, tint, tint ]
    PROP (Int.min_signed <= in_isr <= Int.max_signed;
          Int.min_signed <= in_crit <= Int.max_signed;
          Int.min_signed <= in_async <= Int.max_signed)
    PARAMS (Vint (Int.repr in_isr); Vint (Int.repr in_crit); Vint (Int.repr in_async)) SEP ()
  POST [ tint ] PROP () RETURN (Vint (Int.repr (zer_spawn_context_valid_coq in_isr in_crit in_async))) SEP ().

Definition zer_spawn_return_safe_spec : ident * funspec :=
 DECLARE _zer_spawn_return_safe
  WITH returns_resource : Z
  PRE [ tint ]
    PROP (Int.min_signed <= returns_resource <= Int.max_signed)
    PARAMS (Vint (Int.repr returns_resource)) SEP ()
  POST [ tint ] PROP () RETURN (Vint (Int.repr (zer_spawn_return_safe_coq returns_resource))) SEP ().

Definition zer_spawn_arg_valid_spec : ident * funspec :=
 DECLARE _zer_spawn_arg_valid
  WITH is_shared_ptr : Z, is_value : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= is_shared_ptr <= Int.max_signed;
          Int.min_signed <= is_value <= Int.max_signed)
    PARAMS (Vint (Int.repr is_shared_ptr); Vint (Int.repr is_value)) SEP ()
  POST [ tint ] PROP () RETURN (Vint (Int.repr (zer_spawn_arg_valid_coq is_shared_ptr is_value))) SEP ().

Definition zer_spawn_arg_is_handle_rejected_spec : ident * funspec :=
 DECLARE _zer_spawn_arg_is_handle_rejected
  WITH is_handle : Z
  PRE [ tint ]
    PROP (Int.min_signed <= is_handle <= Int.max_signed)
    PARAMS (Vint (Int.repr is_handle)) SEP ()
  POST [ tint ] PROP () RETURN (Vint (Int.repr (zer_spawn_arg_is_handle_rejected_coq is_handle))) SEP ().

Definition zer_address_of_shared_valid_spec : ident * funspec :=
 DECLARE _zer_address_of_shared_valid
  WITH is_shared_field : Z
  PRE [ tint ]
    PROP (Int.min_signed <= is_shared_field <= Int.max_signed)
    PARAMS (Vint (Int.repr is_shared_field)) SEP ()
  POST [ tint ] PROP () RETURN (Vint (Int.repr (zer_address_of_shared_valid_coq is_shared_field))) SEP ().

Definition zer_shared_in_suspend_valid_spec : ident * funspec :=
 DECLARE _zer_shared_in_suspend_valid
  WITH accesses : Z, has_yield : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= accesses <= Int.max_signed;
          Int.min_signed <= has_yield <= Int.max_signed)
    PARAMS (Vint (Int.repr accesses); Vint (Int.repr has_yield)) SEP ()
  POST [ tint ] PROP () RETURN (Vint (Int.repr (zer_shared_in_suspend_valid_coq accesses has_yield))) SEP ().

Definition zer_volatile_compound_valid_spec : ident * funspec :=
 DECLARE _zer_volatile_compound_valid
  WITH is_vol : Z, is_comp : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= is_vol <= Int.max_signed;
          Int.min_signed <= is_comp <= Int.max_signed)
    PARAMS (Vint (Int.repr is_vol); Vint (Int.repr is_comp)) SEP ()
  POST [ tint ] PROP () RETURN (Vint (Int.repr (zer_volatile_compound_valid_coq is_vol is_comp))) SEP ().

Definition zer_isr_main_access_valid_spec : ident * funspec :=
 DECLARE _zer_isr_main_access_valid
  WITH in_isr : Z, in_main : Z, is_vol : Z
  PRE [ tint, tint, tint ]
    PROP (Int.min_signed <= in_isr <= Int.max_signed;
          Int.min_signed <= in_main <= Int.max_signed;
          Int.min_signed <= is_vol <= Int.max_signed)
    PARAMS (Vint (Int.repr in_isr); Vint (Int.repr in_main); Vint (Int.repr is_vol)) SEP ()
  POST [ tint ] PROP () RETURN (Vint (Int.repr (zer_isr_main_access_valid_coq in_isr in_main is_vol))) SEP ().

Definition zer_yield_context_valid_spec : ident * funspec :=
 DECLARE _zer_yield_context_valid
  WITH in_async : Z, in_crit : Z, in_defer : Z
  PRE [ tint, tint, tint ]
    PROP (Int.min_signed <= in_async <= Int.max_signed;
          Int.min_signed <= in_crit <= Int.max_signed;
          Int.min_signed <= in_defer <= Int.max_signed)
    PARAMS (Vint (Int.repr in_async); Vint (Int.repr in_crit); Vint (Int.repr in_defer)) SEP ()
  POST [ tint ] PROP () RETURN (Vint (Int.repr (zer_yield_context_valid_coq in_async in_crit in_defer))) SEP ().

Definition Gprog : funspecs :=
  [ zer_thread_op_valid_spec;
    zer_thread_cleanup_valid_spec;
    zer_spawn_context_valid_spec;
    zer_spawn_return_safe_spec;
    zer_spawn_arg_valid_spec;
    zer_spawn_arg_is_handle_rejected_spec;
    zer_address_of_shared_valid_spec;
    zer_shared_in_suspend_valid_spec;
    zer_volatile_compound_valid_spec;
    zer_isr_main_access_valid_spec;
    zer_yield_context_valid_spec ].

(* ---- Proofs (all use Pattern C — flat cascade with Z.eq_dec) ---- *)

Ltac prove_pred_c := start_function;
  repeat forward_if;
    forward;
    repeat (first [ unfold zer_thread_op_valid_coq
                  | unfold zer_thread_cleanup_valid_coq, ZER_THR_JOINED
                  | unfold zer_spawn_context_valid_coq
                  | unfold zer_spawn_return_safe_coq
                  | unfold zer_spawn_arg_valid_coq
                  | unfold zer_spawn_arg_is_handle_rejected_coq
                  | unfold zer_address_of_shared_valid_coq
                  | unfold zer_shared_in_suspend_valid_coq
                  | unfold zer_volatile_compound_valid_coq
                  | unfold zer_isr_main_access_valid_coq
                  | unfold zer_yield_context_valid_coq ]);
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.

Lemma body_zer_thread_op_valid:
  semax_body Vprog Gprog f_zer_thread_op_valid zer_thread_op_valid_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_thread_op_valid_coq, ZER_THR_ALIVE;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_thread_cleanup_valid:
  semax_body Vprog Gprog f_zer_thread_cleanup_valid zer_thread_cleanup_valid_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_thread_cleanup_valid_coq, ZER_THR_JOINED;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_spawn_context_valid:
  semax_body Vprog Gprog f_zer_spawn_context_valid zer_spawn_context_valid_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_spawn_context_valid_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_spawn_return_safe:
  semax_body Vprog Gprog f_zer_spawn_return_safe zer_spawn_return_safe_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_spawn_return_safe_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_spawn_arg_valid:
  semax_body Vprog Gprog f_zer_spawn_arg_valid zer_spawn_arg_valid_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_spawn_arg_valid_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_spawn_arg_is_handle_rejected:
  semax_body Vprog Gprog f_zer_spawn_arg_is_handle_rejected zer_spawn_arg_is_handle_rejected_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_spawn_arg_is_handle_rejected_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_address_of_shared_valid:
  semax_body Vprog Gprog f_zer_address_of_shared_valid zer_address_of_shared_valid_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_address_of_shared_valid_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_shared_in_suspend_valid:
  semax_body Vprog Gprog f_zer_shared_in_suspend_valid zer_shared_in_suspend_valid_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_shared_in_suspend_valid_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_volatile_compound_valid:
  semax_body Vprog Gprog f_zer_volatile_compound_valid zer_volatile_compound_valid_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_volatile_compound_valid_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_isr_main_access_valid:
  semax_body Vprog Gprog f_zer_isr_main_access_valid zer_isr_main_access_valid_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_isr_main_access_valid_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_yield_context_valid:
  semax_body Vprog Gprog f_zer_yield_context_valid zer_yield_context_valid_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_yield_context_valid_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

(* ================================================================
   11 concurrency predicates VST-verified. Phase 1 Batches 9+10+11.
   Total: 85 verified compiler functions.
   FULL PHASE 1 COMPLETE — 85/85 (100%).
   ================================================================ *)
