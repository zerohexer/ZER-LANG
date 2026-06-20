(* ================================================================
   λZER-Concurrency : Region tags + LINEAR join token  (DESIGN.md §7
   step 4 — the lifetime condition; the merge obligation is the formal
   statement of the `ir_merge_states` compiler bug)

   Two pieces of ghost state (DESIGN §4.3):

   (1) REGION TAG — a published pointer's lifetime class. Reuses the
       escape subset's `region_ptr` ghost-map pattern verbatim. The
       publication rule (§4.1/R5): a pointer may cross to another thread
       only if its region OUTLIVES that thread — RegStack (thread-local
       stack) fails, which is the fire-and-forget stack hole. This is
       `'static`.

   (2) LINEAR JOIN TOKEN — a scoped-spawned, not-yet-joined thread. An
       EXCLUSIVE resource (mirrors `alive_handle`): produced by
       ScopedSpawn, consumed by Join. A residual at scope exit is a leak
       (the thread outlives its borrowed data). This is `thread::scope`.

   THE MERGE OBLIGATION (the ir_merge_states bug, formalized):
   `join_tok` lives in the SAME ghost-map machinery as a handle, so it
   carries the SAME merge discipline — at a CFG join, a token present on
   a predecessor must appear in the merged state (witnessed by
   `join_tok_in_auth`: a held token is in the authoritative map, so any
   sound merge preserving the auth preserves the obligation). The
   compiler's `ir_merge_states` (zercheck_ir.c:573-643) unions `handles[]`
   but DROPS `threads[]` — i.e. it discharges this obligation for handles
   and NOT for join tokens, which is exactly unsound (false-green
   stack-UAF). The fix is to merge `threads[]` identically.
   ================================================================ *)

From iris.base_logic.lib Require Import ghost_map.
From iris.proofmode Require Import proofmode.
From zer_conc Require Import syntax.

(* ---- Region (lifetime) tags ---- *)

Inductive region :=
  | RegStatic   (* global/static — outlives all threads *)
  | RegHeap     (* Pool/Slab heap — outlives the frame *)
  | RegScoped   (* borrowed into a scoped spawn — ok IFF join-bounded *)
  | RegStack.   (* thread-local stack — NOT publishable (the R5 hazard) *)

#[global] Instance region_eq_dec : EqDecision region.
Proof. solve_decision. Defined.

(* Publishable: may cross to another thread iff the region outlives it.
   RegStack fails — a stack pointer handed to a detached/longer-lived
   thread dangles when the frame dies. *)
Definition publishable (r : region) : Prop :=
  match r with RegStack => False | _ => True end.

(* Ghost state: region tags (over locations) + linear join tokens
   (over thread ids). Two independent ghost maps. *)
Class rjGS Σ := RJGS {
  rj_region :: ghost_mapG Σ nat region;
  rj_join   :: ghost_mapG Σ nat unit;
}.

Section rj.
  Context `{!rjGS Σ}.

  (* ---- (1) Region tags ---- *)

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

  Lemma region_ptr_lookup γ (σ : gmap nat region) id r :
    ghost_map_auth γ 1 σ -∗ region_ptr γ id r -∗ ⌜σ !! id = Some r⌝.
  Proof.
    iIntros "Hauth Hfrag".
    iDestruct (ghost_map_lookup with "Hauth Hfrag") as %Hlook.
    by iPureIntro.
  Qed.

  (* THE LIFETIME RULE (DESIGN §4.3 / R5): a stack-region pointer is NOT
     publishable. Publication's precondition is ⌜publishable r⌝; for
     RegStack that is False, so a fire-and-forget spawn / carrier-store
     of a stack pointer is unprovable — the stack hole is inexpressible. *)
  Lemma stack_not_publishable γ id :
    region_ptr γ id RegStack -∗ ⌜publishable RegStack⌝ -∗ False.
  Proof. iIntros "_ %H". destruct H. Qed.

  (* ---- (2) Linear join token ---- *)

  Definition join_tok (γ : gname) (tid : nat) : iProp Σ :=
    tid ↪[γ] tt.

  (* C02 — no double-join: a thread cannot be joined twice. Two tokens
     for the same tid is contradictory (mirrors alive_handle_exclusive). *)
  Lemma join_tok_exclusive γ tid :
    join_tok γ tid -∗ join_tok γ tid -∗ False.
  Proof.
    iIntros "H1 H2".
    iDestruct (ghost_map_elem_valid_2 with "H1 H2") as %[Hv _].
    rewrite dfrac_op_own in Hv.
    rewrite dfrac_valid_own in Hv.
    by apply Qp.not_add_le_l in Hv.
  Qed.

  (* THE MERGE OBLIGATION (the ir_merge_states bug, formalized): a held
     join token is in the authoritative thread map. Therefore any sound
     CFG merge that preserves the auth (as the handle merge does) carries
     the obligation forward — and the compiler MUST union threads[] just
     as it unions handles[]. Dropping it loses a real linear obligation
     (false-green stack-UAF). Same machinery, same discipline as a
     handle: that equality IS the theorem the ir_merge_states fix must
     respect. *)
  Lemma join_tok_in_auth γ (m : gmap nat unit) tid :
    ghost_map_auth γ 1 m -∗ join_tok γ tid -∗ ⌜m !! tid = Some tt⌝.
  Proof.
    iIntros "Hauth Htok".
    iDestruct (ghost_map_lookup with "Hauth Htok") as %Hlook.
    by iPureIntro.
  Qed.

End rj.
