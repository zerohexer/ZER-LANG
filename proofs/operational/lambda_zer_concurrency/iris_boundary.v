(* ================================================================
   λZER-Concurrency : Opaque boundary capability  (DESIGN.md §7 step 5
   — the VISIBILITY condition, Axis D)

   An `OpaqueCall` (cinclude C that may pthread_create internally, or
   compiler-emitted runtime) steps over code the relation CANNOT see.
   Iris-wise it is a frame the proof cannot instantiate, so a value
   handed across is safe ONLY IF it is self-sufficient — it carries a
   CLOSED capability that needs no further analyzer help:

     - a COPY-SAFE scalar (no location ⇒ nothing to alias / thread), OR
     - a pointer that is BOTH SHARED (§4.2 — the opaque code, even if it
       threads, can only touch it through the invariant) AND STATIC-region
       (§4.3 — it outlives any thread the opaque code might spawn).

   Anything else (a bare / stack-region pointer) has NO boundary
   capability ⇒ crossing is unprovable ⇒ CONSERVATIVE-REJECT. This is the
   cinclude concurrency-capture hole made inexpressible: the ZER pointer
   handed to a thread-spawning C lib must already be shared+static.

   The capability is built by COMPOSING the reach invariant (is_shared,
   persistent) and the region tag (region_ptr) from steps 2 and 4 — no
   new ghost state, just their conjunction. That composition IS the Axis
   D content: the boundary needs exactly reach ∧ lifetime, self-contained.
   ================================================================ *)

From iris.base_logic.lib Require Import invariants.
From iris.proofmode Require Import proofmode.
From zer_conc Require Import syntax iris_state iris_shared_inv iris_region_join.

Section boundary.
  Context `{!concGS Σ, !rjGS Σ}.

  (* COPY-SAFE: a scalar carries no thread-reachable pointer, so it may
     cross any boundary (incl. opaque code that threads internally) —
     there is nothing to alias. A location is NOT copy-safe. *)
  Definition copy_safe (v : val) : Prop :=
    match v with VLoc _ => False | _ => True end.

  Lemma loc_not_copy_safe l : ¬ copy_safe (VLoc l).
  Proof. intros H. exact H. Qed.

  Lemma unit_copy_safe : copy_safe VUnit.
  Proof. exact I. Qed.
  Lemma int_copy_safe z : copy_safe (VInt z).
  Proof. exact I. Qed.

  (* BOUNDARY CAPABILITY for a location: shared (self-synchronizing) AND
     static-region (outlives any thread the opaque code spawns). Composed
     from is_shared (step 2) + region_ptr RegStatic (step 4). *)
  Definition boundary_cap (N : namespace) (l : loc) (P : val -> iProp Σ)
                          (γr : gname) (id : nat) : iProp Σ :=
    is_shared N l P ∗ region_ptr γr id RegStatic.

  (* A STACK-region location has NO boundary capability — it cannot be
     handed to opaque/cinclude code. The capability requires RegStatic,
     which is exclusive with RegStack, so claiming both is contradictory:
     the stack-pointer-to-thread-spawning-C hole is inexpressible. *)
  Lemma stack_no_boundary_cap N l P γr id :
    boundary_cap N l P γr id -∗ region_ptr γr id RegStack -∗ False.
  Proof.
    iIntros "[_ Hstatic] Hstack".
    iApply (region_ptr_exclusive with "Hstatic Hstack").
  Qed.

  (* The boundary capability is PERSISTENT iff its region fact is — here
     we expose the SHARED half as the freely-duplicable knowledge the
     opaque code relies on (the invariant); the region half is the
     analyzer's one-time obligation discharged at the crossing. *)
  Lemma boundary_cap_shared N l P γr id :
    boundary_cap N l P γr id -∗ is_shared N l P.
  Proof. iIntros "[#Hs _]". iApply "Hs". Qed.

End boundary.
