(* ================================================================
   λZER-Concurrency : Discipline specs  (DESIGN.md §7 step 3 — the
   first NONTRIVIAL Iris proof-engineering)

   The DISCIPLINE condition (§4.2): every access to a tainted (cross-
   thread-reachable) location is synchronized. Two halves:

   (a) LOCAL locations carry NO data race because the points-to is
       EXCLUSIVE — two threads cannot both own `l ↦ v`. So a location
       reachable by ≥2 threads MUST be Shared (its points-to given up
       into the invariant); it cannot stay Local. `local_excl` is the
       concrete data-race-freedom statement for Local data.

   (b) SHARED locations are accessed only by OPENING the invariant for
       the duration of one atomic step. The Iris invariant mechanism
       guarantees mutual exclusion of concurrent openings — that IS the
       lock. `shared_acc` exposes this open/close access (a thin
       specialization of `inv_acc`); the ▷ on the contents is stripped
       by the program STEP in the WP load/store specs (step 3b, below).

   ZER's flat (non-ectx) language deliberately avoids `wp_lift_atomic_
   step` (see lambda_zer_handle/iris_step_specs.v header). The
   discipline content here is proved DIRECTLY at the resource level —
   exclusivity (pure separation logic) + invariant access (inv_acc) —
   independent of the WP machinery, the same philosophy as the handle
   step-specs.
   ================================================================ *)

From iris.base_logic.lib Require Import gen_heap invariants.
From iris.proofmode Require Import proofmode.
From zer_conc Require Import syntax semantics iris_lang iris_state iris_shared_inv.

Section specs.
  Context `{!concGS Σ}.

  Local Notation "l '↦' v" := (pointsto l (DfracOwn 1) v)
    (at level 20, format "l  '↦'  v") : bi_scope.

  (* (a) DISCIPLINE — NO DATA RACE ON A LOCAL LOCATION.

     The full (DfracOwn 1) points-to is exclusive: owning it twice is
     contradictory (the fraction 1+1 > 1 is invalid). Operationally:
     a Local location is accessible only by the single thread holding
     its points-to, so two threads can never race on it. A location
     touched by two threads must therefore be Shared (§4.1 publish). *)
  Lemma local_excl l v1 v2 :
    l ↦ v1 -∗ l ↦ v2 -∗ False.
  Proof.
    iIntros "H1 H2".
    iDestruct (pointsto_valid_2 with "H1 H2") as %[Hv _].
    rewrite dfrac_op_own in Hv.
    rewrite dfrac_valid_own in Hv.
    by apply Qp.not_add_le_l in Hv.
  Qed.

  (* Same content, agreement form: if two threads each "have" l↦ they
     must agree on the value — but in fact they cannot both have it, so
     this is vacuous from local_excl. Kept as the canonical no-torn-read
     statement the later specs cite. *)
  Lemma local_agree l v1 v2 :
    l ↦ v1 -∗ l ↦ v2 -∗ ⌜v1 = v2⌝.
  Proof.
    iIntros "H1 H2".
    iDestruct (local_excl with "H1 H2") as %[].
  Qed.

  (* (b) DISCIPLINE — SHARED ACCESS GOES THROUGH THE SERIALIZING
     INVARIANT.  From `is_shared N l P`, open the invariant (mask E ∖ ↑N)
     to obtain the points-to + payload, with the obligation to restore
     them.  Iris invariants serialize concurrent openings = the lock.
     The ▷ is discharged by the program step in the WP specs (3b). *)
  Lemma shared_acc E N l P :
    ↑N ⊆ E →
    is_shared N l P ={E, E ∖ ↑N}=∗
      ▷ (∃ w, l ↦ w ∗ P w) ∗
      (▷ (∃ w, l ↦ w ∗ P w) ={E ∖ ↑N, E}=∗ True).
  Proof.
    iIntros (HN) "#Hinv". rewrite /is_shared.
    iApply (inv_acc with "Hinv"); done.
  Qed.

  (* NOTE (step 3b): the WP-level guarded load/store —
       is_shared N l P ⊢ WP (LockedLoad l) {{ w, ... }}
     opens the invariant via `iInv`, lets the load/store STEP strip the
     ▷, and restores. That needs the WP atomic-step rule for λZC_lang
     (the genuinely hard lifting). `shared_acc` + `local_excl` are the
     resource-level discipline content those WP specs are built from. *)

End specs.
