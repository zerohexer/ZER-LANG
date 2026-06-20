(* ================================================================
   λZER-Concurrency : Reach + Discipline structure  (DESIGN.md §7 steps 2-3)

   REFINEMENT of DESIGN §4.1/§4.2 discovered during implementation
   (strictly simpler — removes a ghost map, does NOT change the
   compiler-facing tracking set):

     The REACH condition is realized STRUCTURALLY, not by a separate
     share-tag ghost map. A location ℓ is:
       - LOCAL   ⟺ a thread exclusively owns `ℓ ↦ v` (gen_heap points-to
                   is already exclusive — only one thread can hold it, so
                   only one thread can access ℓ).
       - SHARED  ⟺ that points-to has been given up into an Iris invariant
                   `inv N (∃ w, ℓ ↦ w ∗ P w)`. Iris invariants are
                   PERSISTENT ⟹ freely duplicable across threads ⟹ ℓ is
                   cross-thread-reachable, but ONLY through the invariant.

   This is the RustBelt mechanism: the DISCIPLINE condition (§4.2) falls
   out for free — there is no rule to obtain `ℓ ↦ w` for a SHARED ℓ
   except by opening its invariant (atomically / under the lock), so an
   unsynchronized access to shared data is UNPROVABLE ⟹ the data-race
   stuck state is unreachable. The compiler still tracks a 1-bit `shared`
   taint per location (DESIGN §5) — that bit is the syntactic
   approximation of "is the points-to in an invariant"; the tracking set
   is unchanged.

   PUBLICATION is the reach transition: a thread holding `ℓ ↦ v` (Local)
   establishes the payload and gives the points-to up into a fresh
   invariant, receiving the persistent `is_shared` token. This is the
   ONLY way to make ℓ cross-thread-reachable, so every shared location is
   guarded by construction.
   ================================================================ *)

From iris.base_logic.lib Require Import gen_heap invariants.
From iris.proofmode Require Import proofmode.
From zer_conc Require Import syntax semantics iris_lang iris_state.

Section shared.
  Context `{!concGS Σ}.

  (* The bare `↦` notation is not active in this Iris build (only the
     `pointsto` function); a local notation keeps the math readable. *)
  Local Notation "l '↦' v" := (pointsto l (DfracOwn 1) v)
    (at level 20, format "l  '↦'  v") : bi_scope.

  (* SHARED: ℓ's points-to lives inside an invariant guarding payload P.
     Persistent ⟹ the reach knowledge is duplicable across threads. *)
  Definition is_shared (N : namespace) (l : loc) (P : val -> iProp Σ) : iProp Σ :=
    inv N (∃ w, l ↦ w ∗ P w)%I.

  Global Instance is_shared_persistent N l P : Persistent (is_shared N l P).
  Proof. apply _. Qed.

  (* PUBLICATION (reach transition, DESIGN §4.1): exclusive ownership of
     `l ↦ v` (Local) + the payload `P v` is consumed to allocate the
     invariant; the caller gets the persistent `is_shared`. After this
     the points-to is INSIDE the invariant — no thread (incl. the
     publisher) can access l except by opening it (discipline, §4.2). *)
  Lemma publish_shared N l v (P : val -> iProp Σ) :
    l ↦ v -∗ P v ={⊤}=∗ is_shared N l P.
  Proof.
    iIntros "Hl HP".
    iMod (inv_alloc N ⊤ (∃ w, l ↦ w ∗ P w)%I with "[Hl HP]") as "#Hinv".
    { iNext. iExists v. iFrame. }
    iModIntro. rewrite /is_shared. iExact "Hinv".
  Qed.

  (* NOTE (step 3): the "no data race on a LOCAL location" lemma —
     `l ↦ v1 -∗ l ↦ v2 -∗ False` via gen_heap points-to exclusivity —
     lands with the guarded load/store specs in iris_shared_specs.v,
     once the exact pointsto-exclusivity lemma name is confirmed against
     this Iris version. The structure here (is_shared + publish) is the
     reach/discipline foundation those specs build on. *)

End shared.
