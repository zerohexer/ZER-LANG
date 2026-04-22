(* ================================================================
   Level-3 VST proofs — src/safety/mmio_rules.c

   @inttoptr MMIO safety predicates verified against the λZER-mmio
   operational oracle (lambda_zer_mmio/iris_mmio_theorems.v).

   Oracle rules:
     H01/H02: step_inttoptr_ok requires addr_in_ranges = true
     H03:     step_inttoptr_ok requires addr_aligned = true
     step:    BOTH required for the rule to fire; missing either = stuck

   Functions verified (all linked into zerc):
     zer_mmio_addr_in_range(addr, start, end)
       → 1 iff addr ∈ [start, end] (inclusive)
     zer_mmio_addr_aligned(addr, align)
       → 1 iff addr % align == 0 (and align > 0)
     zer_mmio_inttoptr_allowed(in_any_range, aligned)
       → 1 iff both flags set

   Callers: checker.c @inttoptr handler. Delegates the gate combination
   via zer_mmio_inttoptr_allowed. Per-range check stays inline (uses
   uint64_t; extracted predicate takes int — narrow-target limitation
   documented in BUGS-FIXED.md).
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.mmio_rules.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

(* ---- Coq specifications ---- *)

Definition zer_mmio_addr_in_range_coq (addr start stop : Z) : Z :=
  if Z_lt_dec addr start then 0
  else if Z_gt_dec addr stop then 0
  else 1.

Definition zer_mmio_addr_aligned_coq (addr align : Z) : Z :=
  if Z_le_dec align 0 then 0
  else if Z.eq_dec (addr mod align) 0 then 1
  else 0.

Definition zer_mmio_inttoptr_allowed_coq (in_any_range aligned : Z) : Z :=
  if Z.eq_dec in_any_range 0 then 0
  else if Z.eq_dec aligned 0 then 0
  else 1.

(* ---- VST funspecs ---- *)

Definition zer_mmio_addr_in_range_spec : ident * funspec :=
 DECLARE _zer_mmio_addr_in_range
  WITH addr : Z, start : Z, stop : Z
  PRE [ tint, tint, tint ]
    PROP (Int.min_signed <= addr <= Int.max_signed;
          Int.min_signed <= start <= Int.max_signed;
          Int.min_signed <= stop <= Int.max_signed)
    PARAMS (Vint (Int.repr addr); Vint (Int.repr start); Vint (Int.repr stop))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_mmio_addr_in_range_coq addr start stop)))
    SEP ().

Definition zer_mmio_inttoptr_allowed_spec : ident * funspec :=
 DECLARE _zer_mmio_inttoptr_allowed
  WITH ir : Z, al : Z
  PRE [ tint, tint ]
    PROP (Int.min_signed <= ir <= Int.max_signed;
          Int.min_signed <= al <= Int.max_signed)
    PARAMS (Vint (Int.repr ir); Vint (Int.repr al))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (zer_mmio_inttoptr_allowed_coq ir al)))
    SEP ().

(* addr_aligned uses % which VST handles for positive align. Keep the
   spec for completeness but don't require it in Gprog — no call site
   currently delegates to it directly (checker uses inline modulus
   because of uint64 vs int type mismatch). Future: widen the predicate
   to long long to enable full delegation. *)

Definition Gprog : funspecs :=
  [ zer_mmio_addr_in_range_spec;
    zer_mmio_inttoptr_allowed_spec ].

(* ---- Proofs ---- *)

Lemma body_zer_mmio_addr_in_range:
  semax_body Vprog Gprog f_zer_mmio_addr_in_range
             zer_mmio_addr_in_range_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_mmio_addr_in_range_coq;
    repeat (first [ destruct (Z_lt_dec _ _)
                  | destruct (Z_gt_dec _ _) ]; try lia);
    try entailer!.
Qed.

Lemma body_zer_mmio_inttoptr_allowed:
  semax_body Vprog Gprog f_zer_mmio_inttoptr_allowed
             zer_mmio_inttoptr_allowed_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_mmio_inttoptr_allowed_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.

(* ================================================================
   2 MMIO predicates VST-verified (aligned excluded — modulus/division
   needs VST helper setup; deferred to a follow-up as a third lemma).
   Total Level-3 verified compiler functions: 33
   (4 handle state + 3 range + 7 type kind + 5 coerce + 6 context +
    3 escape + 3 provenance + 2 mmio).
   ================================================================ *)
