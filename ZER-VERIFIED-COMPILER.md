# ZER Verified Compiler — Full Architecture & Decision Record (Lean 4 / Tier A)

**Status: ADOPTED ARCHITECTURE, 2026-07-19.** This document is the complete, self-contained
record of the decision to rebuild ZER as a **formally verified compiler written in Lean 4**,
CompCert-style (Tier A: per-pass semantic-preservation proofs), with a **dual C backend**
(mainline GCC/Clang daily, CompCert for certification builds) and a fully analyzed trust chain.

**Why this document exists:** every decision below was reached through a long, evidence-heavy
session (2026-07-19) that included web research, LOCAL empirical verification (building and
axiom-checking iris-lean on this machine; probing Lean 4 CLI behavior in Docker), codebase
measurement, and several rounds of adversarial reasoning about trust. A fresh session — human
or Claude — must NOT re-derive, re-litigate, or re-research any of it from scratch. Read this
document first. Where a claim is empirical, the exact command and result are recorded so it can
be re-verified rather than re-discovered. Where an alternative was REJECTED, the alternative and
the reason are recorded so it is not re-proposed.

Related context elsewhere:
- KernelQ's prover switch to Lean 4 (same day, shared reasoning + shared toolchain):
  `KernelQ/docs/kernelq-pedagogy-goal.md` § "Coq-Synthesize", subsections **LS.0–LS.5**.
- The `kernelq-lean` Docker image (Lean v4.32.0 pinned, works under hard sandboxing):
  `KernelQ/backend/Dockerfile.lean`. iris-lean pins the SAME toolchain version.

---

## Table of contents

- §0  The decision in one page
- §1  ZER today — inventory (code, proofs, sizes)
- §2  The implementation-language question (why not C, OCaml, Rust — why Lean 4)
- §3  The prover question (Coq vs Lean 4 — foundations identical, ecosystems differ)
- §4  Iris-in-Lean — empirical status (verified locally, with commands)
- §5  The certificate principle (P-vs-NP asymmetry; the design shift it forces)
- §6  Assurance tiers A/B/C — and the decision: A primary, B bridge, C retired
- §7  The architecture (whole ZER in Lean 4; strangler migration plan)
- §8  The theorems (Stage 0–4 roadmap; the Grand Theorem)
- §9  The two trust chains — TCB analysis, "fully safe" does not exist, the kernel hatch
- §10 Backends — dual-backend strategy; verified CompCert facts (ISAs, pedigree, licensing)
- §11 The emitter contract (C99, CompCert dialect, freestanding, Clight-friendly)
- §12 Toolchain residue (assembler/linker/libc/startup ranking; porting; mitigations)
- §13 Lean 4 implementation practicalities
- §14 Concurrency proofs (Iris) — plan
- §15 Mission-critical / certification framing (the claim language that survives review)
- §16 Roadmap and first steps
- §17 Rejected-alternatives ledger (the no-reloop table)
- §18 Sources and provenance

---

## §0 The decision in one page

**ZER becomes a verified compiler, written in Lean 4, proven correct per pass.**

1. **Implementation language: Lean 4** — the whole compiler core (checker, IR passes,
   emitter), not just the proofs. One artifact where the theorem is about the code that runs.
   Migrated from the current ~47k-line C core via the strangler pattern, checker first.
2. **Assurance tier: A (CompCert-style verified passes)** — each compiler pass carries a
   machine-checked semantic-preservation theorem; composed into an end-to-end Grand Theorem.
   Tier B (per-build translation validation) is the *bridge* for not-yet-proven passes.
   Tier C (CakeML-style verified self-hosting) is *retired* — the self-hosting dream is
   traded for the proof flag, deliberately.
3. **Certificate-based checking model** — ZER's safety judgment moves from implicit
   whole-program inference toward *infer within functions, certify between them*:
   user annotations at boundaries are the certificates; the proven checker only verifies.
   (Checking is P; finding is undecidable — Rice. Rust's model, adopted knowingly.)
4. **Target: C99, dual backend** — the emitter produces C99 (as ZER already does),
   constrained to CompCert's supported dialect and the freestanding profile.
   Daily builds: latest mainline GCC/Clang. Certification builds: **CompCert**
   (ZER →proven→ C99 →CompCert-proven→ assembly = verified chain to machine code).
5. **Trust chain, stated honestly** — the compiler binary itself carries the standard
   prover-toolchain residual TCB (same shape as CompCert's own), monitored by dual-compiler
   builds + differential testing, and **bypassable per-artifact** by kernel-computed
   certification (`by decide`) for the build that matters.
6. **Concurrency proofs** — the existing Coq/Iris corpus (~870 lines) remains valid; a port
   to iris-lean is empirically feasible today (days-scale) and becomes natural once the
   compiler lives in Lean. Not urgent; recorded in §14.
7. **AMENDED same day (§8.2): SINGLE PROVER — Lean 4 only, end to end.** No Coq/Iris in
   the final architecture. CompCert = reference + interim *standalone* certification
   compiler (its guarantee stands beside ours; no cross-prover theorem composition is
   claimed, so no seam). Endgame = **Stage 5**: ZER's own Lean-verified backend
   (ZER-IR → RISC-V / ARMv7-M direct), making the composed full-pipeline theorem live
   entirely in Lean — the first CompCert-class backend native to Lean.

The one-liner: **proven wherever proof exists; trusted only in a residue that is small,
diverse, watched — and bypassable for the one build that goes to the pad.**

---

## §1 ZER today — inventory (measured 2026-07-19)

### 1.1 The C implementation

Measured with `wc -l` in the repo root:

| Component | Files | Lines |
|---|---|---|
| **Core compiler** | `lexer.c parser.c ast.c types.c checker.c ir.c ir_lower.c vrp_ir.c emitter.c zercheck.c zercheck_ir.c zerc_main.c` | **46,751** |
| — of which, the checker | `checker.c zercheck.c zercheck_ir.c types.c` | **24,267** |
| Peripherals | `zer_lsp.c zer_wasm.c` | 1,844 |
| Everything `.c/.h` in root | | 63,713 |

Interpretation: the checker is ~half the core. It is also the component whose correctness IS
the product — which is why the migration is checker-first (§7). Functional-style rewrites of
this class of code typically shrink it 3–5× (ADTs + pattern matching vs hand-rolled C tagged
unions and manual memory), so the Lean core is expected around 10–15k lines.

### 1.2 The existing proof corpus (Coq)

```
proofs/
├── composition.v
├── model1_handle_states.v … model4_static_annotations.v
├── vst/verif_*.v                      # ~26 files: per-rule verifications of checker rules
│                                      #   (arith, atomic, cast, coerce, comptime, concurrency,
│                                      #    container, context bans, escape, handle state, isr,
│                                      #    mmio, move, optional, provenance, range checks,
│                                      #    stack, variant, zer checks, type kind …)
└── operational/
    ├── lambda_zer_concurrency/        # ~870 lines total, 11 files:
    │   syntax.v semantics.v iris_lang.v iris_state.v iris_wp_heap.v
    │   iris_shared_inv.v iris_shared_specs.v iris_region_join.v
    │   iris_boundary.v iris_concurrency_theorems.v DESIGN.md
    ├── lambda_zer_handle/  (incl. iris_concurrency.v, iris_resources.v)
    └── lambda_zer_move/    (iris_move_theorems.v)
```

What the concurrency proofs actually are (read 2026-07-19): they **instantiate Iris's
weakest-precondition framework over ZER's own operational semantics** — the classic pattern
(`syntax.v` → `semantics.v` → `iris_lang.v` builds a `LanguageMixin`; `ESpawn` emits an
expression into the threadpool of the step relation; locks are invariant-opening proof
devices). There is no library spinlock and no runtime concurrency in the proofs — it is a
transition system plus a logic over all its interleavings. Iris imports used:

```
From iris.base_logic.lib Require Import gen_heap ghost_map invariants.
From iris.program_logic  Require Import language weakestpre lifting.
From iris.proofmode      Require Import proofmode.
From stdpp               Require Import gmap …
```

This import list is the exact checklist used in §4.3 to establish iris-lean portability.

The `vst/` files are evidence of the **model–implementation gap** being fought by hand:
verifying C code against the rules via VST is brutal, which is one of the drivers for moving
the implementation into the prover (§7.1).

### 1.3 The current checking model (and its structural weakness)

ZER's checker today establishes safety **by inference**: it derives the safety facts itself
from the program (multiple analysis classes, checked implicitly against each other), rather
than verifying user-supplied annotations. §5 records why this is on the wrong side of the
checking-vs-finding asymmetry for mission-critical use, and the design shift adopted.

---

## §2 The implementation-language question

### 2.1 History: why ZER is in C, and the regret

C was chosen initially because (a) "ZER should be simple like C, so build it in C," and
(b) the bootstrap romance — C descends from B; building a systems language in C felt like
joining that lineage. Both reasons dissolved under examination:

- "Simple like C" is a surface truth that inverts for compilers: a compiler is
  allocation-heavy, recursion-heavy, pointer-graph-heavy (ASTs, symbol tables, IR).
  C's "simplicity" means carrying all of that by hand. For this *specific* workload,
  a GC'd/managed functional language is simpler *to write correctly*, not harder.
- The bootstrap romance is not C-exclusive (OCaml self-hosts; Rust bootstrapped from OCaml;
  Lean 4 self-hosts), and the stronger flag for a memory-safety language was always
  self-hosting in ZER itself — which is itself later traded for an even stronger flag (§6).
- The founder did not know, when choosing C, that OCaml-class languages eliminate the
  memory-vulnerability classes wholesale. Knowing it, C became a *choice* rather than a
  default — and then a choice to move away from.

The cobbler's-children tension (a memory-safety tool written in a memory-unsafe language) is
real but was never fatal: ZER-written-in-C still produces safe output, because the guarantee
lives in ZER's design, not its implementation language. What C costs is ZER-the-tool's own
robustness and, decisively, any possibility of machine-checked implementation correctness.

### 2.2 The options considered

| Option | Memory safety | Provability of the implementation | Verdict |
|---|---|---|---|
| Stay C | ✗ | ✗ (VST-style verification of C is brutal — see `proofs/vst/`) | Rejected as end-state |
| OCaml | ✓ | ✗ (proofs live elsewhere; extraction gap) | Good tool language; superseded |
| Rust | ✓ | ✗ (borrow checker ≠ theorems; graph-shaped compiler data fights the borrow checker — arenas/indices) | Good tool language; superseded |
| **Lean 4** | ✓ (RC runtime, compiles via C) | **✓ — the theorem is about the code that runs** | **ADOPTED** |

The decisive property is the last column. In every split architecture (implementation in X,
proofs in a prover about a *model* of X), the model–implementation gap is unverified: the
hand-transcription of the rules into `checker.c` is exactly where correctness leaks, and
exactly what the VST effort was fighting. When the checker is *written in Lean*, the function
you run **is** the object you prove:

```lean
theorem checker_sound : check p = true → Safe p   -- about the ACTUAL shipped code
```

That is CompCert-class assurance ("verified implementation"), not "verified model."

### 2.3 What Lean-hosted does NOT mean

Writing ZER in Lean does **not** obligate proving everything. Unproven Lean (using
`partial def` where termination proofs are not worth it) is simply a memory-safe,
OCaml-grade implementation — and every component can be *upgraded to proven* incrementally,
in place, in the same language. The strategic value is the **option** to prove any part,
exercised on the roadmap's schedule (§8), not a day-one obligation.

---

## §3 The prover question — Coq vs Lean 4

### 3.1 Foundations: identical

Both are the Calculus of Inductive Constructions (dependent types + inductive types).
**Expressiveness is identical**: anything provable in one is provable in the other; the
theories are of the same logical strength (each models the other modulo mild axioms; both
consistent relative to set theory). There is no theorem ZER will ever need that one prover
can state and the other cannot. Encoding differences exist but are not expressiveness gaps:
Coq has primitive coinduction (Lean re-encodes via quotients); Lean has primitive quotients
(Coq encodes via setoids); universe cumulativity differs. "Expressed differently," never
"inexpressible."

### 3.2 The governing principle: capability in the kernel, convenience in libraries

**Iris is not a Coq primitive.** It is ~200k lines of ordinary Coq *library* — separation
logic DEFINED as step-indexed predicates over resource algebras, every rule PROVED as a
theorem, the proof mode being tactic-layer sugar. No prover anywhere has concurrency-proof
primitives, and that is a *feature*: tiny trusted kernel, everything above it derived.
Symmetrically, Mathlib is ordinary Lean. The provers differ ONLY in which libraries are
built out:

- Coq: mature Iris, CompCert, VST, Software Foundations; narrower math.
- Lean 4: mature Mathlib (where formalized mathematics is consolidating), industrial
  verified software (AWS Cedar in production, SampCert, Veil, lean-mlir), a funded FRO;
  Iris at ~69% ported (§4 — more usable than that number suggests).

**Proof-by-existence:** iris-lean is written in PLAIN Lean 4 — zero new primitives, zero
kernel extensions, zero new axioms. Its existence constructively proves that Lean 4
expresses full Iris.

The one-liner that governs every prover comparison: **never "can it be done" — always
"who already built the library, and how much porting do I pay."**

### 3.3 Why Lean 4 for ZER specifically

1. **One language for programs and proofs, no extraction gap.** Coq's path to an executable
   is extraction to OCaml (unverified extraction + unverified OCaml compiler in the TCB
   anyway — see §9). Lean compiles to C natively; the definitions you prove are the
   definitions you compile.
2. **Programmer ergonomics for a compiler**: ADTs, pattern matching, `partial def` (no
   totality fights for worklist algorithms), tactics written in Lean itself, do-notation.
3. **Ecosystem trajectory** (see LS.2 in the KernelQ doc): math and new industrial
   verification are consolidating on Lean; funding asymmetry (Lean FRO vs a 2-engineer
   Rocq Consortium) is the strongest leading indicator.
4. **Shared toolchain with KernelQ**: the founder's Lean training track (KernelQ synthesize)
   and ZER's proofs now use one prover, one skill ramp, one Docker image family. The KernelQ
   ramp is literally the training program toward ZER's simulation proofs.
5. The one thing Coq holds that Lean lacks — *mature* Iris — binds ZER's concurrency
   proofs, not the compiler; and §4 shows even that gap is nearly closed for a corpus of
   ZER's size.

### 3.4 What stays Coq

The existing proof corpus (§1.2) remains valid Coq and is not deleted. The λ-ZER
formalization gets rebuilt/ported in Lean as Stage 0 (§8) because the compiler now lives
there; the Iris concurrency corpus ports when convenient (§14). Nothing is stranded —
both directions of the Coq↔Lean street are open at translation cost.

---

## §4 Iris-in-Lean — empirical status (verified locally, 2026-07-19)

### 4.1 Warning: do not trust 2024-era write-ups or LLM training data

Status pages from 2024 (and LLM knowledge derived from them) say iris-lean is "mainly a
formalization of the MoSeL frontend" with **no program logic, no adequacy, never
instantiated**. That was true in 2024 and is **FALSE now**. (During the session, a
context-free Claude instance confidently repeated the stale claim — "the WP+adequacy tower
is what iris-lean hasn't finished porting" — and was refuted by the local build below within
the hour. Verify against the repository, not memory.)

### 4.2 What was verified, with commands

Repository: `github.com/leanprover-community/iris-lean` (very active: commits within days,
releases tracking Lean versions, invited talk at Iris Workshop 2026, Mario Carneiro among
top contributors; porting tracker ~69% of Rocq-Iris definitions).

**Build (Lean kernel re-checks the entire concurrency tower):**

```
git clone --depth 1 https://github.com/leanprover-community/iris-lean.git
# inside the kernelq-lean Docker image (deps: Qq + batteries; git must be installed):
cd iris-lean/Iris && lake build Iris.HeapLang.Lib.SpinLock
# RESULT: 201/201 modules built, 0 errors, 0 sorry-warnings.
# Chain: base logic → cameras → proof mode → invariants → WeakestPre → ThreadPool
#        → Adequacy → HeapLang → SpinLock
```

**Axiom oracle (the un-fakeable check — same oracle KernelQ uses on students):**

```lean
import Iris.HeapLang.Lib.SpinLock
open Iris.HeapLang
#print axioms SpinLock.newlock_spec
#print axioms SpinLock.try_acquire_spec
#print axioms SpinLock.acquire_spec
#print axioms SpinLock.release_spec
-- ALL FOUR: depends on axioms: [propext, Classical.choice, Quot.sound]
-- (the three standard Lean axioms; NO sorryAx anywhere in the dependency chain)
```

Meaning: a complete concurrent-lock verification — mutual exclusion via invariants, under a
weakest-precondition logic with threadpool adequacy — **kernel-checks in Lean 4 today**.

Also verified: iris-lean pins `leanprover/lean4:v4.32.0` — the same toolchain as the
`kernelq-lean` image. The static `sorry` grep over the whole library finds only
comment-enclosed occurrences (zero live).

### 4.3 ZER-import mapping (every facility ZER's proofs need, present)

| ZER's Coq import | iris-lean counterpart | Verified how |
|---|---|---|
| `program_logic` `language`/`weakestpre`/`lifting` | `Iris/ProgramLogic/{Language, EctxLanguage, EctxiLanguage, WeakestPre, Lifting, EctxLifting, Adequacy, ThreadPool}` | listed + built |
| `base_logic.lib.invariants` | `Instances/Lib/Invariants.lean` (+ `CInvariants`, `NaInvariants`) | listed + built |
| `base_logic.lib.gen_heap` | `BI/Lib/GenHeap.lean` (used by HeapLang's own `PrimitiveLaws`) | import chain read |
| `base_logic.lib.ghost_map` | `Instances/Lib/GhostMap.lean` | listed |
| `proofmode` | `Iris/ProofMode` (MoSeL port: `istart`, `iintro`, `iframe`, `imod`, …) | built + used in SpinLock |
| `stdpp` `gmap` | `Std.ExtTreeMap` + `Algebra/{Heap,HeapView}` cameras | listed |

Note: the porting tracker's "gmap/gmap_view **0%**" is misleading — the same machinery was
**rebuilt under different names** rather than ported file-by-file. There is no missing wall.

**Conclusion:** a port of ZER's 870-line concurrency corpus to iris-lean is a days-scale
*structural translation* (not a copy — syntax and tactic names differ). ZER's use of
Coq/Iris is a **preference** (maturity, existing fluency), not a constraint. See §14.

---

## §5 The certificate principle — the design shift in ZER's checking model

### 5.1 The asymmetry (the deepest principle in the field)

**Checking a given answer is P; finding the answer is NP-hard at best — and for semantic
properties of programs, undecidable (Rice's theorem).** Every serious verification system is
built around this asymmetry:

- Proof assistants: humans/tactics FIND the proof; a tiny kernel only CHECKS it.
- Rust: the function signature (lifetimes, ownership) is a **certificate the user supplies**;
  the borrow checker verifies each function *locally* against the signatures of what it
  calls — never whole-program inference. Fast, modular, predictable, errors local.
- Proof-carrying code (Necula): ship the certificate with the program; the consumer runs
  only the cheap checker. Invented for mission-critical mobile code.
- Conversely: a static analyzer over C/C++ is a bug-FINDER on the undecidable side — hence
  unsound (Coverity-class, misses bugs by design) or subset-restricted (Astrée-class).
  This is why "extreme analyzer on C++" is *structurally* weaker than a safe language:
  analyzers fight the language; certificates are checked by it.

### 5.2 Applied to ZER

ZER's current implicit-inference checking does the FIND. It can be sound-but-conservative
(safe!), but it inherits the finder's curse: heuristic cliffs, unpredictability, poor error
locality, and — decisive for mission-critical — **near-impossibility of proving the checker
itself sound**, because the object to prove is a search heuristic rather than a finite rule
set. The mission-critical sin of inference is not danger; it is **illegibility and
unprovability**.

**Adopted design shift: infer WITHIN functions, certify BETWEEN them.**

- Inside a function body: inference remains free (Rust does the same).
- At function/module boundaries: user annotations (ownership, region, handle-state,
  escape contracts — ZER's existing vocabulary) become the **certificates**. The checker
  *verifies* them modularly: each function checked against the annotations of what it calls.
- Consequences: (a) the safety judgment becomes a finite rule set over annotations —
  exactly the shape that Stage 1's `checker_sound` theorem can be proven about;
  (b) checking is polynomial, deterministic, reproducible; (c) errors localize to the
  failing annotation; (d) the annotation IS readable intent — IV&V reviewers audit the
  certificate, not the search; (e) separate compilation becomes natural.
- Precedent for the proof step: Rust's model was proven sound *post-hoc* by RustBelt —
  in Iris. ZER does it with the model designed for proof from the start, which is easier.

This shift is worth more to the NASA-grade story than any implementation-language decision;
it is what makes Stage 1 tractable at all.

---

## §6 Assurance tiers — the decision

Two distinct theorems live in a compiler:

1. **Checking correctness** — `check p = true → Safe p` (the safety judgment is right).
2. **Translation correctness** — `compile p = c → c behaves as p` (semantic preservation).
   A buggy emitter can take a *safe* source program and emit *wrong* code. This is real:
   Csmith-class fuzzing found 300+ miscompilation bugs in GCC/LLVM — and famously **zero**
   in CompCert's verified middle-end.

The tiers for obtaining theorem 2, and the decision:

| Tier | What | Cost | Decision |
|---|---|---|---|
| **A — verified passes** (CompCert-style) | Each pass written in Lean with a per-pass semantic-preservation proof; composed end-to-end | CompCert ≈ 100k lines Coq, team, ~decade — but §6.1 explains why ZER's instance is far smaller | **ADOPTED — primary** |
| **B — translation validation** (per-run) | Unproven compiler emits a correspondence witness per build; a small proven validator checks source↔output equivalence for that artifact | Small validator; per-artifact assurance | **ADOPTED — as the bridge**: covers passes not yet proven, and optimizations, until their once-for-all proofs land. (Precedent: CompCert itself *validates* rather than verifies register allocation; seL4 validates its binary.) |
| **C — verified self-hosting** (CakeML-style) | Compiler written in ZER, deeply embedded in the prover, every pass proven, bootstrapping itself into a verified binary | The hardest artifact in the field (CakeML: team, ~decade) | **RETIRED.** The self-hosting flag is traded for the proof flag — for a safety company, "our compiler is proven" beats "our compiler compiles itself," and it is the claim certification authorities can use. ZER-in-ZER survives only as an option for unproven periphery (LSP, tooling), never for the core. |

Note the permanence consequence, accepted with eyes open: **proofs are about specific
code, so every proven pass lives in Lean permanently.** Proofs never live *in* the running
language — they live in the prover, *about* the code (even CakeML's theorems live in HOL4).
"Assurance in the compiler" always means "the compiler stays within the prover's reach."

### 6.1 Why Tier A is far more tractable for ZER than it was for CompCert

1. **CompCert's hardest problem was C's semantics** — decades of legacy weirdness to
   formalize before proving anything. **ZER owns its own semantics** and can *co-design*
   the language with its formalization: when a construct is hell to prove, the construct
   can change. CompCert never had that power. λ-ZER already exists (§1.2).
2. **ZER emits C, not assembly.** The heroic backend passes (register allocation,
   instruction selection, scheduling) are simply not in the pipeline; they are delegated
   to the C backend (§10). The proof surface is: source → IR → C. A much shorter chain.
3. **A safety-first source language has simpler semantics** than a legacy one; the
   safety judgment (Stage 1) and the preservation proofs (Stage 2) share the same
   formal substrate.
4. **Per-pass staging ships value at every stage** (§8): a verified checker alone is
   already a product.

### 6.2 The cost of A, stated honestly

- **Development velocity changes character permanently.** Every change to a proven pass
  requires re-proving it. CompCert evolves slowly *by design*; that is what
  mission-critical maturity looks like. Iteration speed is traded for the right to make
  the strongest claim in the field.
- **The daily proof work is forward-simulation arguments** ("each source step is matched
  by target steps preserving a relation") — a real skill jump from arithmetic lemmas.
  The KernelQ Lean ramp is the deliberate training path toward exactly this.
- **Proof-to-code ratio** historically runs ~3–10× per pass. With an expected ~10–15k-line
  Lean core, the mature proof corpus will be the large majority of the repository. That is
  normal for this class of artifact.

---

## §7 The architecture

### 7.1 Whole ZER in Lean 4 — what it buys

- Kills the model–implementation gap (§2.2): `checker_sound` is about the shipped function.
  The VST effort (verifying C against the rules) becomes unnecessary — the rules and the
  implementation are the same Lean definitions.
- Memory safety of the tool itself (the original C regret, resolved).
- The proofs and the program grow in one artifact; any component upgrades from
  unproven-but-safe to proven without changing language or repo.

### 7.2 Migration: strangler pattern, checker-first

Big-bang rewrites of ~47k working lines are the classic self-inflicted wound. The pipeline
has natural process-level seams; migrate along them, keeping a working system at every step:

```
Phase M1 (checker):    C frontend parses → dumps AST/IR (file/JSON)
                       → LEAN CHECKER validates → C backend continues.
                       Ports checker.c + zercheck*.c + types.c (~24k C → ~5–8k Lean).
                       This is the crown jewel: the component where provability pays first.
Phase M2 (IR+emitter): ir.c / ir_lower.c / vrp_ir.c / emitter.c move into Lean,
                       one pass at a time, each arriving with (or ahead of) its proof.
Phase M3 (frontend):   lexer/parser last (or never — parsing can stay C behind a
                       validated AST dump; CompCert itself validated its parser).
Keep in C longest:     zer_lsp.c, zer_wasm.c (~1.8k) — the WASM story via Lean's C output
                       + emscripten is the roughest edge; do not let the tail block the dog.
```

Interop notes: Lean 4 compiles to C; Lean-emitted C links with ZER's existing C at the
object level (`@[extern]` for C→Lean calls). "Main pipeline in Lean + C periphery as
libraries" needs no process boundaries once M1's file-based seam is retired.

### 7.3 The pipeline (target state)

```
ZER source
  → [Lean] lexer/parser            (unproven initially; AST well-formedness validated)
  → [Lean] CHECKER                 (PROVEN, Stage 1: certificate verification, §5)
  → [Lean] AST → IR lowering       (PROVEN, Stage 2)
  → [Lean] VRP / analyses          (proofs only where they license transformations)
  → [Lean] IR → C99 emission       (PROVEN, Stage 2; dialect contract §11)
  → emitted C99
      → daily:        latest mainline GCC/Clang
      → certification: CompCert → assembly   (verified chain, §10)
```

---

## §8 The theorems — Stage 0–4 roadmap

| Stage | Artifact | Theorem (shape) |
|---|---|---|
| **0** | **Semantics in Lean**: λ-ZER source semantics; IR semantics; target C99-subset semantics | (definitions — the ground truth everything refers to; port/rebuild of the Coq λ-ZER) |
| **1** | **Verified checker** | `theorem checker_sound : check p = true → Safe p` — soundness of the certificate-checking judgment (§5.2) against λ-ZER semantics |
| **2** | **Verified passes**, one at a time | per pass, forward simulation: `theorem lower_correct : Safe p → SemIR (lower p) ≼ SemZER p` ; `theorem emit_correct : SemC (emit ir) ≼ SemIR ir` |
| **3** | **Composition** | the ZER Grand Theorem: `check p = true ∧ compile p = some c → MemSafe c ∧ SemC c ≼ SemZER p` — "compiled programs are memory-safe and mean what the source means" |
| **4** | (later) Retarget emitter to **Clight**, compose with CompCert's own theorem | verified from ZER source to machine code; TCB shrinks to CompCert's residue + Lean's (§9) |

Notes:
- `≼` is behavioral refinement (target behaviors allowed by source), the CompCert-style
  statement; strengthen to bisimulation where determinism allows.
- Analyses (VRP) need correctness proofs **only where their results license
  transformations**; a pure-diagnostic analysis can stay unproven indefinitely.
- Stage 1 alone is a shippable, review-worthy product ("the safety judgment is
  machine-checked against the formal semantics"). Ship it before starting Stage 2.
- Until a pass's Stage-2 proof lands, Tier B covers it per-build (§6): the pass emits a
  witness; a small proven validator checks the instance.

### §8.1 Precedent — and the cross-prover seam at Stage 4 (recorded 2026-07-19)

**Precedent found:** SJTU-PLV maintains `github.com/SJTU-PLV/CompCert` branch
`rust-verified-compiler` — a verified Rust(-subset) compiler built as a **frontend on
CompCertO** (their compositional CompCert): `Rustsurface (OCaml) → Rustsyntax → Rustlight
→ RustIR → Clight → CompCert backend`, in Coq (8.12), with a semantic-preservation theorem
(`transf_rustlight_program_correct` in `driver/Compiler.v`). Status at reading: drop
elaboration fully verified (`ElaborateDropProof.v`), lowering to Clight verified
(`Clightgenproof.v`), move checking partially verified, **Polonius-based borrow checking
still "working on"**; x86-64 only; active research project, ~4.3k commits on the branch.

**What it validates for ZER (this is the closest existing artifact to this document's
plan):** an ownership-semantics safety language, compiled by a verified frontend that
lowers to Clight and inherits CompCert's backend theorem — `Rustlight`/`RustIR` are
precisely the λ-ZER / ZER-IR analogues. The Stage-2/Stage-4 shape is not speculative;
a university group is building exactly it, at effort well below CompCert-scale.

**What it confirms about §5 (the certificate decision):** their hardest, still-unfinished
component is verifying Polonius-based borrow *inference* — i.e., verifying a **finder**.
ZER's certificate model verifies a **checker** (annotations at boundaries, finite modular
rules) — deliberately the tractable side of the same problem. Their open struggle is
empirical support for that design choice.

**The seam it exposes in OUR plan (previously implicit — now recorded):** SJTU gets a
*mechanical, single-prover* end-to-end theorem because everything lives in Coq inside
CompCert. ZER's proofs live in Lean; CompCert's live in Coq. Therefore Stage 4's "compose
with CompCert" is **not** mechanical theorem composition: the interface semantics
(Clight / the C99 subset) would be formalized twice — once in our Lean target semantics,
once in CompCert's Coq — and their agreement is an **argued-and-tested link, not a
machine-checked one**. Position adopted:

1. **Engineer the seam small** — this is exactly what the §11 emitter contract already
   does: a tiny, UB-free, fully-defined C dialect leaves the two formalizations no room
   to disagree. The seam is then a documented TCB item alongside §9's residue, of the
   same character (small, inspectable, testable).
2. **Escape hatch if the seam ever matters**: implement the Stage-4 emission pass and its
   proof *in Coq inside a CompCert fork* (SJTU-style) while the rest of the frontend stays
   Lean; the seam then moves up to the Lean-IR ↔ Coq-IR boundary, where per-artifact
   validation of a small IR is straightforward.
3. The fully-seamless alternative — building all of ZER inside CompCert's Coq — is the
   "all-Coq" path already rejected for the §3 ecosystem reasons; the seam is part of the
   Lean bet's price, accepted with eyes open (ledger entry added to §17).

Also noted: their **CompCertO** base (open-module / compositional correctness) is the
right reference point when ZER later needs *linking* theorems (multi-unit programs,
separate compilation of certified modules).

### §8.2 DECISION AMENDMENT (2026-07-19, same day) — single-prover Lean 4, end to end

Confronted with the §8.1 seam, the decision is to **eliminate it at the root rather than
engineer around it: the ENTIRE verified pipeline is Lean 4, single prover, no Coq/Iris
anywhere in the final architecture.** SJTU-PLV's work is a *reference architecture*
(pipeline shape, IR design, proof structure), not a dependency.

Consequences, precisely:

1. **CompCert is repositioned**: reference material + *interim pragmatic certification
   compiler*. Using CompCert as the cert-build C compiler requires NO Coq work on our
   side and creates NO seam — because no cross-prover theorem composition is claimed;
   CompCert's guarantee stands on its own next to ours ("our C is proven-correct output;
   their compilation of it is proven-correct separately"). The seam only ever existed for
   the *composed single theorem* — which is now deferred to Stage 5 instead.
2. **Stage 5 (new endgame): ZER's own Lean-verified backend** — ZER-IR → ISA directly
   (RISC-V first, then ARMv7-M), replacing the need for CompCert composition entirely.
   Notes that size this honestly:
   - "Porting CompCert to Lean" is a **design-guided rebuild**, not a translation — no
     production-grade Coq→Lean proof translator exists at that scale. CompCert's papers
     and structure are the map; the proofs are re-done natively.
   - ZER does **not** need CompCert's scope. CompCert's crown burden was *all of C99's
     semantics*; ZER's backend consumes ZER-IR (small, UB-free, ours) and targets 1–2
     ISAs. The rebuild is a small fraction of CompCert.
   - For cert builds, the verified path can go **direct to assembly** (ZER-IR → asm in
     Lean) — the C middle-man existed for portability and CompCert composition; C99
     emission remains the daily/portable path (GCC/Clang), unchanged.
   - **ISA semantics substrate is emerging in Lean**: `opencompl/sail-riscv-lean`
     translates the official Sail RISC-V spec into Lean (full coverage, type-checks;
     Sail's Lean backend still WIP/unreleased as of 2026-07). Same group as lean-mlir.
     If matured, the target-semantics half of Stage 5 comes largely for free.
   - Novelty, stated plainly: no CompCert-class verified backend native to Lean exists
     yet (per the 2026-07 ecosystem research). Stage 5 would be the first — an
     opportunity (flagship artifact, community/FRO interest) and a risk (no in-prover
     prior art; CompCert's design is the only map).
3. **Concurrency proofs**: the single-prover decision upgrades §14's "port when
   convenient" to "iris-lean is the designated home" — the Coq/Iris corpus ports at the
   §14 trigger and Coq is retired from ZER entirely at that point.
4. The §8.1 escape hatch (emission pass in a Coq/CompCert fork) is **superseded** — kept
   in the ledger only as a fallback if Stage 5 stalls AND a composed theorem is demanded
   sooner.

Sequencing is unchanged where it matters: **Stages 0–3 are untouched** (semantics,
`checker_sound`, per-pass proofs, Grand Theorem to C99). Stage 5 is the endgame after
them; the dual-backend strategy (§10) covers everything until it lands.

---

## §9 The two trust chains — the honest TCB analysis

This section is the answer to "is it fully safe?" — and the first thing a hostile reviewer
will probe. Two separate chains, with different status:

```
CHAIN 1 — the flight artifact:
ZER source ──(checker: PROVEN)──(passes/emitter: PROVEN)──▶ emitted C99
   emitted C99 ──(CompCert: PROVEN — their small TCB)──▶ flight binary
   STATUS: closed by Stages 1–3 (+ CompCert), i.e. proven end-to-end
           [daily builds swap the last link for GCC/Clang — trusted, see §10]

CHAIN 2 — the compiler binary itself:
Lean definitions (theorems PROVEN, kernel-checked)
   ──(Lean compiler: UNVERIFIED)──(C compiler: UNVERIFIED)──▶ zerc binary
   STATUS: residual TCB — trusted, not verified
```

### 9.1 Chain 2 honestly stated

The theorems are about the Lean *definitions*; the running `zerc` binary is produced by
Lean's unverified compiler plus an unverified C compiler. If that toolchain miscompiled the
checker, the binary could deviate from the proven function — theorems intact. **"We check
the whole semantics" is true of the mathematics and NOT of the executable.**

This is the **universal residual TCB** — CompCert itself has the same structure (proofs in
Coq; executable via unverified extraction + unverified OCaml compiler) and ships to Airbus
that way, certified. Every verified system terminates in trust:

- CompCert trusts: Coq's kernel, extraction, the OCaml compiler, the assembler, hardware.
- CakeML (the only system to close the compiler link, via in-prover bootstrap — our
  retired Tier C) still trusts: HOL4's kernel, and hardware-model-vs-silicon.
- seL4 trusts: hardware, and that its spec captures "correct."
- ZER trusts: Lean's kernel, Lean's compiler, one C compiler, hardware.
- Lean's kernel itself is trusted, not proven-by-something-else — the regress must stop
  (mitigated by being small and by independent re-implementations cross-checking it).

**"Fully safe" is not a state that exists — for anyone.** The floor is always trust;
engineering is making the trusted part small, simple, inspectable, and diverse.

### 9.2 Why the chain-2 residue is acceptable, and its mitigations

- The dangerous failure mode is not "a compiler bug" — it is a *silent* miscompilation
  that *precisely* inverts a checked property (accept↔reject) without crashing. Random
  toolchain bugs overwhelmingly crash or produce grossly wrong output caught instantly.
- **Mitigations (cheap, adopted):**
  - build `zerc` with two different C compilers; cross-check outputs on corpora;
  - **differential testing against the in-prover reference**: `#eval` runs the checker
    *inside Lean* (interpreter path), bypassing the native pipeline; divergence from the
    binary = toolchain bug caught. Testing *bounds* this risk; it does not prove absence
    (Dijkstra; the §5 asymmetry — testing samples, proof covers).

### 9.3 The kernel-computation escalation hatch (per-artifact chain-2 bypass)

For the build that matters most, Lean allows the check to be established **by the proof
kernel itself**, not by any compiled binary:

```lean
theorem flight_build_safe : check flightProgram = true := by decide
-- (or native-free kernel reduction / rfl-style evaluation)
```

The result is then a *kernel-checked theorem*: Lean's compiler and the C compiler drop out
of the trust chain for that artifact; the residue is kernel + hardware. Honest caveat:
kernel evaluation is slow — feasible for moderate inputs, potentially painful for very
large programs; it is the **per-artifact certification hatch** (used once, for the final
flight build), not the daily path. This hatch is something CompCert's users do not
conveniently have, and it matches how certification actually works (per-artifact).

### 9.4 The claim language that survives review

> Chain 1 (source → emitted C → binary via CompCert): **proven end-to-end.**
> Chain 2 (Lean definitions → `zerc` binary): **trusted residue** — same shape as
> CompCert's own — *monitored* by dual-compiler builds and differential testing against
> the in-prover reference, and **eliminable per-artifact** by kernel-computed
> certification of the final build.

---

## §10 Backends — dual-backend strategy + verified CompCert facts

### 10.1 The strategy

- **Daily backend: latest mainline GCC/Clang.** ZER already emits C99; developers compile
  with current toolchains, full speed, zero licensing friction. Trust status: chain-1's
  last link becomes "trusted compiler," acceptable for development and non-certified use,
  optionally hardened per-build by Tier B validation.
- **Certification backend: CompCert.** For certified builds, the same emitted C99
  (constrained to the CompCert dialect, §11) is compiled by CompCert, closing chain 1
  end-to-end with proofs. Customers running DO-178C programs bring their own CompCert
  license (they are buying qualification material anyway; the license is a rounding error
  in a certification program). ZER itself stays unencumbered.

### 10.2 CompCert facts (verified 2026-07-19 against compcert.org / AbsInt / the repo)

**Backends (from `AbsInt/CompCert` `configure`, master):**

| Target | Variants |
|---|---|
| ARM 32 | `armv6`, `armv6t2`, `armv7a` (default), `armv7r` (Cortex-R), **`armv7m` (Cortex-M3/M4/M7)** — each `-eabi`/`-eabihf`/`-linux`, plus big-endian `armeb*` mirrors |
| AArch64 | `aarch64-linux`, `aarch64-macos` |
| PowerPC | `ppc-eabi`, **`ppc-eabi-diab`** (Wind River Diab — the VxWorks/avionics toolchain), `ppc-linux`, `ppc64-*`, `e5500-*` |
| RISC-V | `rv32-linux`, `rv64-linux` |
| x86 | `x86_32-{linux,bsd}`, `x86_64-{linux,bsd,macos,cygwin}` |
| AURIX/TriCore | **commercial edition only** (on AbsInt's page; not in the open repo) |
| Gaps | Cortex-M0/M0+ (ARMv6-M), DSPs, anything exotic → fallback: GCC path (+ Tier B) |

The supported map ≈ the mission-critical embedded map (Cortex-M/R, PowerPC-with-Diab,
RISC-V) — because avionics is CompCert's market.

**Pedigree (current):** qualified on the **ATR 42/72 aircraft (2026)** with certification
credits under **DO-178C, DO-333, DO-330**; **IEC 60880 Category A** (nuclear) and
**IEC 61508 SIL-3** qualification (MTU, 2017); the Airbus lineage throughout. Actively
maintained: v3.16 (Sept 2025; PIC/PIE on x86-64/AArch64/RISC-V), AbsInt release 26.04
(April 2026), manual at v3.17.

**Licensing:** free for research/education; **commercial use is a paid AbsInt license**
(INRIA-licensed). This is exactly why the dual-backend strategy exists — ZER must not
hard-depend on CompCert.

**Performance:** ≈ GCC `-O1` class. Not a loss for certified code: certification shops
often *reduce* optimization for traceability anyway; CompCert provides modest optimization
*with a proof*.

**Boundary of CompCert's proof:** it ends at assembly generation. An external
preprocessor, assembler, linker, and C library are required (vendor toolchain — binutils
or Diab). Those remain trusted (§12).

---

## §11 The emitter contract (adopt at emitter v1 — cheap now, painful to retrofit)

1. **C99** (already ZER's target standard), restricted to **CompCert's supported dialect**:
   no VLAs, no `setjmp`/`longjmp`, no computed goto, plain constructs. Machine-emitted C
   is naturally this boring; the constraint costs near-zero and guarantees the
   certification path is *always available*.
2. **Freestanding profile**: no or minimal libc. Needed primitives (`memcpy`-class) are
   emitted or provided as a tiny audited runtime. This kills the largest non-primitive
   trusted component (§12) and is idiomatic for MCU targets anyway.
3. **Clight-friendliness**: keep emitted constructs within easy reach of CompCert's Clight
   input language, so the Stage-4 retarget (emit Clight directly, compose theorems) is an
   emitter refactor, not a redesign.
4. **Determinism**: byte-identical output for identical input (no timestamps, no iteration
   over unordered containers into output) — required for reproducible certification builds
   and for dual-compiler differential testing.
5. Emitted code carries **no undefined behavior** by construction (that is the point of
   ZER); the emitter contract makes it *checkable*: the Stage-2 emission proof is against
   the formalized C99-subset semantics, which has no UB to fall into.

---

## §12 Toolchain residue — assembler/linker/libc/startup, ranked honestly

The "it's so primitive it's safe" intuition is *directionally correct* and is the actual
argument for why CompCert stops at assembly — but it needs precision. Risk scales with
**semantic freedom** (how much the tool transforms meaning):

| Trusted component | Semantic freedom | Real-world risk | Notes |
|---|---|---|---|
| **Your linker script + startup code (crt0, vector tables, memory maps)** | n/a (human config) | **HIGHEST** | This is where embedded projects actually die. No proof anywhere covers your linker script. "Porting to a board" is 90% this. |
| libc (if linked) | large library | high | **Killed by the freestanding profile (§11.2)** |
| Linker | relocation, layout | low-medium | |
| **Assembler** | ~1:1 table-driven encoding | **LOWEST** | Almost no room to be *subtly* wrong; encoding bugs tend to crash, not silently misbehave. This is why the "primitive → safe" intuition holds *here*, at the bottom. |

Mitigations (boring and effective): keep startup code tiny and reviewed; review the map
file; checksum the image; hardware-in-the-loop tests per board; for the pad build, a
disassembly review of the final image. Nuclear options if a program ever demands closing
even these links: seL4-style **binary translation validation** (decompile the ELF, prove
correspondence via SMT) and CompCertELF-style verified assembly/linking research — cite as
"available if required," do not build day one.

**Porting, both senses:**
- Porting ZER's output to a new *board* = linker scripts + startup + memory maps =
  configuration risk (above), not tool risk.
- Porting *CompCert* to a new ISA = a formalized ISA semantics + fresh proofs =
  person-years per target. Strategy: stay on the supported map; exotic targets take the
  GCC path plus Tier B.

---

## §13 Lean 4 implementation practicalities

- **Compilation model**: Lean 4 compiles via C to native code; memory is reference-counted
  (Perceus-style, no tracing-GC pauses). Fine for a compiler workload (Lean's own ~500k-line
  self-hosted compiler is the proof), and the RC model keeps latency predictable.
- **Totality escape hatch**: `partial def` for worklist/fixpoint algorithms — no
  termination-proof fights for ordinary code; upgrade to total+proven selectively.
- **C interop**: `@[extern]` bridges to existing C; Lean-emitted C links at object level
  with ZER's C during the strangler migration (§7.2).
- **Toolchain**: pin the Lean version explicitly (channel defaults phone home; pinned
  versions do not — empirically bitten and fixed in the `kernelq-lean` image, which pins
  `leanprover/lean4:v4.32.0` with `ELAN_HOME=/opt/elan`, working under `--network=none`,
  read-only FS, and arbitrary-uid Docker). Reuse that image family for ZER CI.
- **The empirical Lean contract that bites** (verified in-container; full detail in the
  KernelQ doc LS.4): `sorry` **exits 0** with only a warning — any CI gate on Lean proofs
  must check `#print axioms` (a sorried theorem depends on `sorryAx`), never exit codes.
- **Process/certification friction, stated honestly**: Lean's toolchain is young and not
  DO-qualified. For a *ground-based development tool* this is acceptable — DO-330 tool
  qualification asks for understood failure modes, and "the rule set is machine-checked;
  the binary is differentially tested against the in-prover reference; the final build can
  be kernel-certified" is the strongest available answer. Flight code itself is ZER's
  *output* (C99 → CompCert), never Lean.
- **Precedents for production Lean**: AWS Cedar (authorization, verified in Lean, in
  production), SampCert (verified differential privacy, deployed), the Lean compiler
  itself.

---

## §14 Concurrency proofs (Iris) — plan

- The existing Coq/Iris corpus (§1.2) **remains the current home** of ZER's concurrency
  reasoning. Nothing forces a move; Coq-Iris is the mature choice today.
- The empirical door is open (§4): every facility the corpus imports exists in iris-lean,
  the concurrency tower kernel-checks locally, and the toolchain matches ours. A port is
  days-scale translation.
- **Trigger for porting**: when Stage 0–1 put λ-ZER and the checker in Lean, keeping the
  concurrency model in Coq means maintaining two formalizations of ZER's semantics. At
  that point, port the corpus to iris-lean so there is ONE formal semantics with both the
  compiler proofs and the concurrency theorems over it.
- Iris remains the right tool ONLY for the concurrent/heap reasoning; the compiler
  correctness proofs (§8) are plain Lean — separation logic is not involved in
  semantic-preservation arguments.
- **Per §8.2 (single-prover amendment): iris-lean is the DESIGNATED end-state home.**
  After the port at the trigger above, Coq is retired from ZER entirely.

---

## §15 Mission-critical / certification framing

- **Certification is per-artifact** (DO-178C verifies specific builds of specific
  software; DO-330 qualifies tools by understood failure modes; DO-333 admits formal
  methods for certification credit — and CompCert already *has* DO-178C/DO-333/DO-330
  credits from the 2026 ATR qualification). ZER's architecture is deliberately shaped for
  per-artifact claims: Tier B witnesses per build, the kernel hatch per flight build,
  deterministic emission for reproducibility.
- **What evaluators probe first** is the two-chain distinction (§9). Lead with it; never
  claim "fully safe"; use the §9.4 claim language.
- **What IV&V can audit**: with the §5 certificate model, the annotations are the
  specification — reviewers read intent at boundaries instead of reverse-engineering an
  inference.
- **The trusted-base summary for a review slide**: Lean kernel + (daily: one C compiler |
  certified: CompCert's residue) + vendor assembler/linker + board config + hardware —
  with the §12 mitigations attached to each.

---

## §16 Roadmap and first steps

Ordered by leverage; every stage ships standalone value:

1. **Emitter contract adoption** (§11) in the *current C emitter* — dialect + freestanding
   + determinism. Cheap now; unlocks the CompCert path immediately for today's ZER.
   - Smoke test: compile ZER's emitted C for a sample program with CompCert
     (`ccomp`, research license) on `armv7m-eabi` or `rv32`; fix dialect violations.
2. **Certificate-model design** (§5.2): specify the boundary-annotation vocabulary against
   the existing checker's rule classes (ownership, regions, handle states, escape, ISR/MMIO
   contexts — the `verif_*` taxonomy is the checklist). This is a design doc + λ-ZER
   extension, before any code.
3. **Stage 0**: λ-ZER semantics in Lean (port/rebuild from `proofs/operational/`), plus IR
   semantics and the C99-subset target semantics.
4. **Phase M1 migration** (§7.2): the checker in Lean behind the AST/IR-dump seam;
   differential-test it against the C checker on the full test suite until parity.
5. **Stage 1**: `checker_sound` — the first headline theorem. Ship/announce.
6. **Stages 2–3** pass by pass (lowering → emission → composition), with Tier B validation
   covering whatever is not yet proven.
7. **Stage 4** (when justified): Clight retarget + CompCert composition.
8. Ongoing: dual-compiler builds + `#eval`-differential harness in CI (§9.2); port the Iris
   corpus at the §14 trigger.

Skill note: the founder's KernelQ Lean track is the deliberate on-ramp; the simulation
proofs of Stage 2 are its destination. Do not block roadmap steps 1–2 (no Lean required)
on the Lean learning curve.

---

## §17 Rejected-alternatives ledger (do not re-propose without new evidence)

| Alternative | Why rejected | Revisit if |
|---|---|---|
| Stay in C permanently | No provable implementation; VST-against-C is brutal (lived experience, `proofs/vst/`); tool itself memory-unsafe | never (as end-state) |
| Rewrite in OCaml or Rust (tool-safety only) | Solves memory safety but not provability; proofs would live in a prover about a *model* → the model–implementation gap remains | if the Lean bet fails wholesale |
| Coq as the prover for new work | Same expressiveness; loses one-language programs+proofs (extraction gap), ecosystem trajectory, and toolchain unification with KernelQ | if Lean FRO collapses AND Rocq resurges |
| Tier B only (validation, no verified passes) | Per-build assurance without once-for-all theorems; weaker headline claim; validator still needs the same semantics work | n/a — B is kept as the bridge |
| Tier C (CakeML-style verified self-hosting) | Hardest artifact in the field (team-decade); the self-hosting flag is worth less than the proof flag for a safety company | if ZER becomes a funded team effort with years of runway |
| ZER-in-ZER self-hosting as a goal for the core | Loses provability (ZER is not a prover) and orphans the proofs (proofs are about specific code) | periphery only (LSP/tooling), never the proven core |
| Making ZER itself proof-capable (refinement/dependent types) | Building a second Lean; a different mountain | a someday-research direction, not the plan |
| "Fully safe" claims / trusting testing as proof | Testing bounds risk, never proves absence; trusted ≠ verified; no zero-trust floor exists | never |
| Hard dependency on CompCert as the only backend | Commercial licensing (AbsInt); backend gaps (ARMv6-M, DSPs) | n/a — dual backend is strictly better |
| Building ZER inside CompCert's Coq (SJTU-PLV-style, §8.1) | Yields the mechanical single-prover end-to-end theorem, but forfeits every §3 reason for Lean; superseded by §8.2 — the single-prover theorem is obtained IN LEAN via the Stage-5 backend instead | fallback only if Stage 5 stalls AND a composed theorem is demanded sooner |
| Cross-prover composed theorem with CompCert (the original Stage-4 endgame) | The Clight interface semantics would exist in two provers — an argued link, not machine-checked (§8.1) | n/a — replaced by §8.2 (CompCert = standalone interim cert compiler; composition via Stage 5 in Lean) |
| "Porting CompCert to Lean" as a mechanical translation | No production-grade Coq→Lean proof translator exists at that scale; Stage 5 is a design-guided REBUILD of only the needed subset (ZER-IR, 1–2 ISAs) — a small fraction of CompCert | n/a — this framing IS the plan, correctly sized |
| Believing 2024-era iris-lean status ("no WP/adequacy") | Empirically false as of 2026-07 (§4.2 build + axiom probe on this machine) | never — re-verify against the repo instead |

---

## §18 Sources and provenance

Empirical results produced on this machine, 2026-07-19 (commands recorded inline above):
iris-lean clone/build/axiom-probe (§4.2); ZER code measurements (§1.1); ZER proof-corpus
reading (§1.2); Lean v4.32.0 CLI contract probes (via the KernelQ `kernelq-lean` image —
full contract in `KernelQ/docs/kernelq-pedagogy-goal.md` LS.4).

Web-verified 2026-07-19:
- CompCert targets: `github.com/AbsInt/CompCert` (`configure`, master)
- CompCert product/qualification/licensing: `absint.com/compcert` (ATR 42/72 2026 with
  DO-178C/DO-333/DO-330 credits; MTU IEC 60880 Cat A / IEC 61508 SIL-3, 2017;
  free-for-research, commercial via AbsInt), `compcert.org` (v3.16 2025-09; manual v3.17),
  AbsInt CompCert factsheet release 26.04 (2026-04)
- iris-lean repository state: `github.com/leanprover-community/iris-lean` (+ its porting
  tracker at `leanprover-community.github.io/iris-lean`)
- Verified-Rust-on-CompCert precedent (§8.1): `github.com/SJTU-PLV/CompCert`, branch
  `rust-verified-compiler` (read 2026-07-19: pipeline, theorem names, verification status)
- Lean ecosystem (research sweep, 13+ sources): AWS Cedar, SampCert, Veil, lean-mlir,
  Lean FRO roadmap — summarized in KernelQ doc LS.2/LS.3

Session provenance: the full reasoning chain (including the refuted stale-Iris claim and
the "fully safe" correction) occurred in the 2026-07-19 KernelQ/ZER working session; the
KernelQ-side record is `kernelq-pedagogy-goal.md` LS.0–LS.5.

---

*End of decision record. If you are a fresh session about to work on ZER's compiler,
verification, backends, or trust story: this document plus KernelQ LS.0–LS.5 is the
complete context. Verify empirical claims by re-running the recorded commands — do not
re-derive the decisions, and do not re-propose entries from §17 without new evidence.*
