(* ================================================================
   λZER-Concurrency : CAPSTONE  (DESIGN.md §7 step 6 / §0 / §1)

   The four NECESSARY conditions for a cross-thread memory hazard
   (DESIGN §1) — reach, discipline, lifetime, visibility — are each
   negated by one piece of the development:

     reach       (§4.1)  is_shared / publish_shared   [iris_shared_inv]
     discipline  (§4.2)  local_excl / shared_acc /    [iris_shared_specs,
                         wp_load                        iris_wp_heap]
     lifetime    (§4.3)  region tags + linear join +   [iris_region_join]
                         the merge obligation
     visibility  (§4.4)  boundary_cap                  [iris_boundary]

   `concurrency_closure` below COMPOSES the four into one theorem: the
   resource-level statement that a cross-thread data race or use-after-
   free is INEXPRESSIBLE — each disjunct of the hazard is contradictory
   under the corresponding condition. This is the resource-level form of
   the §0 theorem; the novel content (the four-condition discipline) is
   exactly here.

   The operational corollary — "a WP-provable program executes safely
   (no stuck thread configuration) for all schedules" — is Iris's GENERIC
   threadpool adequacy (`adequacy`/`wp_strong_adequacy`) applied to
   `λZC_lang`; it adds no new safety argument, only the adequacy
   plumbing. See the closing NOTE.
   ================================================================ *)

From iris.base_logic.lib Require Import gen_heap invariants.
From iris.proofmode Require Import proofmode.
From zer_conc Require Import syntax iris_state iris_shared_inv iris_shared_specs
                            iris_region_join iris_boundary.

Section capstone.
  Context `{!concGS Σ, !rjGS Σ}.

  (* THE CLOSURE THEOREM. The four conditions, together, make every
     cross-thread memory hazard contradictory:

     1. DR-free on Local data: two threads cannot both hold a location's
        exclusive points-to (so Local data has at most one accessor).
     2. No double-join (the linear join obligation is exclusive).
     3. Lifetime: a stack-region pointer is not publishable to a thread.
     4. Visibility: a stack-region pointer has no opaque-boundary
        capability (cannot be handed to thread-spawning cinclude code).

     Each conjunct is discharged by the condition that negates it. The
     conjunction IS the closure: with these, the hazards have no proof. *)
  Theorem concurrency_closure :
    ⊢ (∀ l v1 v2,
         pointsto l (DfracOwn 1) v1 -∗ pointsto l (DfracOwn 1) v2 -∗ False) ∧
      (∀ γ tid, join_tok γ tid -∗ join_tok γ tid -∗ False) ∧
      (∀ γ id, region_ptr γ id RegStack -∗ ⌜publishable RegStack⌝ -∗ False) ∧
      (∀ N l P γr id,
         boundary_cap N l P γr id -∗ region_ptr γr id RegStack -∗ False).
  Proof.
    repeat iSplit.
    - (* reach/discipline: DR-free on Local *)
      iIntros (l v1 v2) "H1 H2". iApply (local_excl with "H1 H2").
    - (* lifetime: no double-join (the linear join obligation) *)
      iIntros (γ tid) "H1 H2". iApply (join_tok_exclusive with "H1 H2").
    - (* lifetime: stack pointer not publishable to a thread *)
      iIntros (γ id) "H1 H2". iApply (stack_not_publishable with "H1 H2").
    - (* visibility: stack pointer has no opaque-boundary capability *)
      iIntros (N l P γr id) "H1 H2".
      iApply (stack_no_boundary_cap with "H1 H2").
  Qed.

  (* Two corollaries naming the safety properties the closure delivers. *)

  (* No data race on a thread-private (Local) location. *)
  Corollary no_local_data_race l v1 v2 :
    pointsto l (DfracOwn 1) v1 -∗ pointsto l (DfracOwn 1) v2 -∗ False.
  Proof. iApply local_excl. Qed.

  (* No cross-thread use-after-scope via a published stack pointer:
     both the spawn/publish path (stack_not_publishable) and the opaque
     boundary path (stack_no_boundary_cap) reject a stack-region pointer. *)
  Corollary stack_pointer_cannot_escape_to_thread γ id :
    region_ptr γ id RegStack -∗ ⌜publishable RegStack⌝ -∗ False.
  Proof. iApply stack_not_publishable. Qed.

End capstone.

(* ---- NOTE: operational adequacy (the remaining generic plumbing) ----

   The operational §0 statement — "a closed, WP-provable λZC program never
   reaches a stuck thread configuration, for all schedules" — follows from
   Iris's GENERIC threadpool adequacy:

     wp_adequacy / wp_strong_adequacy  (iris.program_logic.adequacy)

   applied to λZC_lang with the `concGS` state interpretation
   (`gen_heap_interp`). "Stuck" in our semantics includes a load/store on
   an absent (freed) location — so adequacy yields UAF-freedom directly;
   data-race-freedom is the resource discipline proved above (a racy
   program has no WP proof, because two writers would each need the
   exclusive points-to — `local_excl`). The adequacy instantiation needs
   `concGpreS` (invGpreS + gen_heapGpreS) + the `wp_adequacy` existential;
   it adds plumbing, not safety argument. That instantiation is the one
   remaining mechanical step toward the fully operational theorem. *)
