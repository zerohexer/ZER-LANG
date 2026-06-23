(* ================================================================
   lambda_zer_capture : the OPERATIONAL ORACLE for the if-unwrap capture class
   (`if (opt) |v|` / `|*v|`). No prior oracle existed — which is why BH-18 #6
   (a `|*v|` mutable capture escaping a pointer-to-local to a global) could
   exist: there was no certified rule for what REGION a captured binding has.

   THE FINITE STATE SET reuses the escape region lattice {RegLocal, RegArena,
   RegStatic} (the same domain param_lattice.v certifies). The capture class adds
   ONE transfer rule: a binding captured from a payload of region r HAS region r
   — capture INHERITS the captured value's region, it does NOT reset it.

   The theorems pin: (T1) the correct rule preserves escapability, so a capture
   of a LOCAL payload is itself non-escapable (no UAF); (T2) the BH-18 #6 reset
   (capture defaults to STATIC) is a soundness VIOLATION — witnessed, hence
   forbidden; (T3) no under-rejection: a LOCAL-payload capture never escapes.
   Self-contained, zero admits.
   ================================================================ *)

(* ---- the escape region lattice (mirrors param_lattice.v / iris_escape) ---- *)
Inductive region : Type := RegLocal | RegArena | RegStatic.

(* the escape oracle: only a RegStatic value may escape (store-to-global/return). *)
Definition can_escape (r : region) : bool :=
  match r with RegStatic => true | _ => false end.

(* ---- THE CAPTURE TRANSFER: a binding captured from a payload of region r
   inherits region r. This is the certified rule the `|v|`/`|*v|` desugaring
   in the C must implement. ---- *)
Definition region_of_capture (payload : region) : region := payload.

(* the UNSOUND rule BH-18 #6 actually implemented: the capture's escape
   provenance was never set, so it defaulted to STATIC (escapable). *)
Definition region_of_capture_BUGGY (payload : region) : region := RegStatic.

(* ================================================================
   (T1) CAPTURE PRESERVES ESCAPABILITY — a captured binding can escape iff its
   payload can. So a LOCAL payload yields a non-escapable capture. *)
Theorem capture_preserves_escape : forall payload,
  can_escape (region_of_capture payload) = can_escape payload.
Proof. intro payload. reflexivity. Qed.

(* ================================================================
   (T2) THE RESET IS UNSOUND (BH-18 #6 as a soundness violation) — the buggy
   rule claims a LOCAL-payload capture is escapable while the true region is
   non-escapable. This witness is why the oracle FORBIDS the reset: the C must
   inherit, never default to STATIC. *)
Theorem buggy_reset_unsound :
  can_escape (region_of_capture_BUGGY RegLocal) = true
  /\ can_escape RegLocal = false.
Proof. split; reflexivity. Qed.

(* ================================================================
   (T3) NO UNDER-REJECTION — under the correct (inheriting) rule, a capture of a
   LOCAL payload is never permitted to escape (the dangling-pointer store the
   BH-18 #6 reproducer performs is rejected). *)
Theorem capture_local_never_escapes :
  can_escape (region_of_capture RegLocal) = false.
Proof. reflexivity. Qed.

(* T3' — same for an ARENA payload (also non-escapable, also inherited). *)
Theorem capture_arena_never_escapes :
  can_escape (region_of_capture RegArena) = false.
Proof. reflexivity. Qed.
