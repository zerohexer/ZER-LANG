(* ================================================================
   λZER-Move : Iris resource algebra for move-struct linearity.

   `alive_move γ id : iProp Σ` — exclusive ownership of "move-struct
   id is currently alive." Mirrors λZER-Handle's alive_handle but
   keyed by a single nat instead of (pool_id * nat * nat).

   Using `ghost_map move_id unit` — the value type is unit since
   we only care about PRESENCE, not any associated data.
   ================================================================ *)

From iris.base_logic.lib Require Import ghost_map.
From iris.proofmode Require Import proofmode.
From zer_move Require Import syntax.

(* ---- Ghost-state typeclass ---- *)

Class moveG Σ := MoveG {
  move_ghost_mapG :: ghost_mapG Σ move_id unit;
}.

Definition moveΣ : gFunctors := #[ghost_mapΣ move_id unit].

Global Instance subG_moveΣ Σ : subG moveΣ Σ → moveG Σ.
Proof. solve_inG. Qed.

(* ---- The resource ---- *)

Section resources.
  Context `{!moveG Σ}.

  Definition alive_move (γ : gname) (id : move_id) : iProp Σ :=
    id ↪[γ] tt.

  (* ---- Exclusivity — the foundational linearity property ----

     Cannot own two alive_move for the same id. Direct consequence
     of ghost_map_elem's DfracOwn 1 fraction: combining with itself
     yields an invalid fraction (1+1 > 1), which is False. *)
  Lemma alive_move_exclusive γ id :
    alive_move γ id -∗ alive_move γ id -∗ False.
  Proof.
    iIntros "H1 H2".
    iDestruct (ghost_map_elem_valid_2 with "H1 H2") as %[Hv _].
    rewrite dfrac_op_own in Hv.
    rewrite dfrac_valid_own in Hv.
    by apply Qp.not_add_le_l in Hv.
  Qed.

  (* ---- Allocation primitive ---- *)
  Lemma alive_move_new γ (σ : gmap move_id unit) id :
    σ !! id = None →
    ghost_map_auth γ 1 σ ==∗
      ghost_map_auth γ 1 (<[ id := tt ]> σ) ∗ alive_move γ id.
  Proof.
    iIntros (Hfresh) "Hauth".
    iMod (ghost_map_insert id tt Hfresh with "Hauth") as "[$ $]".
    done.
  Qed.

  (* ---- Consumption primitive: delete from ghost map ----

     After consume/drop, the id is no longer "live" — the ghost
     entry is removed. A subsequent operation cannot produce
     alive_move for this id (would need the auth to have it). *)
  Lemma alive_move_consume γ (σ : gmap move_id unit) id :
    ghost_map_auth γ 1 σ -∗ alive_move γ id ==∗
      ghost_map_auth γ 1 (delete id σ).
  Proof.
    iIntros "Hauth Hfrag".
    iMod (ghost_map_delete with "Hauth Hfrag") as "$".
    done.
  Qed.

  (* ---- Lookup: fragment implies auth has the key ---- *)
  Lemma alive_move_lookup γ (σ : gmap move_id unit) id :
    ghost_map_auth γ 1 σ -∗ alive_move γ id -∗
      ⌜σ !! id = Some tt⌝.
  Proof.
    iIntros "Hauth Hfrag".
    iDestruct (ghost_map_lookup with "Hauth Hfrag") as %Hlook.
    iPureIntro. exact Hlook.
  Qed.

End resources.
