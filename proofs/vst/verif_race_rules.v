(* ================================================================
   Level-3 — verif_race_rules.v

   THE PAIRWISE VERDICT of the data-race judgement (zer_pair_ordered),
   extracted into src/safety/concurrency_rules.c and called from checker.c
   (sync_sets_conflict) for every two access records of two thread
   instances.

   Oracle tier: OPERATIONAL — proofs/operational/lambda_zer_concurrency/
   race_free.v: `race_free` (a program whose every conflicting pair is
   ordered has no data race in any reachable configuration) and
   `pair_ordered_meaning` (what this function's 1 means: no conflict, or
   ordered by a common lock, two different held values of one flag, or a
   thread that is not running).

   The `_coq` definition is imported from concurrency_decisions.v, the file
   race_free.v proves its meaning lemma about — no second copy.
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.concurrency_rules.
From zer_conc Require Import concurrency_decisions.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

Definition zer_pair_ordered_spec : ident * funspec :=
 DECLARE _zer_pair_ordered
  WITH may_alias : Z, a_write : Z, b_write : Z, both_atomic : Z,
       common_lock : Z, tok_differ : Z, apart : Z
  PRE [ tint, tint, tint, tint, tint, tint, tint ]
    PROP (Int.min_signed <= may_alias <= Int.max_signed;
          Int.min_signed <= a_write <= Int.max_signed;
          Int.min_signed <= b_write <= Int.max_signed;
          Int.min_signed <= both_atomic <= Int.max_signed;
          Int.min_signed <= common_lock <= Int.max_signed;
          Int.min_signed <= tok_differ <= Int.max_signed;
          Int.min_signed <= apart <= Int.max_signed)
    PARAMS (Vint (Int.repr may_alias); Vint (Int.repr a_write); Vint (Int.repr b_write);
            Vint (Int.repr both_atomic); Vint (Int.repr common_lock);
            Vint (Int.repr tok_differ); Vint (Int.repr apart))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_pair_ordered_coq may_alias a_write b_write both_atomic
                                                 common_lock tok_differ apart)))
    SEP ().

Definition Gprog : funspecs := [ zer_pair_ordered_spec ].

Lemma body_zer_pair_ordered:
  semax_body Vprog Gprog f_zer_pair_ordered zer_pair_ordered_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_pair_ordered_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.
