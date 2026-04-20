(* ================================================================
   Phase 0 smoke test — verify Iris 4.2 infrastructure compiles in
   our Docker image. No connection to semantics.v yet. Just proves:

     1. `From iris ... Require Import ...` resolves.
     2. iProp / bi syntax works.
     3. `iIntros` / `iApply` tactics are available.
     4. Trivial separation-logic fact is provable.

   If this file compiles, we know the Iris stack is live. Then we
   can build on it in iris_lang.v (language typeclass instance) and
   iris_demo.v (alloc→free proof).
   ================================================================ *)

From iris.base_logic Require Import iprop.
From iris.proofmode Require Import proofmode.

Section smoke.
  Context {Σ : gFunctors}.

  (* The empty separation-logic-style conjunction is reflexive. *)
  Lemma smoke_emp_self : ⊢@{iPropI Σ} emp -∗ emp.
  Proof. iIntros "H". iFrame "H". Qed.

  (* Conjunction commutativity — classic separation-logic sanity check. *)
  Lemma smoke_sep_comm (P Q : iProp Σ) : P ∗ Q -∗ Q ∗ P.
  Proof. iIntros "[HP HQ]". iFrame. Qed.

  (* Pure propositions propagate through separation conjunction. *)
  Lemma smoke_pure_sep (n : nat) : ⊢@{iPropI Σ} ⌜n = n⌝ -∗ ⌜n = n⌝.
  Proof. iIntros "%H". iPureIntro. exact H. Qed.

End smoke.
