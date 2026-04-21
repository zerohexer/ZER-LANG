(* ================================================================
   λZER-Opaque : Small-step operational semantics.

   Key design decision: there's NO step rule for direct *A → *B casts.
   The only way to cast between pointer types is through *opaque:
     1. EOpaqueCast erases the static type (tag remains in runtime)
     2. ETypedCast checks the tag and produces the target type
   If the cast target doesn't match the stored tag, ETypedCast is
   STUCK — the operational manifestation of J04 (cast safety).

   State tracks:
     - next_id: fresh instance counter
     - env: variable bindings
     - ptr_types: what type each PtrTyped instance has (ghost-state
       analog of zercheck's _zer_opaque { ptr, type_id })
   ================================================================ *)

From stdpp Require Import base strings list gmap.
From zer_opaque Require Import syntax.

Record state : Type := mkState {
  st_next  : nat;
  st_env   : gmap var val;
  (* Instance id → type tag mapping (the runtime provenance). *)
  st_ptr_types : gmap nat type_id;
}.

Definition initial_state : state := mkState 0 ∅ ∅.

Inductive step : state -> expr -> state -> expr -> Prop :=

  (* -- Variable lookup -- *)
  | step_var : forall st x v,
      st.(st_env) !! x = Some v ->
      step st (EVar x) st (EVal v)

  (* -- Allocation: fresh id, record its type in ptr_types -- *)
  | step_alloc : forall st t,
      let new_id := st.(st_next) in
      let st' := mkState (S st.(st_next))
                          st.(st_env)
                          (<[ new_id := t ]> st.(st_ptr_types))
      in
      step st (EAlloc t) st' (EVal (VPtr (PtrTyped new_id t)))

  (* -- Opaque cast: erase static type, value is UNCHANGED --
     The tag is still there in ptr_types; only the static type
     (what Iris sees) is different. At the semantics level we just
     wrap the same pointer. *)
  | step_opaque_cast : forall st id t,
      step st (EOpaqueCast (EVal (VPtr (PtrTyped id t))))
           st (EVal (VPtr (PtrTyped id t)))

  (* -- Typed cast from opaque: requires target tag to MATCH recorded --
     This is the safety-enforcing rule. If the target doesn't match,
     no rule fires → stuck configuration.
     st_ptr_types !! id = Some t_stored, t_stored = t_target *)
  | step_typed_cast_ok : forall st id t_target,
      st.(st_ptr_types) !! id = Some t_target ->
      step st (ETypedCast t_target (EVal (VPtr (PtrTyped id t_target))))
           st (EVal (VPtr (PtrTyped id t_target)))

  (* -- Dereference a typed pointer: produces a unit value (placeholder) --
     Real semantics would read the memory at the pointer. For proofs,
     we just need a successful dereference step. *)
  | step_deref : forall st id t,
      st.(st_ptr_types) !! id = Some t ->
      step st (EDeref (EVal (VPtr (PtrTyped id t))))
           st (EVal VUnit)

  (* -- Congruence: let/seq on non-value args -- *)
  | step_let_ctx : forall st x e1 e1' st' e2,
      step st e1 st' e1' ->
      step st (ELet x e1 e2) st' (ELet x e1' e2)

  | step_let_val : forall st x v e2,
      let st' := mkState st.(st_next)
                          (<[ x := v ]> st.(st_env))
                          st.(st_ptr_types)
      in
      step st (ELet x (EVal v) e2) st' e2

  | step_seq_ctx : forall st e1 e1' st' e2,
      step st e1 st' e1' ->
      step st (ESeq e1 e2) st' (ESeq e1' e2)

  | step_seq_val : forall st v e2,
      step st (ESeq (EVal v) e2) st e2

  | step_opaque_cast_ctx : forall st e e' st',
      step st e st' e' ->
      step st (EOpaqueCast e) st' (EOpaqueCast e')

  | step_typed_cast_ctx : forall st t e e' st',
      step st e st' e' ->
      step st (ETypedCast t e) st' (ETypedCast t e')

  | step_deref_ctx : forall st e e' st',
      step st e st' e' ->
      step st (EDeref e) st' (EDeref e').

Definition is_value (e : expr) : bool :=
  match e with EVal _ => true | _ => false end.

Definition stuck (st : state) (e : expr) : Prop :=
  is_value e = false /\ ~ (exists st' e', step st e st' e').

(* ================================================================
   SAFETY NOTE: a "wrong cast" is a STUCK configuration.

   If you have PtrTyped id t_stored and try ETypedCast t_wrong with
   t_wrong ≠ t_stored, no step rule fires — the program is stuck.

   Iris proofs will show that well-typed programs never reach this
   stuck configuration, because the Iris resource `typed_ptr γ id t`
   pins the tag, and ETypedCast requires matching resource/tag.
   ================================================================ *)
