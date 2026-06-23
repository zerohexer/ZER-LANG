(* ================================================================
   lambda_zer_qualifier : the OPERATIONAL ORACLE for the const/volatile
   qualifier class — no prior oracle existed, so the state set was uncertified.

   THE FINITE STATE SET (the abstract domain): a qualifier is a 2-bit record
   {qconst, qvol} — exactly FOUR states {none, const, volatile, const+volatile}.
   That is the complete, closed finite-variable answer for this class; the C
   (qualifier tracking, @cast/@bitcast/@ptrcast handlers) must map every
   pointer's qualifier onto exactly this 4-element set.

   THE SOUND DECISION: a cast/conversion is allowed IFF it does not STRIP a
   qualifier (drop const or volatile). Stripping const enables a write through a
   read-only object; stripping volatile enables eliding a hardware-visible /
   MMIO access. The theorems pin: (T1) an allowed cast never strips; (T2) the
   decision IS the qualifier-preservation order; (T3) a stripping cast is always
   rejected (no under-rejection); (T4) the order is a partial order, so chained
   preserving casts preserve (composability). Covers BOTH const and volatile
   (each is one bit). Self-contained, zero admits.
   ================================================================ *)
From Coq Require Import Bool.

(* ---- ABSTRACT DOMAIN: the 4 qualifier states ---- *)
Record qual := { qconst : bool; qvol : bool }.

(* the safety order: q1 <= q2  iff  q2 preserves (has at least) q1's qualifiers. *)
Definition qle (q1 q2 : qual) : Prop :=
  (qconst q1 = true -> qconst q2 = true) /\ (qvol q1 = true -> qvol q2 = true).

(* ---- THE DECISION: a cast src->dst is allowed iff it preserves qualifiers. ---- *)
Definition cast_ok (qsrc qdst : qual) : bool :=
  implb (qconst qsrc) (qconst qdst) && implb (qvol qsrc) (qvol qdst).

(* the TRUE unsafety: a STRIP = a qualifier present in src, absent in dst. *)
Definition strips (qsrc qdst : qual) : bool :=
  (qconst qsrc && negb (qconst qdst)) || (qvol qsrc && negb (qvol qdst)).

(* ================================================================
   (T1) DECISION SOUNDNESS — an allowed cast NEVER strips a qualifier. *)
Theorem cast_ok_no_strip : forall qsrc qdst,
  cast_ok qsrc qdst = true -> strips qsrc qdst = false.
Proof.
  intros [cs vs] [cd vd]. unfold cast_ok, strips, qconst, qvol. simpl.
  destruct cs, vs, cd, vd; simpl; (reflexivity || discriminate).
Qed.

(* ================================================================
   (T3) NO UNDER-REJECTION — a stripping cast is ALWAYS rejected. *)
Theorem strip_always_rejected : forall qsrc qdst,
  strips qsrc qdst = true -> cast_ok qsrc qdst = false.
Proof.
  intros [cs vs] [cd vd]. unfold cast_ok, strips, qconst, qvol. simpl.
  destruct cs, vs, cd, vd; simpl; (reflexivity || discriminate).
Qed.

(* ================================================================
   (T2) THE DECISION IS THE PRESERVATION ORDER — allowed cast <-> qle. *)
Theorem cast_ok_iff_qle : forall qsrc qdst,
  cast_ok qsrc qdst = true <-> qle qsrc qdst.
Proof.
  intros [cs vs] [cd vd]. unfold cast_ok, qle, qconst, qvol. simpl.
  split.
  - intro H. apply andb_true_iff in H as [H1 H2]. split; intro He.
    + subst cs. simpl in H1. exact H1.
    + subst vs. simpl in H2. exact H2.
  - intros [H1 H2]. apply andb_true_iff. split.
    + destruct cs; simpl; [ apply H1; reflexivity | reflexivity ].
    + destruct vs; simpl; [ apply H2; reflexivity | reflexivity ].
Qed.

(* ================================================================
   (T4) PARTIAL ORDER — reflexive + transitive, so chained preserving casts
   preserve qualifiers (composability of the safe decision). *)
Theorem qle_refl : forall q, qle q q.
Proof. intros [c v]. unfold qle. simpl. split; intro H; exact H. Qed.

Theorem qle_trans : forall a b c, qle a b -> qle b c -> qle a c.
Proof.
  intros a b c [Hc1 Hv1] [Hc2 Hv2]. unfold qle. split; intro H.
  - apply Hc2, Hc1, H.
  - apply Hv2, Hv1, H.
Qed.
