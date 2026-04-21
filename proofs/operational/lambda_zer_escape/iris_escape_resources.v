(* ================================================================
   λZER-Escape : Resource algebra for region tracking.

   `region_ptr γ id r : iProp` = "pointer instance `id` has region
   tag `r`" — exclusive ownership. Pinned by the resource and the
   state_interp so you can't claim a different region.

   The key theorems rely on AGREEMENT: if you own `region_ptr γ id
   RegLocal` and try to use it where RegStatic is required, the
   agreement lemma forces RegLocal = RegStatic, contradiction.
   ================================================================ *)

From iris.base_logic.lib Require Import ghost_map.
From iris.proofmode Require Import proofmode.
From zer_escape Require Import syntax.

Class escapeG Σ := EscapeG {
  escape_ghost_mapG :: ghost_mapG Σ nat region;
}.

Definition escapeΣ : gFunctors := #[ghost_mapΣ nat region].

Global Instance subG_escapeΣ Σ : subG escapeΣ Σ → escapeG Σ.
Proof. solve_inG. Qed.

Section resources.
  Context `{!escapeG Σ}.

  Definition region_ptr (γ : gname) (id : nat) (r : region) : iProp Σ :=
    id ↪[γ] r.

  Lemma region_ptr_agree γ id r1 r2 :
    region_ptr γ id r1 -∗ region_ptr γ id r2 -∗ ⌜r1 = r2⌝.
  Proof.
    iIntros "H1 H2".
    iDestruct (ghost_map_elem_agree with "H1 H2") as %Heq.
    by iPureIntro.
  Qed.

  Lemma region_ptr_exclusive γ id r1 r2 :
    region_ptr γ id r1 -∗ region_ptr γ id r2 -∗ False.
  Proof.
    iIntros "H1 H2".
    iDestruct (ghost_map_elem_valid_2 with "H1 H2") as %[Hv _].
    rewrite dfrac_op_own in Hv.
    rewrite dfrac_valid_own in Hv.
    by apply Qp.not_add_le_l in Hv.
  Qed.

  Lemma region_ptr_new γ (σ : gmap nat region) id r :
    σ !! id = None →
    ghost_map_auth γ 1 σ ==∗
      ghost_map_auth γ 1 (<[ id := r ]> σ) ∗ region_ptr γ id r.
  Proof.
    iIntros (Hfresh) "Hauth".
    iMod (ghost_map_insert id r Hfresh with "Hauth") as "[$ $]".
    done.
  Qed.

  Lemma region_ptr_lookup γ (σ : gmap nat region) id r :
    ghost_map_auth γ 1 σ -∗ region_ptr γ id r -∗
      ⌜σ !! id = Some r⌝.
  Proof.
    iIntros "Hauth Hfrag".
    iDestruct (ghost_map_lookup with "Hauth Hfrag") as %Hlook.
    by iPureIntro.
  Qed.

End resources.
