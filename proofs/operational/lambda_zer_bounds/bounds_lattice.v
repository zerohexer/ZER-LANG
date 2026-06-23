(* ================================================================
   lambda_zer_bounds : the OPERATIONAL ORACLE for array/slice bounds safety.

   ZER had NO oracle for the bounds/VRP class — its abstract state set was
   discovered by red team, which is exactly why BH-18 #2 (a branch-local VRP
   narrowing leaking past a control-flow join -> ASan-confirmed OOB write)
   could exist: there was no certified finite-state domain + sound decision to
   write the C against. This file certifies that domain and decision, in the
   self-contained param_lattice.v style (zero admits).

   THE FINITE STATE SET (the abstract domain of the bounds class): an index is
   abstracted to a `range` — either UNKNOWN (TOP, constrains nothing) or a
   bounded interval [lo,hi]. That two-shape set IS the finite-variable answer
   for this class; the C `find_var_range` / vrp_ir.c IRRange must map every
   index expression onto exactly this.

   THE SOUND DECISION: elide the runtime bounds check IFF the index is provably
   in [0,n). Theorems below pin: (T1) eliding never under-rejects a real OOB;
   (T2) the control-flow MERGE must be the JOIN (over-approximate) so a
   branch-local narrowing cannot license elision on a path it doesn't hold —
   the precise rule BH-18 #2 violated; (T3) a real precision win (constant
   index); (T4) the conservative TOP default keeps the runtime check.
   ================================================================ *)
From Coq Require Import Bool ZArith Lia.
Open Scope Z_scope.

(* ---- CONCRETE: an access is (index i, length n). The TRUE unsafety. ---- *)
Definition oob (i n : Z) : bool := negb ((0 <=? i) && (i <? n)).

(* ---- ABSTRACT DOMAIN (the finite state set for the bounds class) ----
   A value's index abstraction: a validity flag + an interval [lo,hi].
   rvalid=false is TOP (unknown) — the safe over-approximation that
   constrains nothing. *)
Record range := { rlo : Z; rhi : Z; rvalid : bool }.
Definition top_range : range := {| rlo := 0; rhi := 0; rvalid := false |}.

(* abstraction soundness: the concrete index i is COVERED by range r.
   An invalid (TOP) range covers EVERY i (no constraint) — sound. *)
Definition in_range (r : range) (i : Z) : Prop :=
  rvalid r = true -> rlo r <= i <= rhi r.

(* ---- THE DECISION: elide the runtime bounds check iff provably in-bounds. ---- *)
Definition elide_bounds_check (r : range) (n : Z) : bool :=
  rvalid r && (0 <=? rlo r) && (rhi r <? n).

(* ================================================================
   (T1) DECISION SOUNDNESS — NO UNDER-REJECTION. If the analysis elides the
   check, then EVERY concrete index the range soundly covers is in-bounds, so a
   real OOB is never silently elided. (The bounds analog of
   param_lattice.v's new_never_underrejects.) *)
Theorem elide_sound : forall r n i,
  elide_bounds_check r n = true ->
  in_range r i ->
  oob i n = false.
Proof.
  intros r n i Hel Hin. unfold elide_bounds_check in Hel.
  destruct (rvalid r) eqn:Hv; try (simpl in Hel; discriminate).
  destruct (0 <=? rlo r) eqn:Hlo; try (simpl in Hel; discriminate).
  destruct (rhi r <? n) eqn:Hhi; try (simpl in Hel; discriminate).
  unfold in_range in Hin. specialize (Hin Hv).
  unfold oob.
  assert (Hb: (0 <=? i) && (i <? n) = true).
  { apply andb_true_iff. split.
    - apply Z.leb_le in Hlo. apply Z.leb_le. lia.
    - apply Z.ltb_lt in Hhi. apply Z.ltb_lt. lia. }
  rewrite Hb. reflexivity.
Qed.

(* ================================================================
   (T2) MERGE = JOIN (the BH-18 #2 crux). At a control-flow join the merged
   range must CONTAIN both predecessors. Then eliding based on the merged range
   is sound on ALL incoming paths; eliding based on a single predecessor's
   narrower range at a merge (what the flat AST VRP did) is NOT licensed. *)
Definition range_join (a b : range) : range :=
  if rvalid a then
    if rvalid b
    then {| rlo := Z.min (rlo a) (rlo b);
            rhi := Z.max (rhi a) (rhi b); rvalid := true |}
    else top_range
  else top_range.

Theorem join_covers_left : forall a b i,
  in_range a i -> in_range (range_join a b) i.
Proof.
  intros a b i Ha. unfold range_join.
  destruct (rvalid a) eqn:Hva.
  - destruct (rvalid b) eqn:Hvb.
    + unfold in_range in *. simpl. intros _. specialize (Ha Hva).
      pose proof (Z.le_min_l (rlo a) (rlo b)).
      pose proof (Z.le_max_l (rhi a) (rhi b)). lia.
    + unfold in_range, top_range. simpl. intros H; discriminate.
  - unfold in_range, top_range. simpl. intros H; discriminate.
Qed.

Theorem join_covers_right : forall a b i,
  in_range b i -> in_range (range_join a b) i.
Proof.
  intros a b i Hb. unfold range_join.
  destruct (rvalid a) eqn:Hva.
  - destruct (rvalid b) eqn:Hvb.
    + unfold in_range in *. simpl. intros _. specialize (Hb Hvb).
      pose proof (Z.le_min_r (rlo a) (rlo b)).
      pose proof (Z.le_max_r (rhi a) (rhi b)). lia.
    + unfold in_range, top_range. simpl. intros H; discriminate.
  - unfold in_range, top_range. simpl. intros H; discriminate.
Qed.

(* COROLLARY — eliding on the JOIN is sound regardless of which predecessor the
   concrete execution came from. This is the exact property the flat AST VRP
   lacked (it reused a then-branch narrowing past the merge). *)
Corollary elide_on_join_sound : forall a b n i,
  elide_bounds_check (range_join a b) n = true ->
  (in_range a i \/ in_range b i) ->
  oob i n = false.
Proof.
  intros a b n i Hel [Ha | Hb].
  - apply (elide_sound (range_join a b) n i Hel (join_covers_left a b i Ha)).
  - apply (elide_sound (range_join a b) n i Hel (join_covers_right a b i Hb)).
Qed.

(* ================================================================
   (T3) PRECISION WITNESS — a real over-rejection the runtime-check default
   incurs and the analysis removes soundly: a constant in-bounds index. *)
Theorem precision_const_index : forall k n,
  0 <= k -> k < n ->
  elide_bounds_check {| rlo := k; rhi := k; rvalid := true |} n = true.
Proof.
  intros k n H0 Hn. unfold elide_bounds_check. simpl.
  rewrite (proj2 (Z.leb_le 0 k)) by lia.
  rewrite (proj2 (Z.ltb_lt k n)) by lia. reflexivity.
Qed.

(* ================================================================
   (T4) CONSERVATIVE DEFAULT — an UNKNOWN (TOP) range never elides, so the
   runtime check always stays for an unanalyzable index: no under-rejection
   from imprecision. *)
Theorem top_never_elides : forall n, elide_bounds_check top_range n = false.
Proof. intros n. unfold elide_bounds_check, top_range. reflexivity. Qed.
