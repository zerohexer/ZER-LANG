(* ================================================================
   lambda_zer_concurrency / flag_handoff : the FLAG HAND-OFF.

   WHY THIS FILE.  The checker accepts two plain accesses of one global by
   two threads when "a flag orders them":

       writer:  data = 42;  @atomic_store(&ready, 1);
       reader:  while (@atomic_load(&ready) == 0) { }   sink = data;

   That idiom recogniser shipped (BUG-1562 / 1563 / 1565) with tests and no
   model, exactly as the spin-lock one had (lock_region.v, BUG-1663).  It
   matched SHAPES — "some later statement stores the flag", "some earlier
   loop reads the flag" — and four accepted programs raced under
   ThreadSanitizer: a writer that publishes again in a loop or through a
   callee, a "wait" with the wrong polarity, a loop that reads the flag and
   leaves for another reason (BUG-1664).

   THE MODEL.  The flag is a TOKEN: its current value says who may touch the
   data.  Threads take turns executing events on one flag word:
       EObs t v   an atomic load by t returned v (enabled only while the
                  flag holds v); t now HOLDS v
       ESto t x   an atomic store of x by t (ALWAYS enabled — a store asks
                  nobody); t holds nothing afterwards
       EAcc t     t touches the guarded data
   `hold t` is ghost state: the value t last observed, if it has not stored
   since.  `storing v` says some thread stores the flag FROM v (while holding
   v) and `owner v` names the one thread that may.

   WHAT THE COMPILER MUST ESTABLISH, as hypotheses on every execution:
     claim    a value some thread stores from is observed only by its
              owner                                                 (H_obs)
     handoff  every store is made by a thread that HOLDS a value and is
              that value's owner                                    (H_sto)
     start    the initial value is held, before any event, by its owner
              alone if it is a storing value — by everybody if nobody ever
              stores from it                                       (H_init)

   THE THEOREMS (zero admits, plain Coq, self-contained):
     T1  holders_agree      claim + handoff ==> in every reachable state,
                            every thread that holds, holds the CURRENT value.
     T1' claims_exclusive   ... so two accesses made under two DIFFERENT
                            held values are never enabled together: no data
                            race between them.  (Two accesses under the SAME
                            value are not ordered by the flag at all — the
                            compiler must judge them some other way.)
     T3  precision witnesses   the one-shot publication; a ping-pong that
                            goes round twice; two readers of one terminal
                            value.
     T4  necessity, one trace per hypothesis dropped:
         republication_is_unheld    an access after the thread's own store
                                    is an access by a thread that holds
                                    nothing, while the reader holds;
         forged_store_breaks        without handoff (a store by a thread
                                    that holds nothing), two threads hold
                                    two different values at once;
         second_claimant_breaks     without claim (a second observer of a
                                    storing value), likewise;
         wrong_polarity_same_value  a "wait" for the INITIAL value: both
                                    threads hold the same value, and the
                                    flag orders nothing.

   WHAT THE COMPILER DISCHARGES (checker.c, the "flag hand-off" block), each
   by one syntactic check:
     hold     an access is tagged with the value its thread holds: EXPLICIT
              (dominated by an observation `load == V`, with no store of the
              flag by that thread since) or IMPLICIT (it precedes the thread's
              single, unrepeated first store, so the thread still holds the
              initial value).  `EObs` here is a GRANTED observation: for a
              value some thread stores from, only that thread's tags count;
              any other observer of it is treated as holding nothing — which
              is how `claim` is discharged, not assumed.
     handoff  every store SITE of the flag in the program is met by a thread
              walk, under a tag, in every context it is met in; it cannot
              also run where no walk looks (a function the main thread
              reaches outside a spawn-to-join window); the value stored from
              is stored from by one thread, running once.
     start    at most one site stores without a wait, and it runs once; every
              plain write of the flag (legal only where no thread runs) puts
              the initial value back; the flag is one shared word whose every
              atomic use is a load or a store of a literal.

   HONEST LIMITS.  (1) This certifies the RULE; that the syntactic tags are
   right on every execution is argued at the checks and gated by the
   "hand-off" grid of tests/test_conc_matrix.c, not derived here.  (2)
   Sequentially consistent interleaving (every @atomic_* is SEQ_CST).  (3)
   Liveness (a wait that never ends) is out of scope — a floor.  (4) One
   flag at a time: an access ordered by two different flags is judged on
   each alone.  (5) The data itself is not modelled: "EAcc" is any access,
   so read-read sharing under one terminal value is simply not claimed
   exclusive.
   ================================================================ *)
From Coq Require Import List Bool Arith ZArith Lia.
Import ListNotations.
From zer_conc Require Import concurrency_decisions.
Local Open Scope nat_scope.

Section handoff.
  Variable thread : Type.
  Hypothesis thread_eq_dec : forall a b : thread, {a = b} + {a <> b}.
  Variable init_v : nat.               (* the flag's static initial value *)
  Variable storing : nat -> bool.      (* some thread stores the flag FROM v *)
  Variable owner : nat -> thread.      (* ... and this is the one that may *)

  Inductive event :=
  | EObs (t : thread) (v : nat)
  | ESto (t : thread) (x : nat)
  | EAcc (t : thread).

  Record state := St { flag : nat; hold : thread -> option nat }.

  (* H_init *)
  Definition init : state :=
    St init_v (fun t => if storing init_v
                        then (if thread_eq_dec t (owner init_v) then Some init_v else None)
                        else Some init_v).

  Definition upd (f : thread -> option nat) (t : thread) (o : option nat) : thread -> option nat :=
    fun k => if thread_eq_dec k t then o else f k.

  Definition step (s : state) (ev : event) : option state :=
    match ev with
    | EObs t v => if Nat.eq_dec (flag s) v then Some (St (flag s) (upd (hold s) t (Some v))) else None
    | ESto t x => Some (St x (upd (hold s) t None))
    | EAcc _ => Some s
    end.

  (* ---- the hypotheses, per event ---- *)
  Definition claim (ev : event) : Prop :=
    match ev with EObs t v => storing v = true -> t = owner v | _ => True end.

  Definition handoff (s : state) (ev : event) : Prop :=
    match ev with
    | ESto t _ => exists v, hold s t = Some v /\ storing v = true /\ owner v = t
    | _ => True
    end.

  Definition disciplined (s : state) (ev : event) : Prop := claim ev /\ handoff s ev.

  Inductive exec (P : state -> event -> Prop) : state -> list event -> state -> Prop :=
  | exec_nil s : exec P s [] s
  | exec_cons s e s' tr s'' :
      step s e = Some s' -> P s e -> exec P s' tr s'' -> exec P s (e :: tr) s''.

  (* ---- the invariant: a holder holds the CURRENT value, and a storing value
     is held by its owner only ---- *)
  Definition inv (s : state) : Prop :=
    forall t v, hold s t = Some v -> flag s = v /\ (storing v = true -> t = owner v).

  Lemma inv_init : inv init.
  Proof.
    intros t v H. unfold init in *. simpl in *.
    destruct (storing init_v) eqn:Hs.
    - destruct (thread_eq_dec t (owner init_v)) as [->|N]; [|discriminate].
      inversion H; subst. split; [reflexivity | intros _; reflexivity].
    - inversion H; subst. split; [reflexivity | intros Hbad; congruence].
  Qed.

  Lemma upd_same f t o : upd f t o t = o.
  Proof. unfold upd. destruct (thread_eq_dec t t); congruence. Qed.

  Lemma upd_other f t o k : k <> t -> upd f t o k = f k.
  Proof. intros H. unfold upd. destruct (thread_eq_dec k t); congruence. Qed.

  Lemma inv_step s ev s' :
    inv s -> step s ev = Some s' -> disciplined s ev -> inv s'.
  Proof.
    intros Hi Hstep [Hc Hh]. destruct ev as [t v|t x|t]; simpl in *.
    - (* an observation: t now holds the value the flag has *)
      destruct (Nat.eq_dec (flag s) v) as [Hf|Hf]; [|discriminate].
      inversion Hstep; subst; clear Hstep. intros k w Hk. simpl in *.
      destruct (thread_eq_dec k t) as [->|N].
      + rewrite upd_same in Hk. inversion Hk; subst. split; [reflexivity | exact Hc].
      + rewrite upd_other in Hk by assumption. apply Hi. exact Hk.
    - (* a store by the owner of the value it holds: nobody else was holding,
         because every holder holds the current value and that value has one
         owner *)
      destruct Hh as [v [Hv [Hsv Hov]]].
      inversion Hstep; subst; clear Hstep. intros k w Hk. simpl in *.
      destruct (thread_eq_dec k (owner v)) as [->|N].
      + rewrite upd_same in Hk. discriminate.
      + rewrite upd_other in Hk by assumption.
        exfalso. apply N.
        destruct (Hi _ _ Hk) as [Hfw Hw]. destruct (Hi _ _ Hv) as [Hfv _].
        assert (Ew : w = v) by congruence. rewrite Ew in Hw. apply Hw. exact Hsv.
    - inversion Hstep; subst. exact Hi.
  Qed.

  Theorem reachable_inv P tr s :
    (forall x e, P x e -> disciplined x e) -> exec P init tr s -> inv s.
  Proof.
    intros HP Hx.
    assert (G : forall s0 tr0 s1, exec P s0 tr0 s1 -> inv s0 -> inv s1).
    { intros s0 tr0 s1 H. induction H as [|a e m tr' a' Hs Hp Hx' IH]; intros Hi0;
        [assumption | apply IH; eapply inv_step; eauto]. }
    eapply G; [exact Hx | exact inv_init].
  Qed.

  (* T1 *)
  Theorem holders_agree P tr s t1 t2 v1 v2 :
    (forall x e, P x e -> disciplined x e) -> exec P init tr s ->
    hold s t1 = Some v1 -> hold s t2 = Some v2 -> v1 = v2.
  Proof.
    intros HP Hx H1 H2. pose proof (reachable_inv P tr s HP Hx) as Hi.
    destruct (Hi _ _ H1) as [E1 _]. destruct (Hi _ _ H2) as [E2 _]. congruence.
  Qed.

  (* T1' : an access is CLAIMED under v when its thread holds v.  Two accesses
     claimed under different values are never enabled in one reachable state. *)
  Definition claimed (s : state) (t : thread) (v : nat) : Prop := hold s t = Some v.

  Corollary claims_exclusive P tr s t1 t2 v1 v2 :
    (forall x e, P x e -> disciplined x e) -> exec P init tr s ->
    claimed s t1 v1 -> claimed s t2 v2 -> v1 <> v2 -> False.
  Proof. intros HP Hx H1 H2 Hne. apply Hne. eapply holders_agree; eauto. Qed.

  (* A storing value has at most one holder at a time, whatever the values. *)
  Corollary storing_value_one_holder P tr s t1 t2 v :
    (forall x e, P x e -> disciplined x e) -> exec P init tr s ->
    storing v = true -> hold s t1 = Some v -> hold s t2 = Some v -> t1 = t2.
  Proof.
    intros HP Hx Hs H1 H2. pose proof (reachable_inv P tr s HP Hx) as Hi.
    destruct (Hi _ _ H1) as [_ O1]. destruct (Hi _ _ H2) as [_ O2].
    rewrite (O1 Hs), (O2 Hs). reflexivity.
  Qed.
End handoff.

(* ================================================================
   T3 / T4 — concrete traces.  Threads are numbers: 0 the writer, 1 the
   reader, 2 a third party.  The flag starts at 0.
   ================================================================ *)
Section witnesses.
  Let T := nat.
  Let w : T := 0.
  Let r : T := 1.
  Let x : T := 2.
  Notation run := (exec T Nat.eq_dec).

  (* one-shot: only 0 is stored from, by the writer; 1 is terminal *)
  Let st1 (v : nat) : bool := Nat.eqb v 0.
  Let ow1 (_ : nat) : T := w.
  (* ping-pong: 0 is the writer's, 1 the reader's *)
  Let st2 (v : nat) : bool := Nat.leb v 1.
  Let ow2 (v : nat) : T := if Nat.eqb v 0 then w else r.

  Ltac go := repeat (first [ apply exec_nil
                           | eapply exec_cons;
                             [ cbv; reflexivity
                             | cbv; repeat split;
                               first [ reflexivity | exact I | discriminate
                                     | (intros; reflexivity) | (intros; discriminate)
                                     | (eexists; repeat split; reflexivity) ]
                             | ] ]).

  (* T3: the one-shot publication.  The writer holds the initial value without
     observing anything, touches the data, stores; the reader observes 1 and
     touches the data. *)
  Example one_shot_ok :
    exists s, run (disciplined T st1 ow1) (init T Nat.eq_dec 0 st1 ow1)
      [EAcc T w; ESto T w 1; EObs T r 1; EAcc T r] s /\
      hold T s r = Some 1 /\ hold T s w = None.
  Proof. eexists. split; [go | split; cbv; reflexivity]. Qed.

  (* T3: a ping-pong that goes round twice. *)
  Example ping_pong_ok :
    exists s, run (disciplined T st2 ow2) (init T Nat.eq_dec 0 st2 ow2)
      [EAcc T w; ESto T w 1; EObs T r 1; EAcc T r; ESto T r 0;
       EObs T w 0; EAcc T w; ESto T w 1; EObs T r 1; EAcc T r] s.
  Proof. eexists. go. Qed.

  (* T3: two readers of one TERMINAL value both hold it — allowed, and the flag
     orders nothing between them. *)
  Example two_readers_hold_together :
    exists s, run (disciplined T st1 ow1) (init T Nat.eq_dec 0 st1 ow1)
      [ESto T w 1; EObs T r 1; EObs T x 1] s /\
      hold T s r = Some 1 /\ hold T s x = Some 1.
  Proof. eexists. split; [go | split; cbv; reflexivity]. Qed.

  (* T4 (the republished flag): after its own store the writer holds NOTHING —
     an access there is unclaimed — while the reader already holds 1. *)
  Example republication_is_unheld :
    exists s, run (disciplined T st1 ow1) (init T Nat.eq_dec 0 st1 ow1)
      [EAcc T w; ESto T w 1; EObs T r 1] s /\
      hold T s w = None /\ hold T s r = Some 1.
  Proof. eexists. split; [go | split; cbv; reflexivity]. Qed.

  (* T4 (H_sto): a store by a thread that holds nothing.  The trace satisfies
     `claim`; the writer still holds 0 while the reader holds 1. *)
  Example forged_store_breaks :
    exists s, run (fun _ e => claim T st1 ow1 e) (init T Nat.eq_dec 0 st1 ow1)
      [ESto T x 1; EObs T r 1] s /\
      hold T s w = Some 0 /\ hold T s r = Some 1.
  Proof. eexists. split; [go | split; cbv; reflexivity]. Qed.

  (* T4 (H_obs): a second thread observes a STORING value.  Each store is made
     by a holder that owns the value it holds; the second claimant keeps
     holding 0 after the owner has handed 1 to the reader. *)
  Example second_claimant_breaks :
    exists s, run (fun s e => handoff T st1 ow1 s e) (init T Nat.eq_dec 0 st1 ow1)
      [EObs T x 0; ESto T w 1; EObs T r 1] s /\
      hold T s x = Some 0 /\ hold T s r = Some 1.
  Proof. eexists. split; [go | split; cbv; reflexivity]. Qed.

  (* T4 (the wrong polarity): a "wait" that is satisfied by the INITIAL value.
     If 0 is nobody's to store from, every thread holds it from the start — the
     observation is real, and worth nothing: both hold the SAME value, and
     claims_exclusive says nothing about them. *)
  Example wrong_polarity_same_value :
    exists s, run (disciplined T (fun _ => false) ow1)
                  (init T Nat.eq_dec 0 (fun _ => false) ow1)
      [EObs T r 0; EAcc T r; EAcc T w] s /\
      hold T s r = Some 0 /\ hold T s w = Some 0.
  Proof. eexists. split; [go | split; cbv; reflexivity]. Qed.
End witnesses.

(* ================================================================
   What each decision function MEANS (the functions themselves are in
   concurrency_decisions.v; the C is proved equal to them by VST).
   ================================================================ *)
Local Open Scope Z_scope.

(* EObs: the branch tells the thread the literal, or — for a flag with exactly
   two values — the other one. *)
Lemma flag_obs_meaning a tv :
  (zer_flag_obs_coq a tv = 1 <-> a <> 0) /\
  (zer_flag_obs_coq a tv = 2 <-> a = 0 /\ tv <> 0) /\
  (zer_flag_obs_coq a tv = 0 <-> a = 0 /\ tv = 0).
Proof.
  unfold zer_flag_obs_coq.
  repeat destruct (Z.eq_dec _ _); repeat split; intros; try lia.
Qed.

(* H_sto: a store is made under an observed value, or under the initial one by
   the thread's first, unrepeated store — never otherwise
   (forged_store_breaks and republication_is_unheld are why). *)
Lemma flag_store_from_meaning h e l j b :
  (zer_flag_store_from_coq h e l j b = 1 <-> h <> 0) /\
  (zer_flag_store_from_coq h e l j b = 2 <-> h = 0 /\ e <> 0 /\ l = 0 /\ j = 0 /\ b = 0).
Proof.
  unfold zer_flag_store_from_coq.
  repeat destruct (Z.eq_dec _ _); repeat split; intros; try lia.
Qed.

Lemma store_in_loop_holds_nothing e j b :
  zer_flag_store_from_coq 0 e 1 j b = 0.
Proof. unfold zer_flag_store_from_coq. repeat destruct (Z.eq_dec _ _); lia. Qed.

Lemma second_store_holds_nothing e l j :
  zer_flag_store_from_coq 0 e l j 1 = 0.
Proof. unfold zer_flag_store_from_coq. repeat destruct (Z.eq_dec _ _); lia. Qed.

(* H_obs: one claiming thread, running once (second_claimant_breaks is why). *)
Lemma flag_claim_unique_meaning st rp :
  zer_flag_claim_unique_coq st rp = 1 <-> st <> 0 /\ rp = 0.
Proof.
  unfold zer_flag_claim_unique_coq.
  repeat destruct (Z.eq_dec _ _); split; intros H; try lia.
Qed.

(* T1': ordered only under two different held values of a disciplined flag
   (wrong_polarity_same_value is why "different" is not optional). *)
Lemma flag_orders_meaning fv vd :
  zer_flag_orders_coq fv vd = 1 <-> fv <> 0 /\ vd <> 0.
Proof.
  unfold zer_flag_orders_coq.
  repeat destruct (Z.eq_dec _ _); split; intros H; try lia.
Qed.

Lemma same_value_never_ordered fv :
  zer_flag_orders_coq fv 0 = 0.
Proof. unfold zer_flag_orders_coq. repeat destruct (Z.eq_dec _ _); lia. Qed.
