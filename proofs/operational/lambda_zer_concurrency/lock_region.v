(* ================================================================
   lambda_zer_concurrency / lock_region : the SPIN-LOCK REGION.

   WHY THIS FILE.  The checker treats the statements between

       while (!@atomic_cas(&L, 0, 1)) { }      ...      @atomic_store(&L, 0);

   as running "with L held", and three rules rest on that: two accessors of a
   plain global are not a race if both hold a common lock; a thread's
   read-modify-write of a volatile flag is exact under a lock every other
   store holds (BUG-1652); so is the spawner's (BUG-1656).  The last two were
   RELAXATIONS, shipped with tests and no model.  Writing the model's
   hypotheses down found accepted programs that lose updates on every run
   (BUG-1663; 29 cells of the grid) — none by a failing proof, because there
   was no proof to fail.  The missing variables:

     - WHO may write the lock word.  "Held" was read off one statement list,
       but a release nested in the region, in a callee, through a pointer, or
       by another thread opens a lock its holder still believes it holds.
     - WHETHER the read and the write-back of an update are in the SAME
       critical section.  The lock was looked up at the write-back alone.
     - WHICH value means "free".  Two acquires testing different values both
       "acquired L": one wins exactly while the other holds it.
     - WHETHER it is ONE word.  A threadlocal word, a `static` local shadowing
       the global, two fields of one struct: each thread wins its own.
     - WHEN a statement runs.  "Between the acquire and the release" is
       lexical; a `defer` body runs at the block's exit and the spin body runs
       before the acquire has won.

   THE MODEL.  Threads take turns executing events on one lock word (a
   number) and one guarded cell `g`:
       EAcq t e d   the spin test `cas(e -> d)` wins (enabled only while the
                    word holds e)
       ERel t r     a store of r into the word (ALWAYS enabled — that is the
                    point)
       ERead t      t copies g into a local
       EWrite t     t stores into g
   `inside t` is ghost state: t is between an acquire and a release of its own.

   WHAT THE COMPILER MUST ESTABLISH, as hypotheses on every execution:
     agree      every acquire tests the SAME value `free` and writes
                another                                               (H_acq)
     in_role    every release is made by a thread that is inside       (H_rel)
     guarded    every read / write of g is made by a thread inside     (H_acc)
     continuity between the read and the write-back of an update, its
                thread makes no release                               (H_cont)
     one word   the model has ONE word: every event of every thread is on
                it                                                   (H_word)

   THE THEOREMS (zero admits, plain Coq, self-contained):
     T1  mutual_exclusion     agree + in_role  ==>  at most one thread is
                              inside, in every reachable state.  (What a
                              release stores does not matter: a value other
                              than `free` leaves the word taken for ever — a
                              deadlock, not a race.)
     T1' guarded_exclusive    ... so two guarded accesses by different threads
                              are never enabled together (no data race on g).
     T2  rmw_exact            agree + in_role + guarded + continuity  ==>  no
                              other thread writes g between the read and the
                              write-back (no lost update).
     T3  precision witness    two threads each doing a locked update, one after
                              the other, satisfies every hypothesis.
     T4  necessity, one trace per hypothesis dropped:
         mixed_free_breaks        without agree, two threads are inside;
         private_words_exclude_nobody   without one word, likewise;
         rogue_release_breaks     without in_role, two threads are inside;
         split_sections_lose      without continuity, a foreign write lands
                                  between the read and the write-back — and
                                  that trace satisfies in_role AND guarded,
                                  which is exactly what the compiler checked;
         unguarded_store_loses    without guarded, likewise;
         defer_body_before_release_unguarded, spinning_thread_is_outside
                                  the two statements that sit lexically in a
                                  region and do not run in it — each is an
                                  access by a thread that is not inside.

   WHAT THE COMPILER DISCHARGES (checker.c), each by one syntactic check:
     agree       the census records the free value of the first acquire of a
                 word; an acquire testing another breaks the word
                 (lock_acquire_note).  E != D is the recogniser's.
     one word    sync_lock_word: a whole global, not threadlocal, not a name
                 the function binds locally.
     in_role     the census (census_walk) pairs every spin acquire with the one
                 release that closes its region and records every other atomic
                 write of a lock word, every `&L`, every label inside a region;
                 such a word is a lock nowhere (lock_word_broken).  A plain
                 store of the word in a concurrent context is the atomic-cell
                 rule's error.
     guarded     the two-accessor judgement / check_volatile_locked_rmw /
                 vol_win_rmw_locked_ok ("every other store holds that lock"),
                 over a lock context (sync_walk) that leaves out the spin body
                 and a `defer` body registered before the release was deferred.
     continuity  a fact is `cont` only for a one-statement update, or one whose
                 local copies were each read by the copy's own declaration,
                 with no statement that could begin or end a critical section
                 between that declaration and the write-back (rmw_value_fresh,
                 Checker.lock_region), the name being bound once in the function.

   HONEST LIMITS.  (1) This certifies the RULE.  That the syntactic checks
   establish the hypotheses on every execution — that a lexical region with no
   label, no nested release and no out-of-role write IS a dynamic critical
   section for every statement the walk judges inside it, and that a local
   declared inside it cannot carry a value from an earlier one — is argued at
   those checks and gated by the "lock-region" grid of
   tests/test_conc_matrix.c, not derived here.  The list of statements that
   sit in a region without running in it (a `defer` body, the spin body) was
   found by probing; the model has no syntax, so it cannot show that list
   complete.  (2) Sequentially consistent interleaving: every @atomic_* is
   SEQ_CST, and the model has no weaker memory order.  (3) Liveness (a lock
   never released, a release that stores a value other than the free one, an
   acquire that spins for ever) is out of scope — a floor.  (4) The flag
   HAND-OFF idiom the checker also recognises (`data = v; @atomic_store(&F,
   1);` against a wait on F) is NOT modelled, and has open accept-unsafe
   shapes: docs/limitations.md, section B.
   ================================================================ *)
From Coq Require Import List Bool Arith ZArith Lia.
Import ListNotations.
From zer_conc Require Import concurrency_decisions.

Section lock.
  Variable thread : Type.
  Hypothesis thread_eq_dec : forall a b : thread, {a = b} + {a <> b}.
  (* The value every acquire must test (H_acq). *)
  Variable free : nat.

  Inductive event :=
  | EAcq (t : thread) (e d : nat)
  | ERel (t : thread) (r : nat)
  | ERead (t : thread)
  | EWrite (t : thread).

  Record state := St { word : nat; inside : thread -> bool }.

  (* Whatever the word holds at the start, nobody is inside. *)
  Definition init (w0 : nat) : state := St w0 (fun _ => false).

  Definition upd (f : thread -> bool) (t : thread) (b : bool) : thread -> bool :=
    fun k => if thread_eq_dec k t then b else f k.

  (* An acquire `cas(e -> d)` is enabled only while the word holds e.  A release
     is a store: it is enabled for ANY thread at ANY time. *)
  Definition step (s : state) (ev : event) : option state :=
    match ev with
    | EAcq t e d => if Nat.eq_dec (word s) e then Some (St d (upd (inside s) t true)) else None
    | ERel t r => Some (St r (upd (inside s) t false))
    | ERead _ => Some s
    | EWrite _ => Some s
    end.

  (* ---- the hypotheses, per event ---- *)
  Definition agree (ev : event) : Prop :=
    match ev with EAcq _ e d => e = free /\ d <> free | _ => True end.

  Definition in_role (s : state) (ev : event) : Prop :=
    match ev with ERel t _ => inside s t = true | _ => True end.

  Definition lock_ok (s : state) (ev : event) : Prop := agree ev /\ in_role s ev.

  Definition guarded (s : state) (ev : event) : Prop :=
    match ev with
    | ERead t => inside s t = true
    | EWrite t => inside s t = true
    | _ => True
    end.

  (* An execution: every event enabled and satisfying P. *)
  Inductive exec (P : state -> event -> Prop) : state -> list event -> state -> Prop :=
  | exec_nil s : exec P s [] s
  | exec_cons s e s' tr s'' :
      step s e = Some s' -> P s e -> exec P s' tr s'' -> exec P s (e :: tr) s''.

  Lemma exec_app P s tr1 tr2 s'' :
    exec P s (tr1 ++ tr2) s'' <-> exists s', exec P s tr1 s' /\ exec P s' tr2 s''.
  Proof.
    revert s. induction tr1 as [|e tr1 IH]; intros s; simpl.
    - split.
      + intros H. exists s. split; [constructor | exact H].
      + intros [s' [H1 H2]]. inversion H1; subst. exact H2.
    - split.
      + intros H. inversion H; subst.
        apply IH in H6. destruct H6 as [m [Ha Hb]].
        exists m. split; [econstructor; eauto | exact Hb].
      + intros [m [H1 H2]]. inversion H1; subst.
        econstructor; eauto. apply IH. exists m. split; assumption.
  Qed.

  (* ---- the invariant ---- *)
  Definition inv (s : state) : Prop :=
    (word s = free -> forall t, inside s t = false) /\
    (forall t t', inside s t = true -> inside s t' = true -> t = t').

  Lemma inv_init w0 : inv (init w0).
  Proof. split; simpl; intros; try reflexivity; discriminate. Qed.

  Lemma upd_same f t b : upd f t b t = b.
  Proof. unfold upd. destruct (thread_eq_dec t t); congruence. Qed.

  Lemma upd_other f t b k : k <> t -> upd f t b k = f k.
  Proof. intros H. unfold upd. destruct (thread_eq_dec k t); congruence. Qed.

  Lemma inv_step s ev s' :
    inv s -> step s ev = Some s' -> lock_ok s ev -> inv s'.
  Proof.
    intros [Hfree Huniq] Hstep [Hagree Hrole]. destruct ev as [t e d|t r|t|t]; simpl in *.
    - (* acquire: it tested `free`, so nobody was inside; it wrote another value *)
      destruct Hagree as [He Hd]. subst e.
      destruct (Nat.eq_dec (word s) free) as [Hw|Hw]; [|discriminate].
      inversion Hstep; subst; clear Hstep.
      split; simpl; [intros Hbad; exfalso; apply Hd; exact Hbad|].
      intros a b Ha Hb.
      destruct (thread_eq_dec a t) as [->|Na]; destruct (thread_eq_dec b t) as [->|Nb];
        try reflexivity.
      + rewrite upd_other in Hb by assumption. rewrite Hfree in Hb by assumption. discriminate.
      + rewrite upd_other in Ha by assumption. rewrite Hfree in Ha by assumption. discriminate.
      + rewrite upd_other in Ha by assumption. rewrite Hfree in Ha by assumption. discriminate.
    - (* release, by a thread that is inside: afterwards nobody is, whatever
         value it stored *)
      inversion Hstep; subst; clear Hstep.
      assert (Hnone : forall k, upd (inside s) t false k = false).
      { intros k. destruct (thread_eq_dec k t) as [->|Nk].
        - apply upd_same.
        - rewrite upd_other by assumption.
          destruct (inside s k) eqn:Hk; [|reflexivity].
          exfalso. apply Nk. apply Huniq; assumption. }
      split; simpl.
      + intros _ k. apply Hnone.
      + intros a b Ha. rewrite Hnone in Ha. discriminate.
    - inversion Hstep; subst. split; assumption.
    - inversion Hstep; subst. split; assumption.
  Qed.

  (* T1 *)
  Theorem mutual_exclusion P s tr s' :
    (forall x e, P x e -> lock_ok x e) ->
    inv s -> exec P s tr s' -> inv s'.
  Proof.
    intros HP Hi Hx. induction Hx as [|s e m tr s'' Hs Hp Hx IH]; [assumption|].
    apply IH. eapply inv_step; eauto.
  Qed.

  Corollary one_inside P w0 tr s :
    (forall x e, P x e -> lock_ok x e) ->
    exec P (init w0) tr s ->
    forall t t', inside s t = true -> inside s t' = true -> t = t'.
  Proof.
    intros HP Hx. destruct (mutual_exclusion P (init w0) tr s HP (inv_init w0) Hx) as [_ Hu].
    exact Hu.
  Qed.

  (* T1' : two guarded accesses by different threads are never enabled in the
     same reachable state — no data race on g. *)
  Corollary guarded_exclusive P w0 tr s t t' :
    (forall x e, P x e -> lock_ok x e) ->
    exec P (init w0) tr s ->
    guarded s (EWrite t) -> guarded s (ERead t') -> t = t'.
  Proof. intros HP Hx Ha Hb. eapply one_inside; eauto. Qed.

  (* ---- T2 : an update inside one critical section is exact ---- *)
  Definition disciplined (s : state) (ev : event) : Prop := lock_ok s ev /\ guarded s ev.

  Definition foreign_write (t : thread) (ev : event) : Prop :=
    match ev with EWrite t' => t' <> t | _ => False end.

  Definition releases (t : thread) (ev : event) : Prop :=
    match ev with ERel t' _ => t' = t | _ => False end.

  (* While t stays inside and makes no release, the events that can run are
     t's own reads and writes: nobody else can acquire, release or touch g. *)
  Lemma held_section t s mid s' :
    inv s -> inside s t = true ->
    exec disciplined s mid s' ->
    Forall (fun e => ~ releases t e) mid ->
    Forall (fun e => ~ foreign_write t e) mid /\ inside s' t = true.
  Proof.
    intros Hi Hin Hx. revert Hi Hin.
    induction Hx as [|s ev m tr s'' Hs [Hok Hg] Hx IH]; intros Hi Hin Hno.
    - split; [constructor | assumption].
    - inversion Hno as [|? ? Hne Hrest]; subst.
      assert (Hi' : inv m) by (eapply inv_step; eauto).
      destruct Hi as [Hfree Huniq]. destruct Hok as [Hagree Hrole].
      assert (Hin' : inside m t = true /\ ~ foreign_write t ev).
      { destruct ev as [k e d|k r|k|k]; simpl in *.
        - (* an acquire tests `free`; the word is taken, so it is not enabled *)
          destruct Hagree as [He _]. subst e.
          destruct (Nat.eq_dec (word s) free) as [Hw|Hw]; [|discriminate].
          rewrite Hfree in Hin by assumption. discriminate.
        - (* a release: only by a thread inside, i.e. by t — excluded *)
          exfalso. apply Hne. apply Huniq; assumption.
        - inversion Hs; subst. split; [assumption | tauto].
        - inversion Hs; subst. split; [assumption|].
          intros Hf. apply Hf. apply Huniq; assumption. }
      destruct Hin' as [Hin' Hnf].
      destruct (IH Hi' Hin' Hrest) as [Hall Hend].
      split; [constructor; assumption | assumption].
  Qed.

  Theorem rmw_exact t w0 pre mid post s :
    exec disciplined (init w0) (pre ++ ERead t :: mid ++ EWrite t :: post) s ->
    Forall (fun e => ~ releases t e) mid ->
    Forall (fun e => ~ foreign_write t e) mid.
  Proof.
    intros Hx Hno.
    apply exec_app in Hx. destruct Hx as [s1 [Hpre Hrest]].
    inversion Hrest as [|? ? s2 ? ? Hs [Hok Hg] Hx2]; subst.
    simpl in Hs. inversion Hs; subst.
    apply exec_app in Hx2. destruct Hx2 as [s3 [Hmid _]].
    assert (Hi : inv s2).
    { eapply (mutual_exclusion disciplined);
        [intros x e [H _]; exact H | apply (inv_init w0) | exact Hpre]. }
    simpl in Hg.
    destruct (held_section t s2 mid s3 Hi Hg Hmid Hno) as [Hall _]. exact Hall.
  Qed.

  (* A block's exit runs the `defer`s it registered last-in first-out, after its
     direct statements. *)
  Definition block_run (direct defers : list event) : list event := direct ++ rev defers.
End lock.

(* ================================================================
   T3 / T4 — concrete traces, two threads, free value 0.
   ================================================================ *)
Section witnesses.
  Let T := bool.
  Let a : T := true.
  Let b : T := false.
  Notation run := (exec T Bool.bool_dec).
  Notation dis := (disciplined T 0).
  Notation lok := (lock_ok T 0).
  Notation rol := (fun s e => in_role T s e).
  Notation start := (init T 0).

  Ltac go := repeat (first [ apply exec_nil
                           | eapply exec_cons;
                             [ cbv; reflexivity
                             | cbv; repeat split; first [ reflexivity | exact I | discriminate ]
                             | ] ]).

  (* T3: two locked updates, one after the other — every hypothesis holds. *)
  Example two_locked_updates_ok :
    exists s, run dis start
      [EAcq T a 0 1; ERead T a; EWrite T a; ERel T a 0;
       EAcq T b 0 1; ERead T b; EWrite T b; ERel T b 0] s.
  Proof. eexists. go. Qed.

  (* T3: a lock whose free value is 1 is the same theorem at `free := 1`. *)
  Example inverted_lock_ok :
    exists s, run (disciplined T 1) (init T 1)
      [EAcq T a 1 0; EWrite T a; ERel T a 1; EAcq T b 1 0; EWrite T b; ERel T b 1] s.
  Proof. eexists. go. Qed.

  (* T4 (H_acq): two acquires that disagree on the free value — `cas(0 -> 1)` and
     `cas(1 -> 2)`.  No release at all, so the trace is in role; the second
     acquire is enabled exactly BECAUSE the first thread holds the word. *)
  Example mixed_free_breaks :
    exists s, run rol start [EAcq T a 0 1; EAcq T b 1 2] s /\
              inside T s a = true /\ inside T s b = true.
  Proof. eexists. split; [go | split; cbv; reflexivity]. Qed.

  (* T4 (H_word): the model has ONE word.  Two threads on two words — a
     threadlocal word, a `static` local shadowing the global, two fields of one
     struct — are two instances of it: each acquire is enabled in its own word's
     initial state whatever the other thread did to the other word, and nothing
     relates the two `inside`. *)
  Example private_words_exclude_nobody :
    exists sa sb, run lok start [EAcq T a 0 1] sa /\ run lok start [EAcq T b 0 1] sb /\
                  inside T sa a = true /\ inside T sb b = true.
  Proof. eexists. eexists. split; [go | split; [go | split; cbv; reflexivity]]. Qed.

  (* T4 (H_rel): a release by a thread that is NOT inside is always enabled — and
     then two threads are inside at once. *)
  Example rogue_release_breaks :
    exists s, run (fun _ e => agree T 0%nat e) start [EAcq T a 0 1; ERel T b 0; EAcq T b 0 1] s /\
              inside T s a = true /\ inside T s b = true.
  Proof. eexists. split; [go | split; cbv; reflexivity]. Qed.

  (* T4 (H_cont): the read in one critical section, the write-back in the next.
     The trace agrees, is in role AND guarded — everything the compiler used to
     check — and b's write lands between a's read and a's write-back. *)
  Example split_sections_lose :
    exists s, run dis start
      ([EAcq T a 0 1] ++ ERead T a ::
         [ERel T a 0; EAcq T b 0 1; EWrite T b; ERel T b 0; EAcq T a 0 1]
         ++ EWrite T a :: [ERel T a 0]) s /\
      Exists (foreign_write T a)
        [ERel T a 0; EAcq T b 0 1; EWrite T b; ERel T b 0; EAcq T a 0 1].
  Proof.
    eexists. split; [simpl; go | ].
    apply Exists_cons_tl. apply Exists_cons_tl. apply Exists_cons_hd. cbv. discriminate.
  Qed.

  (* T4 (H_acc): a store of g by a thread holding nothing. *)
  Example unguarded_store_loses :
    exists s, run lok start
      ([EAcq T a 0 1] ++ ERead T a :: [EWrite T b] ++ EWrite T a :: [ERel T a 0]) s /\
      Exists (foreign_write T a) [EWrite T b].
  Proof.
    eexists. split; [simpl; go | ].
    apply Exists_cons_hd. cbv. discriminate.
  Qed.

  (* ---- WHEN a statement runs: the two that sit in a region and do not run
     in it. ---- *)

  (* `LOCK; defer UNLOCK; defer { g = ...; }` — the body was registered AFTER
     the release, so it runs BEFORE it: guarded. *)
  Example defer_body_after_deferred_release_ok :
    exists s, run dis start (block_run T [EAcq T a 0 1] [ERel T a 0; EWrite T a]) s.
  Proof. eexists. cbv [block_run rev app]. go. Qed.

  (* `LOCK; defer { g = ...; } defer UNLOCK;` — and equally
     `LOCK; defer { g = ...; } UNLOCK;` — run the same trace: acquire, RELEASE,
     write.  The write is made by a thread that is no longer inside. *)
  Example defer_body_before_release_unguarded :
    block_run T [EAcq T a 0 1] [EWrite T a; ERel T a 0] = [EAcq T a 0 1; ERel T a 0; EWrite T a] /\
    block_run T [EAcq T a 0 1; ERel T a 0] [EWrite T a] = [EAcq T a 0 1; ERel T a 0; EWrite T a] /\
    exists s, run lok start [EAcq T a 0 1; ERel T a 0] s /\ ~ guarded T s (EWrite T a).
  Proof.
    split; [reflexivity | split; [reflexivity | ]].
    eexists. split; [go | cbv; discriminate].
  Qed.

  (* The spin body: b's test has just FAILED (the word is taken), so b is not
     inside — an access there is an unguarded one. *)
  Example spinning_thread_is_outside :
    exists s, run lok start [EAcq T a 0 1] s /\
              step T Bool.bool_dec s (EAcq T b 0 1) = None /\ ~ guarded T s (EWrite T b).
  Proof. eexists. split; [go | split; [cbv; reflexivity | cbv; discriminate]]. Qed.
End witnesses.

(* ================================================================
   What each decision function MEANS (the functions themselves are in
   concurrency_decisions.v; the C is proved equal to them by VST).
   ================================================================ *)
Open Scope Z_scope.

(* H_rel, the release half: in role iff it closes its own region, once. *)
Lemma lock_release_in_role_meaning c r :
  zer_lock_release_in_role_coq c r = 1 <-> c <> 0 /\ r = 0.
Proof.
  unfold zer_lock_release_in_role_coq.
  repeat destruct (Z.eq_dec _ _); split; intros H; try lia.
Qed.

(* H_rel, every atomic write of the word: the sanctioned one, on a whole word. *)
Lemma lock_write_in_role_meaning s w :
  zer_lock_write_in_role_coq s w = 1 <-> s <> 0 /\ w <> 0.
Proof.
  unfold zer_lock_write_in_role_coq.
  repeat destruct (Z.eq_dec _ _); split; intros H; try lia.
Qed.

(* A second release of a region already released by a `defer` is never in role. *)
Lemma second_release_out_of_role c r :
  r <> 0 -> zer_lock_release_in_role_coq c r = 0.
Proof.
  intros H. unfold zer_lock_release_in_role_coq. repeat destruct (Z.eq_dec _ _); lia.
Qed.

(* H_cont, the copy: fresh iff read by its own (non-static) declaration from
   sources that are themselves in the section. *)
Lemma rmw_copy_fresh_meaning d st sf :
  zer_rmw_copy_fresh_coq d st sf = 1 <-> d <> 0 /\ st = 0 /\ sf <> 0.
Proof.
  unfold zer_rmw_copy_fresh_coq.
  repeat destruct (Z.eq_dec _ _); split; intros H; try lia.
Qed.

Lemma rmw_copy_in_section_meaning f r :
  zer_rmw_copy_in_section_coq f r = 1 <-> f <> 0 /\ r <> 0.
Proof.
  unfold zer_rmw_copy_in_section_coq.
  repeat destruct (Z.eq_dec _ _); split; intros H; try lia.
Qed.

(* H_cont, the update: one statement, or only copies taken in this section. *)
Lemma rmw_one_section_meaning o cp :
  zer_rmw_one_section_coq o cp = 1 <-> o <> 0 \/ cp <> 0.
Proof.
  unfold zer_rmw_one_section_coq.
  repeat destruct (Z.eq_dec _ _); split; intros H; try lia.
Qed.

Lemma rmw_lock_covers_meaning l o :
  zer_rmw_lock_covers_coq l o = 1 <-> l <> 0 /\ o <> 0.
Proof.
  unfold zer_rmw_lock_covers_coq.
  repeat destruct (Z.eq_dec _ _); split; intros H; try lia.
Qed.

(* The hole of BUG-1663 as a statement about the decision: a held lock NEVER
   covers an update that is not inside one section (split_sections_lose is
   why). *)
Lemma split_update_never_covered l :
  zer_rmw_lock_covers_coq l 0 = 0.
Proof. unfold zer_rmw_lock_covers_coq. repeat destruct (Z.eq_dec _ _); lia. Qed.

(* H_acc for the flag: exempt iff seen under exactly one lock and no other
   thread stores the flag outside it (unguarded_store_loses is why). *)
Lemma rmw_locked_exempt_meaning sn l m o :
  zer_rmw_locked_exempt_coq sn l m o = 1 <-> sn <> 0 /\ l <> 0 /\ m = 0 /\ o = 0.
Proof.
  unfold zer_rmw_locked_exempt_coq.
  repeat destruct (Z.eq_dec _ _); split; intros H; try lia.
Qed.

Lemma store_outside_never_exempt sn l m o :
  o <> 0 -> zer_rmw_locked_exempt_coq sn l m o = 0.
Proof.
  intros H. unfold zer_rmw_locked_exempt_coq. repeat destruct (Z.eq_dec _ _); lia.
Qed.

(* H_word: a cell is a lock word iff it is a whole global, shared by every
   thread, that the function does not bind locally
   (private_words_exclude_nobody is why). *)
Lemma lock_word_ok_meaning w tl sh :
  zer_lock_word_ok_coq w tl sh = 1 <-> w <> 0 /\ tl = 0 /\ sh = 0.
Proof.
  unfold zer_lock_word_ok_coq.
  repeat destruct (Z.eq_dec _ _); split; intros H; try lia.
Qed.

(* H_acq: the first acquire fixes the free value; a later one must agree
   (mixed_free_breaks is why). *)
Lemma lock_acquire_agrees_meaning f sm :
  zer_lock_acquire_agrees_coq f sm = 1 <-> f <> 0 \/ sm <> 0.
Proof.
  unfold zer_lock_acquire_agrees_coq.
  repeat destruct (Z.eq_dec _ _); split; intros H; try lia.
Qed.

Lemma later_acquire_must_agree :
  zer_lock_acquire_agrees_coq 0 0 = 0.
Proof. reflexivity. Qed.

(* H_acc, the `defer` body: covered iff it is not a defer of the block that
   opened the region, or the release was deferred before it
   (defer_body_before_release_unguarded is why). *)
Lemma lock_covers_defer_meaning o r :
  zer_lock_covers_defer_coq o r = 1 <-> o = 0 \/ r <> 0.
Proof.
  unfold zer_lock_covers_defer_coq.
  repeat destruct (Z.eq_dec _ _); split; intros H; try lia.
Qed.

Lemma defer_before_release_never_covered o :
  o <> 0 -> zer_lock_covers_defer_coq o 0 = 0.
Proof.
  intros H. unfold zer_lock_covers_defer_coq. repeat destruct (Z.eq_dec _ _); lia.
Qed.
