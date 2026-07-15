(* ================================================================
   λZER-Escape : ERASED-REF OWNERSHIP — the origin lattice that closes
   the `*opaque` generic-container over-rejection (PART 6 of
   docs/universal_pointer.md; the "safe void*" unification).

   BACKGROUND.  ZER's `*opaque` is a TRACKED type: zercheck_ir.c (4308 /
   4352) registers ANY pointer-returning call result as an OWNED
   allocation, keyed purely on the RETURN TYPE ("Applies to both extern
   (bodyless) and bodied functions").  That is SOUND for ownership (it
   never drops a real owner) but OVER-REJECTS the GLib-`GHashTable`
   pattern: `?*opaque map_get(m, ...)` on a `*Map m` returns a BORROW (a
   value the caller put into `m`, e.g. an opaque cast of `&global`), yet is
   treated as a
   fresh allocation → false "ghost handle / never freed" leak.

   The fix (Path A, chosen 2026-07-16): derive ownership from the
   result's ORIGIN (provenance), not from the type — exactly how `*T` /
   `[*]T` already derive ownership from `alloc_id`.  This file formalises
   the origin lattice + the CALL transfer, and — crucially — pins the two
   accept-unsafe TRAPS found in the first implementation attempt
   (universal_pointer.md §36.15) as UNSOUNDNESS witnesses, so the C can
   never take those shortcuts.

     Trap 1 (conservative default): an origin the analysis cannot classify
       — e.g. extern `malloc`, universal `alloc()` not modelled by
       `FuncProps.can_alloc` — MUST widen to OWNED, never BORROW.
     Trap 2 (launder through a param): `f(m){ w = alloc; m.f = w;
       return m.f }` — the return traces to a param but IS a fresh alloc
       stashed in it.  "return-traces-to-param ⇒ borrow" is UNSOUND; the
       arg's ACTUAL origin must flow (AOParam n resolves to argorigin n).

   These are exactly the finite states + transfer the implementation must
   realise.  Self-contained; no admits.  Mirrors param_lattice.v.
   ================================================================ *)
From stdpp Require Import base list.

(* ---- the concrete ORIGIN of a call-result pointer ---- *)
Inductive origin : Type :=
  | OFresh  : origin    (* from alloc()/malloc/pool.alloc — an OWNED allocation *)
  | OBorrow : origin.   (* from &global/&param/a stored borrow — NOT owned *)

(* The ownership DECISION, proven operationally elsewhere: the caller must
   leak/UAF-track a result iff it could be a fresh allocation.  Only an
   OFresh value carries an ownership obligation. *)
Definition must_track_owned (o : origin) : bool :=
  match o with OFresh => true | OBorrow => false end.

(* The RISKY decision is "do NOT track" (= treat as a borrow).  If that is
   wrong (the value was actually OFresh) the caller drops the ownership
   obligation → a MISSED leak / use-after-free (accept-unsafe). *)
Definition treat_as_borrow (o : origin) : bool :=
  match o with OBorrow => true | OFresh => false end.

(* Ownership order.  OBorrow is BOTTOM (the permissive "don't track").  A
   SOUND analysis must over-approximate UPWARD (towards OWNED): the TRUE
   origin of a value must sit below the analysis origin, so trusting the
   analysis can never drop a real owner. *)
Definition origin_le (otrue oanalysis : origin) : Prop :=
  otrue = oanalysis \/ otrue = OBorrow.

Lemma origin_le_refl : forall o, origin_le o o.
Proof. intro o. left. reflexivity. Qed.

(* treat_as_borrow is anti-monotone along origin_le: if the ANALYSIS says
   borrow and the true origin sits below it, the true origin is a borrow
   too — so NOT tracking is safe (no owned allocation was dropped). *)
Lemma treat_as_borrow_sound : forall otrue oanalysis,
  origin_le otrue oanalysis ->
  treat_as_borrow oanalysis = true ->
  treat_as_borrow otrue = true.
Proof.
  intros otrue oanalysis Hle Hb.
  unfold origin_le in Hle. destruct Hle as [Heq | Hbo].
  - subst oanalysis. exact Hb.
  - subst otrue. reflexivity.
Qed.

(* ---- the abstract ORIGIN: a per-function RETURN summary ---- *)
Inductive aorigin : Type :=
  | AOFresh  : aorigin              (* returns a fresh allocation *)
  | AOBorrow : aorigin              (* returns a global / static borrow *)
  | AOParam  : nat -> aorigin.      (* RELATIONAL: result's origin = arg n's
                                       origin (pass-through, or stored-then-
                                       returned via a shared container param) *)

(* resolve a summary against the actual argument origins at a call site. *)
Definition resolve (a : aorigin) (argorigin : nat -> origin) : origin :=
  match a with
  | AOFresh   => OFresh
  | AOBorrow  => OBorrow
  | AOParam n => argorigin n
  end.

(* the OLD approximation (zercheck_ir.c 4308/4352): ANY pointer-returning
   call is registered as a fresh allocation, regardless of what it returns. *)
Definition old_track (_ : list origin) : bool := true.

(* ================================================================
   (T1) CALL-TRANSFER SOUNDNESS.  A summary Rf is SOUND for a call when the
   true result origin sits below `resolve Rf argorigin`.  Then deciding "not
   owned" by treat_as_borrow on the resolved origin is sound: it drops the
   ownership obligation only when the true origin is OBorrow.  (The body
   analysis discharges the soundness hypothesis; this is the transfer it
   must meet.) *)
Theorem subst_track_sound :
  forall (Rf : aorigin) (argorigin : nat -> origin) (otrue : origin),
    origin_le otrue (resolve Rf argorigin) ->
    treat_as_borrow (resolve Rf argorigin) = true ->
    treat_as_borrow otrue = true.
Proof.
  intros Rf argorigin otrue Hsound Hb.
  apply (treat_as_borrow_sound otrue (resolve Rf argorigin)); assumption.
Qed.

(* ================================================================
   (T2) RELATIONAL PRECISION — AOParam n resolves to EXACTLY argument n's
   origin, never a coarse "any pointer return is owned". *)
Theorem param_resolve_exact :
  forall n argorigin, resolve (AOParam n) argorigin = argorigin n.
Proof. reflexivity. Qed.

(* ================================================================
   (T3) THE OVER-REJECTION IS GRATUITOUS — the map_get fix, as one fact.
   For a function whose return summary is AOParam n on an argument that is a
   BORROW (the value the caller stored, e.g. an opaque cast of `&global`):
     - the OLD approximation TRACKS it as owned (the false leak — this IS the
       over-rejection that broke the GLib pattern),
     - the NEW analysis does NOT track it (treat_as_borrow = true), and
     - the NEW analysis is SOUND on it (true origin OBorrow sits below). *)
Theorem precision_gain_borrow_through_param :
  forall n (args : list origin) argorigin,
    argorigin n = OBorrow ->
    old_track args = true /\
    treat_as_borrow (resolve (AOParam n) argorigin) = true /\
    origin_le OBorrow (resolve (AOParam n) argorigin).
Proof.
  intros n args argorigin Hb.
  split; [| split].
  - reflexivity.
  - simpl. rewrite Hb. reflexivity.
  - unfold origin_le. right. reflexivity.
Qed.

(* ================================================================
   (T4) NO UNDER-TRACKING — the crown jewel.  The new analysis never DROPS a
   real ownership obligation.  If the true result origin is OFresh (a genuine
   allocation) then no sound summary can make treat_as_borrow say "yes":
   whenever the analysis drops ownership, the true origin was a borrow. *)
Theorem new_never_undertracks :
  forall (Rf : aorigin) (argorigin : nat -> origin) (otrue : origin),
    origin_le otrue (resolve Rf argorigin) ->
    otrue = OFresh ->
    treat_as_borrow (resolve Rf argorigin) = false.
Proof.
  intros Rf argorigin otrue Hsound Hfresh.
  destruct (treat_as_borrow (resolve Rf argorigin)) eqn:E.
  - pose proof (treat_as_borrow_sound _ _ Hsound E) as Ht.
    rewrite Hfresh in Ht. simpl in Ht. discriminate.
  - reflexivity.
Qed.

(* ================================================================
   (TRAP 2) THE UNSOUND SHORTCUT — "return-traces-to-param ⇒ borrow", ignoring
   the actual argument origin.  A value can be alloc'd and LAUNDERED through a
   param field: `f(m){ w = alloc; m.f = w; return m.f }`.  Here the arg's
   stored origin is OFresh, but the buggy rule declares BORROW → the caller
   drops the ownership obligation → a missed leak / UAF.  The witness: the same
   AOParam n is "borrow" under the bug but "owned" under the origin-flowing
   rule, whenever the arg is OFresh.  (Mirrors param_lattice.v's
   buggy_projection_unsound.) *)
Definition resolve_BUGGY (a : aorigin) (_ : nat -> origin) : origin :=
  match a with AOParam _ => OBorrow | AOFresh => OFresh | AOBorrow => OBorrow end.

Theorem buggy_param_undertracks : forall n,
  treat_as_borrow (resolve_BUGGY (AOParam n) (fun _ => OFresh)) = true
  /\ treat_as_borrow (resolve (AOParam n) (fun _ => OFresh)) = false.
Proof. intro n. split; reflexivity. Qed.

(* ================================================================
   (TRAP 1) CONSERVATIVE DEFAULT — MERGE = JOIN with OWNED as the default.  An
   origin the analysis cannot classify (an unrecognised allocator: extern
   `malloc`, universal `alloc()` not modelled by can_alloc, an indirect call)
   MUST widen to OFresh (track-owned), never OBorrow.  Then a missed allocator
   can only OVER-track (safe), never under-track (accept-unsafe). *)
Definition join (o1 o2 : origin) : origin :=
  match o1, o2 with
  | OBorrow, OBorrow => OBorrow
  | _, _ => OFresh
  end.

Theorem join_owned_absorbs : forall o, join OFresh o = OFresh.
Proof. intro o. destruct o; reflexivity. Qed.

Theorem unknown_defaults_owned :
  forall o, treat_as_borrow (join OFresh o) = false.
Proof. intro o. destruct o; reflexivity. Qed.

(* NOTE: the type_id RE-TYPE check (the C-cast `T`-from-erased sugar) is a
   SEPARATE axis from this ownership lattice — it constrains WHICH concrete
   type comes back
   out, not WHETHER the value is owned. It is not modelled here; see
   universal_pointer.md §36.8 (re-type = the type_id trap; ownership = this
   lattice; lifetime = the escape sinks — three orthogonal obligations). *)
