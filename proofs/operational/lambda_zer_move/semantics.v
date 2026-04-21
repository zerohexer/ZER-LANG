(* ================================================================
   λZER-Move : Small-step operational semantics.

   Models move-struct linearity:
     - Each allocation produces a FRESH VMove id.
     - Consume/Drop mark the id as DEAD in the state.
     - A "live" VMove can be passed around freely; re-binding
       transfers ownership.

   The operational UAF analog: any operation on a DEAD VMove gets
   stuck. Double-consume, use-after-consume, use-after-drop all
   manifest as stuck configurations, which the Iris proofs rule
   out for well-typed programs.

   Design: we track "live" move-struct ids in the state via a
   set. Alloc adds; consume/drop removes. Operations on absent
   ids are stuck.
   ================================================================ *)

From stdpp Require Import base strings list gmap sets.
From zer_move Require Import syntax.

(* ---- Program state ----

   - `live`: set of currently-alive move_ids
   - `env`: variable environment
   - `next_id`: monotonic counter for fresh allocations *)

Record state : Type := mkState {
  st_live  : gset move_id;
  st_env   : gmap var val;
  st_next  : move_id;
}.

Definition initial_state : state := mkState ∅ ∅ 0.

(* ---- Small-step reduction ---- *)

Inductive step : state -> expr -> state -> expr -> Prop :=

  (* -- Variable lookup -- *)
  | step_var : forall st x v,
      st.(st_env) !! x = Some v ->
      step st (EVar x) st (EVal v)

  (* -- Allocation: fresh move_id, add to live set, bump counter -- *)
  | step_alloc_move : forall st,
      let new_id := st.(st_next) in
      let st' := mkState ({[ new_id ]} ∪ st.(st_live))
                          st.(st_env)
                          (S st.(st_next))
      in
      step st EAllocMove st' (EVal (VMove new_id))

  (* -- Consume: requires live id, removes from live set -- *)
  | step_consume : forall st id,
      id ∈ st.(st_live) ->
      let st' := mkState (st.(st_live) ∖ {[ id ]})
                          st.(st_env)
                          st.(st_next)
      in
      step st (EConsume (EVal (VMove id))) st' (EVal VUnit)

  (* -- Drop: same as consume, different rule tag -- *)
  | step_drop : forall st id,
      id ∈ st.(st_live) ->
      let st' := mkState (st.(st_live) ∖ {[ id ]})
                          st.(st_env)
                          st.(st_next)
      in
      step st (EDrop (EVal (VMove id))) st' (EVal VUnit)

  (* -- Congruence rules: let/seq/consume/drop on non-value args -- *)
  | step_let_ctx : forall st x e1 e1' st' e2,
      step st e1 st' e1' ->
      step st (ELet x e1 e2) st' (ELet x e1' e2)

  | step_let_val : forall st x v e2,
      let st' := mkState st.(st_live)
                          (<[ x := v ]> st.(st_env))
                          st.(st_next)
      in
      step st (ELet x (EVal v) e2) st' e2

  | step_seq_ctx : forall st e1 e1' st' e2,
      step st e1 st' e1' ->
      step st (ESeq e1 e2) st' (ESeq e1' e2)

  | step_seq_val : forall st v e2,
      step st (ESeq (EVal v) e2) st e2

  | step_consume_ctx : forall st e e' st',
      step st e st' e' ->
      step st (EConsume e) st' (EConsume e')

  | step_drop_ctx : forall st e e' st',
      step st e st' e' ->
      step st (EDrop e) st' (EDrop e').

(* ---- Is-value predicate ---- *)
Definition is_value (e : expr) : bool :=
  match e with EVal _ => true | _ => false end.

(* ---- Stuck = not a value and cannot step ---- *)
Definition stuck (st : state) (e : expr) : Prop :=
  is_value e = false /\ ~ (exists st' e', step st e st' e').

(* ================================================================
   KEY SAFETY INTUITION:

   Operationally, EConsume/EDrop only fire when the id is in
   st_live. An UNsafe program tries to consume twice — the second
   attempt fails because id is already removed from st_live.

   That's a STUCK configuration: not a value, no step fires. The
   main safety theorem (handle_move_safety.v) proves well-typed
   programs never reach such a configuration.

   Iris formalizes this via `alive_move γ id : iProp` — owning the
   resource certifies the id is in st_live (state invariant).
   Consume consumes the resource; second consume can't prove its
   precondition.
   ================================================================ *)
