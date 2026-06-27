(* ================================================================
   lambda_zer_handle / handle_flow_lattice : the FLOW-SENSITIVE handle state
   lattice — the MAX oracle for the use-after-free / MAYBE_FREED class.

   WHY THIS FILE.  The handle class (UAF / double-free / leak) is the compiler's
   #1 safety job, but had the WEAKEST oracle: the operational track
   (handle_safety.v / adequacy.v) leaves its soundness obligations unproved, and
   the gated Iris track is a flat 2-state exclusive resource that never models
   the 3-valued ALIVE/FREED/MAYBE_FREED the compiler actually runs. So the
   abstract domain was uncertified — the root of (a) the idiomatic over-rejection
   `if(c){free(h)} if(!c){use(h)}` and (b) the MAX-oracle audit's "no real handle
   oracle" finding. This file certifies the domain + decision, self-contained,
   param_lattice.v style, ZERO admits.

   TWO LEVELS (climbing the standard, not stopping at the floor):

   (A) the SOUNDNESS lattice — finite states {Uninit, Alive, Freed, Maybe} with
       merge = JOIN (widen-toward-freed). Decision: a USE (deref / field /
       free) requires ALIVE. Certifies no UAF (T1 use_sound), the conservative
       JOIN covers both predecessors (join_covers_left/right), and a FREE on ANY
       branch blocks the post-merge use (T4 join_freed_blocks_use — exactly the
       MAYBE_FREED conservatism the linear compiler implements, here CERTIFIED).
       Transferred is the separate MOVE axis (lambda_zer_move); Uninit is
       pre-allocation. This is the floor — sound, but it OVER-rejects.

   (B) the GUARDED refinement — the rich abstraction that DRIVES THE
       OVER-REJECTION DOWN. A state is qualified by the PATH PREDICATE it holds
       under: `if(c){free} if(!c){use}` frees only under `c`, uses only under
       `!c`, and `c /\ !c = False`, so the use never sees a freed handle. A use
       under guard Gu is safe iff Gu is DISJOINT from the free guard Gf. This
       RECOVERS the idiomatic case the flat lattice rejects, with NO soundness
       loss: a non-disjoint guard pair is unprovable -> falls back to reject
       (guarded_not_disjoint_rejects). Level B's IMPLEMENTATION SHIPPED 2026-06-27
       in zercheck_ir.c (per-block immutable-bool guard sets + per-handle
       free_block/freed_all_paths; soundness gate ir_local_is_immutable_bool).
       Both Level A (the conservative floor) and Level B (the recovery) now run.
       See BUGS-FIXED.md 2026-06-27.
   ================================================================ *)
From Coq Require Import Bool.

(* ================================================================
   LEVEL A — the soundness lattice (finite states + JOIN merge).
   ================================================================ *)

(* THE FINITE STATE SET for the flow lattice. *)
Inductive hstate := HUninit | HAlive | HFreed | HMaybe.

(* the concrete truth a state abstracts: is the handle freed on THIS path?
   HAlive => provably not, HFreed => provably yes, HMaybe/HUninit => unconstrained
   (and use_ok rejects them, so they never license a use). *)
Definition covers (s : hstate) (freed : bool) : Prop :=
  match s with
  | HAlive  => freed = false
  | HFreed  => freed = true
  | HMaybe  => True
  | HUninit => True
  end.

(* THE DECISION: a USE is allowed iff the handle is definitely ALIVE. *)
Definition use_ok (s : hstate) : bool :=
  match s with HAlive => true | _ => false end.

(* (T1) USE SOUNDNESS — an allowed use is on a non-freed handle (no UAF). *)
Theorem use_sound : forall s freed,
  use_ok s = true -> covers s freed -> freed = false.
Proof. intros [] freed Hu Hc; simpl in *; (discriminate || exact Hc). Qed.

(* THE CONTROL-FLOW MERGE = JOIN (union of possibilities): any disagreement
   widens to HMaybe (conservative). HAlive (|) HFreed = HMaybe. *)
Definition hjoin (a b : hstate) : hstate :=
  match a, b with
  | HAlive,  HAlive  => HAlive
  | HFreed,  HFreed  => HFreed
  | HUninit, HUninit => HUninit
  | _, _ => HMaybe
  end.

(* MERGE SOUNDNESS — the join COVERS both predecessors, so deciding on the
   merged state is sound on every incoming path (the handle analog of
   bounds_lattice.v's join_covers_left/right). *)
Theorem join_covers_left : forall a b freed,
  covers a freed -> covers (hjoin a b) freed.
Proof. intros [] [] freed H; simpl in *; (exact H || exact I). Qed.

Theorem join_covers_right : forall a b freed,
  covers b freed -> covers (hjoin a b) freed.
Proof. intros [] [] freed H; simpl in *; (exact H || exact I). Qed.

(* (T4) NO UNDER-REJECTION — a FREE on ANY incoming branch blocks the use after
   the merge: joining anything with HFreed is never HAlive, so use_ok is false.
   This is the soundness core (no UAF after a partial free) — the exact
   MAYBE_FREED conservatism the linear compiler runs, here CERTIFIED. *)
Theorem join_freed_blocks_use : forall a, use_ok (hjoin a HFreed) = false.
Proof. intros []; reflexivity. Qed.

(* (T3) PRECISION (basic) — a handle ALIVE on all incoming paths stays usable. *)
Theorem join_alive_usable : use_ok (hjoin HAlive HAlive) = true.
Proof. reflexivity. Qed.

(* ================================================================
   LEVEL B — the GUARDED refinement (drive the over-rejection down).
   A free guard Gf : world -> bool is true on worlds where the handle was freed;
   a use guard Gu is true on worlds where the use executes. The use is SAFE iff
   every use-world is a non-free-world (the guards are DISJOINT).
   ================================================================ *)
Section Guarded.
  Variable world : Type.

  Definition guarded_use_ok (gf gu : world -> bool) : Prop :=
    forall w, gu w = true -> gf w = false.

  (* GUARDED USE SOUNDNESS — under a satisfied guarded decision, the handle is
     not freed on the actual use path (no UAF). *)
  Theorem guarded_use_sound : forall gf gu w,
    guarded_use_ok gf gu -> gu w = true -> gf w = false.
  Proof. intros gf gu w H Hu. apply H. exact Hu. Qed.

  (* GUARDED NO-UNDER-REJECTION — if some world both frees AND uses (the guards
     are NOT disjoint), the decision CANNOT be satisfied: a genuine UAF is never
     accepted, so the refinement only ever recovers SOUND cases. *)
  Theorem guarded_not_disjoint_rejects : forall gf gu,
    (exists w, gf w = true /\ gu w = true) ->
    ~ guarded_use_ok gf gu.
  Proof.
    intros gf gu [w [Hf Hu]] Hok.
    specialize (Hok w Hu). rewrite Hf in Hok. discriminate.
  Qed.

  (* THE PRECISION WITNESS — `if(c){free(h)} if(!c){use(h)}`: free guard = c,
     use guard = negb c. The flat lattice REJECTS (after the free branch the
     state JOINs to HMaybe, use_ok HMaybe = false); the guarded domain ACCEPTS
     because c and (negb c) are disjoint. This is the idiomatic MAYBE_FREED
     over-rejection, RECOVERED, with zero soundness loss. *)
  Theorem maybe_freed_correlation_recovered : forall c : world -> bool,
    use_ok HMaybe = false                            (* flat lattice: rejected *)
    /\ guarded_use_ok c (fun w => negb (c w)).       (* guarded domain: accepted *)
  Proof.
    intro c. split.
    - reflexivity.
    - intros w Hu. simpl in Hu. apply negb_true_iff in Hu. exact Hu.
  Qed.
End Guarded.
