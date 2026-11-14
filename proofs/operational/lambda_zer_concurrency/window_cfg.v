(* ================================================================
   lambda_zer_concurrency / window_cfg : the SCOPED-SPAWN WINDOW over an
   ARBITRARY control-flow graph.

   WHY THIS FILE.  The scoped-spawn rule ("a variable lent to
   `ThreadHandle th = spawn f(&x)` may not be touched by the parent until
   `th.join()`") was implemented as a TEXT-ORDER fact: borrowed at the spawn
   statement, released at the join statement, as the checker walks the source.
   Text order is not execution order.  Three accepted programs raced (TSan):

     - a `goto` runs code written BEFORE the spawn inside the window (BUG-1648);
     - a `break` leaves the window with the thread running, and the code after
       the loop never reaches the function exit, so the "joined before exit"
       check never fires (BUG-1651);
     - the join of ONE handle released a variable lent by ANOTHER handle whose
       thread was still running (BUG-1651).

   All three were found by red-team, not by a proof, because no model contained
   the variable that distinguishes them: the SET OF PROGRAM POINTS AT WHICH A
   THREAD MAY BE RUNNING.  This file is that model.

   THE MODEL.  A function body is a graph: nodes carry an action (spawn h /
   join h / access x / nothing) and `succ` is ANY edge relation — fallthrough,
   branch, loop back edge, break, continue, goto.  A configuration is a program
   counter plus the set of running threads.  Two bad states:
     race_parent   the parent accesses x while a running thread was lent x;
     race_threads  two running threads were lent the same x.

   THE ABSTRACT DOMAIN.  For each handle h, a WINDOW  W h : node -> Prop  —
   "h may be running on entry to this node".  It must be closed under steps
   (a post-fixpoint): the successors of a spawn of h are in it, and it is left
   only THROUGH a join of h.

   THE THEOREMS (zero admits, self-contained):
     T1  window_invariant      a running thread is always inside its window.
     T2  window_sound          closed windows + "no access / second lend inside
                               a window"  ==>  no reachable race, for EVERY
                               graph and EVERY schedule of the parent.
     I1  closed_window_ok      the CLOSED window (interior of spawn..join with
                               no live exit) is a post-fixpoint.
     I2  tail_window_ok        the TAIL window (everything except the code that
                               is entered only by running the handle's join) is
                               a post-fixpoint.
     I3  latched_window_ok     "borrowed for the whole function" is one.
     T3  precision witness     a closed window accepts an access after its join
                               (the latch alone would refuse it).
     T4  break_exit_races      the old textual window is NOT a post-fixpoint on
                               the break-out graph, and that graph really races:
                               no closed window can accept it.

   WHAT THE COMPILER DISCHARGES (checker.c; each hypothesis below is one
   syntactic check):
     closed_window_ok.Hlive_exit   win_exits_live — no return / goto / break /
                                   continue / orelse-exit leaves the interior
                                   while the thread may run.
     tail_window_ok.Hentry         the tail join is at branch depth 0, outside
                                   @once, and after the last goto / label, so
                                   the code after it is entered only through it.
     accepts (second lend)         one lend site per latched variable, and the
                                   textual "already borrowed" check inside a
                                   closed window.
     no re-spawn while running     zercheck_ir ("re-spawned while the thread
                                   ... may not be joined").

   HONEST LIMIT.  This certifies the RULE, not checker.c: the link from each
   syntactic check to its hypothesis is by inspection + the gates in
   tests/test_conc_matrix.c, not by VST.  And in a function WITHOUT a jump an
   open window is borrowed from its spawn statement on (not from the function
   entry); that the code before the spawn cannot run beside the thread there
   rests on structured control flow + the re-spawn check, which this graph
   model does not derive.
   ================================================================ *)
From stdpp Require Import base decidable.

Section window.
  Context {node handle var : Type}.
  Context `{EqDecision handle}.

  Inductive action :=
  | ASpawn (h : handle)
  | AJoin (h : handle)
  | AAccess (x : var)
  | ANop.

  Context (act : node → action).
  Context (succ : node → node → Prop).
  Context (lends : handle → var → Prop).
  Context (entry : node).

  (* ---- dynamics ---- *)
  Definition live := handle → bool.

  Definition exec (a : action) (L : live) : live :=
    match a with
    | ASpawn h => λ k, if decide (k = h) then true else L k
    | AJoin h => λ k, if decide (k = h) then false else L k
    | AAccess _ => L
    | ANop => L
    end.

  Record config := Config { pc : node; lv : live }.

  Definition init : config := Config entry (λ _, false).

  Inductive step : config → config → Prop :=
  | Step c m : succ (pc c) m → step c (Config m (exec (act (pc c)) (lv c))).

  Inductive reach : config → Prop :=
  | reach_init : reach init
  | reach_step c c' : reach c → step c c' → reach c'.

  Definition race_parent (c : config) : Prop :=
    ∃ x h, act (pc c) = AAccess x ∧ lv c h = true ∧ lends h x.

  Definition race_threads (c : config) : Prop :=
    ∃ x h h', h ≠ h' ∧ lv c h = true ∧ lv c h' = true ∧ lends h x ∧ lends h' x.

  (* ---- the abstract domain: a window per handle ---- *)
  Definition post_fixpoint (h : handle) (W : node → Prop) : Prop :=
    (∀ n m, act n = ASpawn h → succ n m → W m) ∧
    (∀ n m, W n → act n ≠ AJoin h → succ n m → W m).

  (* What the checker refuses inside a window. *)
  Definition accepts (W : handle → node → Prop) : Prop :=
    (∀ n x h, act n = AAccess x → lends h x → ¬ W h n) ∧
    (∀ n h h' x, act n = ASpawn h' → h ≠ h' → lends h x → lends h' x → ¬ W h n).

  (* T1 *)
  Lemma window_invariant (W : handle → node → Prop) :
    (∀ h, post_fixpoint h (W h)) →
    ∀ c, reach c → ∀ h, lv c h = true → W h (pc c).
  Proof.
    intros HW c Hr. induction Hr as [|c c' Hr IH Hs]; intros h Hl.
    - simpl in Hl. discriminate.
    - destruct Hs as [c m Hsucc]. simpl in *.
      destruct (HW h) as [K1 K2].
      destruct (act (pc c)) as [h0|h0|x|] eqn:Ha; simpl in Hl.
      + destruct (decide (h = h0)) as [->|Hne].
        * eapply K1; eauto.
        * eapply K2; eauto. rewrite Ha. discriminate.
      + destruct (decide (h = h0)) as [->|Hne]; [discriminate|].
        eapply K2; eauto. rewrite Ha. intros Heq. inversion Heq. congruence.
      + eapply K2; eauto. rewrite Ha. discriminate.
      + eapply K2; eauto. rewrite Ha. discriminate.
  Qed.

  Lemma no_two_lenders (W : handle → node → Prop) :
    (∀ h, post_fixpoint h (W h)) → accepts W →
    ∀ c, reach c → ¬ race_threads c.
  Proof.
    intros HW [_ Hacc2] c Hr. induction Hr as [|c c' Hr IH Hs].
    - intros (x & h & h' & _ & Hl & _). simpl in Hl. discriminate.
    - destruct Hs as [c m Hsucc].
      intros (x & h & h' & Hne & Hl & Hl' & Hx & Hx'). simpl in *.
      pose proof (window_invariant W HW c Hr) as Hinv.
      destruct (act (pc c)) as [h0|h0|y|] eqn:Ha; simpl in Hl, Hl'.
      + destruct (decide (h = h0)) as [->|Hn1];
          destruct (decide (h' = h0)) as [->|Hn2].
        * congruence.
        * (* h0 is being spawned while h' runs *)
          eapply (Hacc2 (pc c) h' h0 x); eauto.
        * eapply (Hacc2 (pc c) h h0 x); eauto.
        * apply IH. exists x, h, h'. auto.
      + destruct (decide (h = h0)) as [->|Hn1]; [discriminate|].
        destruct (decide (h' = h0)) as [->|Hn2]; [discriminate|].
        apply IH. exists x, h, h'. auto.
      + apply IH. exists x, h, h'. auto.
      + apply IH. exists x, h, h'. auto.
  Qed.

  (* T2 — the soundness theorem. *)
  Theorem window_sound (W : handle → node → Prop) :
    (∀ h, post_fixpoint h (W h)) → accepts W →
    ∀ c, reach c → ¬ race_parent c ∧ ¬ race_threads c.
  Proof.
    intros HW Hacc c Hr. split.
    - intros (x & h & Ha & Hl & Hx).
      destruct Hacc as [Hacc1 _].
      eapply Hacc1; eauto. eapply window_invariant; eauto.
    - eapply no_two_lenders; eauto.
  Qed.

  (* T4 form: a reachable race forces a reject under ANY closed windows. *)
  Corollary race_forces_reject (W : handle → node → Prop) c :
    (∀ h, post_fixpoint h (W h)) → reach c →
    race_parent c ∨ race_threads c → ¬ accepts W.
  Proof.
    intros HW Hr Hrace Hacc.
    destruct (window_sound W HW Hacc c Hr) as [Hp Ht]. tauto.
  Qed.

  (* ---- I1: the CLOSED window ----
     R is the part of the spawn..join interior where the thread may still be
     running (the statements after a nested `h.join()` in the same statement
     list are not in it); j is the textual join. *)
  Section closed.
    Context (h : handle) (R : node → Prop) (j : node).
    Hypothesis Hspawn : ∀ n m, act n = ASpawn h → succ n m → R m ∨ m = j.
    Hypothesis Hlive_exit : ∀ n m, R n → act n ≠ AJoin h → succ n m → R m ∨ m = j.
    Hypothesis Hj : act j = AJoin h.

    Definition Wclosed : node → Prop := λ n, R n ∨ n = j.

    Lemma closed_window_ok : post_fixpoint h Wclosed.
    Proof.
      split.
      - intros n m Ha Hs. exact (Hspawn n m Ha Hs).
      - intros n m [Hn | ->] Hnj Hs.
        + exact (Hlive_exit n m Hn Hnj Hs).
        + contradiction.
    Qed.
  End closed.

  (* ---- I2: the TAIL window ----
     T is the code after the handle's tail join j: entered ONLY by running j. *)
  Section tail.
    Context (h : handle) (T : node → Prop) (j : node).
    Hypothesis Hentry : ∀ n m, succ n m → T m → T n ∨ n = j.
    Hypothesis Hno_spawn : ∀ n, act n = ASpawn h → ¬ T n.
    Hypothesis Hj : act j = AJoin h.

    Definition Wtail : node → Prop := λ n, ¬ T n.

    Lemma tail_window_ok : post_fixpoint h Wtail.
    Proof.
      split.
      - intros n m Ha Hs HT.
        destruct (Hentry n m Hs HT) as [Hn | ->].
        + exact (Hno_spawn n Ha Hn).
        + rewrite Hj in Ha. discriminate.
      - intros n m Hn Hnj Hs HT.
        destruct (Hentry n m Hs HT) as [Hn' | ->].
        + exact (Hn Hn').
        + exact (Hnj Hj).
    Qed.
  End tail.

  (* ---- I3: borrowed for the whole function ---- *)
  Lemma latched_window_ok h : post_fixpoint h (λ _, True).
  Proof. split; auto. Qed.

  (* T3 — precision: a node outside a closed window is not in it, so an access
     there is not refused on h's account (the latch refuses every node). *)
  Lemma closed_window_precise (R : node → Prop) (j n : node) :
    ¬ R n → n ≠ j → ¬ Wclosed R j n.
  Proof. intros HR Hj [H | H]; auto. Qed.

End window.

(* ================================================================
   T4 — the break-out graph.  The shape of
       for (..) { th = spawn w(&x); if (c) { break; } th.join(); }
       for (;;) { x += 1; }
     0: spawn h        -> 1
     1: if (c) break   -> 2 (fall through), -> 3 (break)
     2: join h         -> 0 (next iteration)
     3: access x       -> 3 (never reaches the function exit)
   ================================================================ *)
Section break_exit.
  Definition b_act (n : nat) : @action unit unit :=
    match n with
    | 0 => ASpawn ()
    | 2 => AJoin ()
    | 3 => AAccess ()
    | _ => ANop
    end.
  Definition b_succ (n m : nat) : Prop :=
    (n = 0 ∧ m = 1) ∨ (n = 1 ∧ m = 2) ∨ (n = 1 ∧ m = 3) ∨
    (n = 2 ∧ m = 0) ∨ (n = 3 ∧ m = 3).
  Definition b_lends (_ _ : unit) : Prop := True.

  (* the old textual window: the statement between the spawn and the join *)
  Definition b_textual (n : nat) : Prop := n = 1 ∨ n = 2.

  Lemma textual_window_not_closed :
    ¬ post_fixpoint b_act b_succ () b_textual.
  Proof.
    intros [_ K2].
    assert (b_textual 3) as [H | H].
    { apply (K2 1 3).
      - left. reflexivity.
      - simpl. discriminate.
      - right. right. left. split; reflexivity. }
    - discriminate.
    - discriminate.
  Qed.

  Lemma break_exit_races :
    ∃ c, reach b_act b_succ 0 c ∧ race_parent b_act b_lends c.
  Proof.
    set (c0 := init (node := nat) (handle := unit) 0).
    assert (reach b_act b_succ 0 c0) as H0 by constructor.
    assert (step b_act b_succ c0 (Config 1 (exec (b_act (pc c0)) (lv c0)))) as S1.
    { constructor. left. split; reflexivity. }
    pose proof (reach_step _ _ _ _ _ H0 S1) as H1.
    set (c1 := Config 1 (exec (b_act (pc c0)) (lv c0))) in *.
    assert (step b_act b_succ c1 (Config 3 (exec (b_act (pc c1)) (lv c1)))) as S2.
    { constructor. right. right. left. split; reflexivity. }
    pose proof (reach_step _ _ _ _ _ H1 S2) as H2.
    eexists. split; [exact H2|].
    exists (), (). repeat split.
  Qed.

  (* so no family of closed windows accepts this graph *)
  Corollary break_exit_rejected (W : unit → nat → Prop) :
    (∀ h, post_fixpoint b_act b_succ h (W h)) → ¬ accepts b_act b_lends W.
  Proof.
    intros HW. destruct break_exit_races as (c & Hr & Hrace).
    eapply race_forces_reject; eauto.
  Qed.
End break_exit.

(* ================================================================
   THE DECISION FUNCTIONS the compiler runs (src/safety/concurrency_rules.c,
   VST-verified in proofs/vst/verif_window_rules.v against the SAME definitions,
   imported from concurrency_decisions.v), and what each one MEANS in terms of
   the hypotheses above.  0 = false, anything else = true (C `int`).
   ================================================================ *)
From Coq Require Import ZArith Lia.
(* The definitions themselves live in concurrency_decisions.v — the ONE source
   both this file and the VST proof import (they used to be written out in both
   places, and nothing kept the copies equal). *)
From zer_conc Require Import concurrency_decisions.
Open Scope Z_scope.

(* A join releases iff the window is closed (closed_window_ok), or the join is
   a tail join: no conditional nesting, not in @once, after the last jump
   (the three facts that give tail_window_ok.Hentry). *)
Lemma join_releases_meaning closed bd once aj :
  zer_window_join_releases_coq closed bd once aj = 1 ↔
  closed ≠ 0 ∨ (bd = 0 ∧ once = 0 ∧ aj ≠ 0).
Proof.
  unfold zer_window_join_releases_coq.
  repeat destruct (Z.eq_dec _ _); split; intros H; try lia.
Qed.

Lemma join_releases_bool closed bd once aj :
  zer_window_join_releases_coq closed bd once aj = 0 ∨
  zer_window_join_releases_coq closed bd once aj = 1.
Proof.
  unfold zer_window_join_releases_coq. repeat destruct (Z.eq_dec _ _); auto.
Qed.

(* An OPEN window is never released by a join inside a conditional body. *)
Lemma open_join_in_branch_holds bd once aj :
  bd ≠ 0 → zer_window_join_releases_coq 0 bd once aj = 0.
Proof.
  intros H. unfold zer_window_join_releases_coq.
  repeat destruct (Z.eq_dec _ _); lia.
Qed.

(* ... nor by one written before a later goto / label. *)
Lemma open_join_before_jump_holds bd once :
  zer_window_join_releases_coq 0 bd once 0 = 0.
Proof.
  unfold zer_window_join_releases_coq. repeat destruct (Z.eq_dec _ _); lia.
Qed.

(* A statement leaves the interior iff it is a return / goto, or a loop jump
   with no loop entered inside the interior (closed_window_ok.Hlive_exit). *)
Lemma stmt_leaves_meaning rg lj ld :
  zer_window_stmt_leaves_coq rg lj ld = 1 ↔ rg ≠ 0 ∨ (lj ≠ 0 ∧ ld = 0).
Proof.
  unfold zer_window_stmt_leaves_coq.
  repeat destruct (Z.eq_dec _ _); split; intros H; try lia.
Qed.

Lemma counts_at_spawn_meaning closed latched :
  zer_window_counts_at_spawn_coq closed latched = 1 ↔ closed ≠ 0 ∨ latched = 0.
Proof.
  unfold zer_window_counts_at_spawn_coq.
  repeat destruct (Z.eq_dec _ _); split; intros H; try lia.
Qed.
