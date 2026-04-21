(* ================================================================
   Level-3 VST proof — src/safety/handle_state.c

   This is the FIRST "real code" VST proof: the exact same .c file
   that `make zerc` links into the compiler. If someone modifies
   handle_state.c in a way that breaks the spec, `make check-vst`
   fails — closing the correctness-oracle loop.

   Functions verified (all in src/safety/handle_state.c):
     zer_handle_state_is_invalid(int state)
       returns 1 iff state ∈ {FREED (2), MAYBE_FREED (3), TRANSFERRED (4)}
     zer_handle_state_is_alive(int state)
       returns 1 iff state == ALIVE (1)
     zer_handle_state_is_freed(int state)
       returns 1 iff state == FREED (2)
     zer_handle_state_is_transferred(int state)
       returns 1 iff state == TRANSFERRED (4)

   Corresponds to:
     - Predicates in proofs/operational/lambda_zer_typing/typing.v
     - Callers (invalid):      zercheck.c:is_handle_invalid,
                                zercheck.c:is_handle_consumed,
                                zercheck_ir.c:ir_is_invalid
     - Callers (alive/freed/transferred): inline state-comparison sites
                                           throughout zercheck.c,
                                           zercheck_ir.c

   This file is clightgen'd from the SAME handle_state.c that
   Makefile CORE_SRCS compiles into zerc. The Clight AST is
   generated into src/safety/handle_state.v by the check-vst target.
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.handle_state.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

(* ---- Coq specification ---- *)

(* State constants must match handle_state.h and zercheck.h/zercheck_ir.c. *)
Definition ZER_HS_UNKNOWN     : Z := 0.
Definition ZER_HS_ALIVE       : Z := 1.
Definition ZER_HS_FREED       : Z := 2.
Definition ZER_HS_MAYBE_FREED : Z := 3.
Definition ZER_HS_TRANSFERRED : Z := 4.

Definition zer_handle_state_is_invalid_coq (state : Z) : Z :=
  if Z.eq_dec state ZER_HS_FREED then 1
  else if Z.eq_dec state ZER_HS_MAYBE_FREED then 1
  else if Z.eq_dec state ZER_HS_TRANSFERRED then 1
  else 0.

Definition zer_handle_state_is_alive_coq (state : Z) : Z :=
  if Z.eq_dec state ZER_HS_ALIVE then 1 else 0.

Definition zer_handle_state_is_freed_coq (state : Z) : Z :=
  if Z.eq_dec state ZER_HS_FREED then 1 else 0.

Definition zer_handle_state_is_transferred_coq (state : Z) : Z :=
  if Z.eq_dec state ZER_HS_TRANSFERRED then 1 else 0.

(* ---- VST funspecs ---- *)

Definition zer_handle_state_is_invalid_spec : ident * funspec :=
 DECLARE _zer_handle_state_is_invalid
  WITH state : Z
  PRE [ tint ]
    PROP (Int.min_signed <= state <= Int.max_signed)
    PARAMS (Vint (Int.repr state))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_handle_state_is_invalid_coq state)))
    SEP ().

Definition zer_handle_state_is_alive_spec : ident * funspec :=
 DECLARE _zer_handle_state_is_alive
  WITH state : Z
  PRE [ tint ]
    PROP (Int.min_signed <= state <= Int.max_signed)
    PARAMS (Vint (Int.repr state))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_handle_state_is_alive_coq state)))
    SEP ().

Definition zer_handle_state_is_freed_spec : ident * funspec :=
 DECLARE _zer_handle_state_is_freed
  WITH state : Z
  PRE [ tint ]
    PROP (Int.min_signed <= state <= Int.max_signed)
    PARAMS (Vint (Int.repr state))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_handle_state_is_freed_coq state)))
    SEP ().

Definition zer_handle_state_is_transferred_spec : ident * funspec :=
 DECLARE _zer_handle_state_is_transferred
  WITH state : Z
  PRE [ tint ]
    PROP (Int.min_signed <= state <= Int.max_signed)
    PARAMS (Vint (Int.repr state))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_handle_state_is_transferred_coq state)))
    SEP ().

Definition Gprog : funspecs :=
  [ zer_handle_state_is_invalid_spec;
    zer_handle_state_is_alive_spec;
    zer_handle_state_is_freed_spec;
    zer_handle_state_is_transferred_spec ].

(* ---- Proof ----

   The C source is three cascading early returns:
     if (state == 2) return 1;
     if (state == 3) return 1;
     if (state == 4) return 1;
     return 0;

   Each forward_if opens the "then" branch (early return) and continues
   to the next check. The combined cascade closes all goals via
   destruct + lia + entailer!. *)

Lemma body_zer_handle_state_is_invalid:
  semax_body Vprog Gprog f_zer_handle_state_is_invalid
             zer_handle_state_is_invalid_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_handle_state_is_invalid_coq;
    repeat (destruct (Z.eq_dec _ _); try lia); try entailer!.
Qed.

Lemma body_zer_handle_state_is_alive:
  semax_body Vprog Gprog f_zer_handle_state_is_alive
             zer_handle_state_is_alive_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_handle_state_is_alive_coq;
    repeat (destruct (Z.eq_dec _ _); try lia); try entailer!.
Qed.

Lemma body_zer_handle_state_is_freed:
  semax_body Vprog Gprog f_zer_handle_state_is_freed
             zer_handle_state_is_freed_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_handle_state_is_freed_coq;
    repeat (destruct (Z.eq_dec _ _); try lia); try entailer!.
Qed.

Lemma body_zer_handle_state_is_transferred:
  semax_body Vprog Gprog f_zer_handle_state_is_transferred
             zer_handle_state_is_transferred_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_handle_state_is_transferred_coq;
    repeat (destruct (Z.eq_dec _ _); try lia); try entailer!.
Qed.

(* ================================================================
   QED — the C implementation in src/safety/handle_state.c mechanically
   matches zer_handle_state_is_invalid_coq for every 32-bit signed input.

   This is Level 3 in its INTENDED form: real compiler code, not
   standalone demonstrator code. The function is:
     - Linked into `make zerc` (via Makefile CORE_SRCS)
     - Called by both zercheck.c and zercheck_ir.c
     - Verified by `make check-vst`

   Any edit to handle_state.c that breaks the spec fails check-vst
   and blocks the PR. That's the correctness-oracle loop.
   ================================================================ *)
