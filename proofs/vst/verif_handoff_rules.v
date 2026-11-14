(* ================================================================
   Level-3 — verif_handoff_rules.v

   The flag HAND-OFF decisions (BUG-1664), extracted into
   src/safety/concurrency_rules.c and called from checker.c.

   Oracle tier: OPERATIONAL — proofs/operational/lambda_zer_concurrency/
   flag_handoff.v (`holders_agree`, `claims_exclusive`, and one counterexample
   trace per hypothesis dropped). Each predicate here decides one piece of a
   hypothesis of those theorems:

     zer_flag_obs            what a comparison of an atomic load tells its
                             thread on the branch taken           (EObs)
     zer_flag_store_from     under which held value a store is made — an
                             observed one, the initial one (the thread's
                             first, unrepeated store), or none  (handoff, H_sto)
     zer_flag_claim_unique   a value stored from has one claiming thread
                                                                 (claim, H_obs)
     zer_flag_orders         two accesses are ordered only under two
                             different held values of a disciplined flag
                                                          (claims_exclusive)

   The `_coq` definitions are imported from concurrency_decisions.v, the file
   flag_handoff.v proves its meaning lemmas about — no second copy.

   Imports the SAME clightgen output as the other concurrency verif files
   (zer_safety.concurrency_rules); the files are independent.
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.concurrency_rules.
From zer_conc Require Import concurrency_decisions.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

(* ---- VST funspecs ---- *)

Definition zer_flag_obs_spec : ident * funspec :=
 DECLARE _zer_flag_obs
  WITH agrees : Z, two_valued : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= agrees <= Int.max_signed;
          Int.min_signed <= two_valued <= Int.max_signed)
    PARAMS (Vint (Int.repr agrees); Vint (Int.repr two_valued)) SEP ()
  POST [ tint ] PROP ()
    RETURN (Vint (Int.repr (zer_flag_obs_coq agrees two_valued)))
    SEP ().

Definition zer_flag_store_from_spec : ident * funspec :=
 DECLARE _zer_flag_store_from
  WITH holds_explicit : Z, entry_fn : Z, in_loop : Z, in_jump_fn : Z, stored_before : Z
  PRE [ tint, tint, tint, tint, tint ]
    PROP (Int.min_signed <= holds_explicit <= Int.max_signed;
          Int.min_signed <= entry_fn <= Int.max_signed;
          Int.min_signed <= in_loop <= Int.max_signed;
          Int.min_signed <= in_jump_fn <= Int.max_signed;
          Int.min_signed <= stored_before <= Int.max_signed)
    PARAMS (Vint (Int.repr holds_explicit); Vint (Int.repr entry_fn); Vint (Int.repr in_loop); Vint (Int.repr in_jump_fn); Vint (Int.repr stored_before)) SEP ()
  POST [ tint ] PROP ()
    RETURN (Vint (Int.repr (zer_flag_store_from_coq holds_explicit entry_fn in_loop in_jump_fn stored_before)))
    SEP ().

Definition zer_flag_claim_unique_spec : ident * funspec :=
 DECLARE _zer_flag_claim_unique
  WITH single_thread : Z, repeated : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= single_thread <= Int.max_signed;
          Int.min_signed <= repeated <= Int.max_signed)
    PARAMS (Vint (Int.repr single_thread); Vint (Int.repr repeated)) SEP ()
  POST [ tint ] PROP ()
    RETURN (Vint (Int.repr (zer_flag_claim_unique_coq single_thread repeated)))
    SEP ().

Definition zer_flag_orders_spec : ident * funspec :=
 DECLARE _zer_flag_orders
  WITH flag_valid : Z, values_differ : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= flag_valid <= Int.max_signed;
          Int.min_signed <= values_differ <= Int.max_signed)
    PARAMS (Vint (Int.repr flag_valid); Vint (Int.repr values_differ)) SEP ()
  POST [ tint ] PROP ()
    RETURN (Vint (Int.repr (zer_flag_orders_coq flag_valid values_differ)))
    SEP ().

Definition Gprog : funspecs :=
  [ zer_flag_obs_spec;
    zer_flag_store_from_spec;
    zer_flag_claim_unique_spec;
    zer_flag_orders_spec ].

(* ---- Proofs (the same cascade tactic as verif_lock_rules.v) ---- *)

Lemma body_zer_flag_obs:
  semax_body Vprog Gprog f_zer_flag_obs zer_flag_obs_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_flag_obs_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_flag_store_from:
  semax_body Vprog Gprog f_zer_flag_store_from zer_flag_store_from_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_flag_store_from_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_flag_claim_unique:
  semax_body Vprog Gprog f_zer_flag_claim_unique zer_flag_claim_unique_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_flag_claim_unique_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_flag_orders:
  semax_body Vprog Gprog f_zer_flag_orders zer_flag_orders_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_flag_orders_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.
