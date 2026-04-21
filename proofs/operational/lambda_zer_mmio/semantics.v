(* ================================================================
   λZER-MMIO : Small-step operational semantics.

   The key step rule: step_inttoptr fires ONLY when the address
   is both within a declared MMIO range AND correctly aligned.
   Mismatched alignment or out-of-range = STUCK configuration.
   ================================================================ *)

From stdpp Require Import base strings list gmap.
From zer_mmio Require Import syntax.

(* State holds declared MMIO ranges + var environment. *)
Record state : Type := mkState {
  (* Pairs (start, end) of declared MMIO regions. *)
  st_mmio_ranges : list (addr * addr);
  st_env : gmap var val;
}.

Definition initial_state : state := mkState [] ∅.

(* An address is within a declared MMIO range if some range covers it. *)
Definition addr_in_ranges (a : addr) (ranges : list (addr * addr)) : bool :=
  existsb (fun p => match p with (s, e) => andb (Nat.leb s a) (Nat.leb a e) end) ranges.

(* Alignment check. *)
Definition addr_aligned (a : addr) (al : align) : bool :=
  match al with
  | 0 => true  (* alignment 0 means no constraint *)
  | _ => Nat.eqb (a mod al) 0
  end.

Inductive step : state -> expr -> state -> expr -> Prop :=
  | step_var : forall st x v,
      st.(st_env) !! x = Some v ->
      step st (EVar x) st (EVal v)

  (* @inttoptr: address must be in range AND aligned. *)
  | step_inttoptr_ok : forall st al a,
      addr_in_ranges a st.(st_mmio_ranges) = true ->
      addr_aligned a al = true ->
      step st (EInttoPtr al (EVal (VAddr a)))
           st (EVal (VMMIOPtr a al))

  | step_inttoptr_ctx : forall st al e e' st',
      step st e st' e' ->
      step st (EInttoPtr al e) st' (EInttoPtr al e')

  | step_let_ctx : forall st x e1 e1' st' e2,
      step st e1 st' e1' ->
      step st (ELet x e1 e2) st' (ELet x e1' e2)

  | step_let_val : forall st x v e2,
      let st' := mkState st.(st_mmio_ranges)
                         (<[ x := v ]> st.(st_env)) in
      step st (ELet x (EVal v) e2) st' e2

  | step_seq_ctx : forall st e1 e1' st' e2,
      step st e1 st' e1' ->
      step st (ESeq e1 e2) st' (ESeq e1' e2)

  | step_seq_val : forall st v e2,
      step st (ESeq (EVal v) e2) st e2.

Definition is_value (e : expr) : bool :=
  match e with EVal _ => true | _ => false end.
