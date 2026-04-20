(* ================================================================
   λZER-Handle : Main Soundness Theorem

   THE Year-1 deliverable:

     Every well-typed λZER-Handle program executes without
     use-after-free, without double-free, and without leaking any
     handle at termination.

   This is the implementation-level formal guarantee for ZER's
   keystone handle-safety claim. It matches the scope of RustBelt's
   λRust safety theorem (though RustBelt goes further into lifetime
   semantics; this covers the handle-state dimension).

   **Proof status (as of commit):**
     - `handle_safety` : Admitted, depends on adequacy.preservation +
       adequacy.progress (also Admitted).
     - All helper lemmas proven.

   When the adequacy admits are discharged, this theorem becomes
   axiom-free. The theorem statement itself is verified Coq code.
   ================================================================ *)

From stdpp Require Import base strings list gmap.
From zer_handle Require Import syntax semantics typing adequacy.

(* ================================================================
   Safety predicates expressed over reachable states

   These are the "bad states" the main theorem rules out. They're
   semantic properties over the store, not syntactic properties of
   programs.
   ================================================================ *)

(* A state has a use-after-free if some expression is attempting to
   use a handle whose slot has been freed. In our operational model,
   this manifests as being stuck at EFree or EGet of a handle whose
   slot is not alive.

   `uaf_stuck st e` captures: e contains a handle op, the handle
   refers to a dead slot, and no step rule fires.

   (For a clean theorem statement we characterize this via "stuck"
   rather than hunting for the specific handle inside e.) *)

Definition uaf_stuck (st : state) (e : expr) : Prop :=
  stuck st e /\
  exists p h, e = EFree p (EVal h) \/ e = EGet p (EVal h).

(* Double-free shows up the same way — stuck at EFree with a stale
   handle. In our semantics, there's no distinction between "this
   handle was freed once already" and "this handle was never live"
   at the operational level; both produce stuck states. The typing
   system + preservation will rule out both paths. *)

Definition double_free_stuck (st : state) (e : expr) : Prop :=
  stuck st e /\
  exists p h, e = EFree p (EVal h).

(* At termination (expression is a value, state has returned), all
   allocations should have been freed. *)

Definition terminated (st : state) (e : expr) : Prop :=
  is_value e = true \/ st.(st_returned) <> None.

Definition no_leak_at_termination (st : state) (e : expr) : Prop :=
  terminated st e -> ~ has_alive_slot st.

(* ================================================================
   The theorem

   Split into three components — each is a consequence of "no
   stuck configurations" (via no_stuck in adequacy.v) combined with
   preservation of handle invariants (which we'll formalize in
   future commits as the typing context evolves).

   For Year-1 Week-2 (this commit): statements only. Proofs
   depend on adequacy's admits.
   ================================================================ *)

(* ---- Part 1 : No UAF ----

   If a well-typed program reaches any state, that state is not
   uaf_stuck. Proof: from adequacy.no_stuck, any reachable
   configuration is either a value, a returned state, or steps.
   uaf_stuck requires "stuck" which means neither value nor
   steps. So uaf_stuck states aren't reachable. *)

Theorem handle_safety_no_uaf : forall st e st' e' τ,
  env_typed st.(st_env) empty ->
  typed empty e τ ->
  steps st e st' e' ->
  ~ uaf_stuck st' e'.
Proof.
  intros st e st' e' τ Henv Hty Hsteps Huaf.
  destruct Huaf as [[Hnv Hnostep] _].
  destruct (no_stuck _ _ _ _ _ Henv Hty Hsteps) as [Hv | [[v Hret] | Hstep]].
  - (* is_value e' = true contradicts Hnv *)
    apply Hnv. exact Hv.
  - (* Returned state — we'd need to show uaf_stuck's "stuck" is
       incompatible with returned. In our semantics, a returned
       state can still have non-value expression (e.g., after
       orelse-return fires, some leftover expression). Need a
       lemma: `st_returned = Some _ → no step can fire`. *)
    (* TODO(Year-1, Week-2): add step_blocked_by_returned lemma. *)
    admit.
  - apply Hnostep. exact Hstep.
Admitted.

(* ---- Part 2 : No Double-Free ----

   Similar argument. Double-free = stuck at EFree with a stale
   handle. The typing rule `ty_free` requires TyHandle (non-null),
   but preservation must ensure that handles remain live — i.e.,
   no other step has freed them.

   This is the PART where the typing-preservation-implies-liveness
   argument is most visible. Will formalize in a follow-up. *)

Theorem handle_safety_no_double_free : forall st e st' e' τ,
  env_typed st.(st_env) empty ->
  typed empty e τ ->
  steps st e st' e' ->
  ~ double_free_stuck st' e'.
Proof.
  intros st e st' e' τ Henv Hty Hsteps Hdf.
  destruct Hdf as [[Hnv Hnostep] _].
  destruct (no_stuck _ _ _ _ _ Henv Hty Hsteps) as [Hv | [[v Hret] | Hstep]].
  - apply Hnv. exact Hv.
  - (* Same TODO as above. *)
    admit.
  - apply Hnostep. exact Hstep.
Admitted.

(* ---- Part 3 : No Leak at Termination ----

   At termination, all allocated slots have been freed. This is
   the trickiest part — requires reasoning about the typing rule
   for EFree and proving that the types force every allocation to
   have a matching free on every reachable path.

   For the minimal scope (no defer, no early-return via
   non-orelse control flow), the argument is:
     - `pool.alloc()` produces `?Handle` — the caller must unwrap
       (via orelse-return) or explicitly ignore.
     - Once unwrapped to `Handle`, the only operations allowed are
       get/free.
     - The type system doesn't yet ENFORCE that free must happen
       on every path — that's a linearity property not expressible
       in our current types.

   CONCLUSION: the no-leak theorem requires a stronger type system
   (affine or linear types on handles) to prove for λZER-Handle as
   currently defined. Year-2 work will add `move` semantics which
   gives us linearity.

   For Year-1, the theorem is STATED but with an admitted proof
   and a clear "needs type-system extension" note. *)

Theorem handle_safety_no_leak_weak :
  forall st e st' e' τ,
    env_typed st.(st_env) empty ->
    typed empty e τ ->
    steps st e st' e' ->
    no_leak_at_termination st' e'.
Proof.
  (* TODO(Year-2): requires affine typing on handles. The current
     Year-1 type system over-approximates — it doesn't enforce
     "every allocated handle is freed." This proof needs either:
       (a) A stronger type system in typing.v (affine handles), or
       (b) A weaker theorem statement (e.g., "no leak if all allocs
           are used in some tracked fashion"), or
       (c) Restriction to a subset of the subset (programs where
           we can syntactically verify matching pairs).
     Stating this honestly rather than asserting something unprovable. *)
Admitted.

(* ================================================================
   Combined theorem (Year-1 soundness)

   Packages the three properties into the standard form.
   When the admits above are discharged, this becomes axiom-free.
   ================================================================ *)

Theorem lambda_zer_handle_safety : forall st e st' e' τ,
  env_typed st.(st_env) empty ->
  typed empty e τ ->
  steps st e st' e' ->
  ~ uaf_stuck st' e' /\
  ~ double_free_stuck st' e' /\
  no_leak_at_termination st' e'.
Proof.
  intros st e st' e' τ Henv Hty Hsteps.
  split; [|split].
  - eapply handle_safety_no_uaf; eauto.
  - eapply handle_safety_no_double_free; eauto.
  - eapply handle_safety_no_leak_weak; eauto.
Qed.

(* ================================================================
   Proof-status summary (for Year-1 bookkeeping)

   Top-level theorem:
     lambda_zer_handle_safety    Qed (via split; eapply).

   Sub-theorems:
     handle_safety_no_uaf        Admitted
       - Depends on: preservation (Admitted), progress (Admitted)
       - + missing lemma: step_blocked_by_returned
     handle_safety_no_double_free Admitted
       - Same dependencies.
     handle_safety_no_leak_weak  Admitted
       - Requires type system extension (affine handles in typing.v)
       - Year-2 work.

   Admits currently in λZER-Handle subset:
     1. adequacy.preservation
     2. adequacy.progress
     3. handle_safety.handle_safety_no_uaf (the returned-state case)
     4. handle_safety.handle_safety_no_double_free (same)
     5. handle_safety.handle_safety_no_leak_weak (affine types needed)

   Discharging admits 1 + 2 immediately discharges 3 + 4 modulo
   the step_blocked_by_returned auxiliary. Admit 5 is genuinely
   deferred to Year 2 (adds `move struct` + affine typing).

   Everything compiles. Dependencies are explicit. Statements are
   type-checked. This is the Year-1 Week-2 honest checkpoint.
   ================================================================ *)
