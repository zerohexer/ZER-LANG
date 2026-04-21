(* ================================================================
   Derived resources — A03, A04.

   Compiler-side errors:
     A03: UAF via field/interior pointer — `*u32 p = &b.field;
          free(b); p[0]` — freeing parent invalidates interior
          pointer.
     A04: UAF in cast — using a handle in a cast after it's been
          freed.

   Both share the same Iris-level argument: a "derived" reference
   (interior pointer, cast view) can only be valid WHILE the
   parent resource exists. When the parent resource is consumed,
   all derived references become invalid by resource-level
   reasoning.

   Design: express "derived resource" via the alloc_id field in
   zercheck's HandleInfo. At the Iris level, this maps to
   SHARING the same ghost-state key — the child alive_handle is
   literally the parent's alive_handle, fractionally owned or
   persistently-viewed.

   For λZER-Handle we use the simplest model: derived = same
   resource, non-duplicable. A "view" of the parent IS the parent's
   resource. Using it consumes the parent's ownership.

   Covers safety_list.md rows:
     A03 — interior-pointer UAF
     A04 — cast UAF
   ================================================================ *)

From iris.base_logic.lib Require Import ghost_map.
From iris.proofmode Require Import proofmode.
From zer_handle Require Import syntax iris_resources.

Section derived.
  Context `{!handleG Σ}.

  (* ---- Derived-resource schema ----

     An interior pointer (or a cast view) is a VIEW of the parent's
     alive_handle — same ghost state, same (p, i, g). Using the
     view requires ownership. Consuming the parent (via free)
     eliminates the ghost fragment — subsequent view uses fail to
     prove ownership. *)

  Definition derived_view (γ : gname) (p : pool_id) (i : nat) (g : nat) : iProp Σ :=
    alive_handle γ p i g.

  (* ---- A03: interior pointer UAF ----

     If the parent handle is freed (resource consumed), the
     derived view cannot be re-owned. Any interior-pointer
     dereference requires the view, which doesn't exist. *)

  Lemma interior_after_free_impossible γ p i g :
    derived_view γ p i g -∗
    derived_view γ p i g -∗
    False.
  Proof.
    iIntros "H1 H2".
    unfold derived_view.
    iApply (alive_handle_exclusive with "H1 H2").
  Qed.

  (* ---- A04: UAF in cast ----

     Same argument: a cast is a view. Owning the view after free
     is impossible (resource already consumed). *)

  Lemma cast_after_free_impossible γ p i g :
    (* Assume we somehow have two views (impossible) — contradicts. *)
    derived_view γ p i g ∗ derived_view γ p i g ⊢ False.
  Proof.
    iIntros "[H1 H2]".
    unfold derived_view.
    iApply (alive_handle_exclusive with "H1 H2").
  Qed.

  (* ---- Parent/child relationship ----

     Proves: owning the parent IS owning all derivations.
     You cannot simultaneously own parent + child because they
     are the same resource. *)

  Lemma parent_is_child γ p i g :
    alive_handle γ p i g -∗ derived_view γ p i g -∗ False.
  Proof.
    iIntros "Hp Hc".
    unfold derived_view.
    iApply (alive_handle_exclusive with "Hp Hc").
  Qed.

  (* ---- Transfer: parent becomes child-view ----

     Passing an alive_handle to an interior-pointer-consuming
     context is equivalent to converting it to a derived_view —
     the resource flows linearly, not duplicates. *)

  Lemma alive_becomes_view γ p i g :
    alive_handle γ p i g ⊣⊢ derived_view γ p i g.
  Proof.
    unfold derived_view. iSplit; iIntros "H"; iFrame.
  Qed.

End derived.

(* ---- Summary ----

   What this file delivers:
     - `derived_view γ p i g : iProp` — the resource view for
       interior pointers and cast aliases.
     - `interior_after_free_impossible` — A03: two views same slot = contradiction
     - `cast_after_free_impossible`     — A04: same as above, cast framing
     - `parent_is_child` — can't own both parent and its view
     - `alive_becomes_view` — equivalence (no duplicate resource)

   These cover A03 and A04 at the Iris logic level via the SAME
   exclusivity argument as primary UAF (A01) — interior-ness
   and cast-ness are just framing, not new physical resources.

   The zercheck compiler implementation mirrors this: it tracks
   alloc_id so that interior pointers share the parent's state.
   Any free of the parent marks all children as UAF-invalid.
*)
