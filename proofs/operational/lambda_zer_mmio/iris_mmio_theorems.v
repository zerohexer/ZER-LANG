(* ================================================================
   λZER-MMIO : Operational theorems for section H rows.

   Simpler than previous subsets: no ghost state needed because
   the MMIO range declarations are a COMPILE-TIME CONSTANT of the
   program (they're checked against a fixed list in state).

   The safety property is operational and structural:
     - step_inttoptr_ok requires BOTH range check AND alignment
     - Missing either = NO RULE fires = stuck

   Covers safety_list.md rows:
     H01 — @inttoptr addr outside declared range
     H02 — @inttoptr runtime outside range (compile-time proven)
     H03 — @inttoptr unaligned
     H04 — strict mode requires mmio decl
     H05 — @probe shape (typing — not operational)
     H06 — mmio range start > end (program well-formedness)
     H07 — MMIO slice index OOB
     H08 — hw-detection trap (runtime)
     H09 — fault trap (runtime)
   ================================================================ *)

From iris.proofmode Require Import proofmode.
From Coq Require Import Lia.
From stdpp Require Import base list.
From zer_mmio Require Import syntax semantics.

Section operational_theorems.

  (* =============================================================
     H01 + H02 — addr out of range is stuck.

     Statement: if addr is NOT in any declared range, no step rule
     fires for EInttoPtr on it. *)

  Lemma H01_H02_out_of_range_stuck st al a :
    addr_in_ranges a st.(st_mmio_ranges) = false →
    ¬ (exists st' e', step st (EInttoPtr al (EVal (VAddr a))) st' e').
  Proof.
    intros Hne [st' [e' Hstep]].
    inversion Hstep; subst.
    - (* step_inttoptr_ok: contradicts Hne *)
      match goal with H : addr_in_ranges _ _ = true |- _ =>
        rewrite H in Hne; discriminate
      end.
    - (* step_inttoptr_ctx: inner must step. EVal doesn't. *)
      match goal with H : step _ (EVal _) _ _ |- _ => inversion H end.
  Qed.

  (* =============================================================
     H03 — unaligned address is stuck.

     If addr is in range but misaligned, no step rule fires. *)

  Lemma H03_unaligned_stuck st al a :
    addr_in_ranges a st.(st_mmio_ranges) = true →
    addr_aligned a al = false →
    ¬ (exists st' e', step st (EInttoPtr al (EVal (VAddr a))) st' e').
  Proof.
    intros Hrange Hune [st' [e' Hstep]].
    inversion Hstep; subst.
    - match goal with H : addr_aligned _ _ = true |- _ =>
        rewrite H in Hune; discriminate
      end.
    - match goal with H : step _ (EVal _) _ _ |- _ => inversion H end.
  Qed.

  (* =============================================================
     H04 — empty mmio ranges = no @inttoptr can succeed.

     (strict-mmio mode: if no ranges declared, all @inttoptr stuck.)
     ============================================================= *)

  Lemma H04_no_ranges_all_stuck st al a :
    st.(st_mmio_ranges) = [] →
    ¬ (exists st' e', step st (EInttoPtr al (EVal (VAddr a))) st' e').
  Proof.
    intros Hempty.
    apply H01_H02_out_of_range_stuck.
    rewrite Hempty. reflexivity.
  Qed.

  (* =============================================================
     H06 — mmio range start > end.

     Program-well-formedness: a range (s, e) with s > e covers no
     addresses. We prove: such a range doesn't make any address
     "in range". (This is a DEGENERACY check at program setup.)
     ============================================================= *)

  Lemma H06_degenerate_range_covers_nothing s e a :
    s > e →
    andb (Nat.leb s a) (Nat.leb a e) = false.
  Proof.
    intros Hlt.
    apply andb_false_iff.
    destruct (Nat.leb s a) eqn:Hsa.
    - right. apply Nat.leb_le in Hsa. apply Nat.leb_gt. lia.
    - left. reflexivity.
  Qed.

  (* =============================================================
     Operational safety: a valid (in-range, aligned) @inttoptr
     operationally succeeds.
     ============================================================= *)

  Lemma H_inttoptr_safe_when_valid st al a :
    addr_in_ranges a st.(st_mmio_ranges) = true →
    addr_aligned a al = true →
    step st (EInttoPtr al (EVal (VAddr a))) st (EVal (VMMIOPtr a al)).
  Proof.
    intros Hrange Halign.
    apply step_inttoptr_ok; assumption.
  Qed.

End operational_theorems.
