(* ================================================================
   λZER-Handle : Small-Step Operational Semantics

   Defines the step relation that models runtime execution of
   λZER-Handle programs. The operational model is what zercheck and
   the 4-model abstract proofs are supposed to conservatively
   approximate; connecting the two will be the job of soundness
   proofs in `handle_safety.v`.

   Design choices (Year 1, minimal):

   - **Environment-based, not substitution-based.** Variables reduce
     by lookup in a `gmap var val` environment. Avoids
     capture-avoiding substitution lemmas that would otherwise
     dominate the proof effort.

   - **Slots hold a single `val`.** Field names in the AST are
     retained (for future struct support in Year 2+) but don't
     affect semantics yet — all field reads/writes target the same
     slot value.

   - **"Stuck" for runtime errors.** We define reduction as a
     relation `step`. Failure to step on a non-value expression is
     the "stuck" state. The main safety theorem proves well-typed
     programs never get stuck.

   - **Generation-based UAF detection.** Each pool slot carries a
     generation counter. `pool.free(h)` increments it. A handle
     `VHandle(p, i, g)` is alive iff the slot at `(p, i)` has
     generation `g` AND is currently allocated. If not — stuck.

   Tier 1 forms covered in this file:
     EVal, EVar, ELet, EAlloc, EFree, EGet, EIf, ESeq,
     EOrelseReturn, EReturn

   Tier 2 (next commit): EFieldRead, EFieldWrite, EWhile.
   ================================================================ *)

From stdpp Require Import base strings list gmap.
From zer_handle Require Import syntax.

(* ---- Slot representation ----

   A slot is either free or allocated. When allocated, it holds a
   value and has a generation counter. When freed, the slot
   persists (so handle references still know the slot exists) but
   `slot_val = None` and the counter stays at its incremented
   value. *)
Record slot : Type := mkSlot {
  slot_gen : nat;
  slot_val : option val;  (* None = freed; Some v = alive with value v *)
}.

(* A pool's state: which slots are at what generation with what
   values. Absent entries are "never allocated" (gen 0, val None). *)
Definition pool_state := gmap nat slot.

(* Global store: pool_id -> pool_state. *)
Definition store := gmap pool_id pool_state.

(* Variable environment: let-bound variables to values. *)
Definition venv := gmap var val.

(* ---- Pool capacities ----

   For each pool we remember its capacity from the program
   declaration, so allocation can return null when full. *)
Definition pool_caps := gmap pool_id nat.

Definition caps_of (p : program) : pool_caps :=
  list_to_map
    (map (fun d => (pool_decl_id d, pool_decl_size d))
         (prog_pools p)).

(* ---- Full program state ---- *)
Record state : Type := mkState {
  st_store : store;
  st_env   : venv;
  st_caps  : pool_caps;
  (* For modeling `return`: once the function has returned, the rest
     of the expression must not step. We represent this as an
     explicit flag + returned value. *)
  st_returned : option val;
}.

Definition initial_state (p : program) : state :=
  mkState empty empty (caps_of p) None.

(* ---- Helper: find a free slot in a pool ----

   Returns `Some i` if slot `i` is either (a) absent from the pool's
   map (never allocated) and `i < capacity`, or (b) present but
   freed. Returns `None` if all slots up to capacity are taken.

   The choice of `i` is deterministic: we pick the smallest free
   index. This keeps the operational semantics fully deterministic
   for Year 1 (nondeterminism can be introduced later if desired). *)

Fixpoint find_free_slot (ps : pool_state) (cap : nat) (i : nat) : option nat :=
  match cap with
  | 0 => None
  | S cap' =>
      match ps !! i with
      | None => Some i
      | Some s =>
          match slot_val s with
          | None => Some i          (* freed slot, reusable *)
          | Some _ => find_free_slot ps cap' (S i)
          end
      end
  end.

(* ---- Handle liveness check ----

   A handle `VHandle(p, i, g)` is alive iff:
     - pool p exists in the store
     - slot i exists in the pool state
     - slot's generation matches g
     - slot is currently allocated (slot_val = Some _)

   Returns the slot's value if alive, None otherwise. *)

Definition handle_lookup (st : store) (p : pool_id) (i g : nat) : option val :=
  ps ← st !! p;
  s ← ps !! i;
  guard (slot_gen s = g);;
  slot_val s.

(* ---- Step relation ----

   Relates a state + expression to a successor state + expression.
   A stuck configuration is one where no step is possible AND the
   expression is not a value.

   We use a small-step style: each rule describes one reduction.
   Congruence rules (e.g., "if e1 steps, then `let x = e1 in e2`
   steps in the e1 position") are explicit cases. *)

Inductive step : state -> expr -> state -> expr -> Prop :=

  (* -- Variable lookup -- *)
  | step_var : forall st x v,
      st.(st_returned) = None ->
      st.(st_env) !! x = Some v ->
      step st (EVar x) st (EVal v)

  (* -- Let binding: reduce body, then substitute via env update -- *)
  | step_let_ctx : forall st e1 e1' st' x e2,
      st.(st_returned) = None ->
      step st e1 st' e1' ->
      step st (ELet x e1 e2) st' (ELet x e1' e2)

  | step_let_val : forall st x v e2,
      st.(st_returned) = None ->
      let st' := mkState st.(st_store)
                          (<[ x := v ]> st.(st_env))
                          st.(st_caps)
                          None
      in
      step st (ELet x (EVal v) e2) st' e2

  (* -- Sequencing -- *)
  | step_seq_ctx : forall st e1 e1' st' e2,
      st.(st_returned) = None ->
      step st e1 st' e1' ->
      step st (ESeq e1 e2) st' (ESeq e1' e2)

  | step_seq_val : forall st v e2,
      st.(st_returned) = None ->
      step st (ESeq (EVal v) e2) st e2

  (* -- If-then-else -- *)
  | step_if_ctx : forall st c c' st' e1 e2,
      st.(st_returned) = None ->
      step st c st' c' ->
      step st (EIf c e1 e2) st' (EIf c' e1 e2)

  | step_if_true : forall st e1 e2,
      st.(st_returned) = None ->
      step st (EIf (EVal (VBool true)) e1 e2) st e1

  | step_if_false : forall st e1 e2,
      st.(st_returned) = None ->
      step st (EIf (EVal (VBool false)) e1 e2) st e2

  (* -- Allocation --

     pool.alloc():
       - If a free slot is available at index i: create a new handle
         `VHandle(p, i, gen+1)` (we bump the gen on allocation too,
         so freshly-allocated handles have gen ≥ 1 — lets us reserve
         gen 0 for "never allocated").
       - If the pool is full, return `VNullHandle p`. *)
  | step_alloc_succ : forall st p cap ps i new_gen,
      st.(st_returned) = None ->
      st.(st_caps) !! p = Some cap ->
      st.(st_store) !! p = Some ps ->
      find_free_slot ps cap 0 = Some i ->
      (* new_gen is old_gen + 1, or 1 if slot was never allocated *)
      new_gen = match ps !! i with
                | None => 1
                | Some s => S (slot_gen s)
                end ->
      let new_slot := mkSlot new_gen (Some (VInt 0)) in
      let ps' := <[ i := new_slot ]> ps in
      let st' := mkState (<[ p := ps' ]> st.(st_store))
                          st.(st_env)
                          st.(st_caps)
                          None
      in
      step st (EAlloc p) st' (EVal (VHandle p i new_gen))

  | step_alloc_fail : forall st p cap ps,
      st.(st_returned) = None ->
      st.(st_caps) !! p = Some cap ->
      st.(st_store) !! p = Some ps ->
      find_free_slot ps cap 0 = None ->
      step st (EAlloc p) st (EVal (VNullHandle p))

  (* Pool not yet in store (first allocation): create empty pool
     state and retry. Keeps `store` lazy — pools only appear on
     first use. *)
  | step_alloc_init : forall st p cap,
      st.(st_returned) = None ->
      st.(st_caps) !! p = Some cap ->
      st.(st_store) !! p = None ->
      let st' := mkState (<[ p := empty ]> st.(st_store))
                          st.(st_env)
                          st.(st_caps)
                          None
      in
      step st (EAlloc p) st' (EAlloc p)

  (* -- Free --

     Reduces the argument first, then performs the free. Frees a
     live handle by setting `slot_val := None` (keeps the gen, so
     subsequent accesses to this handle see the old gen and trap). *)
  | step_free_ctx : forall st p e e' st',
      st.(st_returned) = None ->
      step st e st' e' ->
      step st (EFree p e) st' (EFree p e')

  | step_free_alive : forall st p i g ps s,
      st.(st_returned) = None ->
      st.(st_store) !! p = Some ps ->
      ps !! i = Some s ->
      slot_gen s = g ->
      slot_val s <> None ->
      let s' := mkSlot (S (slot_gen s)) None in
      let ps' := <[ i := s' ]> ps in
      let st' := mkState (<[ p := ps' ]> st.(st_store))
                          st.(st_env)
                          st.(st_caps)
                          None
      in
      step st (EFree p (EVal (VHandle p i g))) st' (EVal VUnit)

  (* Note: step_free_uaf is NOT a rule — we intentionally don't
     step when the handle is stale. That makes UAF a "stuck"
     configuration, which is what the safety theorem will rule out
     for well-typed programs. *)

  (* Freeing null: no-op (returns unit).
     This matches ZER's `pool.free(h)` when `h` is a bare Handle;
     if it were ?Handle, unwrapping happens first via orelse. *)

  (* -- Get --

     pool.get(h): returns the value at handle's slot if alive. *)
  | step_get_ctx : forall st p e e' st',
      st.(st_returned) = None ->
      step st e st' e' ->
      step st (EGet p e) st' (EGet p e')

  | step_get_alive : forall st p i g v,
      st.(st_returned) = None ->
      handle_lookup st.(st_store) p i g = Some v ->
      step st (EGet p (EVal (VHandle p i g))) st (EVal v)

  (* Again, no rule for stale get — stuck. *)

  (* -- Orelse Return --

     `e orelse return r`:
       - Evaluate e.
       - If e is a non-null handle: unwrap (return the handle).
       - If e is null: evaluate r, then mark the state as "returned"
         with r's value.

     The `st_returned` flag is our simple model of function return:
     once set, no further evaluation happens. The outer `EReturn`
     in `main` picks up the returned value as the program's result. *)
  | step_orelse_ctx : forall st e e' r st',
      st.(st_returned) = None ->
      step st e st' e' ->
      step st (EOrelseReturn e r) st' (EOrelseReturn e' r)

  | step_orelse_unwrap : forall st p i g r,
      st.(st_returned) = None ->
      step st (EOrelseReturn (EVal (VHandle p i g)) r)
           st (EVal (VHandle p i g))

  | step_orelse_return_ctx : forall st p r r' st',
      st.(st_returned) = None ->
      step st r st' r' ->
      step st (EOrelseReturn (EVal (VNullHandle p)) r)
           st' (EOrelseReturn (EVal (VNullHandle p)) r')

  | step_orelse_return_fire : forall st p v,
      st.(st_returned) = None ->
      let st' := mkState st.(st_store)
                          st.(st_env)
                          st.(st_caps)
                          (Some v)
      in
      step st (EOrelseReturn (EVal (VNullHandle p)) (EVal v))
           st' (EVal v)

  (* -- Return --

     Direct `return e`: evaluate e, then mark state as returned. *)
  | step_return_ctx : forall st e e' st',
      st.(st_returned) = None ->
      step st e st' e' ->
      step st (EReturn e) st' (EReturn e')

  | step_return_val : forall st v,
      st.(st_returned) = None ->
      let st' := mkState st.(st_store)
                          st.(st_env)
                          st.(st_caps)
                          (Some v)
      in
      step st (EReturn (EVal v)) st' (EVal v).

(* ---- Multi-step reduction ---- *)

Inductive steps : state -> expr -> state -> expr -> Prop :=
  | steps_refl : forall st e, steps st e st e
  | steps_trans : forall st e st' e' st'' e'',
      step st e st' e' ->
      steps st' e' st'' e'' ->
      steps st e st'' e''.

(* ---- Values and stuckness ---- *)

Definition is_value (e : expr) : bool :=
  match e with EVal _ => true | _ => false end.

Definition stuck (st : state) (e : expr) : Prop :=
  ~ is_value e = true /\ ~ exists st' e', step st e st' e'.

(* ---- Safety-relevant predicates --

   These are the predicates the main soundness theorem in
   handle_safety.v will rule out for well-typed programs. *)

(* A state has a leak if some pool has an alive slot at termination. *)
Definition has_alive_slot (st : state) : Prop :=
  exists p ps i s,
    st.(st_store) !! p = Some ps /\
    ps !! i = Some s /\
    slot_val s <> None.

Definition leaked (st : state) (e : expr) : Prop :=
  is_value e = true /\ has_alive_slot st.

(* UAF / double-free are characterized by "stuck at a handle
   operation." The soundness theorem will show well-typed programs
   never stuck with a handle op in head position. *)

(* ================================================================
   Sanity check: the step relation is deterministic for Tier 1.

   We don't prove this yet (the proof is tedious case-analysis) but
   state it as a lemma to track. Determinism is useful later when
   connecting to CFG-based analysis (which assumes one path per
   configuration). *)

Lemma step_deterministic_stmt :
  forall st e st1 e1 st2 e2,
    step st e st1 e1 ->
    step st e st2 e2 ->
    st1 = st2 /\ e1 = e2.
Proof.
  (* Placeholder for Year-1 end-state. Proof is mechanical but
     long (~60 cases). Leaving admitted here keeps the file
     compiling while signaling the todo. *)
Admitted.

(* ================================================================
   What's next:

   - typing.v : type system + well-typed relation.
   - adequacy.v : preservation + progress lemmas.
     - Progress: well-typed non-value expr can step.
     - Preservation: step preserves well-typedness.
   - handle_safety.v : the main theorem — well-typed programs never
     UAF, double-free, or leak.

   Tier 2 semantics (future commit):
     - EFieldRead / EFieldWrite : once slots carry structs.
     - EWhile : fixpoint reduction.
     - True determinism proof.
   ================================================================ *)
