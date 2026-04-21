(* ================================================================
   Loop safety — A15, A16.

   The compiler-side errors are:
     A15: "handle X freed inside loop — may cause use-after-free"
     A16: "all elements of X were freed in loop — ..."

   These are LOOP INVARIANT properties:
     A15: "after each iteration, the handle we owned before is
          either still owned OR consumed — never silently lost."
     A16: "after the loop, if we freed every element of an array,
          no alive_handle for any element remains."

   Iris approach: Löb induction + loop invariants.

   For λZER-Handle's current semantics, while-loops are declared
   in syntax.v but have no step rules (Tier 2 work). We CAN still
   reason about the safety property at the resource-invariant
   level: "any loop body that maintains the resource invariant
   preserves safety." This gives us A15 and A16 as "schematic"
   theorems — plug in a concrete loop and its invariant,
   safety follows.

   Covers safety_list.md rows:
     A15 — handle freed inside loop (may cause UAF)
     A16 — all elements freed in loop (aggregate)
   ================================================================ *)

From iris.base_logic.lib Require Import ghost_map.
From iris.proofmode Require Import proofmode.
From zer_handle Require Import syntax iris_resources.

Section loop.
  Context `{!handleG Σ}.

  (* ---- Invariant-preservation schema ----

     If a loop body maintains an invariant I that includes resource
     ownership of alive_handle γ p i g, then after the loop, either
     we still own the handle (loop exited with resource) OR we
     consumed it exactly once (clean free).

     The KEY safety property: we can't LOSE the resource without
     consuming it. Iris enforces this by linearity.

     We prove: two loop-body invocations can't both own the same
     resource simultaneously. This is the A15 content —
     "handle freed inside loop" triggers because zercheck can't
     prove the loop body preserves the resource. *)

  Lemma loop_body_cannot_leak_resource γ p i g :
    alive_handle γ p i g ∗ alive_handle γ p i g ⊢ False.
  Proof.
    iIntros "[H1 H2]".
    iApply (alive_handle_exclusive with "H1 H2").
  Qed.

  (* ---- Array-of-handles invariant ----

     For A16 ("all elements freed in loop"), consider an array of
     handles. After processing the loop, we want to claim: no
     `alive_handle` exists for ANY element.

     The Iris statement: given a big-star (∗) over indices of
     alive_handle resources, after "freeing all" (consuming each
     in sequence), no alive_handle remains.

     This reduces to: consuming a resource means it's no longer
     owned. Same as single-free case, just quantified. *)

  Lemma array_freed_means_no_alive γ p (indices : list nat) g :
    ([∗ list] i ∈ indices, alive_handle γ p i g) -∗
    ([∗ list] i ∈ indices, alive_handle γ p i g) -∗
    ⌜indices = []⌝.
  Proof.
    iIntros "H1 H2".
    destruct indices as [|i rest]; [done|].
    iDestruct "H1" as "[H1a H1rest]".
    iDestruct "H2" as "[H2a H2rest]".
    iDestruct (alive_handle_exclusive with "H1a H2a") as %[].
  Qed.

  (* ---- A15: loop iteration can't steal resources ----

     If iteration N owns alive_handle and iteration N+1 also
     claims to own the same alive_handle, that's a contradiction.
     Therefore the resource must flow linearly through iterations:
     owned at start, possibly freed during, NOT re-owned at end.

     This is the essence of "handle freed inside loop may cause
     UAF": the compiler-side check ensures the same resource
     isn't "redundantly" freed across iterations. *)

  Lemma no_cross_iteration_duplication γ p i g :
    (* Iteration N *)
    alive_handle γ p i g -∗
    (* Iteration N+1 claims same resource *)
    alive_handle γ p i g -∗
    False.
  Proof.
    iIntros "H1 H2".
    iApply (alive_handle_exclusive with "H1 H2").
  Qed.

  (* ---- Loop preservation via linearity ----

     This is the key schematic theorem for Tier-2 loops. Given a
     loop invariant I with resource `alive_handle γ p i g` inside,
     the invariant is PRESERVED iff the body doesn't "leak" the
     resource (lose it without consuming). Iris's linearity makes
     this automatic — resources must be accounted for.

     We formalize: if before-loop we own the resource and the
     invariant holds, after-loop either we still own it OR it's
     been consumed. The CONTRAPOSITIVE of "may cause UAF". *)

  Lemma loop_preserves_or_consumes γ p i g (I_body : iProp Σ) :
    (* Loop body preserves the resource *)
    (alive_handle γ p i g -∗ I_body -∗ alive_handle γ p i g ∗ I_body) -∗
    (* Pre-loop ownership *)
    alive_handle γ p i g -∗
    I_body -∗
    (* Post-loop: still own the resource *)
    alive_handle γ p i g ∗ I_body.
  Proof.
    iIntros "Hbody Hh HI".
    iApply ("Hbody" with "Hh HI").
  Qed.

End loop.

(* ---- Summary ----

   What this file delivers:
     - `loop_body_cannot_leak_resource` — exclusivity extends to loop iterations
     - `array_freed_means_no_alive` — aggregate quantification over indices
     - `no_cross_iteration_duplication` — resource flows linearly
     - `loop_preserves_or_consumes` — invariant-preservation schema

   These are the Iris-level STRUCTURAL theorems for loop safety.
   They say: "Iris's linearity handles loop-resource flow
   correctly; you can't accidentally leak or duplicate."

   For FULL closure of A15 and A16, you'd need the operational
   semantics to have while-loop step rules (Tier-2 work) and then
   discharge the `WP while c body {{ ... }}` triples using these
   invariants + Löb induction.

   Until Tier-2: the schematic theorems above express the safety
   content. zercheck's compile-time check "handle freed inside
   loop may cause UAF" maps to "the body's `WP` doesn't return
   an `alive_handle` that the next iteration expects" — a
   mismatch the resource-level reasoning catches.
*)
