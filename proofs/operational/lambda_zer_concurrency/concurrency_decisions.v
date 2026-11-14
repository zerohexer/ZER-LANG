(* ================================================================
   lambda_zer_concurrency / concurrency_decisions

   THE ONE SOURCE of the decision functions the compiler runs for the
   concurrency rules (src/safety/concurrency_rules.c).  Each `_coq` definition
   below is used twice, and both uses import THIS file:

     - the ORACLE files (window_cfg.v, lock_region.v) prove what each decision
       MEANS in terms of the hypotheses of their soundness theorems;
     - the VST files (proofs/vst/verif_window_rules.v, verif_lock_rules.v) prove
       that the C linked into zerc returns exactly these values.

   Until 2026-11-14 the window definitions were written out in both places and
   nothing kept the copies equal: an edit to one would have left both gates
   green while "the C satisfies the proven rule" stopped being what was proved.
   There is nothing to keep equal now.

   Deliberately plain Coq (ZArith only): the VST image and the Iris image both
   compile it, and neither a VST nor an stdpp notation can change what it says.
   0 = false, anything else = true (C `int`).
   ================================================================ *)
From Coq Require Import ZArith.
Open Scope Z_scope.

(* ---- the scoped-spawn WINDOW (meaning: window_cfg.v) ---- *)

Definition zer_window_join_releases_coq
    (closed branch_depth in_once after_last_jump : Z) : Z :=
  if Z.eq_dec closed 0 then
    (if Z.eq_dec branch_depth 0 then
      (if Z.eq_dec in_once 0 then
        (if Z.eq_dec after_last_jump 0 then 0 else 1)
      else 0)
    else 0)
  else 1.

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

(* ---- the SPIN-LOCK REGION (meaning: lock_region.v) ---- *)

(* A release statement is in role iff it closes the region its own acquire
   opened and that region has not been released already (by a `defer`). *)
Definition zer_lock_release_in_role_coq (closes_own_region already_released : Z) : Z :=
  if Z.eq_dec closes_own_region 0 then 0
  else (if Z.eq_dec already_released 0 then 1 else 0).

(* An atomic write of a lock word is in role iff it is the acquire test or the
   in-role release being walked, and the word is a whole global. *)
Definition zer_lock_write_in_role_coq (sanctioned whole_word : Z) : Z :=
  if Z.eq_dec sanctioned 0 then 0
  else (if Z.eq_dec whole_word 0 then 0 else 1).

(* A local copy of the flag is FRESH iff its own declaration read it, the local
   is not static, and every copy the initializer went through is itself in
   this section. *)
Definition zer_rmw_copy_fresh_coq (by_own_declaration is_static sources_in_section : Z) : Z :=
  if Z.eq_dec by_own_declaration 0 then 0
  else (if Z.eq_dec is_static 0 then
          (if Z.eq_dec sources_in_section 0 then 0 else 1)
        else 0).

(* ... and it may be written back in a statement iff it is fresh AND that
   statement sits in the region the copy was taken in. *)
Definition zer_rmw_copy_in_section_coq (fresh same_region : Z) : Z :=
  if Z.eq_dec fresh 0 then 0
  else (if Z.eq_dec same_region 0 then 0 else 1).

(* A read-modify-write is inside ONE critical section iff it is one statement,
   or every local copy it writes back is in this section. *)
Definition zer_rmw_one_section_coq (one_statement copies_in_section : Z) : Z :=
  if Z.eq_dec one_statement 0 then
    (if Z.eq_dec copies_in_section 0 then 0 else 1)
  else 1.

(* A held lock covers a read-modify-write only if it is inside one section. *)
Definition zer_rmw_lock_covers_coq (lock_held one_section : Z) : Z :=
  if Z.eq_dec lock_held 0 then 0
  else (if Z.eq_dec one_section 0 then 0 else 1).

(* The covered read-modify-write is exempt from the lost-update rule iff it was
   seen under exactly one lock and no other thread stores the flag outside
   that lock. *)
Definition zer_rmw_locked_exempt_coq (seen lock_held mixed store_outside : Z) : Z :=
  if Z.eq_dec seen 0 then 0
  else (if Z.eq_dec lock_held 0 then 0
        else (if Z.eq_dec mixed 0 then
                (if Z.eq_dec store_outside 0 then 1 else 0)
              else 0)).

(* A cell is a LOCK WORD iff it is a whole global every thread shares and the
   function does not bind its name locally. *)
Definition zer_lock_word_ok_coq (whole_word thread_local shadowed : Z) : Z :=
  if Z.eq_dec whole_word 0 then 0
  else (if Z.eq_dec thread_local 0 then
          (if Z.eq_dec shadowed 0 then 1 else 0)
        else 0).

(* Every acquire of one word tests the same FREE value: the first one fixes it. *)
Definition zer_lock_acquire_agrees_coq (first same_free : Z) : Z :=
  if Z.eq_dec first 0 then
    (if Z.eq_dec same_free 0 then 0 else 1)
  else 1.

(* A `defer` body registered in the block that opened the region runs with the
   lock iff the release was deferred before it (defers fire last-in first-out
   at the block's exit, after every direct statement). *)
Definition zer_lock_covers_defer_coq (opened_here release_deferred : Z) : Z :=
  if Z.eq_dec opened_here 0 then 1
  else (if Z.eq_dec release_deferred 0 then 0 else 1).

(* ---- flag HAND-OFF decisions (BUG-1664; meaning lemmas: flag_handoff.v) ---- *)

(* What a comparison of an atomic load with a literal tells its thread on the
   branch taken: 1 = it holds the literal, 2 = it holds the other value of a
   two-valued flag, 0 = nothing. *)
Definition zer_flag_obs_coq (agrees two_valued : Z) : Z :=
  if Z.eq_dec agrees 0 then
    (if Z.eq_dec two_valued 0 then 0 else 2)
  else 1.

(* Under which held value a store is made: 1 = an observed one, 2 = the initial
   one (the thread's first store, made once), 0 = none. *)
Definition zer_flag_store_from_coq
    (holds_explicit entry_fn in_loop in_jump_fn stored_before : Z) : Z :=
  if Z.eq_dec holds_explicit 0 then
    (if Z.eq_dec entry_fn 0 then 0
     else (if Z.eq_dec in_loop 0 then
             (if Z.eq_dec in_jump_fn 0 then
                (if Z.eq_dec stored_before 0 then 2 else 0)
              else 0)
           else 0))
  else 1.

(* A value that is stored FROM has one claiming thread, running once. *)
Definition zer_flag_claim_unique_coq (single_thread repeated : Z) : Z :=
  if Z.eq_dec single_thread 0 then 0
  else (if Z.eq_dec repeated 0 then 1 else 0).

(* Two accesses are ordered by a flag iff it keeps the discipline and they are
   made under two different held values. *)
Definition zer_flag_orders_coq (flag_valid values_differ : Z) : Z :=
  if Z.eq_dec flag_valid 0 then 0
  else (if Z.eq_dec values_differ 0 then 0 else 1).
