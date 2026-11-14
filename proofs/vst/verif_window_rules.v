(* ================================================================
   Level-3 — verif_window_rules.v

   The scoped-spawn WINDOW decisions (BUG-1648 / BUG-1651), extracted
   into src/safety/concurrency_rules.c and called from checker.c.

   Oracle tier: OPERATIONAL — proofs/operational/lambda_zer_concurrency/
   window_cfg.v (`window_sound` over an arbitrary control-flow graph;
   instances `closed_window_ok`, `tail_window_ok`). Each predicate here
   decides one hypothesis of those instances:

     zer_window_join_releases     a CLOSED window is released by its textual
                                  join; an OPEN one only by its TAIL join
                                  (tail_window_ok.Hentry: branch depth 0,
                                  outside @once, after the last goto / label)
     zer_window_stmt_leaves       which statements leave a window's interior
                                  (closed_window_ok.Hlive_exit)
     zer_window_counts_at_spawn   an open window in a latched walk is counted
                                  from the function entry, not at its spawn

   The specs are written against that oracle, not against the C.

   Imports the SAME clightgen output as verif_concurrency_rules.v
   (zer_safety.concurrency_rules); this file proves only the three window
   functions, so the two verif files are independent.
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.concurrency_rules.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

(* ---- Coq specifications (from window_cfg.v) ---- *)

(* closed -> the textual join releases; otherwise the tail-join conditions. *)
Definition zer_window_join_releases_coq
    (closed branch_depth in_once after_last_jump : Z) : Z :=
  if Z.eq_dec closed 0 then
    (if Z.eq_dec branch_depth 0 then
      (if Z.eq_dec in_once 0 then
        (if Z.eq_dec after_last_jump 0 then 0 else 1)
      else 0)
    else 0)
  else 1.

(* return / goto always leave; break / continue only at loop depth 0. *)
Definition zer_window_stmt_leaves_coq
    (is_return_or_goto is_loop_jump loop_depth : Z) : Z :=
  if Z.eq_dec is_return_or_goto 0 then
    (if Z.eq_dec is_loop_jump 0 then 0
     else (if Z.eq_dec loop_depth 0 then 1 else 0))
  else 1.

Definition zer_window_counts_at_spawn_coq (closed latched : Z) : Z :=
  if Z.eq_dec closed 0 then
    (if Z.eq_dec latched 0 then 1 else 0)
  else 1.

(* ---- VST funspecs ---- *)

Definition zer_window_join_releases_spec : ident * funspec :=
 DECLARE _zer_window_join_releases
  WITH closed : Z, branch_depth : Z, in_once : Z, after_last_jump : Z
  PRE [ tint, tint, tint, tint ]
    PROP (Int.min_signed <= closed <= Int.max_signed;
          Int.min_signed <= branch_depth <= Int.max_signed;
          Int.min_signed <= in_once <= Int.max_signed;
          Int.min_signed <= after_last_jump <= Int.max_signed)
    PARAMS (Vint (Int.repr closed); Vint (Int.repr branch_depth);
            Vint (Int.repr in_once); Vint (Int.repr after_last_jump)) SEP ()
  POST [ tint ] PROP ()
    RETURN (Vint (Int.repr (zer_window_join_releases_coq closed branch_depth in_once after_last_jump)))
    SEP ().

Definition zer_window_stmt_leaves_spec : ident * funspec :=
 DECLARE _zer_window_stmt_leaves
  WITH is_return_or_goto : Z, is_loop_jump : Z, loop_depth : Z
  PRE [ tint, tint, tint ]
    PROP (Int.min_signed <= is_return_or_goto <= Int.max_signed;
          Int.min_signed <= is_loop_jump <= Int.max_signed;
          Int.min_signed <= loop_depth <= Int.max_signed)
    PARAMS (Vint (Int.repr is_return_or_goto); Vint (Int.repr is_loop_jump);
            Vint (Int.repr loop_depth)) SEP ()
  POST [ tint ] PROP ()
    RETURN (Vint (Int.repr (zer_window_stmt_leaves_coq is_return_or_goto is_loop_jump loop_depth)))
    SEP ().

Definition zer_window_counts_at_spawn_spec : ident * funspec :=
 DECLARE _zer_window_counts_at_spawn
  WITH closed : Z, latched : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= closed <= Int.max_signed;
          Int.min_signed <= latched <= Int.max_signed)
    PARAMS (Vint (Int.repr closed); Vint (Int.repr latched)) SEP ()
  POST [ tint ] PROP ()
    RETURN (Vint (Int.repr (zer_window_counts_at_spawn_coq closed latched)))
    SEP ().

Definition Gprog : funspecs :=
  [ zer_window_join_releases_spec;
    zer_window_stmt_leaves_spec;
    zer_window_counts_at_spawn_spec ].

(* ---- Proofs (the same cascade tactic as verif_concurrency_rules.v) ---- *)

Lemma body_zer_window_join_releases:
  semax_body Vprog Gprog f_zer_window_join_releases zer_window_join_releases_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_window_join_releases_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_window_stmt_leaves:
  semax_body Vprog Gprog f_zer_window_stmt_leaves zer_window_stmt_leaves_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_window_stmt_leaves_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_window_counts_at_spawn:
  semax_body Vprog Gprog f_zer_window_counts_at_spawn zer_window_counts_at_spawn_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_window_counts_at_spawn_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.
