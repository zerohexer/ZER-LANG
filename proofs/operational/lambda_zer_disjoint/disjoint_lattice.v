(* ================================================================
   lambda_zer_disjoint : the ORACLE for proven-disjointness aliased mutation —
   the one axis where ZER can EXCEED Rust (accept two-mutable-paths code Rust
   rejects by the aliasing-XOR-mutability RULE, when disjointness is PROVEN).

   THE FINITE STATE: an interior pointer descriptor (base allocation id + offset
   interval). The decision `disjoint p q` is a constant-time non-overlap test.

   THE NON-NEGOTIABLE SOUNDNESS SHAPE (the BH-18 #1 structural defense):
   disjointness is ADDITIVE — it may only RELAX the (nonexistent in ZER)
   aliasing-XOR-mutability rejection; it must be architecturally INCAPABLE of
   suppressing the per-pointer UAF/OOB checks. The theorems pin: (D1) a DISJOINT
   verdict implies the concrete address sets do not overlap (sound to mutate
   both); (D2) disjointness can NEVER suppress a UAF/OOB rejection; (D3) the
   EXCEEDS-Rust acceptance — both-safe + proven-disjoint => permitted. Zero
   admits.

   NOTE: this oracle's eventual IMPLEMENTATION needs a relational VRP layer (a
   re-architecture); the theorem here is pure and front-loads the spec.
   ================================================================ *)
From Coq Require Import ZArith Lia Bool.
Open Scope Z_scope.

(* an interior pointer: a base allocation id + an offset interval [lo,hi]. *)
Record iptr := { base : Z; off_lo : Z; off_hi : Z }.

(* CONCRETE overlap: same base AND offset-intervals share a point. *)
Definition overlaps (p q : iptr) : Prop :=
  base p = base q /\ exists o, (off_lo p <= o <= off_hi p) /\ (off_lo q <= o <= off_hi q).

(* THE DECISION: provably disjoint — different base, or non-overlapping offsets. *)
Definition disjoint (p q : iptr) : bool :=
  negb (base p =? base q) || (off_hi p <? off_lo q) || (off_hi q <? off_lo p).

(* ================================================================
   (D1) DECISION SOUNDNESS — a DISJOINT verdict implies the concrete address
   sets do NOT overlap, so simultaneously mutating p and q is sound (no write
   through p is observable through q). *)
Theorem disjoint_no_overlap : forall p q,
  disjoint p q = true -> ~ overlaps p q.
Proof.
  intros p q Hd [Hbase [o [Hp Hq]]].
  unfold disjoint in Hd.
  apply orb_true_iff in Hd as [Hd1 | Hlo].
  - apply orb_true_iff in Hd1 as [Hb | Hphi].
    + rewrite Hbase in Hb. rewrite Z.eqb_refl in Hb. simpl in Hb. discriminate.
    + apply Z.ltb_lt in Hphi. lia.
  - apply Z.ltb_lt in Hlo. lia.
Qed.

(* ---- the per-pointer safety verdict (UAF + OOB), INDEPENDENT of disjointness ---- *)
Definition access_safe (uaf oob : bool) : bool := negb uaf && negb oob.

(* an aliased mutation of p and q is permitted iff BOTH per-pointer accesses are
   safe AND the pointers are disjoint. Disjointness is ANDed in — it can only
   subtract from acceptance, never override the per-pointer safety. *)
Definition aliased_mut_ok
    (uaf_p oob_p uaf_q oob_q : bool) (p q : iptr) : bool :=
  access_safe uaf_p oob_p && access_safe uaf_q oob_q && disjoint p q.

(* ================================================================
   (D2) NO UNDER-REJECTION — disjointness can NEVER suppress a UAF/OOB
   rejection. If either pointer is unsafe (UAF or OOB), the aliased mutation is
   rejected REGARDLESS of the disjoint verdict. This is the structural defense
   against the BH-18 #1 class (a disjointness fact silently un-coupling a freed
   alias). *)
Theorem disjoint_cannot_suppress_unsafe :
  forall uaf_p oob_p uaf_q oob_q p q,
  (uaf_p = true \/ oob_p = true \/ uaf_q = true \/ oob_q = true) ->
  aliased_mut_ok uaf_p oob_p uaf_q oob_q p q = false.
Proof.
  intros uaf_p oob_p uaf_q oob_q p q H.
  unfold aliased_mut_ok, access_safe.
  destruct uaf_p, oob_p, uaf_q, oob_q; simpl;
    try reflexivity;
    exfalso; destruct H as [H | [H | [H | H]]]; discriminate.
Qed.

(* ================================================================
   (D3) THE EXCEEDS-RUST ACCEPTANCE — when both accesses are safe AND the
   pointers are PROVEN disjoint, the aliased mutation is PERMITTED: the pattern
   Rust rejects by the aliasing-XOR-mutability rule, accepted here with zero
   annotations and zero library blessing, because disjointness is proven. *)
Theorem aliased_mut_permitted_when_disjoint : forall p q,
  disjoint p q = true ->
  aliased_mut_ok false false false false p q = true.
Proof.
  intros p q Hd. unfold aliased_mut_ok, access_safe. simpl. exact Hd.
Qed.
