(* ================================================================
   Level-3 VST proofs — src/safety/range_checks.c

   Three range-validity predicates used by the checker when validating
   Pool(T, N), Ring(T, N), Semaphore(N), array bounds, and switch
   variant indices.

   Functions verified (all linked into zerc):
     zer_count_is_positive(int n)
       returns 1 iff n > 0
     zer_index_in_bounds(int size, int idx)
       returns 1 iff 0 <= idx < size
     zer_variant_in_range(int n, int idx)
       returns 1 iff 0 <= idx < n

   Callers:
     - checker.c Pool/Ring count validation (zer_count_is_positive)
     - emitter.c inline bounds check (zer_index_in_bounds)
     - checker.c union dispatch (zer_variant_in_range)

   Same extract-and-link pattern as verif_handle_state.v. The SAME .c
   file linked into zerc by Makefile CORE_SRCS is clightgen'd here.
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.range_checks.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

(* ---- Coq specifications ---- *)

Definition zer_count_is_positive_coq (n : Z) : Z :=
  if Z_gt_dec n 0 then 1 else 0.

Definition zer_index_in_bounds_coq (size idx : Z) : Z :=
  if andb (Z_ge_dec idx 0) (Z_lt_dec idx size) then 1 else 0.

Definition zer_variant_in_range_coq (n idx : Z) : Z :=
  if andb (Z_ge_dec idx 0) (Z_lt_dec idx n) then 1 else 0.

(* ---- VST funspecs ---- *)

Definition zer_count_is_positive_spec : ident * funspec :=
 DECLARE _zer_count_is_positive
  WITH n : Z
  PRE [ tint ]
    PROP (Int.min_signed <= n <= Int.max_signed)
    PARAMS (Vint (Int.repr n))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_count_is_positive_coq n)))
    SEP ().

Definition zer_index_in_bounds_spec : ident * funspec :=
 DECLARE _zer_index_in_bounds
  WITH size : Z, idx : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= size <= Int.max_signed;
          Int.min_signed <= idx <= Int.max_signed)
    PARAMS (Vint (Int.repr size); Vint (Int.repr idx))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_index_in_bounds_coq size idx)))
    SEP ().

Definition zer_variant_in_range_spec : ident * funspec :=
 DECLARE _zer_variant_in_range
  WITH n : Z, idx : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= n <= Int.max_signed;
          Int.min_signed <= idx <= Int.max_signed)
    PARAMS (Vint (Int.repr n); Vint (Int.repr idx))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_variant_in_range_coq n idx)))
    SEP ().

Definition Gprog : funspecs :=
  [ zer_count_is_positive_spec;
    zer_index_in_bounds_spec;
    zer_variant_in_range_spec ].

(* ---- Proofs ---- *)

Lemma body_zer_count_is_positive:
  semax_body Vprog Gprog f_zer_count_is_positive
             zer_count_is_positive_spec.
Proof.
  start_function.
  forward_if;
    forward;
    unfold zer_count_is_positive_coq;
    destruct (Z_gt_dec n 0); try lia;
    entailer!.
Qed.

Lemma body_zer_index_in_bounds:
  semax_body Vprog Gprog f_zer_index_in_bounds
             zer_index_in_bounds_spec.
Proof.
  start_function.
  forward_if.
  { (* idx < 0 — return 0 *)
    forward. unfold zer_index_in_bounds_coq.
    destruct (Z_ge_dec idx 0); try lia. simpl. entailer!. }
  forward_if.
  { (* idx >= size — return 0 *)
    forward. unfold zer_index_in_bounds_coq.
    destruct (Z_ge_dec idx 0); try lia.
    simpl. destruct (Z_lt_dec idx size); try lia. entailer!. }
  (* idx in range — return 1 *)
  forward. unfold zer_index_in_bounds_coq.
  destruct (Z_ge_dec idx 0); try lia.
  simpl. destruct (Z_lt_dec idx size); try lia. entailer!.
Qed.

Lemma body_zer_variant_in_range:
  semax_body Vprog Gprog f_zer_variant_in_range
             zer_variant_in_range_spec.
Proof.
  start_function.
  forward_if.
  { forward. unfold zer_variant_in_range_coq.
    destruct (Z_ge_dec idx 0); try lia. simpl. entailer!. }
  forward_if.
  { forward. unfold zer_variant_in_range_coq.
    destruct (Z_ge_dec idx 0); try lia.
    simpl. destruct (Z_lt_dec idx n); try lia. entailer!. }
  forward. unfold zer_variant_in_range_coq.
  destruct (Z_ge_dec idx 0); try lia.
  simpl. destruct (Z_lt_dec idx n); try lia. entailer!.
Qed.

(* ================================================================
   QED — 3 range predicates mechanically verified against src/safety/
   range_checks.c. Same .c file linked into zerc via Makefile CORE_SRCS.
   Total Level-3 verified compiler functions: 7 (4 handle state + 3 range).
   ================================================================ *)
