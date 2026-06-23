(* ================================================================
   lambda_zer_escape / join_lattice : ESCAPE LADDER RUNG 2 — the n-ary JOIN
   return summary. Refines param_lattice.v's flat per-param bitmask (which
   OR-accumulates and COLLAPSES a disjunctive return to UNKNOWN) into the JOIN
   (the finite SET of regions a return may be over its paths). This is the
   richer abstraction that drives down the disjunctive-return over-rejection
   WITHOUT re-architecting: the C change is `ret_param_mask : u64` ->
   a small member-set, consumed through the SAME call_result_escapes gate.

   The JOIN is sound by the SATURATE-TOWARD-LOCAL discipline: an unclassifiable
   return adds ARLocal to the set (never drops a member), and a set containing
   ARLocal can never escape — so precision is gained (the OTHER members survive
   instead of the whole summary collapsing to UNKNOWN) with ZERO loss of
   soundness. Reuses param_lattice.v's region / can_escape / reg_le / resolve /
   can_escape_sound. Zero admits.
   ================================================================ *)
From zer_escape Require Import param_lattice.
From Coq Require Import List Bool.
Import ListNotations.

(* a return summary = the JOIN (disjunction over return paths) of aregions. *)
Definition summary := list aregion.

(* the call result MAY be any member's resolved region (we don't know which path
   ran). It may escape (be stored to a global / returned) iff EVERY possible
   resolved region is escapable — a single possibly-LOCAL member forbids it. *)
Definition summary_can_escape (S : summary) (argreg : nat -> region) : bool :=
  forallb (fun a => can_escape (resolve a argreg)) S.

(* ================================================================
   (J1) SUMMARY SOUNDNESS — if the analysis permits the result to escape, then
   EVERY member's resolved region escapes; so whichever path actually ran, the
   true result region (a view of some member) is escapable. No under-rejection.
   This is the n-ary generalization of param_lattice.v's subst_escape_sound. *)
Theorem summary_escape_sound : forall S argreg a,
  summary_can_escape S argreg = true ->
  In a S ->
  can_escape (resolve a argreg) = true.
Proof.
  intros S argreg a Hall Hin.
  unfold summary_can_escape in Hall.
  rewrite forallb_forall in Hall. apply Hall. exact Hin.
Qed.

(* tied to the TRUE region via reg_le, reusing can_escape_sound: trusting the
   resolved region of any taken member is sound. *)
Theorem summary_escape_sound_true : forall S argreg a rtrue,
  summary_can_escape S argreg = true ->
  In a S ->
  reg_le rtrue (resolve a argreg) ->
  can_escape rtrue = true.
Proof.
  intros S argreg a rtrue Hall Hin Hle.
  apply (can_escape_sound rtrue (resolve a argreg)).
  - exact Hle.
  - apply (summary_escape_sound S argreg a Hall Hin).
Qed.

(* ================================================================
   (J2) SATURATE-TOWARD-LOCAL — a summary CONTAINING ARLocal can never escape,
   regardless of args. So an unclassifiable return (which adds ARLocal) blocks
   escape WITHOUT collapsing the summary: the OTHER members are retained. *)
Theorem local_member_blocks_escape : forall S argreg,
  In ARLocal S ->
  summary_can_escape S argreg = false.
Proof.
  intros S argreg Hin.
  unfold summary_can_escape. apply not_true_is_false. intro Hall.
  rewrite forallb_forall in Hall. specialize (Hall ARLocal Hin).
  simpl in Hall. discriminate.
Qed.

(* ================================================================
   (J3) PRECISION WITNESS — the disjunctive return `pick(p,c){if c return &local;
   return p}` has summary [ARLocal; ARParam 0]. The flat bitmask collapses this
   to UNKNOWN (loses the ARParam 0 relation); the JOIN RETAINS ARParam 0 AND
   still soundly blocks the escape. *)
Theorem pick_join_retains_param :
  In (ARParam 0) [ARLocal; ARParam 0]
  /\ (forall argreg, summary_can_escape [ARLocal; ARParam 0] argreg = false).
Proof.
  split.
  - simpl. right. left. reflexivity.
  - intro argreg. apply local_member_blocks_escape. simpl. left. reflexivity.
Qed.

(* ================================================================
   (J4) PRECISION GAIN over the flat mask — a function returning STATIC on one
   path and PARAM(0) on another (summary [ARStatic; ARParam 0]) escapes IFF
   arg 0 resolves static. `f(global)` -> escapes; `f(local)` -> rejected. This
   is exactly Rust's `<'a>(x:&'a)->&'a` behaviour, inferred, zero annotations. *)
Theorem static_or_param_resolves_per_arg : forall argreg,
  summary_can_escape [ARStatic; ARParam 0] argreg
    = can_escape (argreg 0).
Proof.
  intro argreg. unfold summary_can_escape. simpl.
  (* can_escape RegStatic = true; && (can_escape (argreg 0) && true) *)
  rewrite andb_true_r. reflexivity.
Qed.
