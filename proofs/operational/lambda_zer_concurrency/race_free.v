(* ================================================================
   lambda_zer_concurrency / race_free : ONE machine, ONE theorem.

   WHY THIS FILE.  The data-race class had four oracles, each a model of
   ONE ordering mechanism with its own state: window_cfg.v (spawn / join),
   lock_region.v (a spin lock), flag_handoff.v (a flag as a token),
   access_alias.v (what an access touches).  Nothing said that the four,
   together with the rules that had no model at all — a shared struct's
   mutex, the atomic-cell rule, "a thread that is not running" — are
   ENOUGH: that a program in which every conflicting pair is ordered by
   one of them has no data race.  Each oracle was sound over the states it
   listed; whether the list covers the semantics was the open question
   (CLAUDE.md, "Current gap").

   THE MODEL.  A program is a fixed set of threads, each a control-flow
   graph of instructions:
       IAcc l w a      a read / write of location l; `a`: it is an @atomic
       IAcq L / IRel L take / drop a lock — a shared struct's mutex, or the
                       successful CAS and the releasing store of a spin lock
       ISet F v        store v to a hand-off flag
       ISee F v        an observation of the flag: passes only when F = v
                       (the exit of a wait loop, the taken branch of an `if`)
       ISpawn u / IJoin u
       INop            anything else
   A configuration is where each thread is (or that it has not started),
   who owns each lock, and each flag's value.  Any enabled thread may step:
   the interleaving semantics.  A DATA RACE is a reachable configuration in
   which two different threads are both AT instructions that access
   overlapping memory, at least one writes, and they are not both atomic —
   two enabled conflicting accesses, the usual definition.

   THE JUDGEMENT.  The compiler's analysis is an ANNOTATION of each
   program point:
       held t n L      thread t holds lock L at n
       tok t n F v     thread t holds value v of flag F at n
       before t n u    u has not been started when t is at n
       after t n u     u has finished when t is at n
   that satisfies LOCAL flow constraints (one per control-flow edge: a
   fact at n' comes from the fact at n, or from the instruction at n), and
   the pairwise rule: every two conflicting instructions of two threads
   are ORDERED — a common lock, two different values of one flag, or one
   of the two threads is not running.  Nothing else.

   THE THEOREM (zero admits, plain Coq, no axioms):
     T1  race_free          a program with such an annotation has no data
                            race in any reachable configuration.
     and what the annotations mean on the way:
         held_is_owned      a lock annotated held IS owned by that thread
                            (so two threads never hold one lock);
         tok_is_current     a held value IS the flag's current value;
         before_not_started / after_finished.
     T3  accepted_example   a three-thread program using a lock, a
                            hand-off and a join is accepted — the
                            judgement is not vacuous.
     T4  necessity, one reachable race per hypothesis dropped:
         rogue_release_races     a release by a thread that does not hold
         forged_store_races      a flag store by a thread holding nothing
         second_claimant_races   two threads granted one stored-from value
         unjudged_pair_races     one pair left unordered
         stranger_spawn_races    a thread started by other than its parent
       Each is stated as `acc k`: EVERY hypothesis but number k holds of
       the program, and a race is reachable.  (final_nop — a finished
       thread's node accesses nothing — is a convention of the race
       definition, not a rule.)

   WHAT THIS IS, AGAINST THE OTHER FOUR.  It is the composition layer.  Its
   IAcq is the mutual-exclusion primitive that lock_region.v derives from
   raw atomic words; its tok is flag_handoff.v's hold; before / after are
   window_cfg.v's window; `overlap` is the relation access_alias.v proves
   the compiler's may-alias contains (`judged_alias` below takes the
   judgement over ANY relation containing it).

   WHAT THE COMPILER MUST ESTABLISH (checker.c), and where it is argued:
     the flow constraints   its contexts (SyncCtx.lock, own_f / own_v, pub,
                            the window phase) are a solution: the lexical
                            walk enters and leaves them at the statements
                            that are IAcq / IRel / ISee / ISet / ISpawn /
                            IJoin.  Gated by the conc-matrix grids; the
                            raw-word halves are lock_region.v and
                            flag_handoff.v.
     the pairwise rule      sync_sets_conflict / sync_ordered, whose final
                            decision is zer_pair_ordered (VST-verified
                            against `pair_ordered_meaning` below).
     COVERAGE               that every construct of the source that reads
                            or writes memory IS an IAcc node of the graph
                            the walk judges.  This is not a hypothesis one
                            can read off below — it is the definition of
                            the translation — and it is where BUG-1667
                            (container methods) and BUG-1668 (static
                            locals) were.  Gated by the "construct x
                            two threads" grid.

   WHAT READING THE HYPOTHESES AGAINST THE WALK FOUND (BUG-1671..1675, the
   day this file was written — a dozen accepted shapes, each TSan-confirmed):
     `after t n u`   u has FINISHED.  The walk also took a join of u to end
                     every thread u had started (a detached grandchild), and
                     never compared a thread with the window of its
                     grandparent at all (BUG-1671).
     IAcq / IRel     a @once block is one: being its single executor.  The
                     walk skipped the body instead, so its writes met
                     nobody (BUG-1672).
     COVERAGE        a poll of an async task rewrites the task (BUG-1673);
                     an asm output operand is a write (BUG-1674).
     `conflict`      excuses a pair only when BOTH accesses are atomic.  The
                     walk recorded neither a plain access of an atomic cell
                     nor the cell's atomic accesses (BUG-1675).

   HONEST LIMITS.  (1) This is the model-level theorem.  That checker.c
   computes an annotation satisfying the constraints for every program is
   NOT proven here or anywhere.  (2) Threads are a fixed set, each started
   at most once; a spawn site that can run twice must be two threads (the
   compiler's "instance").  (3) Locations do not come and go: allocation,
   publication and frame death are access_alias.v's fresh-object model and
   the escape analysis.  (4) Interrupts are not threads: a handler preempts
   and is not preempted, on one core — the ISR rules have no model here.
   (5) Deadlock is not a race: a program that blocks for ever is safe.
   (6) cinclude code is outside.
   ================================================================ *)
From Coq Require Import List Bool Arith Lia ZArith.
Import ListNotations.
From zer_conc Require Import concurrency_decisions.
Local Open Scope nat_scope.

Section machine.
  Variables thread node loc lock flag : Type.
  Hypothesis thread_dec : forall a b : thread, {a = b} + {a <> b}.
  Hypothesis lock_dec : forall a b : lock, {a = b} + {a <> b}.
  Hypothesis flag_dec : forall a b : flag, {a = b} + {a <> b}.

  Inductive instr :=
  | IAcc (l : loc) (w : bool) (a : bool)
  | IAcq (L : lock)
  | IRel (L : lock)
  | ISet (F : flag) (v : nat)
  | ISee (F : flag) (v : nat)
  | ISpawn (u : thread)
  | IJoin (u : thread)
  | INop.

  (* ---- the program ---- *)
  Variable code : thread -> node -> instr.
  Variable succ : thread -> node -> node -> Prop.
  Variable entry : thread -> node.
  Variable final : thread -> node -> Prop.
  Variable main : thread.
  Variable parent : thread -> thread.
  Variable init : flag -> nat.
  Variable owner : flag -> nat -> thread.     (* the one thread that stores FROM a value *)
  Variable overlap : loc -> loc -> Prop.

  (* ---- configurations and steps ---- *)
  Record cfg := Cfg { pc : thread -> option node; own : lock -> option thread; fl : flag -> nat }.

  Definition cfg0 : cfg :=
    Cfg (fun t => if thread_dec t main then Some (entry main) else None) (fun _ => None) init.

  Definition set_pc (c : cfg) (t : thread) (o : option node) : cfg :=
    Cfg (fun x => if thread_dec x t then o else pc c x) (own c) (fl c).
  Definition set_own (c : cfg) (L : lock) (o : option thread) : cfg :=
    Cfg (pc c) (fun x => if lock_dec x L then o else own c x) (fl c).
  Definition set_fl (c : cfg) (F : flag) (v : nat) : cfg :=
    Cfg (pc c) (own c) (fun x => if flag_dec x F then v else fl c x).

  (* what executing an instruction does to everything but the thread's own pc *)
  Inductive eff (t : thread) : instr -> cfg -> cfg -> Prop :=
  | EAcc l w a c : eff t (IAcc l w a) c c
  | ENop c : eff t INop c c
  | EAcq L c : own c L = None -> eff t (IAcq L) c (set_own c L (Some t))
  | ERel L c : eff t (IRel L) c (set_own c L None)
  | ESet F v c : eff t (ISet F v) c (set_fl c F v)
  | ESee F v c : fl c F = v -> eff t (ISee F v) c c
  | ESpawn u c : pc c u = None -> eff t (ISpawn u) c (set_pc c u (Some (entry u)))
  | EJoin u m c : pc c u = Some m -> final u m -> eff t (IJoin u) c c.

  Inductive step : cfg -> cfg -> Prop :=
  | Step c c1 t n n' :
      pc c t = Some n -> succ t n n' -> eff t (code t n) c c1 ->
      step c (set_pc c1 t (Some n')).

  Inductive reach : cfg -> Prop :=
  | R0 : reach cfg0
  | RS c c' : reach c -> step c c' -> reach c'.

  (* ---- a data race ---- *)
  Definition conflict (R : loc -> loc -> Prop) (i j : instr) : Prop :=
    match i, j with
    | IAcc l w a, IAcc l' w' a' =>
        R l l' /\ (w = true \/ w' = true) /\ ~ (a = true /\ a' = true)
    | _, _ => False
    end.

  Definition race (c : cfg) : Prop :=
    exists t u n m, t <> u /\ pc c t = Some n /\ pc c u = Some m /\
                    conflict overlap (code t n) (code u m).

  (* ---- the annotation ---- *)
  Variable held : thread -> node -> lock -> Prop.
  Variable tok : thread -> node -> flag -> nat -> Prop.
  Variable before after : thread -> node -> thread -> Prop.

  (* well-formedness of the program *)
  Hypothesis final_stuck : forall t n n', final t n -> ~ succ t n n'.
  Hypothesis final_nop : forall t n, final t n -> code t n = INop.
  Hypothesis spawn_parent : forall t n u, code t n = ISpawn u -> parent u = t.
  Hypothesis parent_main : parent main = main.

  (* the flow constraints *)
  Hypothesis held_entry : forall t L, ~ held t (entry t) L.
  Hypothesis held_flow : forall t n n' L, succ t n n' -> held t n' L ->
      (held t n L /\ code t n <> IRel L) \/ code t n = IAcq L.
  Hypothesis rel_held : forall t n L, code t n = IRel L -> held t n L.

  Hypothesis tok_entry : forall t F v, tok t (entry t) F v -> v = init F /\ owner F v = t.
  Hypothesis tok_flow : forall t n n' F v, succ t n n' -> tok t n' F v ->
      (tok t n F v /\ forall w, code t n <> ISet F w) \/ code t n = ISee F v.
  Hypothesis set_tok : forall t n F w, code t n = ISet F w -> exists v, tok t n F v.
  Hypothesis tok_grant : forall t n F v s m w,
      tok t n F v -> code s m = ISet F w -> tok s m F v -> owner F v = t.

  Hypothesis before_parent : forall t n u, before t n u -> parent u = t /\ u <> t.
  Hypothesis before_flow : forall t n n' u, succ t n n' -> before t n' u ->
      before t n u /\ code t n <> ISpawn u.
  Hypothesis after_entry : forall t u, ~ after t (entry t) u.
  Hypothesis after_flow : forall t n n' u, succ t n n' -> after t n' u ->
      after t n u \/ code t n = IJoin u.

  (* the pairwise rule *)
  Definition ordered (t : thread) (n : node) (u : thread) (m : node) : Prop :=
    (exists L, held t n L /\ held u m L) \/
    (exists F v v', tok t n F v /\ tok u m F v' /\ v <> v') \/
    before t n u \/ after t n u \/ before u m t \/ after u m t.

  Hypothesis judged : forall t u n m,
      t <> u -> conflict overlap (code t n) (code u m) -> ordered t n u m.

  (* ---- the invariant ---- *)
  Record inv (c : cfg) : Prop := Inv {
    i_main : pc c main <> None;
    i_par : forall x, pc c x <> None -> x = main \/ pc c (parent x) <> None;
    i_held : forall t n L, pc c t = Some n -> held t n L -> own c L = Some t;
    i_tok : forall t n F v, pc c t = Some n -> tok t n F v -> fl c F = v;
    i_bef : forall t n u, pc c t = Some n -> before t n u -> pc c u = None;
    i_aft : forall t n u, pc c t = Some n -> after t n u -> exists m, pc c u = Some m /\ final u m;
    i_ini : forall F, fl c F <> init F -> pc c (owner F (init F)) <> None
  }.

  Lemma inv0 : inv cfg0.
  Proof.
    constructor; simpl.
    - destruct (thread_dec main main); congruence.
    - intros x Hx. destruct (thread_dec x main); [left; assumption | congruence].
    - intros t n L Hp Hh. destruct (thread_dec t main); [|discriminate].
      inversion Hp; subst. exfalso. eapply held_entry; eassumption.
    - intros t n F v Hp Ht. destruct (thread_dec t main); [|discriminate].
      inversion Hp; subst. apply tok_entry in Ht. destruct Ht as [-> _]. reflexivity.
    - intros t n u Hp Hb. destruct (thread_dec t main); [|discriminate].
      subst. apply before_parent in Hb. destruct Hb as [_ Hne].
      destruct (thread_dec u main); [contradiction | reflexivity].
    - intros t n u Hp Ha. destruct (thread_dec t main); [|discriminate].
      inversion Hp; subst. exfalso. eapply after_entry; eassumption.
    - intros F H. congruence.
  Qed.

  (* where a thread is after a step: it moved, it stood by, or it was just born *)
  Lemma pc_after c c1 t n n' x k :
    pc c t = Some n -> eff t (code t n) c c1 ->
    pc (set_pc c1 t (Some n')) x = Some k ->
    (x = t /\ k = n') \/
    (x <> t /\ pc c x = Some k) \/
    (x <> t /\ pc c x = None /\ code t n = ISpawn x /\ k = entry x).
  Proof.
    intros Hp He Hk. simpl in Hk. destruct (thread_dec x t) as [->|Hne].
    - left. inversion Hk. auto.
    - right. inversion He; subst; simpl in Hk; try (left; split; assumption).
      destruct (thread_dec x u) as [->|Hxu].
      + right. inversion Hk; subst. repeat split; assumption.
      + left. split; assumption.
  Qed.

  (* a started thread stays started *)
  Lemma started_mono c c1 t n n' x :
    pc c t = Some n -> eff t (code t n) c c1 -> pc c x <> None ->
    pc (set_pc c1 t (Some n')) x <> None.
  Proof.
    intros Hp He Hx. simpl. destruct (thread_dec x t); [discriminate|].
    inversion He; subst; simpl; try assumption.
    destruct (thread_dec x u); [discriminate | assumption].
  Qed.

  Lemma own_after c c1 t n :
    eff t (code t n) c c1 ->
    forall L, own c1 L =
      match code t n with
      | IAcq L0 => if lock_dec L L0 then Some t else own c L
      | IRel L0 => if lock_dec L L0 then None else own c L
      | _ => own c L
      end.
  Proof. intros He L. inversion He; subst; simpl; reflexivity. Qed.

  Lemma pc1_after c c1 t n :
    eff t (code t n) c c1 ->
    forall x, pc c1 x =
      match code t n with
      | ISpawn u => if thread_dec x u then Some (entry u) else pc c x
      | _ => pc c x
      end.
  Proof. intros He x. inversion He; subst; simpl; reflexivity. Qed.

  Lemma fl_after c c1 t n :
    eff t (code t n) c c1 ->
    forall F, fl c1 F =
      match code t n with
      | ISet F0 v => if flag_dec F F0 then v else fl c F
      | _ => fl c F
      end.
  Proof. intros He F. inversion He; subst; simpl; reflexivity. Qed.

  Lemma inv_step c c' : inv c -> step c c' -> inv c'.
  Proof.
    intros I Hs. destruct Hs as [c c1 t n n' Hp Hsucc He].
    pose proof (own_after _ _ _ _ He) as Hown.
    pose proof (fl_after _ _ _ _ He) as Hfl.
    pose proof (pc1_after _ _ _ _ He) as Hpc1.
    assert (Hpc : forall x k, pc (set_pc c1 t (Some n')) x = Some k ->
              (x = t /\ k = n') \/ (x <> t /\ pc c x = Some k) \/
              (x <> t /\ pc c x = None /\ code t n = ISpawn x /\ k = entry x))
      by (intros; eapply pc_after; eassumption).
    assert (Hmono : forall x, pc c x <> None -> pc (set_pc c1 t (Some n')) x <> None)
      by (intros; eapply started_mono; eassumption).
    assert (Ht_started : pc c t <> None) by congruence.
    constructor.
    - (* main is started *) apply Hmono. apply (i_main _ I).
    - (* a started thread's parent is started *)
      intros x Hx.
      destruct (pc (set_pc c1 t (Some n')) x) as [k|] eqn:Ek; [|congruence].
      destruct (Hpc _ _ Ek) as [[-> _] | [[Hne Hk] | (Hne & _ & Hsp & _)]].
      + destruct (i_par _ I t Ht_started) as [|Hq]; [left; assumption | right; apply Hmono; assumption].
      + assert (Hx0 : pc c x <> None) by congruence.
        destruct (i_par _ I x Hx0) as [|Hq]; [left; assumption | right; apply Hmono; assumption].
      + right. rewrite (spawn_parent _ _ _ Hsp). apply Hmono. assumption.
    - (* held => owned *)
      intros x k L Hk Hh. simpl. rewrite Hown.
      destruct (Hpc _ _ Hk) as [[-> ->] | [[Hne Hk0] | (Hne & _ & _ & ->)]].
      + destruct (held_flow _ _ _ _ Hsucc Hh) as [[Hh0 Hnr] | Hacq].
        * pose proof (i_held _ I _ _ _ Hp Hh0) as Ho.
          destruct (code t n) eqn:Ec; try assumption.
          -- destruct (lock_dec L L0); [reflexivity | assumption].
          -- destruct (lock_dec L L0) as [->|]; [congruence | assumption].
        * rewrite Hacq. destruct (lock_dec L L); [reflexivity | congruence].
      + pose proof (i_held _ I _ _ _ Hk0 Hh) as Ho.
        destruct (code t n) eqn:Ec; try assumption.
        * (* IAcq L0 by t: L0 was free, so L0 <> L *)
          destruct (lock_dec L L0) as [->|]; [|assumption].
          inversion He; subst; try congruence.
        * (* IRel L0 by t: t holds L0, x holds L => L0 <> L *)
          destruct (lock_dec L L0) as [->|]; [|assumption].
          pose proof (rel_held _ _ _ Ec) as Hht.
          pose proof (i_held _ I _ _ _ Hp Hht) as Hot. congruence.
      + exfalso. eapply held_entry; eassumption.
    - (* tok => current *)
      intros x k F v Hk Ht. simpl. rewrite Hfl.
      destruct (Hpc _ _ Hk) as [[-> ->] | [[Hne Hk0] | (Hne & Hnone & Hsp & ->)]].
      + destruct (tok_flow _ _ _ _ _ Hsucc Ht) as [[Ht0 Hns] | Hsee].
        * pose proof (i_tok _ I _ _ _ _ Hp Ht0) as Hv.
          destruct (code t n) eqn:Ec; try assumption.
          destruct (flag_dec F F0) as [->|]; [|assumption].
          exfalso. apply (Hns v0). reflexivity.
        * rewrite Hsee. inversion He; subst; try congruence.
      + pose proof (i_tok _ I _ _ _ _ Hk0 Ht) as Hv.
        destruct (code t n) eqn:Ec; try assumption.
        destruct (flag_dec F F0) as [->|]; [|assumption].
        (* t stores F0 from the value it holds; x holds F0 too => x = t *)
        destruct (set_tok _ _ _ _ Ec) as [v1 Hv1].
        pose proof (i_tok _ I _ _ _ _ Hp Hv1) as Hcur.
        assert (Hvv : v1 = v) by congruence. rewrite Hvv in Hv1.
        pose proof (tok_grant _ _ _ _ _ _ _ Ht Ec Hv1) as Hox.
        pose proof (tok_grant _ _ _ _ _ _ _ Hv1 Ec Hv1) as Hot.
        congruence.
      + (* a thread just born claims only the initial value, which it owns *)
        destruct (tok_entry _ _ _ Ht) as [-> Hox].
        rewrite Hsp.
        destruct (Nat.eq_dec (fl c F) (init F)) as [E|E]; [assumption|].
        exfalso. apply (i_ini _ I) in E. rewrite Hox in E. congruence.
    - (* before => not started *)
      intros x k u Hk Hb.
      destruct (before_parent _ _ _ Hb) as [Hpar Hux].
      assert (Hu0 : pc c u = None).
      { destruct (Hpc _ _ Hk) as [[-> ->] | [[Hne Hk0] | (Hne & Hnone & Hsp & ->)]].
        - destruct (before_flow _ _ _ _ Hsucc Hb) as [Hb0 _]. eapply i_bef; eassumption.
        - eapply i_bef; eassumption.
        - (* x was just born; its children cannot have started *)
          destruct (pc c u) eqn:Eu; [|reflexivity]. exfalso.
          assert (Hu' : pc c u <> None) by congruence.
          destruct (i_par _ I u Hu') as [->|Hq].
          + rewrite parent_main in Hpar. subst x. apply (i_main _ I). assumption.
          + rewrite Hpar in Hq. congruence. }
      simpl. destruct (thread_dec u t) as [->|Hut]; [congruence|].
      rewrite Hpc1. destruct (code t n) eqn:Ec; try assumption.
      destruct (thread_dec u u0) as [->|]; [|assumption].
      (* t spawned u: then t = parent u = x, and x's own flow constraint forbids it *)
      exfalso.
      pose proof (spawn_parent _ _ _ Ec) as Hpt.
      assert (Hxt : x = t) by congruence.
      destruct (Hpc _ _ Hk) as [[_ Hkn] | [[Hne _] | (Hne & _)]]; try contradiction.
      rewrite Hxt, Hkn in Hb.
      destruct (before_flow _ _ _ _ Hsucc Hb) as [_ Hns]. congruence.
    - (* after => finished *)
      intros x k u Hk Ha.
      assert (Hfin : exists m, pc c u = Some m /\ final u m).
      { destruct (Hpc _ _ Hk) as [[-> ->] | [[Hne Hk0] | (Hne & _ & _ & ->)]].
        - destruct (after_flow _ _ _ _ Hsucc Ha) as [Ha0 | Hj].
          + eapply i_aft; eassumption.
          + rewrite Hj in He. inversion He; subst. eexists. split; eassumption.
        - eapply i_aft; eassumption.
        - exfalso. eapply after_entry; eassumption. }
      destruct Hfin as (m & Hm & Hf). exists m. split; [|assumption].
      (* a finished thread does not move, and is not the one spawned *)
      simpl. destruct (thread_dec u t) as [->|Hut].
      + exfalso. assert (m = n) by congruence. subst m. eapply final_stuck; eassumption.
      + rewrite Hpc1. destruct (code t n) eqn:Ec; try assumption.
        destruct (thread_dec u u0) as [->|]; [|assumption].
        (* a spawn needs its target not started; u has finished *)
        inversion He; congruence.
    - (* a flag off its initial value: the initial value's owner has started *)
      intros F Hne. simpl in Hne. rewrite Hfl in Hne.
      apply Hmono.
      destruct (Nat.eq_dec (fl c F) (init F)) as [E|E]; [|apply (i_ini _ I); assumption].
      destruct (code t n) eqn:Ec; try congruence.
      destruct (flag_dec F F0) as [->|]; [|congruence].
      destruct (set_tok _ _ _ _ Ec) as [v1 Hv1].
      pose proof (i_tok _ I _ _ _ _ Hp Hv1) as Hcur.
      pose proof (tok_grant _ _ _ _ _ _ _ Hv1 Ec Hv1) as Hot.
      assert (Hvv : v1 = init F0) by congruence. rewrite Hvv in Hot. rewrite Hot. assumption.
  Qed.

  Lemma reach_inv c : reach c -> inv c.
  Proof. induction 1; [apply inv0 | eapply inv_step; eassumption]. Qed.

  (* ---- what the annotations mean ---- *)
  Theorem held_is_owned c t n L :
    reach c -> pc c t = Some n -> held t n L -> own c L = Some t.
  Proof. intros R. apply (i_held _ (reach_inv _ R)). Qed.

  Theorem tok_is_current c t n F v :
    reach c -> pc c t = Some n -> tok t n F v -> fl c F = v.
  Proof. intros R. apply (i_tok _ (reach_inv _ R)). Qed.

  Theorem before_not_started c t n u :
    reach c -> pc c t = Some n -> before t n u -> pc c u = None.
  Proof. intros R. apply (i_bef _ (reach_inv _ R)). Qed.

  Theorem after_finished c t n u :
    reach c -> pc c t = Some n -> after t n u -> exists m, pc c u = Some m /\ final u m.
  Proof. intros R. apply (i_aft _ (reach_inv _ R)). Qed.

  (* two threads never hold one lock *)
  Corollary mutual_exclusion c t u n m L :
    reach c -> pc c t = Some n -> pc c u = Some m -> held t n L -> held u m L -> t = u.
  Proof.
    intros R Ht Hu H1 H2.
    pose proof (held_is_owned _ _ _ _ R Ht H1). pose proof (held_is_owned _ _ _ _ R Hu H2).
    congruence.
  Qed.

  (* ---- T1 ---- *)
  Lemma ordered_excludes c t u n m :
    inv c -> t <> u -> pc c t = Some n -> pc c u = Some m ->
    (exists l w a, code u m = IAcc l w a) -> (exists l w a, code t n = IAcc l w a) ->
    ordered t n u m -> False.
  Proof.
    intros I Hne Ht Hu Hau Hat [(L & H1 & H2) | [(F & v & v' & H1 & H2 & Hv) | [Hb | [Ha | [Hb | Ha]]]]].
    - pose proof (i_held _ I _ _ _ Ht H1). pose proof (i_held _ I _ _ _ Hu H2). congruence.
    - pose proof (i_tok _ I _ _ _ _ Ht H1). pose proof (i_tok _ I _ _ _ _ Hu H2). congruence.
    - pose proof (i_bef _ I _ _ _ Ht Hb). congruence.
    - destruct (i_aft _ I _ _ _ Ht Ha) as (k & Hk & Hf).
      assert (k = m) by congruence. subst k.
      destruct Hau as (l & w & a & Hc). rewrite (final_nop _ _ Hf) in Hc. discriminate.
    - pose proof (i_bef _ I _ _ _ Hu Hb). congruence.
    - destruct (i_aft _ I _ _ _ Hu Ha) as (k & Hk & Hf).
      assert (k = n) by congruence. subst k.
      destruct Hat as (l & w & a & Hc). rewrite (final_nop _ _ Hf) in Hc. discriminate.
  Qed.

  Theorem race_free c : reach c -> ~ race c.
  Proof.
    intros R (t & u & n & m & Hne & Ht & Hu & Hc).
    pose proof (judged _ _ _ _ Hne Hc) as Ho.
    eapply (ordered_excludes c t u n m (reach_inv _ R)); try eassumption.
    - unfold conflict in Hc. destruct (code t n); try contradiction.
      destruct (code u m); try contradiction. eauto.
    - unfold conflict in Hc. destruct (code t n); try contradiction. eauto.
  Qed.

End machine.

(* The judgement may be made over ANY relation that contains the real
   overlap — the compiler's may-alias (access_alias.v, overlap_may_alias). *)
Lemma conflict_mono thread loc lock flag (R S : loc -> loc -> Prop) i j :
  (forall a b, R a b -> S a b) ->
  conflict thread loc lock flag R i j -> conflict thread loc lock flag S i j.
Proof.
  intros H. unfold conflict. destruct i; try contradiction. destruct j; try contradiction.
  intros (H1 & H2 & H3). auto.
Qed.

(* ================================================================
   The hypotheses as ONE predicate over a program and an annotation, so
   that "accepted" and "accepted but for hypothesis k" can be stated.
   ================================================================ *)
Section accepted.
  Variables thread node loc lock flag : Type.
  Variable thread_dec : forall a b : thread, {a = b} + {a <> b}.
  Variable lock_dec : forall a b : lock, {a = b} + {a <> b}.
  Variable flag_dec : forall a b : flag, {a = b} + {a <> b}.

  Record prog := Prog {
    p_code : thread -> node -> instr thread loc lock flag;
    p_succ : thread -> node -> node -> Prop;
    p_entry : thread -> node;
    p_final : thread -> node -> Prop;
    p_main : thread;
    p_parent : thread -> thread;
    p_init : flag -> nat;
    p_owner : flag -> nat -> thread;
    p_overlap : loc -> loc -> Prop }.

  Record ann := Ann {
    a_held : thread -> node -> lock -> Prop;
    a_tok : thread -> node -> flag -> nat -> Prop;
    a_before : thread -> node -> thread -> Prop;
    a_after : thread -> node -> thread -> Prop }.

  Variables (P : prog) (A : ann).
  Let code := p_code P. Let succ := p_succ P. Let entry := p_entry P.
  Let final := p_final P. Let parent := p_parent P.
  Let held := a_held A. Let tok := a_tok A. Let before := a_before A. Let after := a_after A.

  Definition h_final_stuck := forall t n n', final t n -> ~ succ t n n'.
  Definition h_final_nop := forall t n, final t n -> code t n = INop thread loc lock flag.
  Definition h_spawn_parent := forall t n u, code t n = ISpawn thread loc lock flag u -> parent u = t.
  Definition h_parent_main := parent (p_main P) = p_main P.
  Definition h_held_entry := forall t L, ~ held t (entry t) L.
  Definition h_held_flow := forall t n n' L, succ t n n' -> held t n' L ->
      (held t n L /\ code t n <> IRel thread loc lock flag L) \/ code t n = IAcq thread loc lock flag L.
  Definition h_rel_held := forall t n L, code t n = IRel thread loc lock flag L -> held t n L.
  Definition h_tok_entry := forall t F v, tok t (entry t) F v -> v = p_init P F /\ p_owner P F v = t.
  Definition h_tok_flow := forall t n n' F v, succ t n n' -> tok t n' F v ->
      (tok t n F v /\ forall w, code t n <> ISet thread loc lock flag F w) \/
      code t n = ISee thread loc lock flag F v.
  Definition h_set_tok := forall t n F w, code t n = ISet thread loc lock flag F w -> exists v, tok t n F v.
  Definition h_tok_grant := forall t n F v s m w,
      tok t n F v -> code s m = ISet thread loc lock flag F w -> tok s m F v -> p_owner P F v = t.
  Definition h_before_parent := forall t n u, before t n u -> parent u = t /\ u <> t.
  Definition h_before_flow := forall t n n' u, succ t n n' -> before t n' u ->
      before t n u /\ code t n <> ISpawn thread loc lock flag u.
  Definition h_after_entry := forall t u, ~ after t (entry t) u.
  Definition h_after_flow := forall t n n' u, succ t n n' -> after t n' u ->
      after t n u \/ code t n = IJoin thread loc lock flag u.
  Definition h_judged := forall t u n m, t <> u ->
      conflict thread loc lock flag (p_overlap P) (code t n) (code u m) ->
      ordered thread node lock flag held tok before after t n u m.

  (* every hypothesis but number `skip` (0: every one) *)
  Definition acc (skip : nat) : Prop :=
    (skip = 1 \/ h_final_stuck) /\ (skip = 2 \/ h_final_nop) /\
    (skip = 3 \/ h_spawn_parent) /\ (skip = 4 \/ h_parent_main) /\
    (skip = 5 \/ h_held_entry) /\ (skip = 6 \/ h_held_flow) /\ (skip = 7 \/ h_rel_held) /\
    (skip = 8 \/ h_tok_entry) /\ (skip = 9 \/ h_tok_flow) /\ (skip = 10 \/ h_set_tok) /\
    (skip = 11 \/ h_tok_grant) /\ (skip = 12 \/ h_before_parent) /\
    (skip = 13 \/ h_before_flow) /\ (skip = 14 \/ h_after_entry) /\
    (skip = 15 \/ h_after_flow) /\ (skip = 16 \/ h_judged).

  Definition accepted : Prop := acc 0.

  Definition p_reach := reach thread node loc lock flag thread_dec lock_dec flag_dec
                              code succ entry final (p_main P) (p_init P).
  Definition p_race := race thread node loc lock flag code (p_overlap P).

  Theorem race_free_accepted : accepted -> forall c, p_reach c -> ~ p_race c.
  Proof.
    intros H c R.
    unfold accepted, acc in H.
    repeat match goal with
           | H : _ /\ _ |- _ => destruct H
           | H : 0 = S _ \/ _ |- _ => destruct H; [discriminate|]
           end.
    eapply (race_free thread node loc lock flag thread_dec lock_dec flag_dec
              code succ entry final (p_main P) parent (p_init P) (p_owner P) (p_overlap P)
              held tok before after); eassumption.
  Qed.

  Lemma go c t n n' c1 :
    p_reach c -> pc _ _ _ _ c t = Some n -> succ t n n' ->
    eff thread node loc lock flag thread_dec lock_dec flag_dec entry final t (code t n) c c1 ->
    p_reach (set_pc thread node lock flag thread_dec c1 t (Some n')).
  Proof. intros R H1 H2 H3. eapply RS; [exact R | eapply Step; eassumption]. Qed.
End accepted.

(* ================================================================
   T3 and T4, over one small universe: three threads, a chain of six
   nodes each, one location, one lock, one flag.
   ================================================================ *)
Module examples.
  Inductive th := M | A | B.
  Inductive nd := N0 | N1 | N2 | N3 | N4 | N5.
  Definition th_dec : forall a b : th, {a = b} + {a <> b}. Proof. decide equality. Defined.
  Definition u_dec : forall a b : unit, {a = b} + {a <> b}. Proof. decide equality. Defined.

  Notation I := (instr th bool unit unit).
  Notation Acc := (IAcc th bool unit unit true true false).    (* a plain write of location x *)
  Notation AccY := (IAcc th bool unit unit false true false).  (* a plain write of location y *)
  Notation Acq := (IAcq th bool unit unit tt).
  Notation Rel := (IRel th bool unit unit tt).
  Notation Set_ v := (ISet th bool unit unit tt v).
  Notation See v := (ISee th bool unit unit tt v).
  Notation Spawn u := (ISpawn th bool unit unit u).
  Notation Join u := (IJoin th bool unit unit u).
  Notation Nop := (INop th bool unit unit).

  Definition next (n : nd) : option nd :=
    match n with N0 => Some N1 | N1 => Some N2 | N2 => Some N3 | N3 => Some N4 | N4 => Some N5 | N5 => None end.

  (* a program: its code, who is whose parent, who stores from which value *)
  Definition mk (code : th -> nd -> I) (parent : th -> th) (owner : nat -> th) : prog th nd bool unit unit :=
    Prog th nd bool unit unit code (fun _ n n' => next n = Some n') (fun _ => N0)
         (fun _ n => n = N5) M parent (fun _ => 0) (fun _ v => owner v) (fun a b => a = b).

  (* an annotation, from computable tables *)
  Definition mka (heldb : th -> nd -> bool) (tokb : th -> nd -> option nat)
                 (befb aftb : th -> nd -> th -> bool) : ann th nd unit unit :=
    Ann th nd unit unit (fun t n _ => heldb t n = true) (fun t n _ v => tokb t n = Some v)
        (fun t n u => befb t n u = true) (fun t n u => aftb t n u = true).

  Definition none2 : th -> nd -> bool := fun _ _ => false.
  Definition none3 : th -> nd -> th -> bool := fun _ _ _ => false.
  Definition notok : th -> nd -> option nat := fun _ _ => None.
  Definition par_M : th -> th := fun _ => M.

  Ltac enum :=
    repeat match goal with
           | x : th |- _ => destruct x
           | x : nd |- _ => destruct x
           | x : unit |- _ => destruct x
           end.

  Ltac inj :=
    repeat match goal with
           | H : Some ?a = Some ?b |- _ => is_var b; inversion H; subst; clear H
           | H : Some ?a = Some ?b |- _ => is_var a; inversion H; subst; clear H
           end.

  Ltac ord :=
    unfold ordered; simpl;
    first [ left; exists tt; split; reflexivity
          | right; left; exists tt; eexists; eexists; split; [reflexivity | split; [reflexivity | discriminate]]
          | right; right; left; reflexivity
          | right; right; right; left; reflexivity
          | right; right; right; right; left; reflexivity
          | right; right; right; right; right; reflexivity ].

  Ltac kill :=
    simpl in *; inj; simpl in *;
    repeat match goal with H : _ /\ _ |- _ => destruct H end;
    try discriminate; try contradiction; try reflexivity; try congruence;
    try (exfalso; tauto);
    first [ split; [congruence | congruence]
          | split; [reflexivity | discriminate]
          | left; split; [congruence | intros; discriminate]
          | left; split; [reflexivity | discriminate]
          | right; congruence
          | left; congruence
          | eexists; reflexivity
          | ord
          | tauto ].

  (* prove one hypothesis of a concrete program by exhaustion *)
  Ltac hyp :=
    right;
    unfold h_final_stuck, h_final_nop, h_spawn_parent, h_parent_main, h_held_entry,
           h_held_flow, h_rel_held, h_tok_entry, h_tok_flow, h_set_tok, h_tok_grant,
           h_before_parent, h_before_flow, h_after_entry, h_after_flow, h_judged;
    simpl; intros; enum; kill.

  Ltac all_but := unfold acc; repeat split; first [ left; reflexivity | hyp ].

  Ltac racing t u n m :=
    exists t, u, n, m; simpl;
    split; [discriminate | split; [reflexivity | split; [reflexivity |
      split; [reflexivity | split; [left; reflexivity | intros [X _]; discriminate X]]]]].

  (* ---- T3: a lock, a hand-off and a join, together ----
       M: spawn A; spawn B; join A; join B; write x
       A: lock; write x; unlock; write y (holding the flag's initial 0); flag := 1
       B: lock; write x; unlock; wait flag = 1; write y                         *)
  Definition c3 (t : th) (n : nd) : I :=
    match t, n with
    | M, N0 => Spawn A | M, N1 => Spawn B | M, N2 => Join A | M, N3 => Join B | M, N4 => Acc
    | A, N0 => Acq | A, N1 => Acc | A, N2 => Rel | A, N3 => AccY | A, N4 => Set_ 1
    | B, N0 => Acq | B, N1 => Acc | B, N2 => Rel | B, N3 => See 1 | B, N4 => AccY
    | _, N5 => Nop
    end.
  Definition held3 (t : th) (n : nd) : bool :=
    match t, n with A, N1 | A, N2 | B, N1 | B, N2 => true | _, _ => false end.
  Definition tok3 (t : th) (n : nd) : option nat :=
    match t, n with
    | A, N0 | A, N1 | A, N2 | A, N3 | A, N4 => Some 0
    | B, N4 => Some 1
    | _, _ => None end.
  Definition aft3 (t : th) (n : nd) (u : th) : bool :=
    match t, n, u with
    | M, N3, A | M, N4, A | M, N5, A | M, N4, B | M, N5, B => true
    | _, _, _ => false end.
  Definition P3 := mk c3 par_M (fun v => match v with 0 => A | _ => B end).
  Definition A3 := mka held3 tok3 none3 aft3.

  Theorem accepted_example : accepted th nd bool unit unit P3 A3.
  Proof. unfold accepted. all_but. Qed.

  Corollary example_race_free :
    forall c, p_reach th nd bool unit unit th_dec u_dec u_dec P3 c ->
              ~ p_race th nd bool unit unit P3 c.
  Proof. apply (race_free_accepted th nd bool unit unit th_dec u_dec u_dec P3 A3). apply accepted_example. Qed.

  (* ---- T4: one reachable race per hypothesis dropped ---- *)

  (* 16, the pairwise rule: M spawns A and both write, nothing ordering them *)
  Definition c_un (t : th) (n : nd) : I :=
    match t, n with M, N0 => Spawn A | M, N1 => Acc | A, N0 => Acc | _, _ => Nop end.
  Definition P_un := mk c_un par_M (fun _ => M).
  Definition A_un := mka none2 notok none3 none3.

  Theorem unjudged_pair_races :
    acc th nd bool unit unit P_un A_un 16 /\
    exists c, p_reach th nd bool unit unit th_dec u_dec u_dec P_un c /\
              p_race th nd bool unit unit P_un c.
  Proof.
    split; [all_but|].
    pose proof (R0 th nd bool unit unit th_dec u_dec u_dec c_un
                   (fun _ n n' => next n = Some n') (fun _ => N0) (fun _ n => n = N5) M (fun _ => 0)) as R.
    eapply (go th nd bool unit unit th_dec u_dec u_dec P_un _ M N0 N1) in R;
      [| reflexivity | reflexivity | simpl; constructor; reflexivity].
    eexists. split; [exact R|]. racing M A N1 N0.
  Qed.

  (* 7, release only by the holder: A drops a lock it does not hold, then takes it *)
  Definition c_rr (t : th) (n : nd) : I :=
    match t, n with
    | M, N0 => Spawn A | M, N1 => Acq | M, N2 => Acc
    | A, N0 => Rel | A, N1 => Acq | A, N2 => Acc
    | _, _ => Nop end.
  Definition held_rr (t : th) (n : nd) : bool :=
    match t, n with M, N2 | A, N2 => true | _, _ => false end.
  Definition P_rr := mk c_rr par_M (fun _ => M).
  Definition A_rr := mka held_rr notok none3 none3.

  Theorem rogue_release_races :
    acc th nd bool unit unit P_rr A_rr 7 /\
    exists c, p_reach th nd bool unit unit th_dec u_dec u_dec P_rr c /\
              p_race th nd bool unit unit P_rr c.
  Proof.
    split; [all_but|].
    pose proof (R0 th nd bool unit unit th_dec u_dec u_dec c_rr
                   (fun _ n n' => next n = Some n') (fun _ => N0) (fun _ n => n = N5) M (fun _ => 0)) as R.
    eapply (go th nd bool unit unit th_dec u_dec u_dec P_rr _ M N0 N1) in R;
      [| reflexivity | reflexivity | simpl; constructor; reflexivity].
    eapply (go th nd bool unit unit th_dec u_dec u_dec P_rr _ M N1 N2) in R;
      [| reflexivity | reflexivity | simpl; constructor; reflexivity].
    eapply (go th nd bool unit unit th_dec u_dec u_dec P_rr _ A N0 N1) in R;
      [| reflexivity | reflexivity | simpl; constructor].
    eapply (go th nd bool unit unit th_dec u_dec u_dec P_rr _ A N1 N2) in R;
      [| reflexivity | reflexivity | simpl; constructor; reflexivity].
    eexists. split; [exact R|]. racing M A N2 N2.
  Qed.

  (* 10, a store only by a holder: A forges the value M's reader side waits for *)
  Definition c_fs (t : th) (n : nd) : I :=
    match t, n with
    | M, N0 => Spawn A | M, N1 => Acc
    | A, N0 => Set_ 1 | A, N1 => See 1 | A, N2 => Acc
    | _, _ => Nop end.
  Definition tok_fs (t : th) (n : nd) : option nat :=
    match t, n with M, N0 | M, N1 => Some 0 | A, N2 => Some 1 | _, _ => None end.
  Definition P_fs := mk c_fs par_M (fun _ => M).
  Definition A_fs := mka none2 tok_fs none3 none3.

  Theorem forged_store_races :
    acc th nd bool unit unit P_fs A_fs 10 /\
    exists c, p_reach th nd bool unit unit th_dec u_dec u_dec P_fs c /\
              p_race th nd bool unit unit P_fs c.
  Proof.
    split; [all_but|].
    pose proof (R0 th nd bool unit unit th_dec u_dec u_dec c_fs
                   (fun _ n n' => next n = Some n') (fun _ => N0) (fun _ n => n = N5) M (fun _ => 0)) as R.
    eapply (go th nd bool unit unit th_dec u_dec u_dec P_fs _ M N0 N1) in R;
      [| reflexivity | reflexivity | simpl; constructor; reflexivity].
    eapply (go th nd bool unit unit th_dec u_dec u_dec P_fs _ A N0 N1) in R;
      [| reflexivity | reflexivity | simpl; constructor].
    eapply (go th nd bool unit unit th_dec u_dec u_dec P_fs _ A N1 N2) in R;
      [| reflexivity | reflexivity | simpl; constructor; reflexivity].
    eexists. split; [exact R|]. racing M A N1 N2.
  Qed.

  (* 11, a stored-from value has ONE holder: M and A both hold the initial 0;
     A hands the flag on to itself while M still writes under it *)
  Definition c_sc (t : th) (n : nd) : I :=
    match t, n with
    | M, N0 => Spawn A | M, N1 => Acc
    | A, N0 => See 0 | A, N1 => Set_ 1 | A, N2 => See 1 | A, N3 => Acc
    | _, _ => Nop end.
  Definition tok_sc (t : th) (n : nd) : option nat :=
    match t, n with M, N0 | M, N1 => Some 0 | A, N1 => Some 0 | A, N3 => Some 1 | _, _ => None end.
  Definition P_sc := mk c_sc par_M (fun _ => M).
  Definition A_sc := mka none2 tok_sc none3 none3.

  Theorem second_claimant_races :
    acc th nd bool unit unit P_sc A_sc 11 /\
    exists c, p_reach th nd bool unit unit th_dec u_dec u_dec P_sc c /\
              p_race th nd bool unit unit P_sc c.
  Proof.
    split; [all_but|].
    pose proof (R0 th nd bool unit unit th_dec u_dec u_dec c_sc
                   (fun _ n n' => next n = Some n') (fun _ => N0) (fun _ n => n = N5) M (fun _ => 0)) as R.
    eapply (go th nd bool unit unit th_dec u_dec u_dec P_sc _ M N0 N1) in R;
      [| reflexivity | reflexivity | simpl; constructor; reflexivity].
    eapply (go th nd bool unit unit th_dec u_dec u_dec P_sc _ A N0 N1) in R;
      [| reflexivity | reflexivity | simpl; constructor; reflexivity].
    eapply (go th nd bool unit unit th_dec u_dec u_dec P_sc _ A N1 N2) in R;
      [| reflexivity | reflexivity | simpl; constructor].
    eapply (go th nd bool unit unit th_dec u_dec u_dec P_sc _ A N2 N3) in R;
      [| reflexivity | reflexivity | simpl; constructor; reflexivity].
    eexists. split; [exact R|]. racing M A N1 N3.
  Qed.

  (* 3, a thread is started by its parent only: M counts on A not having
     started; B starts it *)
  Definition c_ss (t : th) (n : nd) : I :=
    match t, n with M, N0 => Spawn B | M, N1 => Acc | B, N0 => Spawn A | A, N0 => Acc | _, _ => Nop end.
  Definition bef_ss (t : th) (n : nd) (u : th) : bool :=
    match t, n, u with M, N0, A | M, N1, A => true | _, _, _ => false end.
  Definition P_ss := mk c_ss par_M (fun _ => M).
  Definition A_ss := mka none2 notok bef_ss none3.

  Theorem stranger_spawn_races :
    acc th nd bool unit unit P_ss A_ss 3 /\
    exists c, p_reach th nd bool unit unit th_dec u_dec u_dec P_ss c /\
              p_race th nd bool unit unit P_ss c.
  Proof.
    split; [all_but|].
    pose proof (R0 th nd bool unit unit th_dec u_dec u_dec c_ss
                   (fun _ n n' => next n = Some n') (fun _ => N0) (fun _ n => n = N5) M (fun _ => 0)) as R.
    eapply (go th nd bool unit unit th_dec u_dec u_dec P_ss _ M N0 N1) in R;
      [| reflexivity | reflexivity | simpl; constructor; reflexivity].
    eapply (go th nd bool unit unit th_dec u_dec u_dec P_ss _ B N0 N1) in R;
      [| reflexivity | reflexivity | simpl; constructor; reflexivity].
    eexists. split; [exact R|]. racing M A N1 N0.
  Qed.
End examples.

(* ================================================================
   What the pairwise decision MEANS (the function is in
   concurrency_decisions.v; the C is proved equal to it by VST).  The
   checker asks it of two access records of two thread instances:
     may_alias     the records may name overlapping memory
     a_write / b_write
     both_atomic
     common_lock   one lock is held at both
     tok_differ    two different held values of one flag
     apart         one of the two threads is not running at the other's
                   access (not yet started, or joined)
   It answers 1 = "this pair needs nothing more", exactly `conflict ->
   ordered` of the theorem above.
   ================================================================ *)
Local Open Scope Z_scope.

Lemma pair_ordered_meaning al aw bw ba cl td ap :
  zer_pair_ordered_coq al aw bw ba cl td ap = 1 <->
  (al = 0 \/ (aw = 0 /\ bw = 0) \/ ba <> 0) \/ (cl <> 0 \/ td <> 0 \/ ap <> 0).
Proof.
  unfold zer_pair_ordered_coq.
  repeat destruct (Z.eq_dec _ _); split; intros H; try lia.
Qed.

(* a conflicting pair with no ordering is never accepted *)
Lemma unordered_conflict_refused aw bw :
  aw <> 0 \/ bw <> 0 -> zer_pair_ordered_coq 1 aw bw 0 0 0 0 = 0.
Proof. intros H. unfold zer_pair_ordered_coq. repeat destruct (Z.eq_dec _ _); lia. Qed.
