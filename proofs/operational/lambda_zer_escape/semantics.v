(* ================================================================
   λZER-Escape : Small-step operational semantics.

   Key design decision: step_store_global and step_return ONLY
   fire when the pointer's region tag is RegStatic. Attempting to
   store a RegLocal/RegArena pointer in a global — or return one
   from a function — is a STUCK configuration (no rule fires).

   Iris proofs show that well-typed programs never reach the stuck
   state: owning a RegLocal-tagged resource and trying to cast it
   to a RegStatic-required slot is forbidden by the resource algebra.
   ================================================================ *)

From stdpp Require Import base strings list gmap.
From zer_escape Require Import syntax.

Record state : Type := mkState {
  st_next : nat;
  st_env  : gmap var val;
  (* Ghost-tracking: instance id → region. *)
  st_regions : gmap nat region;
  (* Count of ptrs currently stored in global slots (safety metric). *)
  st_globals_stored : nat;
  (* Count of ptrs currently returned (safety metric). *)
  st_returned : nat;
}.

Definition initial_state : state := mkState 0 ∅ ∅ 0 0.

Inductive step : state -> expr -> state -> expr -> Prop :=

  | step_var : forall st x v,
      st.(st_env) !! x = Some v ->
      step st (EVar x) st (EVal v)

  (* Allocation — fresh id, record region *)
  | step_alloc_local : forall st,
      let new_id := st.(st_next) in
      let st' := mkState (S st.(st_next)) st.(st_env)
                         (<[ new_id := RegLocal ]> st.(st_regions))
                         st.(st_globals_stored) st.(st_returned)
      in
      step st EAllocLocal st' (EVal (VPtr (PtrTagged new_id RegLocal)))

  | step_alloc_arena : forall st,
      let new_id := st.(st_next) in
      let st' := mkState (S st.(st_next)) st.(st_env)
                         (<[ new_id := RegArena ]> st.(st_regions))
                         st.(st_globals_stored) st.(st_returned)
      in
      step st EAllocArena st' (EVal (VPtr (PtrTagged new_id RegArena)))

  | step_alloc_static : forall st,
      let new_id := st.(st_next) in
      let st' := mkState (S st.(st_next)) st.(st_env)
                         (<[ new_id := RegStatic ]> st.(st_regions))
                         st.(st_globals_stored) st.(st_returned)
      in
      step st EAllocStatic st' (EVal (VPtr (PtrTagged new_id RegStatic)))

  (* Store-global: ONLY fires when the pointer is RegStatic.
     Local/arena pointers → stuck. *)
  | step_store_global : forall st id,
      st.(st_regions) !! id = Some RegStatic ->
      let st' := mkState st.(st_next) st.(st_env) st.(st_regions)
                         (S st.(st_globals_stored)) st.(st_returned) in
      step st (EStoreGlobal (EVal (VPtr (PtrTagged id RegStatic))))
           st' (EVal VUnit)

  (* Return: ONLY fires when the pointer is RegStatic. *)
  | step_return : forall st id,
      st.(st_regions) !! id = Some RegStatic ->
      let st' := mkState st.(st_next) st.(st_env) st.(st_regions)
                         st.(st_globals_stored) (S st.(st_returned)) in
      step st (EReturn (EVal (VPtr (PtrTagged id RegStatic))))
           st' (EVal VUnit)

  (* Congruence rules *)
  | step_let_ctx : forall st x e1 e1' st' e2,
      step st e1 st' e1' ->
      step st (ELet x e1 e2) st' (ELet x e1' e2)

  | step_let_val : forall st x v e2,
      let st' := mkState st.(st_next)
                         (<[ x := v ]> st.(st_env))
                         st.(st_regions)
                         st.(st_globals_stored) st.(st_returned)
      in
      step st (ELet x (EVal v) e2) st' e2

  | step_seq_ctx : forall st e1 e1' st' e2,
      step st e1 st' e1' ->
      step st (ESeq e1 e2) st' (ESeq e1' e2)

  | step_seq_val : forall st v e2,
      step st (ESeq (EVal v) e2) st e2

  | step_store_ctx : forall st e e' st',
      step st e st' e' ->
      step st (EStoreGlobal e) st' (EStoreGlobal e')

  | step_return_ctx : forall st e e' st',
      step st e st' e' ->
      step st (EReturn e) st' (EReturn e').

Definition is_value (e : expr) : bool :=
  match e with EVal _ => true | _ => false end.

Definition stuck (st : state) (e : expr) : Prop :=
  is_value e = false /\ ~ (exists st' e', step st e st' e').
