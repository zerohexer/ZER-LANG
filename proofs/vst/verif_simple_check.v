(* ================================================================
   Level-3 VST proof — simple_check.c:is_alive

   Verifies the C implementation of `is_alive` matches the Coq
   specification: the function returns 1 iff the input is 1,
   else 0.

   First VST proof in the ZER project. Pattern:
     1. clightgen converts simple_check.c → simple_check.v (Coq AST)
     2. We write a VST spec (DECLARE + WITH + PRE/POST)
     3. We prove `semax_body Vprog Gprog f_is_alive is_alive_spec`
        using VST tactics (forward, forward_if, entailer!)

   VST 3.0 is Iris-based. `VST.floyd.compat` (precompiled in the
   zer-vst Docker image) provides backward-compatible `funspec`
   without exposing Σ for simple proofs.
   ================================================================ *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_vst.simple_check.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

(* ---- Coq specification of the function's intended behavior.
        Mirrors a safety predicate from lambda_zer_typing/typing.v. *)

Definition is_alive_coq (state : Z) : Z :=
  if Z.eq_dec state 1 then 1 else 0.

(* ---- VST funspec ---- *)

Definition is_alive_spec : ident * funspec :=
 DECLARE _is_alive
  WITH state : Z
  PRE [ tint ]
    PROP (Int.min_signed <= state <= Int.max_signed)
    PARAMS (Vint (Int.repr state))
    SEP ()
  POST [ tint ]
    PROP ()
    RETURN (Vint (Int.repr (is_alive_coq state)))
    SEP ().

Definition Gprog : funspecs := [ is_alive_spec ].

(* ---- Proof: the C source matches the spec ---- *)

Lemma body_is_alive: semax_body Vprog Gprog f_is_alive is_alive_spec.
Proof.
  start_function.
  forward_if;
    forward;
    unfold is_alive_coq;
    destruct (Z.eq_dec state 1); try contradiction; try subst; try contradiction;
    entailer!.
Qed.

(* ================================================================
   QED — is_alive's C implementation mechanically verified against
   is_alive_coq. The C source and the Coq predicate compute the
   same function on matching inputs.

   This is the Level-3 pattern: compiler correctness oracle made
   MECHANICAL at the C-source level, closing the gap between
   Level-1+2 (predicate proven) and the actual running compiler.

   Scaling to zercheck.c: per-function proofs, 5-20 hours each.
   ~50 safety-critical functions → 150-500 hours total. This is
   session 1 of that path.
   ================================================================ *)
