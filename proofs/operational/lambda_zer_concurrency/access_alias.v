(* ================================================================
   lambda_zer_concurrency / access_alias : WHAT an access touches.

   WHY THIS FILE.  When a thread uses an @atomic, the compiler judges every
   plain access of that thread against the other threads' ("two accesses of
   one object, one a write, nothing ordering them").  That judgement compares
   RECORDS — and until BUG-1666 a record existed only for an access the
   checker could attribute to a global by NAME.  An access through a
   reference it could not name was attributed to nothing (treated as the
   thread's own), and an access through a global pointer was keyed by the
   pointer's name, so two names for one object never met.  Eight accepted
   programs raced under ThreadSanitizer: a pointer loaded from memory,
   returned by a call, re-aimed by a callee or later in the loop; two global
   pointers to one heap object, two Handles for one slot, a struct's pointer
   field against the global it points at.

   THE MODEL.  Memory is a set of top-level OBJECTS, each with a type; a
   type is a tree of sub-objects (struct fields, array elements).  An ACCESS
   touches the sub-object at a PATH inside one object.  Two accesses OVERLAP
   when they are in the same object and one path is a prefix of the other.
   A RECORD is what the compiler writes down for an access:
       RNamed g    "inside the global g"
       RTyped t    "inside some object of type t" — the type of what the
                   dereferenced reference points at
   WHAT THE COMPILER MUST ESTABLISH (sound_rec), per access:
       RNamed g    the access IS inside g                           (H_named)
       RTyped t    the reference it went through points at a sub-object of
                   type t that the accessed path lies inside        (H_typed)
                   — ZER's reference typing: no pointer arithmetic, casts
                   between pointer types are provenance-checked.
     and, outside this model: an access with NO record is to memory no other
     thread reaches (the thread's own stack)                      (H_private)

   THE THEOREMS (zero admits, plain Coq, self-contained):
     T1  overlap_may_alias    two overlapping accesses with sound records
                              have records that MAY ALIAS — so a judgement
                              that compares every may-aliasing pair misses no
                              overlapping pair.
     T3  precision            two different named globals never may-alias;
                              two typed records of unrelated types do not; a
                              named global does not may-alias a type it does
                              not contain.
     T4  necessity            each way of may-aliasing is realised by two
                              overlapping accesses with sound records:
                              same_global_overlaps, named_typed_overlaps,
                              typed_typed_overlaps.
     and for the one fact the compiler trusts about a LOCAL reference:
     T1  local_value_listed   a variable written only by its listed
                              assignments holds one of the listed values;
     T4  foreign_write_escapes  one write through its address (or a second
                              variable of the same name) and it need not.
     and for the one HEAP object an access needs no record for:
     T1  fresh_stays_own      an object its allocating thread never PUBLISHES
                              is reached, and so used, by that thread alone;
     T3  two_owners_never_meet  two threads, an allocation each: every use is
                              the owner's;
     T4  published_is_shared  one publication and any thread may use it.

   WHAT THE COMPILER DISCHARGES (checker.c, the "what an access touches"
   block), each by one syntactic check:
     H_named    as before: the name, `&g`, a binding whose every value is
                followed.
     H_typed    every dereference step whose reference is not KNOWN to point
                only into recorded globals or the thread's own stack gets
                RTyped (pointee type).  KNOWN is the relaxation, and is
                granted only: to `&place` on a global or an own local; to a
                never-mutated global pointer (its initializer); to a local
                reference bound once, whose address is never taken, every
                value ever assigned to which is itself known
                (local_value_listed is why those three, and
                foreign_write_escapes why not fewer).
     A Handle dereference is RNamed (its Pool / Slab).
     H_private  the thread's own stack, as before; and (fresh_stays_own) a
                FRESH heap object — the builtin `alloc` — bound to a known
                local whose value never LEAVES the name: every occurrence of
                the name is the base of a field / element access, or the
                argument of `free`.  Any other occurrence — a copy, a call
                argument, a return, a store, `&name.f`, a sub-slice, an array
                field in a position that may form a view — is a possible
                publication, and the name is then unknown everywhere in its
                function (published_is_shared is why).

   HONEST LIMITS.  (1) This certifies the RULE; that the syntactic checks
   classify every reference correctly on every execution is argued at the
   checks and gated by the "pointer-reach" grid of tests/test_conc_matrix.c.
   (2) The type tree has structs and arrays.  A UNION puts unrelated types at
   one address; the compiler treats a union as containing every variant, and
   two pointers into two DIFFERENT variants of one union are covered only by
   the argument that one of them needs a write of the whole union to exist —
   not modelled.  (3) An Arena carves typed objects out of a byte buffer: the
   compiler makes a named access to an arena's backing buffer may-alias every
   typed record, by fiat.  (4) C code (cinclude) and MMIO are floors.
   (5) H_private for the STACK rests on the scoped-borrow rules and the escape
   analysis.  (6) The fresh-object model has no `free`: an address handed out
   again after a free is a new object, and the old name still holds it.  A use
   through the old name is a use-after-free, which zercheck rejects on its own
   — that the two lifetimes do not overlap rests on that analysis.  (7) The
   leak walk is syntactic and whole-function: it proves "never published" by
   never seeing the reference move at all, so it is far from complete (a
   helper that only fills the buffer makes the name unknown).
   ================================================================ *)
From Coq Require Import List Bool Arith ZArith Lia.
Import ListNotations.
From zer_conc Require Import concurrency_decisions.
Local Open Scope nat_scope.

(* ---- types as trees of sub-objects ---- *)
Inductive ty :=
| TScalar (n : nat)
| TStruct (fs : list ty)
| TArr (e : ty).

Fixpoint sub (t : ty) (p : list nat) : option ty :=
  match p with
  | [] => Some t
  | i :: p' =>
      match t with
      | TStruct fs => match nth_error fs i with Some f => sub f p' | None => None end
      | TArr e => sub e p'
      | TScalar _ => None
      end
  end.

(* `outer` holds `inner`: inner is the type of some sub-object of outer. *)
Definition holds (outer inner : ty) : Prop := exists p, sub outer p = Some inner.

Lemma sub_app t q r :
  sub t (q ++ r) = match sub t q with Some u => sub u r | None => None end.
Proof.
  revert t. induction q as [|i q IH]; intros t; simpl; [reflexivity|].
  destruct t as [n|fs|e]; simpl.
  - reflexivity.
  - destruct (nth_error fs i); [apply IH | reflexivity].
  - apply IH.
Qed.

Lemma holds_refl t : holds t t.
Proof. exists []. reflexivity. Qed.

(* ---- prefixes ---- *)
Definition prefix (a b : list nat) : Prop := exists r, b = a ++ r.

Lemma prefix_comparable a b c : prefix a c -> prefix b c -> prefix a b \/ prefix b a.
Proof.
  revert b c. induction a as [|x a IH]; intros b c [ra Ha] [rb Hb].
  - left. exists b. reflexivity.
  - destruct b as [|y b].
    + right. exists (x :: a). reflexivity.
    + subst c. simpl in Hb. inversion Hb as [[Hxy Hrest]]. subst y.
      destruct (IH b (a ++ ra)) as [[r Hr]|[r Hr]].
      * exists ra. reflexivity.
      * exists rb. exact Hrest.
      * left. exists r. simpl. rewrite Hr. reflexivity.
      * right. exists r. simpl. rewrite Hr. reflexivity.
Qed.

Lemma prefix_trans a b c : prefix a b -> prefix b c -> prefix a c.
Proof. intros [r1 H1] [r2 H2]. exists (r1 ++ r2). subst. rewrite app_assoc. reflexivity. Qed.

(* ---- memory, accesses, records ---- *)
Section mem.
  Variable obj_ty : nat -> ty.          (* the type of each top-level object *)

  Definition access := (nat * list nat)%type.   (* object, path *)

  Definition overlap (a1 a2 : access) : Prop :=
    fst a1 = fst a2 /\ (prefix (snd a1) (snd a2) \/ prefix (snd a2) (snd a1)).

  Inductive rec := RNamed (g : nat) | RTyped (t : ty).

  (* H_named / H_typed *)
  Definition sound_rec (a : access) (r : rec) : Prop :=
    match r with
    | RNamed g => fst a = g
    | RTyped t => exists q, prefix q (snd a) /\ sub (obj_ty (fst a)) q = Some t
    end.

  Definition may_alias (r1 r2 : rec) : Prop :=
    match r1, r2 with
    | RNamed g1, RNamed g2 => g1 = g2
    | RNamed g, RTyped t => holds (obj_ty g) t
    | RTyped t, RNamed g => holds (obj_ty g) t
    | RTyped t1, RTyped t2 => holds t1 t2 \/ holds t2 t1
    end.

  Lemma sub_prefix_holds T q1 q2 t1 t2 :
    prefix q1 q2 -> sub T q1 = Some t1 -> sub T q2 = Some t2 -> holds t1 t2.
  Proof.
    intros [r Hr] H1 H2. subst q2. rewrite sub_app, H1 in H2. exists r. exact H2.
  Qed.

  (* T1 *)
  Theorem overlap_may_alias a1 a2 r1 r2 :
    overlap a1 a2 -> sound_rec a1 r1 -> sound_rec a2 r2 -> may_alias r1 r2.
  Proof.
    intros [Ho Hp] S1 S2. destruct a1 as [o1 p1], a2 as [o2 p2]. simpl in *. subst o2.
    destruct r1 as [g1|t1], r2 as [g2|t2]; simpl in *.
    - congruence.
    - destruct S2 as [q [_ Hq]]. subst g1. exists q. exact Hq.
    - destruct S1 as [q [_ Hq]]. subst g2. exists q. exact Hq.
    - destruct S1 as [q1 [P1 H1]]. destruct S2 as [q2 [P2 H2]].
      assert (C : prefix q1 q2 \/ prefix q2 q1).
      { destruct Hp as [Hp|Hp].
        - eapply prefix_comparable; [eapply prefix_trans; eauto | exact P2].
        - eapply prefix_comparable; [exact P1 | eapply prefix_trans; eauto]. }
      destruct C as [C|C].
      + left. eapply sub_prefix_holds; eauto.
      + right. eapply sub_prefix_holds; eauto.
  Qed.

  (* T3 *)
  Lemma different_globals_never_alias g1 g2 : g1 <> g2 -> ~ may_alias (RNamed g1) (RNamed g2).
  Proof. intros H A. simpl in A. congruence. Qed.

  (* T4: each way of may-aliasing is realised *)
  Lemma same_global_overlaps g :
    exists a1 a2, overlap a1 a2 /\ sound_rec a1 (RNamed g) /\ sound_rec a2 (RNamed g).
  Proof.
    exists (g, []), (g, []). repeat split; simpl; try reflexivity.
    left. exists []. reflexivity.
  Qed.

  Lemma named_typed_overlaps g t :
    holds (obj_ty g) t ->
    exists a1 a2, overlap a1 a2 /\ sound_rec a1 (RNamed g) /\ sound_rec a2 (RTyped t).
  Proof.
    intros [p Hp]. exists (g, []), (g, p). repeat split; simpl; try reflexivity.
    - left. exists p. reflexivity.
    - exists p. split; [exists []; rewrite app_nil_r; reflexivity | exact Hp].
  Qed.

  Lemma typed_typed_overlaps o t2 :
    holds (obj_ty o) t2 ->
    exists a1 a2, overlap a1 a2 /\ sound_rec a1 (RTyped (obj_ty o)) /\ sound_rec a2 (RTyped t2).
  Proof.
    intros [p Hp]. exists (o, []), (o, p). repeat split; simpl; try reflexivity.
    - left. exists p. reflexivity.
    - exists []. split; [exists []; reflexivity | reflexivity].
    - exists p. split; [exists []; rewrite app_nil_r; reflexivity | exact Hp].
  Qed.
End mem.

(* T3, concrete: unrelated scalar types do not hold one another. *)
Example unrelated_types_do_not_alias :
  ~ may_alias (fun _ => TScalar 0) (RTyped (TScalar 1)) (RTyped (TScalar 2)).
Proof.
  intros [[p H]|[p H]]; destruct p; simpl in H; discriminate.
Qed.

Example global_does_not_alias_foreign_type :
  ~ may_alias (fun _ => TStruct [TScalar 0; TScalar 0]) (RNamed 7) (RTyped (TScalar 1)).
Proof.
  intros [p H]. destruct p as [|i p]; simpl in H; [discriminate|].
  destruct i as [|[|i]]; simpl in H.
  - destruct p; simpl in H; discriminate.
  - destruct p; simpl in H; discriminate.
  - destruct i; simpl in H; discriminate.
Qed.

(* ================================================================
   What the compiler trusts about a LOCAL reference: it holds one of the
   values assigned to it — IF every write of it is one of the listed
   assignments (its address is never taken, and the name is one variable).
   ================================================================ *)
Section local.
  Variable value : Type.
  Variable listed : value -> Prop.      (* the values the function assigns to it *)

  Inductive wr := WListed (v : value) | WForeign (v : value).

  Definition run (s : value) (w : wr) : value :=
    match w with WListed v => v | WForeign v => v end.

  Definition only_listed (w : wr) : Prop :=
    match w with WListed v => listed v | WForeign _ => False end.

  (* T1 *)
  Theorem local_value_listed init ws :
    listed init -> Forall only_listed ws -> listed (fold_left run ws init).
  Proof.
    intros Hi H. revert init Hi. induction H as [|w ws Hw _ IH]; intros init Hi; simpl.
    - exact Hi.
    - apply IH. destruct w; simpl in *; [exact Hw | contradiction].
  Qed.
End local.

(* T4: one foreign write — through the address, or to another variable that
   shares the name — and the value is not among the listed ones. *)
Example foreign_write_escapes :
  exists ws, ~ (fun v : nat => v = 0) (fold_left (run nat) ws 0).
Proof. exists [WForeign nat 9]. simpl. discriminate. Qed.

(* ================================================================
   A FRESH object that never LEAVES its name.  Which thread can reach which
   object is the state; allocating gives the allocator a reference nobody
   else has, using needs one, PUBLISHING — putting the reference anywhere
   another thread can take it from: a global, memory behind a reference, a
   call or spawn argument — gives everyone one.
   ================================================================ *)
Section fresh.
  Variable thread loc : Type.

  Definition reach := thread -> loc -> Prop.

  Inductive ev :=
  | EAlloc (t : thread) (l : loc)
  | EUse (t : thread) (l : loc)
  | EPublish (t : thread) (l : loc).

  Inductive step : reach -> ev -> reach -> Prop :=
  | SAlloc r t l : (forall u, ~ r u l) ->
      step r (EAlloc t l) (fun u x => r u x \/ (u = t /\ x = l))
  | SUse r t l : r t l -> step r (EUse t l) r
  | SPublish r t l : r t l ->
      step r (EPublish t l) (fun u x => r u x \/ x = l).

  Inductive exec : reach -> list ev -> reach -> Prop :=
  | XNil r : exec r [] r
  | XCons r e r1 es r2 : step r e r1 -> exec r1 es r2 -> exec r (e :: es) r2.

  Definition publishes (l : loc) (e : ev) : Prop :=
    match e with EPublish _ x => x = l | _ => False end.

  Definition own (r : reach) (t : thread) (l : loc) : Prop :=
    forall u, r u l -> u = t.

  Definition used_by (t : thread) (l : loc) (e : ev) : Prop :=
    match e with EUse u x => x = l -> u = t | _ => True end.

  (* T1: never published, never shared — every use of it is its owner's. *)
  Theorem fresh_stays_own r es r' t l :
    exec r es r' -> r t l -> own r t l ->
    Forall (fun e => ~ publishes l e) es ->
    (r' t l /\ own r' t l) /\ Forall (used_by t l) es.
  Proof.
    intros H. induction H as [r | r e r1 es r2 Hs _ IH]; intros Ht Ho Hp.
    - split; [split; assumption | constructor].
    - inversion Hp as [| e0 es0 Hpe Hps]; subst.
      assert (Hinv : r1 t l /\ own r1 t l /\ used_by t l e).
      { inversion Hs as [r0 t0 l0 Hf | r0 t0 l0 Hr | r0 t0 l0 Hr]; subst; clear Hs.
        - split; [left; exact Ht|]. split; [|exact I].
          intros u Hu. destruct Hu as [Hu | [_ Hl]]; [apply Ho; exact Hu|].
          subst l0. exfalso. exact (Hf t Ht).
        - split; [exact Ht|]. split; [exact Ho|].
          simpl. intros Hl. subst l0. apply Ho. exact Hr.
        - split; [left; exact Ht|]. split; [|exact I].
          intros u Hu. destruct Hu as [Hu | Hl]; [apply Ho; exact Hu|].
          exfalso. apply Hpe. simpl. symmetry. exact Hl. }
      destruct Hinv as (Ht1 & Ho1 & Hu).
      destruct (IH Ht1 Ho1 Hps) as [Hfin Huses].
      split; [exact Hfin | constructor; assumption].
  Qed.

  (* T4: one publication, and any thread at all may use it. *)
  Theorem published_is_shared r t u l :
    r t l -> exists r', exec r [EPublish t l; EUse u l] r'.
  Proof.
    intros Ht. eexists.
    eapply XCons; [apply SPublish; exact Ht|].
    eapply XCons; [apply SUse; right; reflexivity | apply XNil].
  Qed.
End fresh.

(* T3: two threads, an allocation each, used freely — and neither object is
   ever reached by the other thread. *)
Example two_owners_never_meet :
  exists r',
    exec bool nat (fun _ _ => False)
         [EAlloc bool nat true 0; EAlloc bool nat false 1;
          EUse bool nat true 0; EUse bool nat false 1] r'
    /\ own bool nat r' true 0 /\ own bool nat r' false 1.
Proof.
  eexists. split.
  - eapply XCons. { apply SAlloc. intros u H. exact H. }
    eapply XCons. { apply SAlloc. intros u [H | [_ H]]; [exact H | discriminate]. }
    eapply XCons. { apply SUse. left. right. split; reflexivity. }
    eapply XCons. { apply SUse. right. split; reflexivity. }
    apply XNil.
  - split; intros u H.
    + destruct H as [[[] | [H _]] | [_ H]]; [exact H | discriminate].
    + destruct H as [[[] | [_ H]] | [H _]]; [discriminate | exact H].
Qed.

(* ================================================================
   What each decision function MEANS (the functions themselves are in
   concurrency_decisions.v; the C is proved equal to them by VST).
   ================================================================ *)
Local Open Scope Z_scope.

(* may_alias, over what the checker computes: `same_global` is set only for two
   named records of one global; for a named and a typed record `a_holds_b` is
   "the global's type holds the type". *)
Lemma access_may_alias_meaning sg bn ab ba :
  zer_access_may_alias_coq sg bn ab ba = 1 <->
  sg <> 0 \/ (bn = 0 /\ (ab <> 0 \/ ba <> 0)).
Proof.
  unfold zer_access_may_alias_coq.
  repeat destruct (Z.eq_dec _ _); split; intros H; try lia.
Qed.

Lemma two_named_alias_only_if_same bn ab ba :
  bn <> 0 -> zer_access_may_alias_coq 0 bn ab ba = 0.
Proof.
  intros H. unfold zer_access_may_alias_coq. repeat destruct (Z.eq_dec _ _); lia.
Qed.

(* local_value_listed's three hypotheses, and no fewer. *)
Lemma local_ref_known_meaning bo at_ vk :
  zer_local_ref_known_coq bo at_ vk = 1 <-> bo <> 0 /\ at_ = 0 /\ vk <> 0.
Proof.
  unfold zer_local_ref_known_coq.
  repeat destruct (Z.eq_dec _ _); split; intros H; try lia.
Qed.

Lemma addr_taken_never_known bo vk :
  zer_local_ref_known_coq bo 1 vk = 0.
Proof. unfold zer_local_ref_known_coq. repeat destruct (Z.eq_dec _ _); lia. Qed.

(* A global reference is its initializer only if nothing ever re-aims it. *)
Lemma global_ref_known_meaning ic nm :
  zer_global_ref_known_coq ic nm = 1 <-> ic <> 0 \/ nm <> 0.
Proof.
  unfold zer_global_ref_known_coq.
  repeat destruct (Z.eq_dec _ _); split; intros H; try lia.
Qed.

(* fresh_stays_own's hypothesis, and published_is_shared's: a fresh object is
   the thread's own only while its reference never leaves the name. *)
Lemma fresh_ref_own_meaning f lv :
  zer_fresh_ref_own_coq f lv = 1 <-> f <> 0 /\ lv = 0.
Proof.
  unfold zer_fresh_ref_own_coq.
  repeat destruct (Z.eq_dec _ _); split; intros H; try lia.
Qed.

Lemma leaving_never_own f : zer_fresh_ref_own_coq f 1 = 0.
Proof. unfold zer_fresh_ref_own_coq. repeat destruct (Z.eq_dec _ _); lia. Qed.
