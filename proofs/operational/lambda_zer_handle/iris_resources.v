(* ================================================================
   Phase 1a — Resource algebra for handle safety.

   Defines `alive_handle γ p i g : iProp Σ` — the Iris resource that
   encodes "this handle is currently alive" for pool p, slot i,
   generation g.

   Design:
     - A ghost map tracks the current generation of every slot:
         γ : gname
         state : gmap (pool_id * nat) nat
         the map sends slot (p,i) to its current generation.
     - `alive_handle γ p i g` owns the fragment `(p,i) ↪[γ] g`
       with full fraction (DfracOwn 1) — exclusive ownership.
     - Exclusivity means you CANNOT own two `alive_handle` for the
       same slot. This is the key property:
         * pool.alloc produces `alive_handle γ p i g` (fresh handle).
         * pool.free consumes it (rule: can't free without owning).
         * Double free is impossible: after consuming, the caller
           no longer has the resource.

   Why ghost_map (not plain ghost_var):
     - ghost_map gives `gmap`-style fragmented ownership (one
       fragment per slot). Each slot is independent.
     - pool.alloc allocates a fresh fragment for a new slot; old
       slots' resources stay untouched.

   Phase 1b will add state_interp connecting this ghost state to
   ZER's concrete runtime state (slot_gen in semantics.v).
   ================================================================ *)

From iris.base_logic.lib Require Import ghost_map.
From iris.proofmode Require Import proofmode.
From zer_handle Require Import syntax.

(* ---- Ghost-state type class ----

   `handleG Σ` says "the gFunctors Σ support the handle ghost map."
   Users of this module assume this class. `handleΣ` is the
   functor list that satisfies it; `subG_handleΣ` instantiates the
   class from inclusion in Σ. Standard Iris boilerplate. *)

Class handleG Σ := HandleG {
  handle_ghost_mapG :: ghost_mapG Σ (pool_id * nat) nat;
}.

Definition handleΣ : gFunctors := #[ghost_mapΣ (pool_id * nat) nat].

Global Instance subG_handleΣ Σ : subG handleΣ Σ → handleG Σ.
Proof. solve_inG. Qed.

(* ---- The resource ----

   Parameterized by a ghost name γ (the identity of the handle
   ghost map — a fresh one is allocated per program execution). *)

Section resources.
  Context `{!handleG Σ}.

  Definition alive_handle (γ : gname) (p : pool_id) (i : nat) (g : nat) : iProp Σ :=
    (p, i) ↪[γ] g.

  (* Exclusivity: you can't own two alive_handle for the same slot.
     This is THE double-free prevention at the resource level —
     every free consumes the resource, so you can't free twice. *)
  Lemma alive_handle_exclusive γ p i g1 g2 :
    alive_handle γ p i g1 -∗ alive_handle γ p i g2 -∗ False.
  Proof.
    iIntros "H1 H2".
    iDestruct (ghost_map_elem_valid_2 with "H1 H2") as %[Hv _].
    (* Hv : ✓ (DfracOwn 1 ⋅ DfracOwn 1) — which is False. *)
    rewrite dfrac_op_own in Hv.
    rewrite dfrac_valid_own in Hv.
    (* Hv : (1 + 1 ≤ 1)%Qp — contradiction *)
    by apply Qp.not_add_le_l in Hv.
  Qed.

  (* Agreement: if you own two alive_handle for the same slot
     (impossible per exclusivity), they agree on the generation.
     Useful as a building block; seldom used directly. *)
  Lemma alive_handle_agree γ p i g1 g2 :
    alive_handle γ p i g1 -∗ alive_handle γ p i g2 -∗ ⌜g1 = g2⌝.
  Proof.
    iIntros "H1 H2".
    iDestruct (ghost_map_elem_agree with "H1 H2") as %Heq.
    by iPureIntro.
  Qed.

  (* Allocation primitive: given the authoritative ghost map
     `ghost_map_auth γ 1 σ` and the knowledge that (p,i) is NOT
     yet in σ, we can carve out a fresh `alive_handle γ p i g`
     at any generation g.

     The auth fragment gains a new entry; the client gets
     exclusive ownership of the fragment. This will be used in
     Phase 1c to prove the wp spec for pool.alloc. *)
  Lemma alive_handle_new γ σ p i g :
    σ !! (p, i) = None →
    ghost_map_auth γ 1 σ ==∗
      ghost_map_auth γ 1 (<[(p, i) := g]> σ) ∗ alive_handle γ p i g.
  Proof.
    iIntros (Hfresh) "Hauth".
    iMod (ghost_map_insert (p, i) g Hfresh with "Hauth") as "[$ $]".
    done.
  Qed.

  (* Free primitive: consuming `alive_handle γ p i g` + authoritative
     map lets us bump the generation at (p,i) from g to g+1. The
     consumed resource is gone; re-alloc at (p,i) would produce a
     NEW alive_handle with generation g+1, not the old g. This is
     the generation-counter mechanism at the resource level. *)
  Lemma alive_handle_free γ σ p i g :
    ghost_map_auth γ 1 σ -∗ alive_handle γ p i g ==∗
      ghost_map_auth γ 1 (<[(p, i) := S g]> σ).
  Proof.
    iIntros "Hauth Hfrag".
    iMod (ghost_map_update (S g) with "Hauth Hfrag") as "[$ _]".
    done.
  Qed.

  (* Lookup: having `alive_handle γ p i g` implies the authoritative
     map maps (p,i) to g. Used when proving pool.get returns the
     right slot — get requires the resource and uses this to
     extract the current generation. *)
  Lemma alive_handle_lookup γ σ p i g :
    ghost_map_auth γ 1 σ -∗ alive_handle γ p i g -∗
      ⌜σ !! (p, i) = Some g⌝.
  Proof.
    iIntros "Hauth Hfrag".
    iDestruct (ghost_map_lookup with "Hauth Hfrag") as %Hlook.
    by iPureIntro.
  Qed.

End resources.

(* ---- Summary ---- *)

(*
   What this file delivers (Phase 1a):
     - `alive_handle γ p i g` : the Iris resource for handle safety.
     - `alive_handle_exclusive` : double-free prevention at resource level.
     - `alive_handle_agree` : two owners (impossible) agree on generation.
     - `alive_handle_new` : fresh resource on allocation.
     - `alive_handle_free` : consumed on free, bumps generation in auth.
     - `alive_handle_lookup` : auth sees same generation as fragment.

   Not yet:
     - state_interp (Phase 1b) — connects `ghost_map_auth γ 1 σ`
       to ZER's concrete `st_store` from semantics.v.
     - wp specs for pool.alloc/free/get (Phase 1c).
     - alloc→free program proof (Phase 1d).

   Covers `safety_list.md` rows:
     - A01-A02 (UAF) : via the resource absent after free
     - A06-A08 (double-free) : via exclusivity
     - A09-A11 (leak) : via residual resource at program end (adequacy)
     - A12 (ghost handle) : via wp_alloc binding the result resource
     - A17-A18 (runtime UAF trap) : redundant — resource discipline
       already rules out the trap condition at compile time
*)
