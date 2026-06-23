(* ================================================================
   λZER-Escape : PARAM(n) — the relational region that closes the
   call-result over-rejection (theorem-first grounding for the
   Stage-2 escape-analysis refactor).

   BACKGROUND.  The 3-region oracle (syntax.v / semantics.v /
   iris_escape_specs.v) proves OPERATIONALLY that only a RegStatic
   pointer may escape (store-to-global / return); RegLocal and RegArena
   leave the machine stuck.  That oracle is SOUND.  The imprecision is
   in the CALL-SITE analysis that ASSIGNS a region to `f(a1..ak)`:
   checker.c:9937 does

        if (call_has_local_derived_arg(call)) result := is_local_derived

   i.e. the result is tarred LOCAL whenever ANY argument is local-derived,
   REGARDLESS of whether `f` actually returns that argument.  That
   gratuitously over-rejects two everyday shapes:

     - unrelated-static:  lookup(local_key)  where  lookup returns g_table
     - pick-one-of-many:  longest(local, global)  returning the 2nd arg

   THIS FILE formalises the fix: a per-function RETURN SUMMARY over an
   extended abstract region with a RELATIONAL constructor `ARParam n`,
   resolved at the call site against the actual argument regions.  It
   proves the substitution is SOUND and STRICTLY MORE PRECISE than the
   old approximation.  These are exactly the finite states + transfer
   function the implementation must realise.

   Self-contained: `region` and `can_escape` are redefined here to match
   syntax.v (RegLocal/RegArena/RegStatic) and the operational oracle
   (only RegStatic escapes, iris_escape_specs.v).  No admits.
   ================================================================ *)
From stdpp Require Import base list.

(* ---- the concrete region (identical to syntax.v) ---- *)
Inductive region : Type :=
  | RegLocal  : region
  | RegArena  : region
  | RegStatic : region.

(* The escape oracle, proven operationally in iris_escape_specs.v:
   only a RegStatic pointer may be stored-to-global or returned. *)
Definition can_escape (r : region) : bool :=
  match r with RegStatic => true | _ => false end.

(* Escapability order.  RegStatic is BOTTOM (most permissive).  A SOUND
   analysis must over-approximate UPWARD (towards "less escapable"): the
   TRUE region of a value must sit below the analysis region. *)
Definition reg_le (rtrue ranalysis : region) : Prop :=
  rtrue = ranalysis \/ rtrue = RegStatic.

Lemma reg_le_refl : forall r, reg_le r r.
Proof. intro r. left. reflexivity. Qed.

(* can_escape is anti-monotone along reg_le: if the ANALYSIS region is
   escapable and the TRUE region sits below it, the true region escapes
   too — so trusting the analysis can never permit an unsafe escape. *)
Lemma can_escape_sound : forall rtrue ranalysis,
  reg_le rtrue ranalysis ->
  can_escape ranalysis = true ->
  can_escape rtrue = true.
Proof.
  intros rtrue ranalysis Hle Hesc.
  unfold reg_le in Hle. destruct Hle as [Heq | Hst].
  - subst ranalysis. exact Hesc.
  - subst rtrue. reflexivity.
Qed.

(* ---- the extended abstract region: a per-function RETURN summary ---- *)
Inductive aregion : Type :=
  | ARStatic : aregion
  | ARLocal  : aregion
  | ARArena  : aregion
  | ARParam  : nat -> aregion.   (* RELATIONAL: "aliases parameter n" *)

(* resolve a summary against the actual argument regions at a call site. *)
Definition resolve (a : aregion) (argreg : nat -> region) : region :=
  match a with
  | ARStatic  => RegStatic
  | ARLocal   => RegLocal
  | ARArena   => RegArena
  | ARParam n => argreg n
  end.

(* the OLD call-site approximation (checker.c:9937): result is RegLocal
   if ANY argument is local; else RegStatic. *)
Fixpoint any_local (l : list region) : bool :=
  match l with
  | nil => false
  | RegLocal :: _ => true
  | _ :: t => any_local t
  end.
Definition old_approx (args : list region) : region :=
  if any_local args then RegLocal else RegStatic.

(* ================================================================
   (T1) SUBSTITUTION SOUNDNESS — the call transfer.
   A summary Rf is SOUND for a call when the true result region sits
   below `resolve Rf argreg`.  Then deciding escape by `can_escape` on the
   resolved region is sound: it permits escape only when the true region
   is RegStatic.  (Stage-2's body analysis discharges the soundness
   hypothesis; this is the transfer it must meet.) *)
Theorem subst_escape_sound :
  forall (Rf : aregion) (argreg : nat -> region) (rtrue : region),
    reg_le rtrue (resolve Rf argreg) ->
    can_escape (resolve Rf argreg) = true ->
    can_escape rtrue = true.
Proof.
  intros Rf argreg rtrue Hsound Hesc.
  apply (can_escape_sound rtrue (resolve Rf argreg)); assumption.
Qed.

(* ================================================================
   (T2) RELATIONAL PRECISION — ARParam n resolves to EXACTLY argument n's
   region, never the coarse "any local arg". *)
Theorem param_resolve_exact :
  forall n argreg, resolve (ARParam n) argreg = argreg n.
Proof. reflexivity. Qed.

(* a pick-one function escapes iff the CHOSEN argument is static — not iff
   "no argument is local".  This is the multi-param precision. *)
Theorem pick_escapes_iff_chosen_static :
  forall (chosen : nat) argreg,
    can_escape (resolve (ARParam chosen) argreg) = can_escape (argreg chosen).
Proof. reflexivity. Qed.

(* ================================================================
   (T3) THE OVER-REJECTION IS GRATUITOUS — unrelated-static, as one fact.
   For a function whose return summary is ARStatic (returns a global,
   aliases no parameter), on a call that contains a local argument:
     - the OLD approximation REJECTS the escape (this IS the over-rejection)
     - the NEW analysis ALLOWS it, and
     - the NEW analysis is SOUND on it (true region RegStatic sits below).   *)
Theorem precision_gain_unrelated_static :
  forall args argreg,
    any_local args = true ->
    can_escape (old_approx args) = false /\
    can_escape (resolve ARStatic argreg) = true /\
    reg_le RegStatic (resolve ARStatic argreg).
Proof.
  intros args argreg Hlocal.
  split; [| split].
  - unfold old_approx. rewrite Hlocal. reflexivity.
  - reflexivity.
  - unfold reg_le. right. reflexivity.
Qed.

(* ================================================================
   (T4) NO UNDER-REJECTION — the new analysis never permits an UNSAFE
   escape.  If the true result region is RegLocal (a genuine dangling
   risk) then no sound summary can make can_escape say "yes": whenever the
   analysis permits escape, the true region was RegStatic. *)
Theorem new_never_underrejects :
  forall (Rf : aregion) (argreg : nat -> region) (rtrue : region),
    reg_le rtrue (resolve Rf argreg) ->
    rtrue = RegLocal ->
    can_escape (resolve Rf argreg) = false.
Proof.
  intros Rf argreg rtrue Hsound Hlocal.
  destruct (can_escape (resolve Rf argreg)) eqn:E.
  - (* if it claimed escapable, soundness forces rtrue static — contradiction *)
    pose proof (can_escape_sound _ _ Hsound E) as Htrue.
    rewrite Hlocal in Htrue. simpl in Htrue. discriminate.
  - reflexivity.
Qed.

(* ================================================================
   (T5) FIELD / INDEX PROJECTION INHERITS THE BASE REGION — the BUG-737
   field-launder soundness (the P9 hole, 2026-06-24).

   Projecting a pointer FIELD (or element) out of a value — `base.field`,
   `base[i]` — does NOT make it more escapable than `base`: the projected
   pointer has the same (caller-unknown) provenance as the value it came out
   of. So the C classification of `g = param.field` MUST descend the
   projection to the ROOT and use the root's region (here: ARParam n / the
   non-keep taint), NOT treat the field as a fresh escapable value. *)
Definition aregion_of_projection (a : aregion) : aregion := a.

Theorem projection_preserves_escape : forall a argreg,
  can_escape (resolve (aregion_of_projection a) argreg)
    = can_escape (resolve a argreg).
Proof. reflexivity. Qed.

(* the UNSOUND classification P9 actually performed: a pointer field of a
   by-value struct PARAM was never tainted (the non-keep sink only matched a
   whole-struct ident, not `param.field`), so storing it to a global was
   treated as storing a STATIC (escapable-safe) value. This witness — a field
   of a param bound to a LOCAL is "escapable" under the bug, "not escapable"
   under the inheriting rule — is why the C must descend the projection.
   (Mirrors capture_lattice.v's buggy_reset_unsound for the if-unwrap case.) *)
Definition aregion_of_projection_BUGGY (a : aregion) : aregion := ARStatic.

Theorem buggy_projection_unsound : forall n,
  can_escape (resolve (aregion_of_projection_BUGGY (ARParam n)) (fun _ => RegLocal)) = true
  /\ can_escape (resolve (ARParam n) (fun _ => RegLocal)) = false.
Proof. intro n. split; reflexivity. Qed.
