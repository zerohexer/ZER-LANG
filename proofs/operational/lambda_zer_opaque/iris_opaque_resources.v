(* ================================================================
   λZER-Opaque : Resource algebra for provenance tracking.

   `typed_ptr γ id t : iProp Σ` = "pointer instance `id` has
   provenance type `t`" — owned exclusively by the current
   reference holder.

   Uses ghost_map nat type_id. Owning the fragment certifies the
   pointer's type. Cast-back to the matching type consumes nothing
   (read-only check). Allocation produces a fresh resource.
   ================================================================ *)

From iris.base_logic.lib Require Import ghost_map.
From iris.proofmode Require Import proofmode.
From zer_opaque Require Import syntax.

(* Ghost-state typeclass *)
Class opaqueG Σ := OpaqueG {
  opaque_ghost_mapG :: ghost_mapG Σ nat type_id;
}.

Definition opaqueΣ : gFunctors := #[ghost_mapΣ nat type_id].

Global Instance subG_opaqueΣ Σ : subG opaqueΣ Σ → opaqueG Σ.
Proof. solve_inG. Qed.

Section resources.
  Context `{!opaqueG Σ}.

  (* The resource — parametric in γ (ghost name) and (id, t) pair. *)
  Definition typed_ptr (γ : gname) (id : nat) (t : type_id) : iProp Σ :=
    id ↪[γ] t.

  (* ---- Agreement ----

     Two owners of the same id must agree on the type tag. Used to
     justify the cast safety argument: if we claim *A at id but the
     stored tag is B, something's wrong. *)
  Lemma typed_ptr_agree γ id t1 t2 :
    typed_ptr γ id t1 -∗ typed_ptr γ id t2 -∗ ⌜t1 = t2⌝.
  Proof.
    iIntros "H1 H2".
    iDestruct (ghost_map_elem_agree with "H1 H2") as %Heq.
    by iPureIntro.
  Qed.

  (* ---- Exclusivity ----

     Can't own the resource twice. Same ghost_map DfracOwn 1
     pattern as alive_handle / alive_move. *)
  Lemma typed_ptr_exclusive γ id t1 t2 :
    typed_ptr γ id t1 -∗ typed_ptr γ id t2 -∗ False.
  Proof.
    iIntros "H1 H2".
    iDestruct (ghost_map_elem_valid_2 with "H1 H2") as %[Hv _].
    rewrite dfrac_op_own in Hv.
    rewrite dfrac_valid_own in Hv.
    by apply Qp.not_add_le_l in Hv.
  Qed.

  (* ---- Allocation primitive ---- *)
  Lemma typed_ptr_new γ (σ : gmap nat type_id) id t :
    σ !! id = None →
    ghost_map_auth γ 1 σ ==∗
      ghost_map_auth γ 1 (<[ id := t ]> σ) ∗ typed_ptr γ id t.
  Proof.
    iIntros (Hfresh) "Hauth".
    iMod (ghost_map_insert id t Hfresh with "Hauth") as "[$ $]".
    done.
  Qed.

  (* ---- Lookup ---- *)
  Lemma typed_ptr_lookup γ (σ : gmap nat type_id) id t :
    ghost_map_auth γ 1 σ -∗ typed_ptr γ id t -∗
      ⌜σ !! id = Some t⌝.
  Proof.
    iIntros "Hauth Hfrag".
    iDestruct (ghost_map_lookup with "Hauth Hfrag") as %Hlook.
    by iPureIntro.
  Qed.

End resources.
