(* ================================================================
   Level-3 — verif_alias_rules.v

   The "what an access touches" decisions (BUG-1666), extracted into
   src/safety/concurrency_rules.c and called from checker.c.

   Oracle tier: OPERATIONAL — proofs/operational/lambda_zer_concurrency/
   access_alias.v (`overlap_may_alias`, `local_value_listed`, and a witness
   per case). Each predicate here decides one piece of those theorems:

     zer_access_may_alias    may two access records name overlapping memory
                                                          (overlap_may_alias)
     zer_local_ref_known     a local reference points only where the values
                             assigned to it point       (local_value_listed)
     zer_global_ref_known    a global reference is its initializer
     zer_fresh_ref_own       a fresh heap object is the thread's own

   The `_coq` definitions are imported from concurrency_decisions.v, the file
   access_alias.v proves its meaning lemmas about — no second copy.
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.concurrency_rules.
From zer_conc Require Import concurrency_decisions.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

(* ---- VST funspecs ---- *)

Definition zer_access_may_alias_spec : ident * funspec :=
 DECLARE _zer_access_may_alias
  WITH same_global : Z, both_named : Z, a_holds_b : Z, b_holds_a : Z
  PRE [ tint, tint, tint, tint ]
    PROP (Int.min_signed <= same_global <= Int.max_signed;
          Int.min_signed <= both_named <= Int.max_signed;
          Int.min_signed <= a_holds_b <= Int.max_signed;
          Int.min_signed <= b_holds_a <= Int.max_signed)
    PARAMS (Vint (Int.repr same_global); Vint (Int.repr both_named); Vint (Int.repr a_holds_b); Vint (Int.repr b_holds_a)) SEP ()
  POST [ tint ] PROP ()
    RETURN (Vint (Int.repr (zer_access_may_alias_coq same_global both_named a_holds_b b_holds_a)))
    SEP ().

Definition zer_local_ref_known_spec : ident * funspec :=
 DECLARE _zer_local_ref_known
  WITH bound_once : Z, addr_taken : Z, values_known : Z
  PRE [ tint, tint, tint ]
    PROP (Int.min_signed <= bound_once <= Int.max_signed;
          Int.min_signed <= addr_taken <= Int.max_signed;
          Int.min_signed <= values_known <= Int.max_signed)
    PARAMS (Vint (Int.repr bound_once); Vint (Int.repr addr_taken); Vint (Int.repr values_known)) SEP ()
  POST [ tint ] PROP ()
    RETURN (Vint (Int.repr (zer_local_ref_known_coq bound_once addr_taken values_known)))
    SEP ().

Definition zer_global_ref_known_spec : ident * funspec :=
 DECLARE _zer_global_ref_known
  WITH is_const : Z, never_mutated : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= is_const <= Int.max_signed;
          Int.min_signed <= never_mutated <= Int.max_signed)
    PARAMS (Vint (Int.repr is_const); Vint (Int.repr never_mutated)) SEP ()
  POST [ tint ] PROP ()
    RETURN (Vint (Int.repr (zer_global_ref_known_coq is_const never_mutated)))
    SEP ().

Definition zer_fresh_ref_own_spec : ident * funspec :=
 DECLARE _zer_fresh_ref_own
  WITH fresh : Z, leaves : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= fresh <= Int.max_signed;
          Int.min_signed <= leaves <= Int.max_signed)
    PARAMS (Vint (Int.repr fresh); Vint (Int.repr leaves))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_fresh_ref_own_coq fresh leaves)))
    SEP ().

Definition Gprog : funspecs :=
  [ zer_access_may_alias_spec;
    zer_local_ref_known_spec;
    zer_global_ref_known_spec;
    zer_fresh_ref_own_spec ].

(* ---- Proofs (the same cascade tactic as verif_lock_rules.v) ---- *)

Lemma body_zer_access_may_alias:
  semax_body Vprog Gprog f_zer_access_may_alias zer_access_may_alias_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_access_may_alias_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_local_ref_known:
  semax_body Vprog Gprog f_zer_local_ref_known zer_local_ref_known_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_local_ref_known_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_global_ref_known:
  semax_body Vprog Gprog f_zer_global_ref_known zer_global_ref_known_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_global_ref_known_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

Lemma body_zer_fresh_ref_own:
  semax_body Vprog Gprog f_zer_fresh_ref_own zer_fresh_ref_own_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_fresh_ref_own_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.
