(* ================================================================
   λZER-Handle : Syntax

   The smallest ZER subset that captures the handle-safety story.
   Intentionally minimal — complexity comes in later files (semantics,
   typing, soundness).

   Scope (see docs/formal_verification_plan.md):
     - Pool(T, N) declarations (one scalar payload type per pool)
     - pool.alloc() → ?Handle(T)
     - pool.free(h)
     - pool.get(h).field read/write
     - orelse return for optional unwrap
     - if / while / let / seq control flow
     - u32 / bool / unit primitives
     - Single-threaded

   Deferred to later subsets:
     - defer, move struct, goto (Year 2)
     - MMIO / volatile (Year 3)
     - concurrency (Year 4)
     - async (Year 5)
   ================================================================ *)

From stdpp Require Import base strings list gmap.

(* ---- Identifiers ---- *)

(* Pool identifiers — name-tagged so the operational semantics can
   look up the right pool in the program's pool environment. *)
Definition pool_id := string.

(* Variable names in expressions. *)
Definition var := string.

(* Struct field names (for pool.get(h).field access). *)
Definition field := string.

(* ---- Values ---- *)

(* λZER-Handle values are finite:
     - Unit: for void / no-value
     - Bool: true / false
     - Int: we use Z (arbitrary integer) rather than a bounded width
            for simplicity; width-specific reasoning is Year 3+ work.
     - Handle: (pool_id, index, generation) triple. Matches ZER's
       runtime representation of `Handle(T)` as u64 = idx|gen.
     - Null handle: represents ?Handle = null. *)
Inductive val : Type :=
  | VUnit : val
  | VBool : bool -> val
  | VInt  : Z -> val
  | VHandle : pool_id -> nat -> nat -> val  (* pool, index, gen *)
  | VNullHandle : pool_id -> val.           (* ?Handle(T) = null *)

(* ---- Expressions ----

   Small-step friendly: every form either reduces, binds, or is a value.
   The `orelse return` form is a bit unusual — it needs the function's
   return expression as a syntactic argument, which we model by passing
   it explicitly. A richer language would use delimited continuations. *)
Inductive expr : Type :=
  (* Values and variables *)
  | EVal : val -> expr
  | EVar : var -> expr

  (* Pool operations *)
  | EAlloc : pool_id -> expr                         (* pool.alloc() *)
  | EFree  : pool_id -> expr -> expr                 (* pool.free(h) *)
  | EGet   : pool_id -> expr -> expr                 (* pool.get(h)  *)
  | EFieldRead  : pool_id -> expr -> field -> expr   (* pool.get(h).f *)
  | EFieldWrite : pool_id -> expr -> field -> expr -> expr
                                                     (* pool.get(h).f := v *)

  (* Optional unwrap: `e orelse return r`
       - If e evaluates to a non-null handle, strip the option and
         continue.
       - If e evaluates to null, the enclosing function returns r.
     We encode this as a single expression form with the return value
     baked in. The enclosing `EReturn` in the small-step rule
     captures function exit. *)
  | EOrelseReturn : expr -> expr -> expr

  (* Control flow *)
  | EIf    : expr -> expr -> expr -> expr
  | ELet   : var -> expr -> expr -> expr             (* let x = e1 in e2 *)
  | ESeq   : expr -> expr -> expr                    (* e1 ; e2 *)
  | EWhile : expr -> expr -> expr                    (* while c do body *)
  | EReturn : expr -> expr.                          (* return e *)

(* ---- Programs ----

   A program declares:
     - A set of pools (pool_id, element size — size as nat for
       simplicity, refined in typing).
     - A single entry-point expression (Year 1 has no function
       definitions; extending that is a Year 2 concern).

   Functions will be added when we extend to a calling convention.
   For Year 1's safety theorem we only need a top-level `main`. *)

Record pool_decl : Type := mkPool {
  pool_decl_id    : pool_id;
  pool_decl_size  : nat;        (* capacity — Pool(T, N) *)
}.

Record program : Type := mkProgram {
  prog_pools : list pool_decl;
  prog_main  : expr;
}.

(* ---- Well-formedness of pool declarations (early check) ----

   We require pool IDs to be unique in the declaration list. This is
   a syntactic check independent of the operational semantics; we
   prove it to rule out ambiguous pool references. *)

Definition pool_ids (p : program) : list pool_id :=
  map pool_decl_id (prog_pools p).

Definition pools_unique (p : program) : Prop :=
  NoDup (pool_ids p).

(* ---- Basic lemmas (smoke tests that the encoding is sane) ----

   Decidable equality on values. We derive it via stdpp's
   `solve_decision` tactic, which builds on `EqDecision` typeclass
   instances for Z / nat / string / bool (all provided by stdpp). *)

#[global] Instance val_eq_dec : EqDecision val.
Proof. solve_decision. Defined.

Lemma expr_ind_bundled : forall (P : expr -> Prop),
  (forall v, P (EVal v)) ->
  (forall x, P (EVar x)) ->
  (forall p, P (EAlloc p)) ->
  (forall p e, P e -> P (EFree p e)) ->
  (forall p e, P e -> P (EGet p e)) ->
  (forall p e f, P e -> P (EFieldRead p e f)) ->
  (forall p e f v, P e -> P v -> P (EFieldWrite p e f v)) ->
  (forall e r, P e -> P r -> P (EOrelseReturn e r)) ->
  (forall c e1 e2, P c -> P e1 -> P e2 -> P (EIf c e1 e2)) ->
  (forall x e1 e2, P e1 -> P e2 -> P (ELet x e1 e2)) ->
  (forall e1 e2, P e1 -> P e2 -> P (ESeq e1 e2)) ->
  (forall c b, P c -> P b -> P (EWhile c b)) ->
  (forall e, P e -> P (EReturn e)) ->
  forall e, P e.
Proof.
  fix IH 15.
  intros P HVal HVar HAlloc HFree HGet HFR HFW HOR HIf HLet HSeq HWhile HRet e.
  destruct e.
  - apply HVal.
  - apply HVar.
  - apply HAlloc.
  - apply HFree. apply IH; assumption.
  - apply HGet. apply IH; assumption.
  - apply HFR. apply IH; assumption.
  - apply HFW; apply IH; assumption.
  - apply HOR; apply IH; assumption.
  - apply HIf; apply IH; assumption.
  - apply HLet; apply IH; assumption.
  - apply HSeq; apply IH; assumption.
  - apply HWhile; apply IH; assumption.
  - apply HRet. apply IH; assumption.
Qed.

(* ================================================================
   What comes next (semantics.v):

   - Define `heap : gmap (pool_id * nat) val` — the allocator state.
   - Define `gen : gmap (pool_id * nat) nat` — per-slot generation.
   - Define `alive : gmap (pool_id * nat) bool` — slot in use.
   - Define `step : (expr, state) -> (expr, state) -> Prop` — the
     small-step stepping relation.
   - Prove `VHandle (p, i, g)` is safe to deref iff
     `alive[(p, i)] = true /\ gen[(p, i)] = g`.
     This is the operational model that zercheck_ir is supposed to
     conservatively approximate.

   In typing.v:

   - Define `tctx` (type environment), `ptctx` (pool environment).
   - Define `typed : tctx -> ptctx -> expr -> type -> Prop`.
   - Prove substitution, weakening, exchange.

   In adequacy.v:

   - `preservation` : typing preserved by step.
   - `progress`     : typed non-value expressions can step.

   In handle_safety.v:

   - `lambda_zer_handle_safety` — the main theorem.
   ================================================================ *)
