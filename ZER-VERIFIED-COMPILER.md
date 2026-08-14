# ZER Verified Compiler — Full Architecture & Decision Record (Coq / RefinedC / Tier A)

**Status: ADOPTED ARCHITECTURE, 2026-08-15.** This document is the complete, self-contained
record of the decision to verify ZER as **two linked artifacts** — the mathematics in **Coq**
and the implementation in **C, proved against that mathematics by RefinedC** — with a dual C
backend (mainline GCC/Clang daily, CompCert for certification builds) and a fully analyzed
trust chain.

**It supersedes the 2026-07-19 record**, which adopted a rewrite of the whole compiler into
**Lean 4** and later instantiated its translation-validation tier with **CBMC**. Lean and CBMC
are removed from the architecture entirely. §0.1 records exactly what changed, what the old
decision got right, and which of its recorded problems this one dissolves — read it before
concluding that anything here is a re-litigation.

**Why this document exists:** every decision below was reached through evidence-heavy working
sessions with LOCAL empirical verification (building and axiom-checking provers on this
machine; measuring RefinedC against real C; codebase measurement) and several rounds of
adversarial reasoning about trust. A fresh session — human or Claude — must NOT re-derive,
re-litigate, or re-research any of it from scratch. Read this document first. Where a claim is
empirical, the exact command and result are recorded so it can be re-verified rather than
re-discovered. Where an alternative was REJECTED, the alternative and the reason are recorded
so it is not re-proposed.

Related context elsewhere:
- KernelQ's synthesize track — the same Coq + RefinedC stack, built and measured end to end:
  `KernelQ/docs/kernelq-pedagogy-goal.md` **LS.26** (architecture + elimination ledger),
  **LS.27** (the linked spec, measured), **LS.28** (the pipeline as built + authoring
  contract), **LS.29** (porting traps).
- The `kernelq-refinedc` Docker image (Rocq 9.1.0 + RefinedC, works under `--network=none`,
  read-only FS, arbitrary-uid): `KernelQ/backend/Dockerfile.refinedc`.

---

## Table of contents

- §0  The decision in one page
- §0.1 **What changed from the 2026-07-19 Lean record, and why** — read this first
- §1  ZER today — inventory (code, proofs, sizes)
- §2  The implementation-language question (why C STAYS, and how the model gap closes anyway)
- §3  The prover question (Coq — and why the 2026-07 answer inverted)
- §4  RefinedC — what it is, what it proves, and its measured envelope
- §5  The certificate principle (§5.3 the annotation contract; §5.4 the RefinedC tension, resolved)
- §6  Assurance tiers A/B/C — and the decision
- §7  The architecture (two artifacts, one link; no rewrite)
- §8  The theorems (Stage 0–4 roadmap; the Grand Theorem)
- §9  The two trust chains — TCB analysis, "fully safe" does not exist, the kernel hatch
- §10 Backends — dual-backend strategy; verified CompCert facts
- §11 The emitter contract (C99, CompCert dialect, freestanding)
- §12 Toolchain residue (assembler/linker/libc/startup, ranked honestly)
- §13 Coq / RefinedC implementation practicalities
- §14 Concurrency proofs (Iris) — no port, no trigger
- §15 Mission-critical / certification framing
- §16 Roadmap and first steps
- §17 Rejected-alternatives ledger (the no-reloop table)
- §19 **The linked pipeline as built** — the measured mechanism, its guards, its envelope
- §18 Sources and provenance

---

## §0 The decision in one page

**ZER becomes a verified compiler by proving its existing C implementation against Coq
semantics, not by rewriting itself into a prover's language.**

1. **Implementation language: C — it stays.** The ~47k-line C core is not migrated. The
   model–implementation gap that motivated a rewrite is closed by **RefinedC**, which proves
   the actual `.c` file, not a transcription of it.
2. **Prover: Coq (Rocq 9.1).** ZER already has **94 `.v` files** — λ-ZER operational
   semantics, the per-class MAX oracles, the Iris concurrency corpus, and 23 VST files. That
   corpus stops being a legacy asset and becomes the foundation.
3. **The link: RefinedC.** Author-owned Coq definitions state what a function computes; the C
   carries `[[rc::...]]` annotations; `refinedc check` proves the C meets that specification
   for **all** inputs, kernel-checked, in seconds. This is the automation-first successor to
   the `proofs/vst/` effort, which the 2026-07 record itself called *"brutal"*.
4. **Assurance tier: A (per-pass semantic preservation)**, with the per-pass theorem stated in
   Coq over λ-ZER and the pass's C implementation proved to realise it. Tier B (per-build
   validation) remains the bridge for passes not yet proven. Tier C (verified self-hosting)
   stays retired.
5. **Certificate-based checking model — unchanged and unaffected by the prover switch.**
   ZER infers every safety property; boundary annotations are a **restatement the compiler
   verifies against its own derivation** — checked, never trusted (§5.3).
6. **Target: C99, dual backend** — daily GCC/Clang, certification CompCert. **CompCert is
   Coq**, so the cross-prover seam that the Lean record spent three amendments engineering
   around (§8.1→§8.2→§8.3 in the old document) **does not exist here**.
7. **Trust chain, stated honestly** — the residual TCB is Coq's kernel, RefinedC's frontend
   and automation, one C compiler, and hardware; monitored by differential testing and the
   mutant discipline, and bypassable per-artifact by kernel-computed certification.
8. **Concurrency proofs stay where they are.** The Iris corpus is already Coq. No port, no
   trigger, no second formalization of ZER's semantics.

The one-liner: **proven wherever proof exists; trusted only in a residue that is small,
watched, and bypassable for the one build that goes to the pad — and obtained without
rewriting a working compiler.**

---

## §0.1 What changed from the 2026-07-19 Lean record, and why

This is the section a fresh session must read before assuming anything above is a mistake.

### What the old record got RIGHT, and this one keeps unchanged

```
§5   the CERTIFICATE PRINCIPLE          checking is P, finding is undecidable; infer within,
                                        certify between; checked-never-trusted annotations.
                                        Prover-independent. Untouched.
§6   the TIER TAXONOMY                  A / B / C, and the decision A-primary, B-bridge,
                                        C-retired. Untouched.
§9   the TWO-CHAIN TCB analysis         "fully safe is not a state that exists". The chains
                                        change contents, not shape.
§10  the DUAL BACKEND                   GCC daily, CompCert for certification.
§11  the EMITTER CONTRACT               C99, CompCert dialect, freestanding, deterministic.
§12  the TOOLCHAIN RESIDUE ranking      linker script > libc > linker > assembler.
§15  CERTIFICATION framing              per-artifact, composed evidence, the claim language.
```

### What is REMOVED, and the specific reason

```
LEAN 4 as the implementation language
    The rewrite existed to close ONE gap: "the proofs are about a MODEL of checker.c, not
    about checker.c" (old §2.2). RefinedC closes that gap on the C directly. Once the gap is
    closed without a rewrite, a 47k-line migration of a working compiler buys nothing and
    costs everything -- and old §7.2 knew it, opening with "big-bang rewrites of ~47k working
    lines are the classic self-inflicted wound."

LEAN 4 as the prover
    Old §3.3's five reasons for Lean: (1) no extraction gap, (2) ergonomics for writing a
    compiler, (3) ecosystem trajectory, (4) shared toolchain with KernelQ, (5) Iris is the
    only thing Coq holds. Under the new architecture: (1) and (2) evaporate because nothing
    is being written in the prover -- the compiler stays C; (4) INVERTS, because KernelQ's
    synthesize track is now Coq + RefinedC; (5) becomes decisive in Coq's favour, since ZER's
    Iris corpus already exists and would have to be ported. Only (3) survives, and an
    ecosystem-trajectory argument does not outweigh 94 existing .v files.

CBMC, and the whole §19 Lean-emit -> CBMC mechanism
    It was Tier B instantiated: emit a reference C from Lean, then ask a BOUNDED model checker
    whether hand-written C agrees with it. RefinedC replaces it with something strictly
    stronger on the axis that matters -- UNBOUNDED proof for all inputs rather than
    equivalence up to a bound -- and removes the reference-emission half entirely, which was
    the part carrying the unproved `render` function and the four silent semantic traps.
    The GUARDS discovered while building it are NOT removed: they are prover-independent and
    are ported into §19 below, because every one of them is reachable here.

The CROSS-PROVER SEAM, and everything built to survive it
    Old §8.1 recorded that composing with CompCert would mean formalising the C interface
    TWICE (once in Lean, once in CompCert's Coq) -- "an argued-and-tested link, not a
    machine-checked one." §8.2 then eliminated the seam by declaring a Lean-native backend
    (Stage 5); §8.3 retired Stage 5 because it traded ~50 GCC targets for 2, and accepted
    COMPOSED EVIDENCE instead of one theorem.
    ALL OF THAT DISSOLVES. CompCert is Coq. ZER's proofs are Coq. The interface semantics is
    formalised ONCE. A composed, machine-checked theorem from ZER source to assembly is
    available in principle without a native backend and without a seam -- see §8 Stage 4.
    This is the single largest structural gain of the switch, and it was not the reason for
    making it.
```

### What is genuinely LOST

Stated plainly, because a decision record that only lists gains is a sales document:

```
MATHLIB              Coq's mathematics library is narrower than Lean's. Irrelevant to a
                     compiler: ZER's proofs are operational semantics and simulation
                     arguments, not analysis or algebra.
ONE ARTIFACT         Lean's "the function you run IS the object you prove" is genuinely
                     elegant. Here there are two artifacts -- Coq definitions and C code --
                     and RefinedC is what makes them one claim. If RefinedC cannot prove a
                     given function, the two artifacts drift apart with nothing to stop it.
                     That is the real cost, and §4.4 gives it an honest envelope.
TOOL MEMORY SAFETY   the "cobbler's children" regret (a memory-safety tool written in a
                     memory-unsafe language) is NOT resolved by a rewrite any more. It is
                     answered differently and, where RefinedC reaches, more strongly:
                     RefinedC PROVES memory safety of the C, rather than a runtime providing
                     it. Where RefinedC does not reach, the regret stands. Do not pretend
                     otherwise.
ECOSYSTEM MOMENTUM   the Lean FRO / industrial-Lean trajectory argument is given up. Recorded
                     as a real cost, not argued away.
```

### The rule for a fresh session

**Do not re-propose Lean.** The question is settled by the fact that nothing is written in the
prover: once the implementation stays in C, the prover choice is decided by which one has the
libraries that verify C (Iris, RefinedC, VST, CompCert — all Coq) and by which one already
holds ZER's corpus (Coq). Revisit only if RefinedC's envelope (§4.4) proves too narrow for
ZER's actual passes **and** a measured alternative exists — not on ecosystem grounds.

---

## §1 ZER today — inventory

### 1.1 The C implementation (measured 2026-07-19)

| Component | Files | Lines |
|---|---|---|
| **Core compiler** | `lexer.c parser.c ast.c types.c checker.c ir.c ir_lower.c vrp_ir.c emitter.c zercheck.c zercheck_ir.c zerc_main.c` | **46,751** |
| — of which, the checker | `checker.c zercheck.c zercheck_ir.c types.c` | **24,267** |
| Peripherals | `zer_lsp.c zer_wasm.c` | 1,844 |
| Everything `.c/.h` in root | | 63,713 |

Interpretation, and it changes with this decision: the checker is ~half the core and its
correctness IS the product. Under the Lean plan that made it the *first thing to rewrite*.
Under this plan it makes it the **first thing to specify and prove in place** — the same
priority, without the rewrite.

### 1.2 The existing proof corpus — 94 `.v` files (measured 2026-08-15)

```
proofs/                                              94 .v files total
├── composition.v
├── model1_handle_states.v … model4_static_annotations.v
├── vst/verif_*.v                      23 files: per-rule verifications of checker rules
│                                      (arith, atomic, cast, coerce, comptime, concurrency,
│                                       container, context bans, escape, handle state, isr,
│                                       mmio, move, optional, provenance, range checks,
│                                       stack, variant, zer checks, type kind …)
└── operational/                       66 files
    ├── lambda_zer_concurrency/        ~870 lines, 11 files, zero admits — the ONE worked
    │   syntax.v semantics.v iris_lang.v iris_state.v iris_wp_heap.v …   Iris instance
    ├── lambda_zer_handle/             handle_flow_lattice.v — the UAF MAX oracle
    ├── lambda_zer_escape/             param_lattice.v, join_lattice.v
    ├── lambda_zer_{bounds,qualifier,capture,volatile}/   the four late oracles
    └── lambda_zer_move/, lambda_zer_disjoint/
```

**Under the Lean plan this corpus was a liability** — old §3.4 promised it "remains valid Coq
and is not deleted", §8 Stage 0 planned to *rebuild* λ-ZER in Lean, and §14 scheduled the Iris
port. Three formalization-maintenance obligations, all now cancelled.

**Under this plan it is the foundation.** The oracles ARE the abstract domains the checker's C
must be proved to implement; `lambda_zer_concurrency` is the worked operational instance;
`composition.v` is the shape Stage 3 composes into. Nothing is ported, nothing is rebuilt.

### 1.3 The `vst/` directory is the predecessor of this architecture, not a dead end

Old §1.2 read the 23 `verif_*.v` files as *"evidence of the model–implementation gap being
fought by hand: verifying C code against the rules via VST is brutal, which is one of the
drivers for moving the implementation into the prover."*

That reading was right about the pain and wrong about the conclusion. The pain is **VST's
interaction model** (interactive Coq tactics, days per function), not the goal. The goal —
prove the shipped `.c` against a Coq spec — is exactly what this architecture adopts, with an
automation-first tool in place of a tactic-first one. Everything already learned writing
`verif_*.v` (VST-friendly C style: flat cascades of early-return ifs, no nesting, no compound
conditions) transfers directly; RefinedC's C subset wants the same shape for the same reason.

### 1.4 The current checking model (unchanged)

ZER's checker establishes safety **by inference**. §5 records why the design shifts to
*infer within functions, certify between them*, and that shift is prover-independent.

---

## §2 The implementation-language question — RESOLVED DIFFERENTLY: C stays

### 2.1 History: why ZER is in C, and what the regret actually was

C was chosen because "ZER should be simple like C, so build it in C" and for the bootstrap
romance. Both dissolve under examination — a compiler is allocation-heavy, recursion-heavy and
pointer-graph-heavy, which is precisely the workload where C's simplicity inverts.

But separate two complaints that the 2026-07 record ran together:

```
COMPLAINT A   the tool itself is memory-unsafe            -> a real robustness cost
COMPLAINT B   the implementation cannot be machine-checked -> the DECISIVE one
```

**B was decisive and B is what a rewrite was for.** A alone never justified 47k lines of
migration; old §2.1 says so itself (*"ZER-written-in-C still produces safe output, because the
guarantee lives in ZER's design, not its implementation language"*).

RefinedC answers B directly — and answers A wherever it reaches, since its type system proves
absence of UB, out-of-bounds access and integer overflow as side conditions, independent of
whether the functional specification is strong (measured; see §4.2).

### 2.2 The options, re-scored

| Option | Memory safety | Provability of the implementation | Cost | Verdict |
|---|---|---|---|---|
| Stay C, unverified | ✗ | ✗ | 0 | Rejected (the status quo ante) |
| Rewrite in OCaml / Rust | ✓ | ✗ (proofs live elsewhere; model gap remains) | 47k lines | Rejected |
| Rewrite in Lean 4 | ✓ | ✓ (theorem about the running code) | 47k lines + permanent proof-language coupling | **Superseded — the gap it closed is closable without it** |
| **Stay C + RefinedC** | **✓ where proved** | **✓ — the theorem is about the actual `.c`** | **annotation + spec work, incremental, per function** | **ADOPTED** |

The decisive column is now the last one. A rewrite is a step function: nothing is verified
until a component has been fully migrated. RefinedC is incremental at the granularity of a
**function**: `checker.c` can have five proved functions and the rest unproved, shipping the
whole time, with the proved set growing. For a one-maintainer project that difference is not a
preference — it is the difference between a plan that happens and a plan that does not.

### 2.3 What "C stays" does NOT mean

It does not mean the C is exempt from discipline. RefinedC accepts a **subset** of C, and code
written outside it cannot be verified at all (§4.3). The practical consequence is a style
constraint on newly-written safety-critical C — the same constraint `proofs/vst/` already
imposed, now with a faster tool behind it. Peripheral code (`zer_lsp.c`, `zer_wasm.c`, the
conversion tools) is under no such constraint and is never proved.

---

## §3 The prover question — Coq

### 3.1 Foundations: identical

Coq (Rocq) and Lean 4 are both the Calculus of Inductive Constructions. **Expressiveness is
identical**; anything provable in one is provable in the other. Encoding differences exist
(Coq has primitive coinduction, Lean primitive quotients) and are not expressiveness gaps.
This section is retained verbatim in substance from the old record because it was correct and
because it is what makes the switch cheap: no theorem ZER needs becomes unstatable.

### 3.2 The governing principle: capability in the kernel, convenience in libraries

**Iris is not a Coq primitive** — it is ~200k lines of ordinary Coq library. Neither is
RefinedC, nor VST, nor CompCert. No prover has program-verification primitives; that is a
feature (tiny trusted kernel, everything above derived). The provers differ ONLY in which
libraries are built out, so the governing question is never *"can it be done"* but **"who
already built the library, and how much porting do I pay."**

### 3.3 Why Coq for ZER specifically — the 2026-07 reasoning, re-run

The old record's five reasons for Lean, each re-scored under "the compiler stays in C":

| Old reason for Lean | Status now |
|---|---|
| 1. One language for programs and proofs; no extraction gap | **Void.** Nothing is written in the prover. There is no extraction, in either prover. |
| 2. Programmer ergonomics for writing a compiler | **Void.** Same reason. |
| 3. Ecosystem trajectory (Mathlib, FRO funding) | **Survives, and is given up.** Real, and outweighed by the four `.v`-shaped facts below. |
| 4. Shared toolchain with KernelQ | **INVERTED.** KernelQ's synthesize track is Coq + RefinedC as of 2026-08. The shared-toolchain argument now points at Coq. |
| 5. Coq holds only *mature Iris*, which binds concurrency not the compiler | **Decisive for Coq.** ZER's Iris corpus already exists, in Coq, working, zero-admit. |

And four things only Coq has, all of which ZER specifically needs:

```
RefinedC     verifies C against Coq specs, automation-first.       BSD licensed.
VST          the predecessor effort; 23 files already written.
Iris         mature, and ZER's concurrency corpus is built on it.
CompCert     Coq -- which makes Stage 4 composition SINGLE-PROVER (§8).
```

The last one deserves emphasis because it was invisible in 2026-07: the entire §8.1/§8.2/§8.3
amendment chain in the old document existed to manage a seam that **only existed because the
prover was Lean**. Choosing Coq removes the problem rather than engineering around it.

### 3.4 What this costs in practice

Coq's tactic language and ergonomics are, by most accounts, less pleasant than Lean's. Since
ZER's proof work is *specification plus simulation arguments over its own semantics*, and
since 94 files of it already exist in Coq written by this project, the ergonomic delta is a
known quantity rather than a risk. Recorded as a cost, not argued away.

---

## §4 RefinedC — what it is, what it proves, and its measured envelope

**Everything in this section is measured, in the `kernelq-refinedc` image (Rocq 9.1.0,
RefinedC `dev.2026-07-16`), on this machine, during 2026-08-14/15. Do not re-derive it.**

### 4.1 The mechanism — it converts C into Coq, and proves a theorem about it

This is the single most-misunderstood point, so it is stated first and precisely.

```
   your .c
     |  Cerberus frontend (frontend/ail_to_coq.ml) -- "Ail" is Cerberus's C AST
     v
   generated_code.v     the C as a CAESIUM PROGRAM TERM. A transcript, carrying source
                        locations. NOT a function.
   generated_spec.v     your rc:: annotations, elaborated into a REFINEMENT TYPE:
                          given <precondition>, returns a value refined by <Coq function>
                                                                          ^ the CITATION
   generated_proof_f.v  a Coq THEOREM to prove: `type_f : typed_function f_def f_spec`
     |
     |  LITHIUM runs as a Coq TACTIC: walks the program symbolically -- loop entry, one
     |  body, loop exit -- emitting SIDE CONDITIONS, which are pure mathematical goals.
     v
   coqc CHECKS THE WHOLE THING. Qed or nothing.
   Print Assumptions type_f  ->  "Closed under the global context"
```

**Nothing is compared.** There is no AST diff, in RefinedC or anywhere in this pipeline. A
program term (Caesium) and a function (`nat -> nat`) are different types; you cannot write `=`
between them. The specification **names** the Coq function, and the proof shows the program's
result equals it. The link is by citation, and that is why the generated `.v` being a
"transcript, not a function" stopped mattering.

Consequence for the ZER architecture: relating the checker's C to λ-ZER is **not** a
translation-validation problem and does not need a reference implementation, a rendered
artifact, or a bounded equivalence check. It is one Hoare triple per function.

### 4.2 What is proved regardless of the specification's strength

Measured: the obligation `(i + 8 <= max_int i64)` was raised against a specification that says
nothing whatsoever about overflow. So the type system's own side conditions give, for free:

```
absence of undefined behaviour, out-of-bounds access, and integer overflow
```

This matters more for ZER than the functional half, at least initially. `checker.c` has never
had a machine-checked memory-safety argument; RefinedC gives one function-by-function without
requiring anyone to first write down what that function computes.

**Worked example of the value, measured.** An 8-way-unrolled counting loop whose condition is
`k + 8 <= n` was REJECTED with exactly that obligation. The C is functionally correct — gcc
over `n ∈ [0,5000]` under UBSan reports `ALL AGREE` — but when `n` is within 8 of `LLONG_MAX`,
`k + 8` overflows:

```
runtime error: signed integer overflow: 9223372036854775803 + 8
               cannot be represented in type 'long long int'
```

No finite behavioural test reaches that input. `n - k >= 8` is the non-overflowing idiom and
proves immediately. **This is the class of bug ZER's own C is full of and its test suite
cannot see.**

### 4.3 The C subset — measured, and it constrains style not capability

RefinedC's frontend accepts a subset. Measured directly:

```
ACCEPTED    r += a;      k++ in a for-header      a > b ? a : b       plain statements
REJECTED    return a = b;      "Forbidden: nested assignment"
            -- assignment used as an EXPRESSION
```

Rejections of this kind come back in **1–2 seconds**; a real proof attempt takes 5–30s. That
timing difference is the reliable tell that a verdict is a *parse* refusal rather than a proof
outcome, and any tooling built around RefinedC should classify on it (KernelQ's does).

The style this forces — one operation per statement, no assignment-in-expression — is the same
style `docs/proof-internals.md` already mandates for VST-friendly C. No new discipline.

### 4.4 THE ENVELOPE — where it stalls, honestly, with numbers

This is the cost of giving up "one artifact" (§0.1) and must not be softened.

RefinedC is a proof **search**. It can fail to close on code that is correct. Two distinct
modes, and they look identical to the caller:

```
GIVES UP    finishes FAST, reports "Cannot solve side condition". The common one.
BLOWS UP    the obligation count or solver time explodes. Bounded only by a timeout.
```

Measured verdicts (`refinedc/logs/VERDICTS` in the KernelQ repo):

```
plain accumulation loop         ACCEPT       16s
chunked loop                    ACCEPT       52s
counting loop, 8x unrolled      ACCEPT        6s     same annotation as 1x
counting loop, 32x unrolled     ACCEPT       23s     same annotation as 1x
list/fold, 32x unrolled         ACCEPT      226s     needs a vocabulary prelude
pointer-walking variant         NOT PROVED   12s     no annotation is expressible
```

Three findings that shape the ZER plan:

1. **Unrolling is not a stall.** A 32-wide body needs no more invariant than a 1-wide one,
   because the invariant is about the *loop*, not the body. Cost scales with obligation count
   (6s → 23s), not with difficulty.
2. **Pointer-walking is a real, structural stall.** The residual is a separation-logic goal
   that no annotation reaches. **This is the one that matters for ZER**, whose passes are
   pointer-graph-shaped — and it is exactly what §19.4's arena decision answers.
3. **The annotation burden is the ongoing cost.** Loop invariants are written by hand, per
   loop. That is the honest recurring price.

### 4.5 The specification cost — it follows what the SPEC CITES, not the algorithm

Measured across the KernelQ rungs, and this corrects a figure that was widely mis-quoted:

```
spec cites a LITERAL            0 lines of author Coq.  ...and links to NOTHING.
spec cites ONE Coq function     ~6 lines of substance:
                                  1 Definition (the Z-level twin of the nat-level function)
                                  2 Lemmas
                                  1 Ltac + its enrich_context_hook registration
spec uses LISTS / FOLDS         a vocabulary prelude, paid ONCE per vocabulary
```

Two rules that cost real iterations to learn:

- **DEFINING A LEMMA IS NOT REGISTERING IT.** A prelude carrying the lemmas but no
  `enrich_context_hook` registration changes *nothing* — same unsolved goals before and after.
  This is the single easiest way to wrongly conclude RefinedC cannot do something it can.
- **The lemma must match the term shape the automation PRODUCES**, not the shape you would
  write. Measured: a lemma about `fZ` never fired because the automation had already unfolded
  `fZ` into `fN ∘ Z.to_nat`. The loop is: run → read `Cannot solve side condition` → look at
  the goal → restate the lemma over ITS shape → repeat. Two rounds for a simple function.

### 4.6 Licensing and provenance

RefinedC is **BSD**; Iris is BSD; Coq/Rocq is LGPL-2.1. There is no per-seat licence anywhere
in the verification stack, which is a different position from the CompCert backend (§10.2) and
is worth stating in any certification conversation.

---

## §5 The certificate principle — the design shift in ZER's checking model

**This section is prover-independent and is carried forward unchanged in substance.** It is
the deepest design commitment in the document and nothing in the Lean→Coq switch touches it.

### 5.1 The asymmetry

**Checking a given answer is P; finding the answer is NP-hard at best — and for semantic
properties of programs, undecidable (Rice).** Every serious verification system is built
around this asymmetry:

- Proof assistants: humans/tactics FIND the proof; a tiny kernel only CHECKS it.
- Rust: the function signature is a **certificate the user supplies**; the borrow checker
  verifies each function *locally*. Never whole-program inference.
- Proof-carrying code (Necula): ship the certificate; the consumer runs only the cheap checker.
- Conversely: a static analyzer over C/C++ is a bug-FINDER on the undecidable side — hence
  unsound (Coverity-class) or subset-restricted (Astrée-class).

### 5.2 Applied to ZER

ZER's current implicit-inference checking does the FIND. Sound-but-conservative is safe, but
it inherits the finder's curse: heuristic cliffs, poor error locality, and — decisive here —
**near-impossibility of proving the checker sound**, because the object to prove is a search
heuristic rather than a finite rule set.

**Adopted design shift: infer WITHIN functions, certify BETWEEN them.**

- Inside a function body: inference remains free.
- At function/module boundaries: annotations (ownership, region, handle-state, escape) become
  the certificates; the checker *verifies* them modularly.
- Consequences: the safety judgment becomes a finite rule set — exactly the shape Stage 1's
  `checker_sound` can be proven about; checking is polynomial and reproducible; errors
  localize; the annotation IS readable intent for IV&V; separate compilation becomes natural.

### 5.3 The annotation contract — CHECKED, never TRUSTED

Three models exist; ZER is the third.

| Model | Who states the property | Failure mode of a WRONG annotation | Example |
|---|---|---|---|
| **Trusted annotation** | user states, tool ASSUMES | you proved the WRONG THING, silently | RefinedC, SPARK, contract systems |
| **No annotation** | compiler alone | nothing to be wrong — but illegible and near-unprovable | ZER before this shift |
| **Checked annotation** | compiler INFERS independently; user RESTATES; compiler COMPARES | **compile error** | **ZER (adopted)**; Rust `fn` signatures |

The load-bearing property: **the annotation carries ZERO trust.** It is a restatement the
compiler verifies against its own derivation, never an information source it relies on.

**The decision rule for whether an annotation is written or hidden.** One question — *does
this annotation change the emitted C?*

| Answer | Policy | Cases |
|---|---|---|
| **NO** — analysis-only | **INFER it, hide it.** | `keep`, escape, provenance, handle state, alloc colour |
| **YES** — changes layout, ABI, codegen | **REQUIRE it, and CHECK it against inference.** | `*T` vs `[*]T`, `const`, `volatile`, `packed`, `mmio` |

*Infer what is invisible; require what changes the machine.*

### 5.4 The RefinedC tension — resolved explicitly, because the table above names it

**§5.3's first row lists RefinedC as the model ZER rejects.** Adopting RefinedC as ZER's
verification tool therefore looks, on a fast read, like adopting the thing the design refuses.
It is not, and the distinction is sharp:

```
ZER's USERS      write ZER programs. Their annotations are CHECKED against the compiler's
                 own inference. A wrong one is a compile error. This is §5.3 and it is
                 UNCHANGED -- RefinedC is nowhere near this layer.

ZER's AUTHORS    write the compiler, and the Coq specification of what it should do. The
                 rc:: annotations on checker.c are AUTHOR-side: they say what this C
                 function computes, so RefinedC can prove it does.
```

A wrong *user* annotation cannot happen — the compiler catches it. A wrong *author*
specification absolutely can, and would mean the C was proved to compute the wrong thing. That
risk is real and is managed by three rules, all of which cost minutes:

1. **The specification cites λ-ZER, it is not written free-hand.** The Coq function a
   `rc::returns` names must be a definition from `proofs/operational/`, i.e. the same object
   the soundness theorem is stated over — not a fresh definition transcribed alongside the C.
   Single-home the definition; splice it, never re-type it. (Measured trap: writing the
   function twice — once in the semantics, once in the prelude — lets both halves pass while
   grading different functions. Same multi-site class CLAUDE.md calls the #1 recurring bug.)
2. **A vacuous specification is a specification that proves anything.** Measured: a
   precondition pair `{0 <= n}` and `{n < 0}` is contradictory, so the triple is vacuously
   true and `return 12345;` PROVES against `returns n` — **ACCEPT in 3 seconds.** RefinedC
   will not warn you.
3. **Therefore: every proved function keeps a MUTANT.** A deliberately wrong version that must
   be REJECTED. If the mutant also proves, the specification is the bug. This is §19.2's
   governing principle applied to authoring, and it is the only defence against 2.

**The honest summary line:** *RefinedC can say more; ZER can never be wrong* — the old §5.3
closing line — remains exactly true, because it was always about ZER's user-facing contract,
not about how ZER's own implementation is verified.

---

## §6 Assurance tiers — the decision

Two distinct theorems live in a compiler:

1. **Checking correctness** — `check p = true → Safe p`.
2. **Translation correctness** — `compile p = c → c behaves as p`. A buggy emitter can take a
   *safe* source program and emit *wrong* code. Csmith-class fuzzing found 300+ miscompilation
   bugs in GCC/LLVM and famously **zero** in CompCert's verified middle-end.

| Tier | What | Decision |
|---|---|---|
| **A — verified passes** | Per-pass semantic-preservation theorem in Coq over λ-ZER, with the pass's C implementation proved to realise it by RefinedC | **ADOPTED — primary** |
| **B — translation validation** | Per-build correspondence witness checked by a small proven validator | **ADOPTED — the bridge**, for passes not yet proven and for anything outside RefinedC's envelope (§4.4). Precedent: CompCert validates register allocation; seL4 validates its binary. |
| **C — verified self-hosting** | Compiler in ZER, deeply embedded, bootstrapped | **RETIRED.** Unchanged from the old record. |

**What changed inside Tier A.** Under the Lean plan, "the theorem is about the code that runs"
came from writing the pass in the prover. Here it comes from RefinedC proving the C. The
theorem statement is identical; the mechanism differs, and the mechanism no longer requires a
rewrite.

**What changed inside Tier B.** Its old instantiation (Lean-emitted reference C + CBMC bounded
equivalence) is gone. Tier B's role shrinks accordingly: it covers what RefinedC's envelope
does not reach, and it is per-artifact evidence rather than a once-for-all theorem.

### 6.1 Why Tier A is more tractable for ZER than it was for CompCert

1. **CompCert's hardest problem was C's semantics.** ZER owns its own semantics and can
   co-design the language with its formalization. λ-ZER already exists.
2. **ZER emits C, not assembly.** The heroic backend passes are not in the pipeline.
3. **A safety-first source language has simpler semantics** than a legacy one.
4. **Per-pass staging ships value at every stage** — a verified checker alone is a product.
5. **NEW: no migration precedes any of it.** Under the Lean plan, Stage 1 was gated behind
   porting ~24k lines of checker to Lean. Here Stage 1 is gated behind specifying and proving
   functions *that already exist*, one at a time, with the compiler shipping throughout.

### 6.2 The cost of A, stated honestly

- **Development velocity changes character permanently.** Every change to a proved function
  requires re-proving it. That is what mission-critical maturity looks like.
- **The daily proof work is simulation arguments** plus, now, **annotation maintenance**: a
  refactor that changes a loop changes its invariant.
- **The envelope is a hard constraint, not a slope** (§4.4). A function RefinedC cannot reach
  is not "expensive to prove" — it is *unproved*, and stays under Tier B.

---

## §7 The architecture

### 7.1 Two artifacts, one link

```
   proofs/operational/          COQ. λ-ZER semantics, the per-class oracles, the
     lambda_zer_*.v             soundness theorems. What SAFE MEANS.
        |
        |  cited by name in the specification -- never transcribed
        v
   spec/checker_*.rch           AUTHOR-OWNED RefinedC specs: for each proved C function,
                                what it computes, in terms of the Coq definitions above.
        |
        |  refinedc check  ->  Lithium  ->  coqc  ->  Print Assumptions
        v
   checker.c, zercheck_ir.c     THE SHIPPED C. Annotated with loop invariants.
     ir_lower.c, emitter.c      Proved, function by function, for ALL inputs.
        |
        |  gcc / CompCert
        v
   zerc binary
```

**There is no migration phase and no seam between the halves.** A function is either proved or
not; the compiler works either way; the proved set grows monotonically.

### 7.2 Sequencing — by verifiability, not by subsystem

The old §7.2 sequenced a *migration* (checker → IR/emitter → frontend). This plan sequences
*proof effort*, and the ordering principle is different: start where RefinedC's envelope is
widest and the safety payoff is highest.

```
P1  THE PURE PREDICATES        src/safety/*.c -- ALREADY extracted, already VST-verified,
                               already flat-cascade style. 85/85 predicates. This is the
                               warm start: re-prove them with RefinedC, measure the delta
                               against the VST effort, and calibrate everything else on
                               real numbers rather than on this document's estimates.
P2  THE ORACLE TRANSFERS       each MAX oracle (param_lattice, handle_flow_lattice, the
                               four late ones) already specifies a transfer function. Prove
                               the C that implements it computes the certified transfer.
                               This is where "the oracle certifies the DOMAIN but nothing
                               proves the C implements it" -- CLAUDE.md's stated current
                               gap -- actually closes.
P3  ARENA-FLATTEN THE IR       §19.4. The prerequisite for anything pointer-shaped. Do it
                               before P4, not after.
P4  THE FIXPOINT PASSES        zercheck_ir's CFG lattice, VRP. Bounded-trip-count form
                               (§19.4) so the loop invariant is expressible.
P5  THE EMITTER                Stage 2's `emit_correct`. Largest, last, and the one most
                               likely to need Tier B for parts.
NEVER                          zer_lsp.c, zer_wasm.c, tools/. Peripheral, unproved, fine.
```

### 7.3 The pipeline (target state)

```
ZER source
  → [C] lexer/parser              unproved (AST well-formedness validated)
  → [C, PROVED] CHECKER           Stage 1: certificate verification (§5)
  → [C, PROVED] AST → IR          Stage 2
  → [C, PROVED] VRP / analyses    proofs where they license transformations
  → [C, PROVED] IR → C99 emission Stage 2; dialect contract §11
  → emitted C99
      → daily:        latest mainline GCC/Clang
      → certification: CompCert → assembly (verified chain, §10)
```

Every "PROVED" above means: *a Coq theorem over λ-ZER, realised by this C, checked by
RefinedC, axiom-free under `Print Assumptions`.*

---

## §8 The theorems — Stage 0–4 roadmap

| Stage | Artifact | Theorem (shape) |
|---|---|---|
| **0** | **Semantics in Coq**: λ-ZER source semantics; IR semantics; the C99-subset target semantics | definitions — **and most of it already exists** (`proofs/operational/`), which is the largest single schedule change from the old record |
| **1** | **Verified checker** | `Theorem checker_sound : check p = true -> Safe p` — over λ-ZER, realised by `checker.c` |
| **2** | **Verified passes**, one at a time | per pass, forward simulation: `lower_correct : Safe p -> SemIR (lower p) ≼ SemZER p` ; `emit_correct : SemC (emit ir) ≼ SemIR ir` |
| **3** | **Composition** | the ZER Grand Theorem: `check p = true /\ compile p = Some c -> MemSafe c /\ SemC c ≼ SemZER p` |
| **4** | **Compose with CompCert** — **single-prover, no seam** | verified from ZER source to machine code; TCB shrinks to CompCert's residue + Coq's |

Notes:

- `≼` is behavioral refinement, the CompCert-style statement.
- Analyses need correctness proofs **only where their results license transformations**.
- **An analysis that fails OPEN cannot be proven sound.** Where a class falls back to "unknown,
  therefore accept", its abstract state does not over-approximate the concrete one; the
  forward-simulation diagram will not close and the soundness theorem is not merely unproven
  but **false**. Converting a fail-open class to fail-closed is a **prerequisite** for its
  Stage-1 proof, not follow-up work. Current instances: `docs/limitations.md`.
- Stage 1 alone is a shippable, review-worthy product. Ship it before starting Stage 2.
- Until a pass's Stage-2 proof lands, Tier B covers it per-build.

### §8.1 Stage 4 is now ordinary — the seam is gone

**The 2026-07 record's three-amendment chain (§8.1 seam → §8.2 single-prover-Lean → §8.3
Stage-5-retired) does not apply and must not be carried forward.** Its entire content was:
*ZER's proofs are in Lean, CompCert's are in Coq, therefore the C interface semantics would be
formalised twice and their agreement is argued rather than machine-checked.*

Both sides are Coq here. The C99-subset semantics is formalised once. Composing
`emit_correct` with CompCert's `transf_c_program_correct` is a Coq-level composition like any
other.

Three consequences, recorded so nobody re-derives them:

1. **A single machine-checked theorem from ZER source to assembly is available in principle** —
   without a native backend, and without the "composed evidence" fallback §8.3 accepted.
2. **Stage 5 (a ZER-native verified backend) stays retired, and for its own reason** — it
   traded ~50 GCC targets for 2 and a zero-maintenance dependency for a permanent per-ISA
   obligation. **Architecture breadth IS the product.** That argument never depended on the
   prover and survives intact. Do not revive Stage 5 on the strength of §8.1's disappearance.
3. **The precedent is directly usable now.** `SJTU-PLV/CompCert` branch `rust-verified-compiler`
   builds a verified Rust-subset compiler as a Coq frontend on CompCertO
   (`Rustsurface → Rustsyntax → Rustlight → RustIR → Clight → CompCert backend`). Under the
   Lean plan that was a *reference architecture we could not use*. Under this plan it is the
   same prover, the same shape, and `Rustlight`/`RustIR` are precisely the λ-ZER / ZER-IR
   analogues. Their hardest unfinished component is verifying Polonius-based borrow
   *inference* — a **finder** — which is empirical support for §5's decision to verify a
   **checker** instead. Their CompCertO base is the right reference when ZER later needs
   linking theorems for separate compilation.

---

## §9 The two trust chains — the honest TCB analysis

```
CHAIN 1 — the flight artifact:
ZER source ──(checker: PROVEN)──(passes/emitter: PROVEN)──▶ emitted C99
   emitted C99 ──(CompCert: PROVEN — their small TCB)──▶ flight binary
   STATUS: closed by Stages 1–3 (+ CompCert), and at Stage 4 COMPOSABLE into one
           machine-checked theorem (§8.1) — the seam that blocked this is gone
           [daily builds swap the last link for GCC/Clang — trusted, see §10]

CHAIN 2 — the compiler binary itself:
Coq definitions (theorems PROVEN, kernel-checked)
   ──(RefinedC's frontend + automation)──(C compiler: UNVERIFIED)──▶ zerc binary
   STATUS: residual TCB — trusted, not verified
```

### 9.1 Chain 2 honestly stated — and how it DIFFERS from the Lean version

Under the Lean plan, chain 2 read: *theorems are about Lean definitions; the binary comes from
Lean's unverified compiler plus an unverified C compiler.* The gap was **extraction-shaped**.

Here it is **frontend-shaped**, and that is a genuinely different risk with a different profile:

```
GONE          the Lean compiler. Nothing in ZER is compiled from a prover's language.
GONE          "the theorem is about a definition, the binary is about something else."
              The theorem is about THIS .c file. gcc compiles that same file.
NEW           the CERBERUS FRONTEND. RefinedC proves things about `generated_code.v`,
              its Coq rendering of your C. If that rendering does not faithfully capture
              what the C means, the theorem is about a different program.
NEW           LITHIUM + the Coq automation. Bugs here cost SOUNDNESS only if they close
              a goal that is false; the coqc kernel re-checks the resulting proof term,
              which is exactly what makes this residue small.
```

The mitigating structure is the same shape as everywhere else in this document: **the kernel
re-checks.** Lithium is a tactic; a tactic that "proves" something invalid produces a proof
term the kernel rejects. So Lithium's size does not enter the TCB the way the Lean compiler's
did. What DOES enter is the frontend's C-to-Caesium translation and Caesium's model of C.

**Every verified system terminates in trust:**

- CompCert trusts: Coq's kernel, extraction, the OCaml compiler, the assembler, hardware.
- seL4 trusts: hardware, and that its spec captures "correct."
- **ZER trusts: Coq's kernel, RefinedC's frontend + Caesium's C model, one C compiler, hardware.**

**"Fully safe" is not a state that exists — for anyone.** The floor is always trust;
engineering is making the trusted part small, simple, inspectable, and diverse.

### 9.2 Mitigations

- **Differential testing of the frontend's model against reality.** Caesium's C semantics and
  what GCC emits are different objects. Running the proved C — with sanitizers — is a check ON
  THE MODEL, and it is cheap. Precedent for why this is not paranoia: a bounded model checker
  in the same family proved `malloc`-alignment facts that are FALSE on the real machine (glibc
  returned three different bases mod 64).
- **Build `zerc` with two different C compilers; cross-check on corpora.**
- **The mutant discipline (§5.4 rule 3)** as a permanent CI gate, not an authoring habit.

### 9.3 The kernel-computation escalation hatch

For the build that matters most, the check can be established **by the proof kernel itself**:

```coq
Theorem flight_build_safe : check flightProgram = true.
Proof. vm_compute. reflexivity. Qed.
```

Coq's `vm_compute`/`native_compute` reduce inside the kernel's trust story. The result is a
*kernel-checked theorem*: the C compiler drops out of the trust chain for that artifact.

Honest caveats, both larger here than the old record implied for Lean:
- This requires the checker to exist as a **Coq function**, not only as proved C. Where a
  function is specified in Coq and realised in C (the normal case here), the hatch applies to
  the Coq function, and the C's agreement with it is what RefinedC proved. That is still a
  bypass of the C compiler, but it is a two-step argument rather than one.
- Kernel evaluation is slow. Feasible for moderate inputs; it is the **per-artifact
  certification hatch**, not the daily path.

### 9.4 The claim language that survives review

> Chain 1 (source → emitted C → binary via CompCert): **proven end-to-end**, and composable
> into a single machine-checked theorem since both halves are Coq.
> Chain 2 (Coq definitions → `zerc` binary): **trusted residue** — Coq's kernel, RefinedC's
> C frontend, one C compiler — *monitored* by differential testing and dual-compiler builds,
> and **eliminable per-artifact** by kernel-computed certification of the final build.

Note what is no longer claimed and no longer needed: nothing about a prover's *runtime speed*.
Old §9.5 existed to answer "isn't Lean slow?" because `zerc` was going to BE a compiled-Lean
program. `zerc` is a C program. The question does not arise, and ZER's compile-time
performance is unchanged by this entire architecture — which is a real product benefit, quietly.

---

## §10 Backends — dual-backend strategy + verified CompCert facts

### 10.1 The strategy

- **Daily backend: latest mainline GCC/Clang.** ZER already emits C99. Trust status: chain-1's
  last link is "trusted compiler," acceptable for development, optionally hardened per-build
  by Tier B.
- **Certification backend: CompCert.** The same emitted C99 (constrained to the CompCert
  dialect, §11) compiled by CompCert closes chain 1 end-to-end with proofs. Customers running
  DO-178C programs bring their own licence. ZER itself stays unencumbered.

**What changed:** CompCert is now the *same prover* as ZER's proofs, so it is no longer only a
pragmatic standalone certification compiler — it is a composable theorem (§8.1). That raises
its value without changing the licensing position.

### 10.2 CompCert facts (verified 2026-07-19; unchanged)

**Backends** (from `AbsInt/CompCert` `configure`, master):

| Target | Variants |
|---|---|
| ARM 32 | `armv6`, `armv6t2`, `armv7a`, `armv7r`, **`armv7m` (Cortex-M3/M4/M7)** — each `-eabi`/`-eabihf`/`-linux`, plus big-endian mirrors |
| AArch64 | `aarch64-linux`, `aarch64-macos` |
| PowerPC | `ppc-eabi`, **`ppc-eabi-diab`** (Wind River Diab), `ppc-linux`, `ppc64-*`, `e5500-*` |
| RISC-V | `rv32-linux`, `rv64-linux` |
| x86 | `x86_32-{linux,bsd}`, `x86_64-{linux,bsd,macos,cygwin}` |
| AURIX/TriCore | commercial edition only |
| Gaps | Cortex-M0/M0+ (ARMv6-M), DSPs → fallback: GCC path (+ Tier B) |

**Pedigree:** qualified on the **ATR 42/72 aircraft (2026)** with credits under **DO-178C,
DO-333, DO-330**; **IEC 60880 Category A** and **IEC 61508 SIL-3** (MTU, 2017). v3.16
(Sept 2025), AbsInt release 26.04 (April 2026).

**Licensing:** free for research/education; **commercial use is a paid AbsInt licence**. This
is exactly why the dual-backend strategy exists — ZER must not hard-depend on CompCert.

**Performance:** ≈ GCC `-O1` class. Not a loss for certified code.

**Boundary of CompCert's proof:** it ends at assembly generation. Preprocessor, assembler,
linker and C library remain trusted (§12).

---

## §11 The emitter contract

1. **C99**, restricted to **CompCert's supported dialect**: no VLAs, no `setjmp`/`longjmp`, no
   computed goto.
2. **Freestanding profile**: no or minimal libc. Kills the largest non-primitive trusted
   component (§12) and is idiomatic for MCU targets.
3. **Clight-friendliness**: keep emitted constructs within reach of CompCert's Clight input,
   so the Stage-4 composition is an emitter refactor, not a redesign. **This is now
   substantially more valuable than it was**, because Stage 4 is a real composed theorem
   rather than an argued link (§8.1).
4. **Determinism**: byte-identical output for identical input.
5. Emitted code carries **no undefined behavior** by construction; the Stage-2 emission proof
   is against the formalized C99-subset semantics, which has no UB to fall into.

### 11.1 MEASURED 2026-08-01 — the contract is NOT adopted today

Every emitted file — including hello-world — carries GCC-only constructs in the *preamble*, so
`ccomp` rejects ZER output before reaching any user code:

| Construct | Count | Where | CompCert |
|---|---|---|---|
| `({ ... })` statement expressions | 2 | `_zer_shl` / `_zer_shr` macros | **rejected** |
| `__typeof__` | 2 | same macros | **rejected** |
| `__builtin_add_overflow` / `__builtin_sub_overflow` | 2 | `@addc` / `@subb` runtime | CompCert has its own builtin set |
| `__builtin_trap()` | 1 | trap path | replace with `abort()` or an emitted trap |
| `__attribute__` | 1 | packed/interrupt | partially supported — verify per use |

**Fix — a `--portable` (CompCert-dialect) emission mode:** per-width `static inline` shift
functions; portable carry/borrow detection (the `@mulw` `__int128` fallback is the template);
`abort()` for `@trap`; audit `__attribute__` uses; and **gate it** with a CI job running
`ccomp -c` over the emitted corpus, or it regresses immediately, exactly as it regressed
silently to reach this state.

**DECISION 2026-08-01 — DEFERRED, and this decision is now WEAKER than it was.** The portable
mode was deferred because CompCert was declined on licensing grounds, making the dialect
violation non-blocking. That reasoning stands on licensing but **not on architecture**: with
the seam gone, CompCert composition is the difference between "composed evidence" and "one
machine-checked theorem from source to assembly." Re-weigh the deferral when Stage 3 lands;
it does not need re-weighing before then.

**How this lands under the new plan.** Old §11 anticipated that once the emitter moved to
Lean, dialect conformance would become a property of an emitted-AST type — illegal states
unrepresentable. **There is no move, so that mechanism is not available.** The equivalent here
is a `--portable` mode plus the `ccomp -c` CI gate: conformance by *gate* rather than by
*construction*. Weaker, and the honest reason to keep the gate mandatory rather than advisory.

---

## §12 Toolchain residue — ranked honestly

Risk scales with **semantic freedom** (how much the tool transforms meaning):

| Trusted component | Semantic freedom | Real-world risk | Notes |
|---|---|---|---|
| **Linker script + startup code (crt0, vector tables, memory maps)** | n/a (human config) | **HIGHEST** | This is where embedded projects actually die. No proof anywhere covers your linker script. |
| libc (if linked) | large library | high | **Killed by the freestanding profile (§11.2)** |
| Linker | relocation, layout | low-medium | |
| **Assembler** | ~1:1 table-driven encoding | **LOWEST** | Almost no room to be *subtly* wrong. |

Mitigations: keep startup code tiny and reviewed; review the map file; checksum the image;
hardware-in-the-loop per board; disassembly review for the pad build. Nuclear options if a
programme ever demands closing even these: seL4-style **binary translation validation** and
CompCertELF-style verified assembly/linking — cite as "available if required," do not build.

**Porting, both senses:** porting ZER's *output* to a new board is linker scripts + startup =
configuration risk. Porting *CompCert* to a new ISA is person-years. Stay on the supported map;
exotic targets take the GCC path plus Tier B.

---

## §13 Coq / RefinedC implementation practicalities

- **Toolchain**: pin explicitly. The `kernelq-refinedc` image is **Rocq 9.1.0** with RefinedC
  `dev.2026-07-16`, working under `--network=none`, read-only root, and arbitrary-uid Docker.
  Reuse that image family for ZER CI.
- **The axiom oracle is the gate, never the exit code.** `Admitted` **exits 0** — measured. A
  CI gate on Coq proofs must check `Print Assumptions` (an admitted theorem shows an `Axioms:`
  block) exactly as `make check-proofs` already does for the existing corpus. The same applies
  to RefinedC: `refinedc check` exit 0 is **not** a verdict on its own, because two measured
  cheats pass it (§19.2).
- **The C subset is a style rule, not a capability limit** (§4.3). One operation per statement;
  no assignment used as an expression. Same shape `docs/proof-internals.md` already mandates.
- **Coq comments NEST**, so `(*opaque)` inside a `(* … *)` comment opens a nested comment and
  breaks the file. Reword C-syntax examples in `.v` comments. (Cost two cycles historically.)
- **Iteration loop for a new proved function**: annotate → `refinedc check` → read the residual
  goal → register a lemma over ITS term shape → repeat. Two rounds is normal; four means the
  vocabulary is wrong, not the lemma.
- **Verdict timing is diagnostic.** 1–2s = the frontend refused to parse the C; 5–30s = the
  prover actually ran. Any wrapper tooling should classify on this, and should never report a
  parse refusal as a failed proof.

---

## §14 Concurrency proofs (Iris) — no port, no trigger

The existing Coq/Iris corpus (`proofs/operational/lambda_zer_concurrency/`, ~870 lines, 11
files, zero admits) **is the concurrency reasoning, in the same prover as everything else.**

Under the Lean plan this section carried a scheduled obligation: a port to iris-lean, triggered
when λ-ZER moved to Lean, to avoid maintaining two formalizations of ZER's semantics.
**That obligation is cancelled.** There is one formalization, it is in Coq, and the compiler
proofs and the concurrency theorems live over it.

- Iris remains the right tool ONLY for the concurrent/heap reasoning; the compiler-correctness
  proofs (§8) are plain Coq — separation logic is not involved in semantic-preservation
  arguments.
- What the corpus actually is: it **instantiates Iris's weakest-precondition framework over
  ZER's own operational semantics** (`syntax.v` → `semantics.v` → `iris_lang.v` builds a
  `LanguageMixin`; `ESpawn` emits into the threadpool of the step relation; locks are
  invariant-opening proof devices). No library spinlock, no runtime concurrency — a transition
  system plus a logic over all its interleavings.
- Remaining work is unchanged and unaffected: operational adequacy, `wp_store`/shared specs,
  formal necessity. Compiler implementation of the closure not started.
- **Note a structural adjacency worth exploiting later:** RefinedC is itself built on Iris.
  The concurrency corpus and the C-verification stack sit on the same logical foundation, which
  is the natural route if ZER ever needs to verify *concurrent* C in its own runtime. Not
  scheduled; recorded so it is not missed.

---

## §15 Mission-critical / certification framing

- **Certification is per-artifact.** DO-178C verifies specific builds; DO-330 qualifies tools
  by understood failure modes; DO-333 admits formal methods for credit — and CompCert already
  *has* DO-178C/DO-333/DO-330 credits from the 2026 ATR qualification. ZER's architecture is
  shaped for per-artifact claims: Tier B witnesses per build, the kernel hatch per flight
  build, deterministic emission for reproducibility.
- **What evaluators probe first** is the two-chain distinction (§9). Lead with it; never claim
  "fully safe"; use the §9.4 claim language.
- **What IV&V can audit**: with the §5 certificate model, the annotations are the
  specification — reviewers read intent at boundaries instead of reverse-engineering inference.
- **The trusted-base summary for a review slide**: Coq kernel + RefinedC's C frontend +
  (daily: one C compiler | certified: CompCert's residue) + vendor assembler/linker + board
  config + hardware — with §12's mitigations attached to each.
- **A framing advantage worth using**: the verification stack is BSD/LGPL with no per-seat
  licence (§4.6), and the *prover* under ZER's proofs is the same one under CompCert's. "Same
  prover as the compiler Airbus qualified" is a sentence that does real work in a review, and
  it was not available under the Lean plan.

---

## §16 Roadmap and first steps

Ordered by leverage; every stage ships standalone value.

1. **Calibrate on `src/safety/*.c`** (P1 in §7.2). 85 already-extracted, already-VST-verified
   pure predicates, already in the flat style RefinedC wants. Prove a handful with RefinedC and
   **measure**: annotation lines per function, iterations to close, wall-clock. Every estimate
   in this document is from KernelQ-scale code; ZER-scale numbers replace them.
2. **Build the authoring gate before the second function.** Spec + mutant + `Print Assumptions`
   in CI (§5.4, §19.2). A pipeline whose green light cannot fail is worth nothing, and this is
   the cheapest moment to prevent that.
3. **The arena decision** (§19.4) — before any new IR code exists. It is the prerequisite for
   proving anything pointer-shaped and it is expensive to retrofit.
4. **Certificate-model design** (§5.2): specify the boundary-annotation vocabulary against the
   existing rule classes; the `verif_*` taxonomy is the checklist. Design doc + λ-ZER
   extension, before code. *No prover work required — do not block this on proof skills.*
5. **Stage 0 completion**: whatever λ-ZER still lacks (IR semantics, the C99-subset target
   semantics). Much of Stage 0 already exists, which is the biggest schedule change.
6. **Stage 1**: `checker_sound` — the first headline theorem. Ship/announce.
7. **Stages 2–3** pass by pass, with Tier B covering whatever is not yet proven.
8. **Stage 4** when justified: the CompCert composition — now a real theorem (§8.1). Gated on
   the §11 portable-emission mode, which is the concrete cost of taking it.
9. Ongoing: dual-compiler builds, differential testing of the proved C against its Coq spec,
   and the `ccomp -c` dialect gate.

Skill note: the founder's KernelQ track is the deliberate on-ramp and it is now **the same
stack** — Coq proofs plus RefinedC-linked C. Steps 1–4 need no new prover skill.

---

## §17 Rejected-alternatives ledger (do not re-propose without new evidence)

| Alternative | Why rejected | Revisit if |
|---|---|---|
| Stay in C with no verification | No provable implementation; the tool is memory-unsafe with nothing checking it | never (as end-state) |
| Rewrite in OCaml or Rust (tool-safety only) | Solves memory safety but not provability; the model–implementation gap remains | if the RefinedC bet fails wholesale |
| **Rewrite the compiler into Lean 4 (the 2026-07-19 decision)** | **SUPERSEDED 2026-08-15 (§0.1).** It existed to close the model–implementation gap; RefinedC closes that gap on the shipped C without migrating 47k working lines. It also stranded 94 `.v` files, scheduled an Iris port, and created a cross-prover seam with CompCert that cost three amendments to manage. | only if RefinedC's envelope (§4.4) proves too narrow for ZER's real passes AND no Tier-B route covers the remainder |
| **CBMC / bounded translation validation as the Tier B mechanism** | **SUPERSEDED 2026-08-15.** Bounded equivalence up to an unwind depth, requiring an emitted reference C (an unproved `render` step) and carrying four measured silent semantic traps. RefinedC proves the real C for ALL inputs with no reference artifact. | for a specific function outside RefinedC's envelope where a bounded claim is genuinely enough — as Tier B, per §6 |
| Lean 4 as the prover for new work | Same expressiveness; but nothing is written in the prover, so its ergonomic and no-extraction-gap advantages do not apply; and it strands Iris/VST/RefinedC/CompCert and 94 existing files | if all four Coq libraries ZER depends on are matched in Lean AND the corpus is already ported |
| Tier B only (validation, no verified passes) | Per-build assurance without once-for-all theorems; weaker headline claim | n/a — B is kept as the bridge |
| Tier C (CakeML-style verified self-hosting) | Hardest artifact in the field; the self-hosting flag is worth less than the proof flag | if ZER becomes a funded team effort with years of runway |
| ZER-in-ZER self-hosting for the core | Loses provability and orphans the proofs | periphery only (LSP/tooling), never the proven core |
| Making ZER itself proof-capable (refinement/dependent types) | Building a second Coq; a different mountain | someday-research, not the plan |
| "Fully safe" claims / trusting testing as proof | Testing bounds risk, never proves absence | never |
| Hard dependency on CompCert as the only backend | Commercial licensing; backend gaps (ARMv6-M, DSPs) | n/a — dual backend is strictly better |
| **A ZER-native backend emitting ISA directly (the old Stage 5)** | **RETIRED, and the retirement SURVIVES the prover switch.** Trades ~50 GCC targets for 2 and a zero-maintenance dependency for a permanent per-ISA obligation. Architecture breadth IS the product. Note the seam argument that once motivated it is now void (§8.1) — that makes it *less* attractive, not more. | only if a customer requires end-to-end assurance on a target CompCert does not support — AND funds it |
| Verifying `checker.c` with **VST** instead of RefinedC | Same goal, same prover, 23 files of lived experience: interactive tactics, days per function. The pain was the interaction model, not the target | for a specific function whose shape RefinedC's automation cannot reach but VST's tactics can — a real fallback, not a competitor |
| Writing the RefinedC specification free-hand alongside the C | A specification transcribed rather than cited can drift from λ-ZER, and both halves then pass while proving the wrong thing (§5.4 rule 1) | never — cite `proofs/operational/`, single-home the definition |
| Trusting `refinedc check` exit 0 as the verdict | Two measured cheats pass it: `rc::trust_me` emits a "proof" that is literally a comment; an injected axiom discharges everything (§19.2) | never — `Print Assumptions` + the surface allow-list |

---

## §19 The linked pipeline as built — the mechanism, its guards, its envelope

**Read this before building any verification automation around RefinedC.** The mechanism below
is running in production in KernelQ and every trap in it was *measured*, not anticipated. The
KernelQ-side full record is `docs/kernelq-pedagogy-goal.md` **LS.26–LS.29**.

### 19.1 The three stages, and what each is worth

```
STAGE 1   coqc over the Coq definitions + the theorem      is the MATH right?
          + Print Assumptions                              ALL inputs. Kernel-checked.

STAGE 2   refinedc check over the annotated C              does the C compute that math?
          + Print Assumptions type_<fn>                    ALL inputs. Kernel-checked.

STAGE 3   gcc -Wall -Wextra -fsanitize=undefined,          does the REAL BINARY behave,
          run it, time it                                  and how fast?  SAMPLED.
```

**Stage 3 does not become vestigial when Stage 2 exists — its JOB changes.** Stage 2 proves
things inside Caesium's model of C. Stage 3 covers what that model does not:

```
1  MODEL vs REALITY    Caesium's C semantics is not what gcc emits. Running the binary is a
                       check ON THE MODEL. Not hypothetical: a sibling tool proved
                       malloc-alignment facts that are FALSE on glibc.
2  DOES IT EVEN BUILD  the prover accepting the source does not mean gcc does.
3  UB AT -O2           the optimiser exploits UB in ways a model may permit. Measured: UBSan
                       caught an `int` accumulator that the proof layer had no opinion on.
4  THE CLOCK           Stage 2 says nothing about speed, and compile time is a product
                       feature (§2). This is the whole reason C is here at all.
```

### 19.2 THE GOVERNING PRINCIPLE, and every guard that exists because of it

```
   A CHECK THAT CANNOT FAIL IS INDISTINGUISHABLE FROM A CHECK THAT PASSES.
```

Every bug found building this had that shape, and **not one was in the prover** — all were in
the *invocation*. This list is a checklist for ZER, not history.

```
EXIT CODE IS NOT A VERDICT   `refinedc check` exit 0 passes two MEASURED cheats:
                             [[rc::trust_me]] emits a proof file that is literally a
                             comment (no theorem at all), and an injected
                             //@rc::inlined_final Axiom ... : False discharges everything.
                             -> Print Assumptions type_<fn> must read "Closed under the
                                global context", AND a surface allow-list must refuse
                                every attribute outside the intended set, BEFORE a
                                container starts. Same discipline as `Admitted` exiting 0.

VACUOUS SPECIFICATION        contradictory preconditions make the triple vacuously true.
                             MEASURED: {0 <= n} with {n < 0} PROVES `return 12345;`
                             against `returns n`. ACCEPT in 3s.
                             -> every proved function keeps a MUTANT that must be REJECTED.

MISATTRIBUTED VERDICT        a PARSER refusal is not a failed proof, and reporting it as one
                             sends the reader to the wrong place entirely. MEASURED: a
                             `return a = b;` produced "the loop annotation may be too weak"
                             on a function with no loop.
                             -> classify frontend rejections as their own stage. The timing
                                tell (1-2s vs 5-30s) is a reliable secondary signal.

DISCARDED DIAGNOSTIC         the prover states EXACTLY what it could not establish -- the
                             goal, with hypotheses above the line and the target below.
                             Summarising it away is the same defect class as misattributing
                             it: the answer was in the log and the message said something
                             else.
                             -> surface the residual goal verbatim.

WRONG LINE NUMBERS,          two different coordinate systems, MEASURED:
TWO KINDS                      Cerberus FRONTEND errors are in SPLICED-file coordinates --
                               subtract the author prefix or you point 15 lines past the
                               mistake.
                               Lithium `Location:` numbers are in PREPROCESSED coordinates
                               -- an 18-line unit reported line 132, true offset 118, and
                               the offset depends on the image's headers.
                             -> map the first; DO NOT SURFACE the second. A confidently
                                wrong line is worse than none.

MULTI-HOMED DEFINITION       the Coq function written twice -- once in the semantics, once
                             in the RefinedC prelude -- lets both stages pass while grading
                             DIFFERENT functions.
                             -> single-home it and splice. This is CLAUDE.md's documented
                                #1 recurring bug class, in a new location.

FIXES THAT NEVER SHIP        a verified fix absent from every live artifact because each
                             embeds its own copy of the prelude.
                             -> one propagation step plus a drift gate at sync time.
```

### 19.3 GENERATED vs HAND-WRITTEN — decide by FAILURE MODE, not by taste

```
DRIFT FAILS SILENTLY  ->  MUST BE GENERATED
  anything the proof obligation rides on. A hand-written artifact that disagrees with the
  specification still reports success -- it just proved something else, and nothing says so.

DRIFT FAILS LOUDLY    ->  MAY BE HAND-WRITTEN
  behavioural/differential tests. Wrong signature = link error. It is TESTING, not proving.
```

…with one rule that makes hand-written safe: **it must reject the mutant.** A hand-written
test can be vacuous (compare the artifact to itself, forget to call it) and nothing would say
so. Running it against the mutant costs milliseconds.

### 19.4 THE ARENA DECISION — make it before any new IR code exists

**The one architectural choice that is expensive to retrofit, and it is now load-bearing for a
second reason.**

Under the old record this decision existed because the emission AST spoke int64 scalars and
arrays, and pointers in the AST break totality. **The reason has changed and strengthened**:
§4.4 measured that **pointer-walking is RefinedC's one structural stall** — the residual is a
separation-logic goal no annotation reaches. ZER's passes operate on ASTs, symbol tables,
region lattices and CFGs, which are exactly that shape.

```
POINTER TREES     struct Node { Node *l, *r; }
                  -> the shape that does not close. Verification stops here.
ARENA + INDICES   int32 arrays: kind[], lhs[], rhs[], type[]
                  a "pointer" is an INDEX into a flat array
                  -> integer and array reasoning, which is where the automation is strongest
```

This is not a concession to the verifier. **Fast compilers represent IRs this way anyway** —
cache-friendly, no allocator churn, trivially serialisable. It happens to also be the
representation the proof pipeline can express.

Same technique for fixpoint passes: dataflow on a lattice of height `h` converges in `≤ h·n`
steps, so a bounded loop with a proven-sufficient trip count replaces the unbounded `while`,
and its invariant is expressible.

### 19.5 The specification-side traps that survive the prover switch

Two of the four semantic traps recorded in the old §19.4 were about emitting C from a prover
and are gone with it. Two are about the *relationship between a prover's arithmetic and C's*,
and they apply here unchanged:

```
INTEGER PROMOTION   C makes `u8 + u8` an `int`. A mixed-width specification is well-typed
                    and wrong. -> be explicit about widths in the spec; the C's own type is
                    not the specification's type.

DIVISION            Coq's `Z.div` is EUCLIDEAN; C TRUNCATES toward zero. They agree only on
                    non-negative operands. -> `Z.quot`/`Z.rem` mirror C; `Z.div`/`Z.modulo`
                    do not. Getting this wrong produces a proof of the wrong theorem, and
                    the C is fine.
```

A third, added from measurement here: **you type ASCII, the prover prints Unicode.** Coq
displays `≤` for `<=` and `¬` for `~`. Specifications should be written in ASCII (measured
working) so that what an author types matches what a reviewer can reproduce; the divergence in
output is display only.

### 19.6 What is NOT built for ZER

```
λ-ZER completion                  IR semantics + the C99-subset target semantics. ZER's own
                                  content; nothing in the tooling supplies it.
the SPEC->C link for real passes  measured only on small functions. The §16.1 calibration on
                                  src/safety/*.c is what replaces estimates with numbers.
arena-flattened IR                the 19.4 decision.
the authoring gate                spec + mutant + Print Assumptions in CI. §16.2. Cheapest
                                  now; the thing whose absence makes everything else a
                                  green light that cannot fail.
a portable emission mode          §11.1. Gated on wanting Stage 4.
```

---

**Status: ADOPTED 2026-08-15 (§0–§19).** Supersedes the 2026-07-19 Lean-4 record in full.
Retains §5 (certificate principle), §6 (tiers), §9 (two chains), §10 (dual backend), §11
(emitter contract), §12 (residue), §15 (certification framing) in substance. Removes Lean 4
and CBMC entirely, with the reasons in §0.1. Does not re-propose any §17 entry.

---

## §18 Sources and provenance

Empirical results produced on this machine:

- **2026-08-14/15** — RefinedC measured end to end in the `kernelq-refinedc` image
  (Rocq 9.1.0, RefinedC `dev.2026-07-16`): the free counting-loop spec; the linked spec at
  ~6 lines of author Coq; the unroll series (8x ACCEPT 6s, 32x ACCEPT 23s, uncapped); the
  `k + 8 <= n` overflow catch and its UBSan confirmation at `LLONG_MAX - 4`; the pointer-walking
  stall; the C-subset probes (`+=`, `k++`, ternary accepted; nested assignment refused); the
  two cheats that pass exit 0; the vacuous-precondition ACCEPT; the two line-number coordinate
  systems. Verdict log: `KernelQ/refinedc/logs/VERDICTS`.
- **2026-08-15** — repo measurement: **94 `.v` files** (23 `proofs/vst/`, 66
  `proofs/operational/`); RefinedC licence **BSD**; Rocq **9.1.0**.
- **2026-07-19** — ZER code measurement (§1.1); ZER proof-corpus reading (§1.2).

Web-verified 2026-07-19 (unchanged and still load-bearing):

- CompCert targets: `github.com/AbsInt/CompCert` (`configure`, master)
- CompCert product/qualification/licensing: `absint.com/compcert` (ATR 42/72 2026 with
  DO-178C/DO-333/DO-330 credits; MTU IEC 60880 Cat A / IEC 61508 SIL-3, 2017), `compcert.org`
  (v3.16 2025-09; manual v3.17), AbsInt factsheet release 26.04 (2026-04)
- Verified-Rust-on-CompCert precedent (§8.1): `github.com/SJTU-PLV/CompCert`, branch
  `rust-verified-compiler`

Superseded provenance, kept so the reasoning can be audited rather than re-run: the 2026-07-19
Lean decision (iris-lean build + axiom probe, Lean v4.32.0 CLI contract) and the 2026-08-11
Lean→CBMC pipeline record. Both are in git history and in the KernelQ doc's LS.0–LS.25; LS.26
onward is the current stack.

---

*End of decision record. If you are a fresh session about to work on ZER's compiler,
verification, backends, or trust story: this document is the complete context. Verify
empirical claims by re-running the recorded commands — do not re-derive the decisions, do not
re-propose entries from §17, and do not re-propose Lean or CBMC without reading §0.1 first.*
