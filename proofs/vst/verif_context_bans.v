(* ================================================================
   Level-3 VST proofs — src/safety/context_bans.c

   Six context-ban predicates. Each takes context flags (defer_depth,
   critical_depth, in_loop, in_naked) as ints and returns 0/1 for
   whether the corresponding control-flow statement is allowed.

   Functions verified (all linked into zerc):
     zer_return_allowed_in_context(defer_depth, critical_depth)
     zer_break_allowed_in_context(defer_depth, critical_depth, in_loop)
     zer_continue_allowed_in_context(defer_depth, critical_depth, in_loop)
     zer_goto_allowed_in_context(defer_depth, critical_depth)
     zer_defer_allowed_in_context(defer_depth)
     zer_asm_allowed_in_context(in_naked)

   Callers: checker.c NODE_RETURN / BREAK / CONTINUE / GOTO / DEFER /
   ASM handlers. All delegate to these predicates.

   Rules from CLAUDE.md "Ban Decision Framework":
     - return: banned in defer (corrupts cleanup) and @critical
       (skips interrupt re-enable)
     - break/continue: banned in defer, @critical; must be in a loop
     - goto: banned in defer, @critical
     - defer: no nested defer
     - asm: only in `naked` functions (no prologue/epilogue)
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.context_bans.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

(* ---- Coq specifications ---- *)

Definition zer_return_allowed_in_context_coq (dd cd : Z) : Z :=
  if Z_gt_dec dd 0 then 0
  else if Z_gt_dec cd 0 then 0
  else 1.

Definition zer_break_allowed_in_context_coq (dd cd il : Z) : Z :=
  if Z_gt_dec dd 0 then 0
  else if Z_gt_dec cd 0 then 0
  else if Z.eq_dec il 0 then 0
  else 1.

Definition zer_continue_allowed_in_context_coq (dd cd il : Z) : Z :=
  zer_break_allowed_in_context_coq dd cd il.

Definition zer_goto_allowed_in_context_coq (dd cd : Z) : Z :=
  zer_return_allowed_in_context_coq dd cd.

Definition zer_defer_allowed_in_context_coq (dd : Z) : Z :=
  if Z_gt_dec dd 0 then 0 else 1.

Definition zer_asm_allowed_in_context_coq (in_naked : Z) : Z :=
  if Z.eq_dec in_naked 0 then 0 else 1.

(* ---- VST funspecs ---- *)

Definition zer_return_allowed_in_context_spec : ident * funspec :=
 DECLARE _zer_return_allowed_in_context
  WITH dd : Z, cd : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= dd <= Int.max_signed;
          Int.min_signed <= cd <= Int.max_signed)
    PARAMS (Vint (Int.repr dd); Vint (Int.repr cd))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_return_allowed_in_context_coq dd cd)))
    SEP ().

Definition zer_break_allowed_in_context_spec : ident * funspec :=
 DECLARE _zer_break_allowed_in_context
  WITH dd : Z, cd : Z, il : Z
  PRE [ tint, tint, tint ]
    PROP (Int.min_signed <= dd <= Int.max_signed;
          Int.min_signed <= cd <= Int.max_signed;
          Int.min_signed <= il <= Int.max_signed)
    PARAMS (Vint (Int.repr dd); Vint (Int.repr cd); Vint (Int.repr il))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_break_allowed_in_context_coq dd cd il)))
    SEP ().

Definition zer_continue_allowed_in_context_spec : ident * funspec :=
 DECLARE _zer_continue_allowed_in_context
  WITH dd : Z, cd : Z, il : Z
  PRE [ tint, tint, tint ]
    PROP (Int.min_signed <= dd <= Int.max_signed;
          Int.min_signed <= cd <= Int.max_signed;
          Int.min_signed <= il <= Int.max_signed)
    PARAMS (Vint (Int.repr dd); Vint (Int.repr cd); Vint (Int.repr il))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_continue_allowed_in_context_coq dd cd il)))
    SEP ().

Definition zer_goto_allowed_in_context_spec : ident * funspec :=
 DECLARE _zer_goto_allowed_in_context
  WITH dd : Z, cd : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= dd <= Int.max_signed;
          Int.min_signed <= cd <= Int.max_signed)
    PARAMS (Vint (Int.repr dd); Vint (Int.repr cd))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_goto_allowed_in_context_coq dd cd)))
    SEP ().

Definition zer_defer_allowed_in_context_spec : ident * funspec :=
 DECLARE _zer_defer_allowed_in_context
  WITH dd : Z
  PRE [ tint ]
    PROP (Int.min_signed <= dd <= Int.max_signed)
    PARAMS (Vint (Int.repr dd))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_defer_allowed_in_context_coq dd)))
    SEP ().

Definition zer_asm_allowed_in_context_spec : ident * funspec :=
 DECLARE _zer_asm_allowed_in_context
  WITH in_naked : Z
  PRE [ tint ]
    PROP (Int.min_signed <= in_naked <= Int.max_signed)
    PARAMS (Vint (Int.repr in_naked))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_asm_allowed_in_context_coq in_naked)))
    SEP ().

Definition Gprog : funspecs :=
  [ zer_return_allowed_in_context_spec;
    zer_break_allowed_in_context_spec;
    zer_continue_allowed_in_context_spec;
    zer_goto_allowed_in_context_spec;
    zer_defer_allowed_in_context_spec;
    zer_asm_allowed_in_context_spec ].

(* ---- Proofs ---- *)

Lemma body_zer_return_allowed_in_context:
  semax_body Vprog Gprog f_zer_return_allowed_in_context
             zer_return_allowed_in_context_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_return_allowed_in_context_coq;
    repeat (first [ destruct (Z_gt_dec _ _)
                  | destruct (Z.eq_dec _ _) ]; try lia);
    try entailer!.
Qed.

Lemma body_zer_break_allowed_in_context:
  semax_body Vprog Gprog f_zer_break_allowed_in_context
             zer_break_allowed_in_context_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_break_allowed_in_context_coq;
    repeat (first [ destruct (Z_gt_dec _ _)
                  | destruct (Z.eq_dec _ _) ]; try lia);
    try entailer!.
Qed.

Lemma body_zer_continue_allowed_in_context:
  semax_body Vprog Gprog f_zer_continue_allowed_in_context
             zer_continue_allowed_in_context_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_continue_allowed_in_context_coq,
           zer_break_allowed_in_context_coq;
    repeat (first [ destruct (Z_gt_dec _ _)
                  | destruct (Z.eq_dec _ _) ]; try lia);
    try entailer!.
Qed.

Lemma body_zer_goto_allowed_in_context:
  semax_body Vprog Gprog f_zer_goto_allowed_in_context
             zer_goto_allowed_in_context_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_goto_allowed_in_context_coq,
           zer_return_allowed_in_context_coq;
    repeat (first [ destruct (Z_gt_dec _ _)
                  | destruct (Z.eq_dec _ _) ]; try lia);
    try entailer!.
Qed.

Lemma body_zer_defer_allowed_in_context:
  semax_body Vprog Gprog f_zer_defer_allowed_in_context
             zer_defer_allowed_in_context_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_defer_allowed_in_context_coq;
    repeat (destruct (Z_gt_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_asm_allowed_in_context:
  semax_body Vprog Gprog f_zer_asm_allowed_in_context
             zer_asm_allowed_in_context_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_asm_allowed_in_context_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

(* ================================================================
   6 context-ban predicates VST-verified.
   Total Level-3 verified compiler functions: 25
   (4 handle state + 3 range + 7 type kind + 5 coerce + 6 context).
   ================================================================ *)
