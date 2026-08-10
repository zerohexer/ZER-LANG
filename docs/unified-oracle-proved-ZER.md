# Unified Oracle-Proved ZER — the Level A product plan

**Status: PLAN (not implemented). Written 2026-08-10.**
**Owner decision: LOCKED direction. Sequencing below is a proposal, not a commitment.**

---

## 0. How to use this document

This is a **pickup document**. It exists because the reasoning behind this plan took a
long session to derive, most of it by measurement, and none of it is recoverable by
reading the source. A fresh session that reads only the code will re-derive the wrong
conclusions — specifically it will conclude the per-class architecture is fine, because
each class looks locally correct.

Read in this order:

| you want | read |
|---|---|
| the decision in 2 minutes | §1 |
| why the current design leaks | §2, §3 |
| the theory you must not get wrong | §4 |
| what to actually build | §5, §6, §7 |
| the worked template to copy | §8 |
| the trap that already bit once | §9 |
| where code has to move | §10 |
| what to do first | §11 |
| what NOT to do | §12 |
| how to know it worked | §13 |

Prerequisite reading (do not skip, this document assumes them):
- `CLAUDE.md` — "The Verification Endgame", "MAX-ORACLE STANDARD", "THE PRECISION
  CEILING", "MULTI-SITE SAFETY IS THE #1 RECURRING BUG CLASS", "IMPLEMENTING A
  RELAXATION"
- `docs/compiler-internals.md` — "Sound relaxation (reject->accept)", "Escape & keep
  analysis", "Consuming a documented open hole — the MEASURE-FIRST protocol"
- `docs/proof-internals.md` — the three levels, the Phase 1 extraction recipe
- `proofs/operational/lambda_zer_handle/handle_flow_lattice.v` — the template (162 lines,
  read it in full, it is the single most important artifact for this plan)

---

## 1. TL;DR — the decision

**Problem.** ZER's safety is ~29 tracking systems answering questions independently. A
semantic question ("does a reference to this allocation reach here?") is implemented
separately at every syntactic site that needs it. Nothing forces a site to answer all the
questions. So a new syntactic form, or a new sink, silently answers some and skips
others. That is not a hypothetical failure mode — it is the origin of essentially every
soundness hole found in this codebase, including all four found on 2026-08-08..10.

**Target.** ONE judgment, forced by the type of the query:

```
state   = (provenance, liveness, ownership, bounds, capability)   per abstract location
join    = componentwise join                                       (sound, coarse)
use_ok  = conjunction of all five components at the use site
```

A sink can no longer ask one question and receive one answer. It asks *the* question and
receives the tuple. A new form must produce all five components; it cannot silently
produce four.

**Layering.**

| layer | what it is | property |
|---|---|---|
| **Level A** | plain product lattice, componentwise join, conjunction at use sites | sound, coarse, OVER-REJECTS |
| **Level B** | reduced product (components refine each other) + per-class guarded recoveries | recovers precision, one relaxation at a time |

**Over-rejection at Level A is accepted deliberately.** It is the cost of soundness, not
a defect. Level B pays back the part of it that is payable.

**This is not a greenfield design.** Level A and Level B both already exist, proven and
shipped, for exactly one class (UAF / handle state). The task is to *generalize a worked
instance*, not to invent an architecture. See §8.

---

## 2. The problem, with evidence

### 2.1 The claim

Independent safety classes leak **because they are independent**, not because any
individual class is badly written. Each class is locally correct. The holes live in the
space *between* classes and *between sites of the same class*, which no class owns.

### 2.2 The evidence (session 2026-08-08..10)

Five defects, four of them the same shape:

| defect | the split that caused it |
|---|---|
| **BUG-769 (G3)** atomic-cell plain access | "is this global an atomic cell?" was WHOLE-PROGRAM; "flag the plain access" was PER-FUNCTION. One property, cut in half. The cut was the hole. |
| **view-alias family, 8 forms** | "does a reference reach here?" and "is the allocation freed?" were two analyses talking past each other. |
| **BUG-767/768** funcptr reach | ONE question — "does the callback this spawn target invokes touch a non-shared global?" — answered independently at SIX syntactic sites. |
| **volatile exemption** (2026-08-03) | The same exemption granted at TWO sites (spawn scan, ISR check). Fixed one, missed the sibling, shipped a tearing bare-metal access. |
| **BUG-770** defer fire count | Defer arming tracked as a COUNT in `ir_lower.c` rather than per-defer state — a representation that structurally cannot express "this defer's registration did not execute". |

Note what these are NOT. None is a hard reasoning error. None required deep insight to
fix. Each is "nobody forced this to be answered once."

### 2.3 The strongest single data point

Every defect found in that session was in a class with **no Level-1 oracle**. Zero
exceptions. Meanwhile the well-oracled classes — `handle` (25 proof files), `concurrency`
(10), `escape` (9), `move` (6), `opaque` (6) — produced **none**.

That is the priority signal. It is not that oracles are nice; it is that the bug
distribution is almost perfectly explained by oracle absence.

### 2.4 Why enumeration alone does not fix it

The obvious counter-proposal is "just enumerate all the sites and all the forms." That
was tried, repeatedly, and it does not terminate:

- The funcptr REACH axis was enumerated as **6 forms**. Writing them down as a grid axis
  immediately surfaced a **7th** (a factory returning another factory's result) that
  nobody had reported. The enumeration was incomplete at the exact moment it was believed
  complete.
- The view-alias family was closed at **7 forms** on 2026-08-06 and gated with a 21-cell
  matrix. An **8th** form (optional-unwrap) was found two days later.

Syntactic forms are unbounded. Abstract states are finite by construction. This is the
whole reason the target is a state product and not a form checklist. See §4.3.

---

## 3. Measured state of the codebase (2026-08-10)

Everything in this section was measured on the day of writing, not recalled. Re-measure
before trusting it; the repo moves.

### 3.1 Where safety code lives

```
checker.c        19702 lines
zercheck_ir.c     7206 lines     <- the CFG analyzer; handle state ONLY
ir_lower.c        3993 lines
zercheck.c         156 lines     <- backward-compat shim (LSP, firmware tests)
vrp_ir.c           349 lines     <- the SOUND CFG-based VRP
```

### 3.2 The orphan

```
vrp_ir.c        Makefile references: 0
                symbols in built zerc: 0
```

The sound CFG-based value-range analysis **is not compiled**. Production runs the flat
AST pass, which has a known live under-rejection (a branch-local narrowing leaks past a
control-flow join; the compiler then proves an OOB index safe and emits no bounds check).

This matters structurally, not just as a bug: **bounds is one of the five factors.** You
cannot form a product with a component that is not in the binary.

### 3.3 Every flow-shaped property is on the AST, none on the CFG

| property | class | `checker.c` | `zercheck_ir.c` |
|---|---|---|---|
| `var_range_count` | bounds / VRP | 39 | 0 |
| `is_local_derived` | escape / provenance | 97 | 0 |
| `after_spawn_in_func` | atomic-cell | 10 | 0 |
| `is_borrowed_by_thread` | scoped borrow | 7 | 0 |
| `atomic_plain_writes` | atomic-cell | 6 | 0 |
| `branch_depth` | scoped borrow | 5 | 0 |

The last row is the tell. `Checker.branch_depth` was added on 2026-08-03 as a hand-rolled
counter approximating control-flow nesting, on the AST, **because the borrow state was not
in the CFG**. That is reimplementing the CFG, badly, inside a class that should have been
consuming it.

`zercheck_ir.c` is 7,206 lines of CFG machinery — fixpoint, block merge, alias groups,
compound handles — used by exactly one class.

### 3.4 Oracle inventory

```
lambda_zer_handle        25 .v files    <- Level A + Level B, both shipped
lambda_zer_concurrency   10             <- closure SUFFICIENCY only
lambda_zer_escape         9             <- param_lattice, join_lattice
lambda_zer_move           6
lambda_zer_opaque         6
lambda_zer_mmio           3
lambda_zer_typing         2
lambda_zer_bounds         1             <- oracle exists; impl orphaned (3.2)
lambda_zer_capture        1
lambda_zer_disjoint       1             <- the EXCEEDS-Rust sketch
lambda_zer_qualifier      1
lambda_zer_volatile       1
```

Classes with **no** oracle include: atomic-cell, defer/goto arming, funcptr reachability,
view-provenance (as a distinct question from escape), ISR, stack, comptime, container,
async, asm, emitter width/wrap.

The June 2026 MAX-ORACLE GAP AUDIT tally: ~14 live under-rejections (6 CRITICAL
memory-corruption), ~4 real over-rejections, **3 classes genuinely at-max**.

### 3.5 Existing verification instruments (do not lose these)

```
10 axis-crossed grids:  shape escape keep cflow conc view_alias hw async asm defer_goto
tools/sink_matrix.sh:   54 cells, permanent make check gate
audit gates:            walker_default_audit, audit_type_dispatch, audit_fixed_buffers,
                        audit_carrier_dispatch
-Werror=switch:         hard build failure on an unhandled node/op kind
```

These are the only reason the 2026-08 defects were found at all. **§13 explains why they
must survive the migration.**

---

## 4. The theory you must not get wrong

Four propositions. Getting any of them backwards produces months of misdirected work.

### 4.1 Soundness is achievable; precision is not

- **100% SOUNDNESS** (never accept an unsafe program) is achievable and **Rice-immune**.
- **100% PRECISION** (accept every safe program) is **impossible** by Rice — not hard,
  impossible — and is explicitly NOT a goal.

A wrongly-rejected program simply never runs; no UB results. Permanent over-rejection is
harmless to safety. Rice is the enemy of ergonomics, never of safety.

### 4.2 Over-rejection is the COST, not the GOAL

This distinction is load-bearing and easy to lose. Restating the ceiling decomposition:

**FORCED over-rejection (semantic, UNPAYABLE).** Any finite sound abstraction of an
infinite concrete space must lose information — at control-flow joins, at operations the
domain cannot represent, at cross-class interactions. At every loss point soundness forces
rounding toward reject. You cannot have "finite + sound" without "rounds toward reject
somewhere." Do not chase this.

**PAYABLE over-rejection (engineering, RECOVERABLE).** Extra over-rejection from a
cheaper-than-necessary abstraction, or a refinement not yet built. **This is the entire
design target of Level B.** Two levers recover it:
- **reduced product** — components feed each other (recovers cross-class loss)
- **relational domains** — octagon, outlives ordering, `i < j` (recovers within-class
  join loss)

If "over-reject is the goal" becomes the operating framing, payable over-rejection gets
accepted that did not need to be paid, and the language feels obstinate for no soundness
benefit.

### 4.3 You enumerate STATES, not FORMS

Syntactic forms are unbounded and the enumeration provably does not terminate in practice
(§2.4: a 7th funcptr form, an 8th view form, both found *by* the enumeration that was
believed complete).

Abstract states are **finite by construction**. And completeness is not established by
inspection — it falls out of the simulation proof: **a missing abstract state is a diagram
that will not close, i.e. a STUCK PROOF**, not a silent accept.

The terminating condition for form-chasing is therefore: **the desugaring is verified.**
Once every surface form provably lowers into the core, the core's finite state set covers
all of them. This is exactly why the endgame is "small core + verified desugaring" and not
"formalize all 53 node kinds" — formalizing 53 recreates the drift bug class *inside the
trusted spec*, where no test catches it.

### 4.4 ZER's theorem is harder than Rust's, and the difference is annotations

A common error is to model the target on Rust as "lifetime x ownership x annotation."
That is not Rust's structure and it is not available to ZER.

**Rust is ONE relation:** aliasing XOR mutability, over regions. Lifetimes are not a peer
factor — they are the *proof obligation* of that one rule. Annotations are not a factor
either — they are a patch for where inference is incomplete, and they are *checked*, so a
wrong annotation is caught.

**ZER is annotation-free.** All the trust sits on the inference; no human-supplied
lifetime shares the burden. That makes the soundness theorem the ONLY thing that can
justify trusting a fully-automatic analysis — and it makes the object being certified
(the auto-inference itself) strictly stronger than RustBelt's human-assisted system.

**Consequence for the design:** ZER's unified model is a *conjunction of independently
inferred property judgments over a shared abstract state* — a reduced product. That is a
different and more refinable construction than Rust's single relation. Rust's borrow
relation structurally cannot be refined by value-range information, which is why safe Rust
rejects provably-disjoint mutation and routes it through `unsafe`. A reduced product can.
See `lambda_zer_disjoint/disjoint_lattice.v`.

### 4.5 "Validity" is the conclusion, not a factor

Do not write the product as `Validity x lifetime x ownership`. That mixes the theorem into
its own product, and it breaks the moment the lattice is defined: there is no join for
"validity" — it is a predicate *over* the state, not a component of it.

```
Validity  ==  the conjunction holds at every use site
          ==  accepted(p) -> forall reachable c, not bad(c)
```

---

## 5. The target architecture

### 5.1 Level A — the unified product

```coq
(* the five component domains, each already a finite lattice *)
Record astate := {
  a_prov  : provenance;     (* which allocation does this reference?     *)
  a_live  : liveness;       (* is that allocation valid here?            *)
  a_own   : ownership;      (* who may consume it?                       *)
  a_bound : bounds;         (* is the offset in range?                   *)
  a_cap   : capability      (* const / volatile / shared / context       *)
}.

Definition ajoin (x y : astate) : astate := {|
  a_prov  := prov_join  (a_prov x)  (a_prov y);
  a_live  := live_join  (a_live x)  (a_live y);
  a_own   := own_join   (a_own x)   (a_own y);
  a_bound := bound_join (a_bound x) (a_bound y);
  a_cap   := cap_join   (a_cap x)   (a_cap y)
|}.

Definition use_ok (s : astate) : bool :=
  prov_ok  (a_prov s)  &&
  live_ok  (a_live s)  &&
  own_ok   (a_own s)   &&
  bound_ok (a_bound s) &&
  cap_ok   (a_cap s).
```

### 5.2 What makes it FORCED

The forcing is not discipline, documentation, or review. It is the **type of the query**.

Today a sink calls `call_result_escapes(...)` and gets a bool about escape. It does not
consult liveness, and nothing notices. Under Level A the sink calls one query and receives
`astate`. To decide, it must run `use_ok`, which is the conjunction. There is no way to
consult four components and skip the fifth, because there is no per-component entry point
at the sink layer.

A new syntactic form must produce an `astate`. It cannot produce four fifths of one.

This is `-Werror=switch` at the granularity of safety classes: the compiler refuses to
build a sink that has not answered everything.

### 5.3 Level B — the reduced product

Plain product is coarse *precisely because components cannot see each other*. The
reduction step is where the precision comes back:

```
reduce : astate -> astate      (* sound: reduce s <= s, and covers is preserved *)
```

Examples of real reductions available to ZER:
- **bounds refines provenance** — a proven in-range index into a single allocation cannot
  alias a different allocation.
- **provenance refines liveness** — if a reference provably points into allocation A, only
  A's liveness matters; a free of B does not widen it to MAYBE.
- **capability refines ownership** — a `const` reference cannot be the consuming use.
- **guard disjointness refines liveness** — the shipped Level B for handles.

Plus per-class guarded recoveries (§8, §9), added one at a time.

### 5.4 The mapping to the user-facing framing

| framing | formal object |
|---|---|
| "main, over-rejects" | Level A: plain product, componentwise join, conjunction |
| "relaxation below" | Level B: reduce + guarded refinements |
| "forced validity" | `use_ok` is total over `astate`; sinks cannot subset it |
| "100% safe with over-rejection as cost" | `accepted(p) -> forall reachable c, not bad(c)`, precision unconstrained |

"Over-reject at the top, relax below" is not a compromise in this architecture. It is
literally **product-then-reduce**, which is the standard construction.

---

## 6. The five factors

Each already exists as an oracle subset, so the factoring is not hypothetical.

### 6.1 provenance / aliasing

**Question:** which allocation does this reference point into?

**Existing:** `lambda_zer_escape` (param_lattice.v, join_lattice.v), `lambda_zer_opaque`.
Implementation: `is_local_derived`, `is_arena_derived`, `is_nonkeep_derived`,
`Symbol.ret_summary_complete` / `ret_param_mask`, `call_result_escapes`.

**State sketch:** `PUnknown | PStatic | PLocal | PParam(n) | PHeap(id) | PTop`
with `PTop` the conservative default. Merge = join, saturating toward `PLocal` / `PTop`.

**Known weakness:** the view-provenance question ("does a *reference* to this allocation
reach here", as opposed to "does this value escape") has NO oracle and produced 8 forms of
holes. It is the highest-value oracle to write.

### 6.2 liveness

**Question:** is the referenced allocation still valid at this program point?

**Existing:** `lambda_zer_handle` — the most complete subset in the tree, Level A AND
Level B, both shipped. `hstate = HUninit | HAlive | HFreed | HMaybe`.

**This is the template.** See §8.

### 6.3 ownership / linearity

**Question:** who may consume this, and has it already been consumed?

**Existing:** `lambda_zer_move` (6 files). Implementation: `HS_TRANSFERRED`, `move struct`,
keep inference (`check_keep_inference`).

**Known weakness:** `Tok b = *p;` — move via deref — is a recorded open hole (HOLE-A4).
Aliases taken BEFORE a transfer are not registered in the source's state group.

### 6.4 bounds / value

**Question:** is this offset provably in range?

**Existing:** `lambda_zer_bounds/bounds_lattice.v` — oracle exists, including the
`elide_on_join_sound` merge template.

**Known weakness:** the implementation is orphaned (§3.2). This is the single most
mechanical prerequisite in the whole plan: the sound analysis is written, proven-shaped,
and not compiled.

### 6.5 capability

**Question:** const / volatile / shared / execution context — is this operation permitted
here at all?

**Existing:** `lambda_zer_qualifier`, `lambda_zer_volatile`, `lambda_zer_capture`.
Implementation: qualifier tracking, `shared` auto-lock, ISR/`@critical`/async context flags.

**Known weakness:** exemptions. The `volatile` single-word exemption was granted at two
sites with only one testing width. **Every exemption is a multi-site class** and must be
modeled as an explicit precondition, not a shortcut (§9).

---

## 7. Level A, formally

### 7.1 The contract each component must satisfy

Every component domain `D` must supply, and PROVE:

```coq
(* the concrete truth D abstracts *)
Parameter covers : D -> concrete -> Prop.

(* the decision *)
Parameter d_ok : D -> bool.

(* T1 DECISION SOUNDNESS — an allowed use is on a safe concrete state *)
Theorem d_sound : forall s c, d_ok s = true -> covers s c -> safe c.

(* MERGE = JOIN, and the join COVERS both predecessors *)
Parameter djoin : D -> D -> D.
Theorem join_covers_left  : forall a b c, covers a c -> covers (djoin a b) c.
Theorem join_covers_right : forall a b c, covers b c -> covers (djoin a b) c.

(* T4 NO UNDER-REJECTION — a definitely-unsafe predecessor blocks the use *)
Theorem join_unsafe_blocks : forall a, d_ok (djoin a D_UNSAFE) = false.

(* T3 PRECISION WITNESS — name the exact over-rejection this domain closes *)
Theorem <named>_usable : d_ok (djoin D_SAFE D_SAFE) = true.
```

This is the 4-theorem contract from the MAX-ORACLE STANDARD, instantiated. `param_lattice.v`
and `handle_flow_lattice.v` are both worked examples of it.

### 7.2 The product's obligations

Given five components each satisfying the contract, the product obligations are almost
free — which is the point:

```coq
(* product covers = componentwise covers *)
Definition acovers (s : astate) (c : concrete) : Prop :=
  prov_covers  (a_prov s)  c /\ live_covers  (a_live s)  c /\
  own_covers   (a_own s)   c /\ bound_covers (a_bound s) c /\
  cap_covers   (a_cap s)   c.

(* PRODUCT SOUNDNESS — follows componentwise, no new insight required *)
Theorem ause_sound : forall s c, use_ok s = true -> acovers s c -> safe c.

(* PRODUCT MERGE — follows componentwise *)
Theorem ajoin_covers_left : forall x y c, acovers x c -> acovers (ajoin x y) c.

(* PRODUCT NO-UNDER-REJECTION — if ANY component is definitely-unsafe, reject *)
Theorem ajoin_unsafe_blocks :
  forall x, use_ok (ajoin x A_UNSAFE_IN_ANY_COMPONENT) = false.
```

**The product theorems are mechanical.** All the real work is in the component contracts.
That is deliberate: it means the plan's cost is dominated by writing the five component
oracles, and the unification itself is cheap. It also means a weak component cannot be
hidden by the product — `use_ok` is a conjunction, so the product is exactly as sound as
its weakest component and no more.

### 7.3 Why `covers` and not an abstraction function

`covers : D -> concrete -> Prop` (a relation) rather than `abstract : concrete -> D` (a
function) is a deliberate choice already used throughout the existing oracles. It is what
lets a state be deliberately imprecise (`HMaybe` covers both `freed = true` and
`freed = false`) without a lie. An abstraction function would force a single answer where
the analysis genuinely does not have one.

---

## 8. The template — `handle_flow_lattice.v` walked through

**Read the file. 162 lines. This section is a guide to it, not a replacement.**

Both levels exist and both ship. This is the single most important fact in this plan: the
architecture being proposed has already been executed once, end to end, for the hardest
safety class in the language.

### 8.1 Level A in the template

```coq
Inductive hstate := HUninit | HAlive | HFreed | HMaybe.

Definition covers (s : hstate) (freed : bool) : Prop :=
  match s with
  | HAlive  => freed = false
  | HFreed  => freed = true
  | HMaybe  => True          (* deliberately unconstrained *)
  | HUninit => True
  end.

Definition use_ok (s : hstate) : bool :=
  match s with HAlive => true | _ => false end.

Definition hjoin (a b : hstate) : hstate :=
  match a, b with
  | HAlive,  HAlive  => HAlive
  | HFreed,  HFreed  => HFreed
  | HUninit, HUninit => HUninit
  | _, _ => HMaybe                      (* any disagreement widens *)
  end.
```

Theorems, mapped to the contract in §7.1:

| theorem | role |
|---|---|
| `use_sound` | T1 — an allowed use is on a non-freed handle |
| `join_covers_left` / `join_covers_right` | merge soundness |
| `join_freed_blocks_use` | **T4** — a free on ANY incoming branch blocks the use |
| `join_alive_usable` | T3 — alive on all paths stays usable |

`join_freed_blocks_use : forall a, use_ok (hjoin a HFreed) = false` is the formal statement
of "mostly goes MAYBE_FREED". It is the MAYBE_FREED conservatism **certified**, not
assumed. That theorem is what the top layer of this whole plan looks like when written
down.

### 8.2 Level B in the template

```coq
Definition guarded_use_ok (gf gu : world -> bool) : Prop :=
  forall w, gu w = true -> gf w = false.        (* guards are DISJOINT *)
```

| theorem | role |
|---|---|
| `guarded_use_sound` | the refinement is sound |
| `guarded_not_disjoint_rejects` | **no under-rejection** — a real UAF is never accepted |
| `maybe_freed_correlation_recovered` | the **precision witness**: `if(c){free} if(!c){use}` |

`maybe_freed_correlation_recovered` names the exact over-rejection it closes, and proves
BOTH halves: the flat lattice rejects it (`use_ok HMaybe = false`) and the guarded domain
accepts it. That two-sided form is what makes a precision claim checkable.

Implementation shipped 2026-06-27 in `zercheck_ir.c`: per-block immutable-bool guard sets
plus per-handle `free_block` / `freed_all_paths`, applied at the use site, the double-free
site, and the leak check.

### 8.3 What to copy

For each of the other four factors:

1. the finite state set, DERIVED from the operational semantics (not guessed)
2. `covers` as a relation, with the imprecise states genuinely unconstrained
3. the decision predicate
4. `join` with the conservative default, and both `join_covers` directions
5. the T4 no-under-rejection theorem — **the crown jewel; write it first if unsure**
6. a named T3 precision witness

Zero admits. In the `make check-proofs` admit-gate. Written against the SEMANTICS, never
against current C behaviour — a code-driven spec is a tautology that freezes bugs into CI
forever.

---

## 9. The precondition trap (this already bit once)

**Read this before writing any Level B.** It is the dominant failure mode of a relaxation
layer, and the reason a relaxation is the one change class where a bug is a shipped UAF.
Everything else in this codebase tightens, where a bug merely over-rejects.

### 9.1 What happened

Level B's oracle proved "disjoint guards -> safe use", with guards modeled as abstract
`world -> bool` evaluated consistently. Sound *for that model*.

But it omitted a finite variable: **guard STABILITY** — that `c` at the free and `c` at the
use are the SAME value. The free and the use are at DIFFERENT program points. If the
guard's underlying value is mutated in between, "disjoint" is a lie:

```zer
if (c) { free(h); }
c = e;                    // <-- the guard moved
if (!c) { use(h); }       // real UAF, and the model has no variable to notice
```

### 9.2 The diagnostic signature

> **The two accept-unsafe holes were found by RED-TEAM, not by a failing proof — because
> the model had no variable to fail on.**

That is the textbook missing-variable symptom, and it is the tell to watch for. If a
soundness hole in a relaxed class is found by testing rather than by a proof that will not
close, **the abstract domain lacked the distinguishing variable.**

The fix is to ADD the variable to the oracle (climb the oracle), NOT merely to patch the
C. A C patch leaves the oracle still certifying the wrong thing, and the next form of the
same hole still unmodeled.

### 9.3 The rule

> A relaxation `P -> accept` is a MAX oracle only if `P` carries **every** operational
> precondition reality requires — not just the interesting one.

For Level B handles that is **disjointness AND stability**. Enumerate the operational
preconditions of every decision and make each an explicit hypothesis.

### 9.4 The bridge when the oracle cannot yet model it

Where the oracle cannot model a precondition, the C must DISCHARGE it with a **complete**
gate, and the gap must be written down in the oracle file itself.

Level B's stability precondition is discharged by `ir_local_is_immutable_bool` — a
**no-default exhaustive AST walk** (`ast_name_mutated_or_addrd`) that rejects any
reassigned or address-taken condition. The oracle file records that its `world -> bool`
abstraction PRESUMES stability and names where it is discharged.

Note the shape of that gate: **exhaustive AST walk, not IR-field inspection.** Writes and
address-takes hide in AST expressions the flat IR does not expose (`c = e` becomes an
`IR_ASSIGN` inside an expression; `flip(&c)` is a call-arg expression). An IR-field scan is
structurally incomplete and yields accept-unsafe holes. Both original Level B holes were
exactly that: a reassigned param and `&c` in a call arg.

### 9.5 Relaxation build discipline

From `compiler-internals.md` "Sound relaxation (reject->accept)", restated because it is
the operating procedure for the entire Level B phase:

1. **Diagnose the LAYER first.** Over-rejection is almost always a COARSE DOMAIN — the
   theorem is sound, the C is faithful, the lattice lost information at a join. The fix is
   a RICHER abstraction proven sound, never an ad-hoc C patch.
2. **The soundness gate must be a complete exhaustive walk** under `-Werror=switch`,
   conservative on unknown.
3. **Prefer decision-layer recovery over state-layer** when the state layer is the
   fixpoint. Overriding the accept/reject decision through a read-only side channel leaves
   the lattice coarse but keeps convergence untouched and concentrates the risk in ONE
   predicate. Refining the state itself is the higher-risk endgame, not the default.
4. **Build incrementally with a no-behaviour-change checkpoint:** foundation + tracking ->
   verify `make check` GREEN (proves convergence, no regression) -> THEN flip the
   relaxation plus the full negative matrix.

---

## 10. The migration

### 10.1 The boundary is statics vs dynamics, NOT "everything to IR"

A tempting simplification is "migrate everything into `zercheck_ir.c`". That is wrong, and
`compiler-internals.md` warns against the "core = IR" conflation specifically because it
drops the type-system half.

| half | what it is | where it belongs |
|---|---|---|
| **statics** | type resolution, optional-ness, qualifiers, provenance *typing*, distinct unwrapping | AST / typed layer (`checker.c`) |
| **dynamics** | flow: liveness, bounds, ownership state, borrow, reachability | CFG (`zercheck_ir.c` / a successor) |

The five product factors are **dynamics**. Their *typing inputs* are statics.

### 10.2 What actually has to move

From §3.3 — six flow properties currently on the AST with zero CFG presence:

| property | current home | why it must move |
|---|---|---|
| `var_range_count` (VRP) | `checker.c` flat pass | branch-local narrowing leaks past joins -> live OOB under-rejection. Sound CFG version exists, unwired. |
| `is_local_derived` (escape) | `checker.c`, 97 refs | the per-sink patchwork; the semantic question is flow, the implementation is syntax |
| `after_spawn_in_func` | `checker.c` | per-function flag standing in for a reachability property; caused BUG-769 |
| `is_borrowed_by_thread` | `checker.c` | linear statement-order approximation; caused the cross-block borrow hole |
| `atomic_plain_writes` | `checker.c` | the recording half of a whole-program property |
| `branch_depth` | `checker.c` | a hand-rolled CFG-nesting counter — delete it once the CFG owns the borrow |

### 10.3 Migration invariants

- **`zercheck_ir.c` is the sole production analyzer for anything migrated.** `zercheck.c`
  stays a shim. Never add new safety code to the shim.
- **Never call `ir_lower_func` twice on the same AST.** `pre_lower_orelse` destructively
  rewrites `NODE_ORELSE`.
- **`IR_POOL_ALLOC` / `IR_SLAB_ALLOC` / `IR_POOL_FREE` are never emitted** by `ir_lower.c`;
  method detection lives in `IR_ASSIGN` / `IR_CALL` via `ir_classify_method_call_ex`. Do
  not "fix" that absence.
- **Global tracking uses the `IR_GLOBAL_ROOT_ID (-2)` pseudo-root**, and those entries
  always carry `escaped = true` — the exit pass indexes `func->locals[]` only after the
  escaped skip.
- **Argument-precise barrier principle:** anything HANDED to an operation the analyzer
  cannot resolve may be consumed by it — and ONLY what was handed.

### 10.4 The emitter is a SEPARATE axis

A sound checker is not a safe binary. The emitter (core -> C) and GCC (C -> asm) are a
distinct trust axis. Two of this session's defects (bit-slice miscompiles, and arguably
the defer fire count) were **emission** bugs, not checker bugs — the checker was correct
and the generated C was wrong.

**Never conflate "sound checker" with "verified compiler to asm."** The product plan
covers the checker. Emitter correctness is a separate, later, and currently-trusted layer.
The `AST->IR emission diff audit protocol` is the instrument for that axis, not this plan.

---

## 11. Sequencing

Each phase has an **exit criterion**. Do not start the next phase until the current one
meets it. The ordering is chosen so that every phase is independently valuable — if the
plan is abandoned halfway, everything landed so far still improved the compiler.

### Phase 0 — wire `vrp_ir.c` (prerequisite, small, high value)

**Why first:** it is 349 lines, already written, retires a CRITICAL live under-rejection,
and is the first real migration of a flow class onto the CFG. It proves the migration
pattern on the smallest possible surface before anything depends on it.

**Work:** add to the Makefile; reconcile its interface with the flat pass; run both in
parallel behind an env flag and diff their verdicts across the whole test corpus; then cut
over.

**Exit:** `nm zerc | grep vrp_ir` non-empty; the flat-pass scope-leak reproducer rejected;
`make check` green; sink matrix clean.

**Risk:** LOW. Additive; the flat pass can stay until the diff is clean.

### Phase 1 — Level A oracles for the four unoracled factors

**Why now:** pure spec work. No C, no re-architecture, cannot worsen soundness. It goes
first *because it is free* — the only cost is time, and it produces the artifact everything
else is checked against.

**Order** (by measured bug density, §2.3):
1. **view-provenance** — the 8-form family; spans escape x handle; the reduced-product pair
2. **callback reachability** — the 6-form funcptr family
3. **atomic-cell** — whole-program property x per-function flag
4. **defer arming** — per-defer state, not a count

**Per oracle:** the 4-theorem contract of §7.1, `handle_flow_lattice.v` as the template,
zero admits, added to `proofs/operational/_CoqProject` (explicit list, not glob) in the
same commit.

**Exit:** four new `*_lattice.v` files, all in the admit-gate, `make check-proofs` green.

**Risk:** LOW. Nothing ships to users.

### Phase 2 — certify Level A implementations against the new oracles

**Why:** for most factors the coarse behaviour is what already runs. This phase is mostly
*certification of existing behaviour*, plus closing whatever the oracle exposes as missing.

Expect the oracles to expose holes. That is the point — a hole exposed by a stuck proof is
the outcome this whole plan exists to produce, as opposed to a hole exposed by red-team
two months later.

**Exit:** for each factor, the implementation's state set and transfer match the oracle;
new negatives in `tests/zer_fail/` with `// expect-error:`; grids extended with the forms
the oracle names.

**Risk:** MEDIUM. This is where behaviour changes. Every change here TIGHTENS, so a
mistake over-rejects rather than shipping UB.

### Phase 3 — migrate the flow properties to the CFG

**Why after Phase 2:** migrating an uncertified analysis just moves an unknown. Migrating a
certified one is a refactor with an oracle to check against.

**Order:** escape/provenance (largest, 97 refs, most valuable) -> borrow -> atomic-cell.
`branch_depth` is deleted when the borrow moves.

**Exit:** the §3.3 table reads zero in the `checker.c` column for the migrated rows;
`make check` green; all 10 grids green; sink matrix clean.

**Risk:** MEDIUM-HIGH. Large mechanical refactor. Mitigation: one property per commit, with
a no-behaviour-change checkpoint before each cutover.

### Phase 4 — form the product

**Why last among the structural phases:** the product is mechanical (§7.2) once the
components exist and are on the same substrate.

**Work:** the `astate` record, `ajoin`, `use_ok`; the product theorems; then convert sinks
from per-question calls to the tuple query, one sink at a time, keeping the old call as an
assertion during the transition.

**Exit:** no sink consults a single component directly; the sink matrix's 54 cells pass
through the unified query; `use_ok` is the only accept decision.

**Risk:** MEDIUM. Wide but shallow. The old per-question calls are the oracle for the new
ones during transition.

### Phase 5 — Level B, one relaxation at a time, forever

**Never batch these.** Each relaxation is a separate commit with:
- its full precondition set as explicit oracle hypotheses (§9.3)
- a complete exhaustive gate for any precondition the oracle cannot model (§9.4)
- a negative matrix proving no under-rejection
- a named precision witness saying exactly which over-rejection it closes

**Exit:** none. This is the permanent steady state — the payable over-rejection is paid
down indefinitely, and the ceiling is never reached (§4.1).

### What is explicitly NOT in this plan

- **Coq-first on a moving design.** Do not formalize the whole thing before the memory
  model freezes. Still finding holes means not frozen. Phase 1 oracles are per-class
  specs, which is different — they are written against the SEMANTICS and are stable even
  as the C moves.
- **The full core-lambda-ZER + verified desugaring endgame.** This plan is the
  *precondition* for it, not it. The core semantics, the abstraction relation per class,
  and the simulation bridge come after the product exists.
- **Emitter verification.** Separate axis (§10.4).

---

## 12. Anti-patterns

Things that look like this plan and are not.

### 12.1 Five per-class Level As

**The single most likely way to do this wrong.** Level A as it exists today is *per-class*.
Writing five per-class Level As and stopping leaves five independent judgments — the exact
problem being solved. The deliverable is Level A **of the tuple**, with the forcing coming
from the query type (§5.2).

### 12.2 `Validity` as a product factor

See §4.5. There is no join for validity; it is the conclusion.

### 12.3 Modeling on "Rust = lifetime x ownership x annotation"

See §4.4. Rust is one relation; annotations patch inference gaps and are checked; ZER has
no annotations, so the whole burden is on inference.

### 12.4 Chasing forms instead of states

See §2.4 and §4.3. The enumeration does not terminate. Two proofs of that in one session.

### 12.5 Writing the oracle against current C behaviour

A code-driven spec is a tautology that freezes bugs into CI forever. Oracle-driven specs
make a proof failure EXPOSE a compiler bug. If no Level-1 oracle exists for a subsystem,
audit first — you cannot write an oracle-driven spec without an oracle.

### 12.6 Deleting the per-class grids after unification

See §13. They are the discriminating layer and the only thing that has ever found these
holes.

### 12.7 Treating over-rejection as the goal

See §4.2. Soundness is the goal; over-rejection is the cost; payable over-rejection should
still be paid down.

### 12.8 Assuming a coarse component is fine because the product is sound

`use_ok` is a conjunction, so the product is exactly as sound as its weakest component. A
weak component is not hidden by unification — but it is also not *fixed* by it. Forcing the
product stops you forgetting a class; it does not make any component correct.

---

## 13. Verification discipline

The instruments listed in §3.5 found every defect in the 2026-08 sessions. A unified
judgment has **no axis to cross**, so unification without keeping them would simultaneously
inherit the existing holes and remove the mechanism that finds them. That would be the
worst possible ordering.

### 13.1 Keep the grids underneath, permanently

The 10 axis-crossed grids stay. They test the *implementation* against the *oracle's*
enumerated forms. When the oracle names a new form, add its cell in the SAME commit. A new
form with no cell is invisible.

### 13.2 Verify a gate FIRES before trusting it

**A gate that has only ever passed is a script, not a net.** Run every new or modified gate
against a pre-fix build and confirm it fails. Recipe:

```sh
cp checker.c /tmp/checker_fixed.c            # save your work FIRST
git show HEAD:checker.c > checker.c
rm -f checker.o && make zerc >/dev/null 2>&1  # rm the .o: no header deps in the Makefile
./zerc tests/zer_fail/new_negative.zer -o /tmp/n.c 2>&1 | grep -q '<expected substring>' \
  && echo "PRE-FIX rejected -> DOES NOT discriminate" || echo "PRE-FIX accepted -> DISCRIMINATES"
cp /tmp/checker_fixed.c checker.c
rm -f checker.o && make zerc >/dev/null 2>&1
```

### 13.3 A gate can cover the right FEATURE and measure the wrong QUANTITY

Measured 2026-08-10. `test_defer_goto_matrix.c` had 34 cells over exactly the defer/goto
feature and passed BOTH before and after a live double-fire miscompile — because its cells
assert an acquire/release BALANCE, and a balance is invariant to an extra fire that also
acquires. The raw FIRE COUNT discriminated (35/36 pre-fix).

When adding a grid, ask not only "does this cover the feature?" but "does this measure the
quantity the defect changes?"

### 13.4 Negative tests must state WHY

`// expect-error: <substring>` in the first 5 lines. Without it a negative passes on ANY
non-zero exit, so an entire safety rule can be deleted and its negatives keep passing
because some other rule happens to reject the same file. Measured twice on one entry.

Assert the reason the rule is SUPPOSED to give, never paste what it currently prints.

### 13.5 A reconstruction can CONFIRM a hole, never REFUTE one

Measured twice in two days, and it cost two real bugs that were recorded as closed.

If an entry names a reproducer file, fetch it:
`git show origin/claude/<branch>:tests/zer_gaps/<file>.zer`.

A NEGATIVE result from a hand-written reconstruction is not evidence — the reconstruction
may simply have the wrong shape (G3's real form wrote a global BY NAME; the reconstruction
used a pointer-taking helper and was masked by an unrelated rule). Keep such a negative test
BYTE-IDENTICAL to the original and say so in a header comment.

### 13.6 Masking is the dominant measurement failure

ZER has many independent rules over the same programs, so a reproducer often trips a
*stronger* rule first. Measured: 3 of 6 branch reproducers were masked, one of them twice
over. Always read the rejection REASON, never the exit code — and note that
`zerc f.zer -o out.c` exits 0 even on checker errors AND prints a success line, so
non-empty output is not an error.

### 13.7 Keep the ledger current in the same commit as the fix

`docs/limitations.md` drifted for ~3 weeks and accumulated 14 entries that were already
closed. A stale ledger reads as coverage exactly the way a stale gate does. Closing a bug
and updating its entry are two acts and only the first feels like progress.

---

## 14. Risks and open questions

### 14.1 Risks

| risk | severity | mitigation |
|---|---|---|
| Product formed over coarse components; over-rejection becomes unusable | HIGH | Phase 1 oracles include a T3 precision witness each; measure over-rejection on the test corpus before and after Phase 4 |
| Phase 3 migration destabilizes the CFG fixpoint | MEDIUM | one property per commit; no-behaviour-change checkpoint; `MAX_ITERATIONS` already raised to 96 with rationale |
| Level B relaxation ships an under-rejection | **CRITICAL** | §9 in full; never batch; negative matrix per relaxation; the accept-unsafe discipline |
| Grids deleted as "redundant" after unification | HIGH | §13.1; they are the discriminating layer |
| Plan abandoned halfway leaving a hybrid | MEDIUM | phases ordered so each is independently valuable |

### 14.2 Open questions

1. **Is `capability` really one factor or three?** const/volatile, shared/lock, and
   execution context (ISR / `@critical` / async) may not share a lattice shape. Resolve when
   writing its oracle; splitting is cheap, merging later is not.

2. **What is the abstract location?** The product is "per abstract location". ZER has
   locals, globals, heap allocations, Pool/Slab slots, arena regions, interior pointers,
   and compound keys (`(root, ".path")`). The existing `alloc_id` grouping plus the
   `IR_GLOBAL_ROOT_ID (-2)` pseudo-root is the closest existing answer and is probably the
   right basis, but it has not been checked against all five factors.

3. **Does `provenance` subsume `view-provenance`, or are they two factors?** The session
   evidence suggests "does this VALUE escape" and "does a REFERENCE to this allocation
   reach here" behave differently — the pointer-vs-scalar refinement (a pointer field read
   yields a reference, a scalar field read yields a value) is exactly this distinction, and
   it broke `test_modules/move_user` twice when conflated.

4. **How much does the plain product over-reject in practice?** Unknown. Measure on the
   corpus during Phase 4 before committing to the reduction work; the answer determines how
   urgent Phase 5 is.

5. **Where does `keep` inference live?** It is a per-function summary (Model 3) feeding a
   provenance question. Probably an input to provenance rather than a factor, but not
   verified.

6. **Concurrency's closure oracle proves sufficiency, not reachability.** The four-condition
   closure is proven sufficient; nothing proves the compiler's reachability scan finds every
   access. Callback reachability (Phase 1, item 2) is the missing half.

---

## 15. Appendix A — measurements (2026-08-10)

All measured, not recalled. Commands included so they can be re-run.

```sh
# file sizes
wc -l checker.c zercheck_ir.c zercheck.c ir_lower.c vrp_ir.c
#   19702 checker.c
#    7206 zercheck_ir.c
#     156 zercheck.c
#    3993 ir_lower.c
#     349 vrp_ir.c

# the orphan
grep -c vrp_ir Makefile            # 0
nm zerc | grep -ci vrp_ir          # 0

# flow properties: checker.c vs zercheck_ir.c
#   var_range_count        39 / 0
#   is_local_derived       97 / 0
#   after_spawn_in_func    10 / 0
#   is_borrowed_by_thread   7 / 0
#   atomic_plain_writes     6 / 0
#   branch_depth            5 / 0

# oracle inventory
for d in proofs/operational/lambda_zer_*; do echo "$(basename $d) $(ls $d/*.v | wc -l)"; done
#   handle 25 | concurrency 10 | escape 9 | move 6 | opaque 6 | mmio 3
#   typing 2 | bounds 1 | capture 1 | disjoint 1 | qualifier 1 | volatile 1

# instruments
ls tests/test_*_matrix.c            # 10 grids
grep -c '^\s*cell ' tools/sink_matrix.sh   # 54 cells
```

## 16. Appendix B — file map

| path | role in this plan |
|---|---|
| `proofs/operational/lambda_zer_handle/handle_flow_lattice.v` | **the template** — Level A + Level B, 162 lines |
| `proofs/operational/lambda_zer_escape/param_lattice.v` | the 4-theorem contract worked example |
| `proofs/operational/lambda_zer_escape/join_lattice.v` | n-ary JOIN return summary — richer-abstraction example |
| `proofs/operational/lambda_zer_bounds/bounds_lattice.v` | `elide_on_join_sound` — the merge template |
| `proofs/operational/lambda_zer_disjoint/disjoint_lattice.v` | the exceeds-Rust sketch; needs relational VRP |
| `proofs/operational/_CoqProject` | explicit file list — add new oracles here |
| `vrp_ir.c` | Phase 0 — the orphan |
| `zercheck_ir.c` | the CFG analyzer; destination for the flow properties |
| `checker.c` | current home of all five flow properties; keeps the statics |
| `tools/sink_matrix.sh` | 54-cell escape/free sink gate |
| `tests/test_*_matrix.c` | the 10 grids |
| `docs/limitations.md` | the living ledger — keep current in the same commit |
| `docs/compiler-internals.md` | durable architecture; the relaxation methodology |
| `docs/proof-internals.md` | the three levels; Phase 1 extraction recipe |

## 17. Appendix C — glossary

| term | meaning |
|---|---|
| **Level A** | the conservative floor: finite lattice, merge = JOIN, conservative default. Over-rejects by design. |
| **Level B** | the recovery: guarded/relational refinements that pay back payable over-rejection. |
| **plain product** | componentwise join, conjunction at use sites. Sound, coarse. |
| **reduced product** | plain product plus a `reduce` step where components refine each other. |
| **oracle** | a Coq/Iris Level-1 spec of a safety class, written against the SEMANTICS. |
| **MAX oracle** | the richest sound decidable abstraction for a class, not the coarsest that certifies soundness. |
| **T1 / T3 / T4** | decision soundness / precision witness / no-under-rejection. |
| **covers** | the relation between an abstract state and the concrete truths it admits. |
| **forced / payable** | over-rejection that is semantically unavoidable / recoverable by engineering. |
| **sink** | a site that must make a safety decision (store-to-global, return, keep-call, spawn-arg, ...). |
| **masking** | a reproducer rejected by a stronger unrelated rule, so the rule under test never fires. |
| **vacuous test** | a test whose pass condition is weaker than its claim. |
| **statics / dynamics** | type-level judgments (AST) / flow-level judgments (CFG). |

## 18. Appendix D — session evidence log

The defects that motivated this plan, with what each proves.

| id | defect | proves |
|---|---|---|
| BUG-767 | funcptr bound to a LOCAL, passed as spawn arg, evaded the race scan | one question, N sites |
| BUG-768 | 2-hop factory return — the 5th reach form | **enumeration does not terminate**; found BY writing the axis, never reported |
| BUG-769 | atomic-cell plain access not transitive through a helper | a property split whole-program / per-function; the split IS the hole |
| BUG-770 | loop-scoped defer fired N+1 times on the goto-not-taken path | a COUNT cannot represent per-defer arming; also: a 34-cell gate measuring the wrong QUANTITY |
| view-alias x8 | a reference reaching a use site through 8 syntactic forms | provenance x liveness must be one judgment |
| bit-slice x2 | runtime-position UB; compound assign compiled as plain assign | the emitter is a separate trust axis (§10.4) |
| volatile exemption | spawn site fixed, ISR sibling missed | an EXEMPTION is a multi-site class too |
| G3 reconstruction | "did not reproduce in 5 shapes" was wrong | a reconstruction can confirm, never refute |
| goto-defer reconstruction | "closed, both variants" was wrong | same, second instance in two days |

---

## 19. One-paragraph summary for a fresh session

ZER's safety is ~29 independent tracking systems. Every soundness hole found recently came
from that independence — one semantic question answered separately at N sites, or one
property split across two mechanisms, with nothing forcing a site to answer everything. The
fix is a single forced judgment: a product of five factors (provenance, liveness, ownership,
bounds, capability) over a shared abstract state, with componentwise join and a conjunction
at every use site, so a sink cannot consult four components and skip the fifth. That top
layer deliberately over-rejects; a Level B layer of guarded and relational refinements pays
back the recoverable part. This is not a new architecture — Level A and Level B both already
exist, proven and shipped, for the handle/UAF class, in a 162-line Coq file that is the
template. Start by wiring the orphaned `vrp_ir.c`, then write Level A oracles for the four
factors that lack them (bug density is almost perfectly explained by oracle absence), then
migrate the flow properties from the AST to the CFG, then form the product. Keep every
existing grid and gate throughout: a unified judgment has no axis to cross, and those grids
are the only thing that has ever found these holes.
