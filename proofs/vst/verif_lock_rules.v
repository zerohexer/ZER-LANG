(* ================================================================
   Level-3 — verif_lock_rules.v

   The SPIN-LOCK REGION decisions (BUG-1663), extracted into
   src/safety/concurrency_rules.c and called from checker.c.

   Oracle tier: OPERATIONAL — proofs/operational/lambda_zer_concurrency/
   lock_region.v (`mutual_exclusion`, `rmw_exact`, and one counterexample trace
   per hypothesis dropped). Each predicate here decides one piece of a
   hypothesis of those theorems:

     zer_lock_word_ok           the cell is ONE word every thread shares
                                                            (one_word, H_word)
     zer_lock_acquire_agrees    every acquire tests the same free value
                                                               (agree, H_acq)
     zer_lock_release_in_role   a release is in role iff it closes the region
     zer_lock_write_in_role     its own acquire opened, once, on a whole word
                                                              (in_role, H_rel)
     zer_lock_covers_defer      a `defer` body of the region's own block runs
                                with the lock only if the release was deferred
                                before it                    (guarded, H_acc)
     zer_rmw_copy_fresh         a local copy of the flag may be written back
     zer_rmw_copy_in_section    only in the section it was read in
     zer_rmw_one_section        an update is inside ONE critical section
     zer_rmw_lock_covers        ... and only then does a held lock cover it
                                                           (continuity, H_cont)
     zer_rmw_locked_exempt      no other thread stores the flag outside the
                                lock                         (guarded, H_acc)

   The `_coq` definitions are NOT written here: they are imported from
   concurrency_decisions.v, the file lock_region.v proves its meaning lemmas
   about. What VST proves below is therefore "the C that is linked into zerc
   returns the value of the definition the oracle reasons about", with no
   second copy to drift.

   Imports the SAME clightgen output as verif_concurrency_rules.v and
   verif_window_rules.v (zer_safety.concurrency_rules); the three verif files
   are independent.
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.concurrency_rules.
From zer_conc Require Import concurrency_decisions.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

(* ---- VST funspecs ---- *)

Definition zer_lock_release_in_role_spec : ident * funspec :=
 DECLARE _zer_lock_release_in_role
  WITH closes_own_region : Z, already_released : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= closes_own_region <= Int.max_signed;
          Int.min_signed <= already_released <= Int.max_signed)
    PARAMS (Vint (Int.repr closes_own_region); Vint (Int.repr already_released)) SEP ()
  POST [ tint ] PROP ()
    RETURN (Vint (Int.repr (zer_lock_release_in_role_coq closes_own_region already_released)))
    SEP ().

Definition zer_lock_write_in_role_spec : ident * funspec :=
 DECLARE _zer_lock_write_in_role
  WITH sanctioned : Z, whole_word : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= sanctioned <= Int.max_signed;
          Int.min_signed <= whole_word <= Int.max_signed)
    PARAMS (Vint (Int.repr sanctioned); Vint (Int.repr whole_word)) SEP ()
  POST [ tint ] PROP ()
    RETURN (Vint (Int.repr (zer_lock_write_in_role_coq sanctioned whole_word)))
    SEP ().

Definition zer_rmw_copy_fresh_spec : ident * funspec :=
 DECLARE _zer_rmw_copy_fresh
  WITH by_own_declaration : Z, is_static : Z, sources_in_section : Z
  PRE [ tint, tint, tint ]
    PROP (Int.min_signed <= by_own_declaration <= Int.max_signed;
          Int.min_signed <= is_static <= Int.max_signed;
          Int.min_signed <= sources_in_section <= Int.max_signed)
    PARAMS (Vint (Int.repr by_own_declaration); Vint (Int.repr is_static); Vint (Int.repr sources_in_section)) SEP ()
  POST [ tint ] PROP ()
    RETURN (Vint (Int.repr (zer_rmw_copy_fresh_coq by_own_declaration is_static sources_in_section)))
    SEP ().

Definition zer_rmw_copy_in_section_spec : ident * funspec :=
 DECLARE _zer_rmw_copy_in_section
  WITH fresh : Z, same_region : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= fresh <= Int.max_signed;
          Int.min_signed <= same_region <= Int.max_signed)
    PARAMS (Vint (Int.repr fresh); Vint (Int.repr same_region)) SEP ()
  POST [ tint ] PROP ()
    RETURN (Vint (Int.repr (zer_rmw_copy_in_section_coq fresh same_region)))
    SEP ().

Definition zer_rmw_one_section_spec : ident * funspec :=
 DECLARE _zer_rmw_one_section
  WITH one_statement : Z, copies_in_section : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= one_statement <= Int.max_signed;
          Int.min_signed <= copies_in_section <= Int.max_signed)
    PARAMS (Vint (Int.repr one_statement); Vint (Int.repr copies_in_section)) SEP ()
  POST [ tint ] PROP ()
    RETURN (Vint (Int.repr (zer_rmw_one_section_coq one_statement copies_in_section)))
    SEP ().

Definition zer_rmw_lock_covers_spec : ident * funspec :=
 DECLARE _zer_rmw_lock_covers
  WITH lock_held : Z, one_section : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= lock_held <= Int.max_signed;
          Int.min_signed <= one_section <= Int.max_signed)
    PARAMS (Vint (Int.repr lock_held); Vint (Int.repr one_section)) SEP ()
  POST [ tint ] PROP ()
    RETURN (Vint (Int.repr (zer_rmw_lock_covers_coq lock_held one_section)))
    SEP ().

Definition zer_rmw_locked_exempt_spec : ident * funspec :=
 DECLARE _zer_rmw_locked_exempt
  WITH seen : Z, lock_held : Z, mixed : Z, store_outside : Z
  PRE [ tint, tint, tint, tint ]
    PROP (Int.min_signed <= seen <= Int.max_signed;
          Int.min_signed <= lock_held <= Int.max_signed;
          Int.min_signed <= mixed <= Int.max_signed;
          Int.min_signed <= store_outside <= Int.max_signed)
    PARAMS (Vint (Int.repr seen); Vint (Int.repr lock_held); Vint (Int.repr mixed); Vint (Int.repr store_outside)) SEP ()
  POST [ tint ] PROP ()
    RETURN (Vint (Int.repr (zer_rmw_locked_exempt_coq seen lock_held mixed store_outside)))
    SEP ().

Definition zer_lock_word_ok_spec : ident * funspec :=
 DECLARE _zer_lock_word_ok
  WITH whole_word : Z, thread_local : Z, shadowed : Z
  PRE [ tint, tint, tint ]
    PROP (Int.min_signed <= whole_word <= Int.max_signed;
          Int.min_signed <= thread_local <= Int.max_signed;
          Int.min_signed <= shadowed <= Int.max_signed)
    PARAMS (Vint (Int.repr whole_word); Vint (Int.repr thread_local); Vint (Int.repr shadowed)) SEP ()
  POST [ tint ] PROP ()
    RETURN (Vint (Int.repr (zer_lock_word_ok_coq whole_word thread_local shadowed)))
    SEP ().

Definition zer_lock_acquire_agrees_spec : ident * funspec :=
 DECLARE _zer_lock_acquire_agrees
  WITH first : Z, same_free : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= first <= Int.max_signed;
          Int.min_signed <= same_free <= Int.max_signed)
    PARAMS (Vint (Int.repr first); Vint (Int.repr same_free)) SEP ()
  POST [ tint ] PROP ()
    RETURN (Vint (Int.repr (zer_lock_acquire_agrees_coq first same_free)))
    SEP ().

Definition zer_lock_covers_defer_spec : ident * funspec :=
 DECLARE _zer_lock_covers_defer
  WITH opened_here : Z, release_deferred : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= opened_here <= Int.max_signed;
          Int.min_signed <= release_deferred <= Int.max_signed)
    PARAMS (Vint (Int.repr opened_here); Vint (Int.repr release_deferred)) SEP ()
  POST [ tint ] PROP ()
    RETURN (Vint (Int.repr (zer_lock_covers_defer_coq opened_here release_deferred)))
    SEP ().

Definition Gprog : funspecs :=
  [ zer_lock_release_in_role_spec;
    zer_lock_write_in_role_spec;
    zer_rmw_copy_fresh_spec;
    zer_rmw_copy_in_section_spec;
    zer_rmw_one_section_spec;
    zer_rmw_lock_covers_spec;
    zer_rmw_locked_exempt_spec;
    zer_lock_word_ok_spec;
    zer_lock_acquire_agrees_spec;
    zer_lock_covers_defer_spec ].

(* ---- Proofs (the same cascade tactic as verif_window_rules.v) ---- *)

Lemma body_zer_lock_release_in_role:
  semax_body Vprog Gprog f_zer_lock_release_in_role zer_lock_release_in_role_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_lock_release_in_role_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_lock_write_in_role:
  semax_body Vprog Gprog f_zer_lock_write_in_role zer_lock_write_in_role_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_lock_write_in_role_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_rmw_copy_fresh:
  semax_body Vprog Gprog f_zer_rmw_copy_fresh zer_rmw_copy_fresh_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_rmw_copy_fresh_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_rmw_copy_in_section:
  semax_body Vprog Gprog f_zer_rmw_copy_in_section zer_rmw_copy_in_section_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_rmw_copy_in_section_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_rmw_one_section:
  semax_body Vprog Gprog f_zer_rmw_one_section zer_rmw_one_section_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_rmw_one_section_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_rmw_lock_covers:
  semax_body Vprog Gprog f_zer_rmw_lock_covers zer_rmw_lock_covers_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_rmw_lock_covers_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_rmw_locked_exempt:
  semax_body Vprog Gprog f_zer_rmw_locked_exempt zer_rmw_locked_exempt_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_rmw_locked_exempt_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_lock_word_ok:
  semax_body Vprog Gprog f_zer_lock_word_ok zer_lock_word_ok_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_lock_word_ok_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_lock_acquire_agrees:
  semax_body Vprog Gprog f_zer_lock_acquire_agrees zer_lock_acquire_agrees_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_lock_acquire_agrees_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_lock_covers_defer:
  semax_body Vprog Gprog f_zer_lock_covers_defer zer_lock_covers_defer_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_lock_covers_defer_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.
