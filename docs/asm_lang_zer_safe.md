# ZER-Asm Safety — Hybrid Operand-Annotated Approach

**Status:** Planning document. Decision made 2026-05-12. Not yet executed.
**Date:** 2026-05-12 (drafted from session-long architectural discussion)
**Supersedes:** the extension trajectory of `docs/asm_plan.md` (Session G Phase 5,
Z9/Z10/Z13, further per-instruction database growth).
**Does NOT supersede:** the already-shipped work in `asm_plan.md` (Z1-Z12,
F4-F6 register tables, F7-light state machine, F7-full Step 2 constraints,
130 intrinsics). All shipped work stays.

**Scope:** Internal architectural decision on how ZER's asm safety story
evolves from here. No new safety claims. No user-visible behavior change
for existing asm code. Pivots the asm-safety work from "make the compiler
smart about specific instructions" to "make the language expressive enough
that the compiler enforces user-declared contracts via existing generic
infrastructure."

**Effort to complete:** ~3-4 months one-time. Maintenance after: near-zero.
**Risk:** LOW. Builds on existing Z-rules infrastructure, doesn't rewrite
anything. Each addition is independently testable and revertable.
**Architectural certainty:** HIGH. Decision backed by:
- Production compiler precedent (Rust, Zig, seL4 all chose "validate boundary, not contents")
- ZER's own existing architecture (smart language + generic compiler)
- Empirical maintenance experience (F4-F7 per-instruction work)

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [The Decision in One Page](#2-the-decision-in-one-page)
3. [Why This Pivot — Bug History and Maintenance Reality](#3-why-this-pivot--bug-history-and-maintenance-reality)
4. [The Architectural Principle Being Applied](#4-the-architectural-principle-being-applied)
5. [Journey of the Discussion](#5-journey-of-the-discussion)
6. [Three Options Considered](#6-three-options-considered)
7. [What We're Keeping from asm_plan.md](#7-what-were-keeping-from-asm_planmd)
8. [What We're Stopping or Deferring](#8-what-were-stopping-or-deferring)
9. [The Concrete Design](#9-the-concrete-design)
10. [The Tiny Implicit-Precondition Table](#10-the-tiny-implicit-precondition-table)
11. [State Machine Annotation Generalization](#11-state-machine-annotation-generalization)
12. [Integration with Existing Safety Systems](#12-integration-with-existing-safety-systems)
13. [Safety Coverage Matrix — Proven Equivalence](#13-safety-coverage-matrix--proven-equivalence)
14. [Production-Compiler Precedents](#14-production-compiler-precedents)
15. [Effort Breakdown](#15-effort-breakdown)
16. [Implementation Strategy and Phasing](#16-implementation-strategy-and-phasing)
17. [Testing Strategy](#17-testing-strategy)
18. [Risks and Mitigations](#18-risks-and-mitigations)
19. [Open Questions — Decided](#19-open-questions--decided)
20. [Out of Scope (Explicitly NOT Doing)](#20-out-of-scope-explicitly-not-doing)
21. [The "Smart Language vs Smart Compiler" Principle](#21-the-smart-language-vs-smart-compiler-principle)
22. [Fresh-Session Onboarding Checklist](#22-fresh-session-onboarding-checklist)
23. [Appendix A: Code Samples](#appendix-a-code-samples)
24. [Appendix B: Annotation Reference](#appendix-b-annotation-reference)
25. [Appendix C: Implicit-Precondition Table — Concrete Contents](#appendix-c-implicit-precondition-table--concrete-contents)
26. [Appendix D: Mapping from asm_plan to This Approach](#appendix-d-mapping-from-asm_plan-to-this-approach)
27. [Appendix E: References](#appendix-e-references)

---

## 1. Executive Summary

ZER asm safety reached a decision point in May 2026. The original direction
(`docs/asm_plan.md`, D-Alpha-7.5 Phase 2) committed to building per-instruction
safety knowledge into the compiler: vendored instruction tables, classification
rules, per-instruction constraints (NONZERO/ALIGNED/etc.), and a CFG-aware
OrderingState pass (Session G).

By 2026-05-02, ~225 hours had been invested and ~80 hours remained (Session G
Phase 5 + Z9/Z10/Z13 + ongoing per-ISA-extension maintenance). The framework
was working — F4-F6 tables vendored, F7-light state machine catching real UB,
F7-full Step 2 constraints rejecting BSR-on-zero / MOVAPS-misaligned / etc.

**The problem the session-long discussion identified:**

The per-instruction database is a **moving target**. Every ISA generation
adds 50-200 new instructions. Every new sub-extension (AVX-10.2, SVE 3.0,
RISC-V V 1.1) requires hundreds of new entries. The "compiler smart about
asm" approach commits to perpetual maintenance against an evolving silicon
ecosystem.

**The pivot:**

Don't make the compiler smart about specific instructions. Instead, make
the **LANGUAGE expressive enough** that users declare instruction
preconditions as **operand-level annotations**, and the **compiler enforces
them via existing generic safety infrastructure** (VRP, alignment, state
machines, context flags, escape analysis).

For the ~100 most-common UB-prone instructions (BSR-on-zero, IDIV-on-zero,
MOVAPS-misaligned, LR/SC pairing, MONITOR/MWAIT pairing, AMX
TILECONFIG/TILELOAD, etc.), maintain a **tiny implicit-precondition
table** that auto-applies the annotations so users don't have to type
them. This table is **frozen** — BSR's UB hasn't changed since 8086 in
1978; future ISA extensions don't add new well-known UB classics.

**The result:**

| Property | Original asm_plan (fully done) | This approach |
|---|---|---|
| Compile-time safety coverage | ~99% | ~99% (identical) |
| Per-instruction database | ~4,400+ entries growing | ~100 frozen entries |
| Maintenance per new ISA extension | Hundreds of entries | Zero |
| Effort remaining | ~80 hours + perpetual maintenance | ~3-4 months + zero maintenance |
| User experience | Identical | Identical |
| Architectural cleanness | Parallel Z-rule pipeline | Z-rules stay; language carries contracts |

**Net effect:** same safety, dramatically less maintenance, builds on
existing infrastructure instead of rewriting.

---

## 2. The Decision in One Page

**Keep (already shipped, working):**
- All 130 intrinsics from D-Alpha-1 through D-Alpha-14
- Z-rules Z1 through Z8, Z11, Z12 (10 of 13 wired into NODE_ASM)
- Structured asm syntax (Session A: `asm { instructions: ... safety: ... }`)
- Typed operand bindings (Session B: `inputs:`, `outputs:`, `clobbers:`)
- Per-arch register tables (F2/F5/F6, auto-probed from GCC, vendored)
- CPU feature gating (F4 dispatch via `--target-features=`)
- F7-light C3 state machine (LR/SC pairing across all 3 archs)
- F7-full Step 2 constraints (NONZERO/COMPOUND/ALIGNED/BOUNDED via VRP+alignment)
- C8 instruction classification (vendored ordering metadata for memory barriers)
- Sub-extension architecture validation (3-arch empirical proof, locked-in design)

**Add (incremental work, 3-4 months):**
- Operand metadata fields on existing typed bindings: `size`, `align`, `role`
- Operand-level precondition annotations: `requires:`, `align:`, `opens_state:`, `closes_state:`
- Tiny implicit-precondition table (~100 well-known UB classics) auto-applying annotations
- State-machine annotation generalization (extend F7-light pattern to MONITOR/MWAIT, AMX, etc.)
- Wire annotations to existing safety systems (VRP for nonzero, alignment for align, etc.)
- Documentation update reflecting honest claim
- Test suite extension

**Stop / Defer (don't extend):**
- Session G Phase 5 (IR-level CFG OrderingState for acquire/release pairing)
- Z9 / Z10 / Z13 forward-compat Z-rules (blocked on S1 relaxation we're not doing)
- Adding more entries to F4/F5/F6 instruction tables beyond what's vendored
- Per-instruction UB detection for obscure instructions (covered by user annotations if needed)
- C5 explicit kernel-context model (covered by S1 naked-only restriction)
- Naked-attribute migration (deferred indefinitely)

**Honest framing for public claim:**
- ✓ "Rust-level operand-type safety + per-arch register validation + CPU feature gating"
- ✓ "130 intrinsics cover ~80% of common kernel/firmware asm patterns"
- ✓ "Operand-level precondition annotations + ~100 well-known UB classics auto-protected"
- ✓ "Raw asm escape hatch in `naked fn` — contents not validated (same as Rust)"
- ✓ "Frozen framework — no per-ISA-extension maintenance burden"
- ✗ NOT claiming "100% language-safe asm via 13 Z-rules + 8 categories + System #30"
- ✗ NOT claiming "every instruction's preconditions verified at compile time"

---

## 3. Why This Pivot — Bug History and Maintenance Reality

### 3.1 The bug rate signal

Over the 6-week period from 2026-04-23 to 2026-05-02, the asm-safety work
hit a recurring pattern:

| Date | Issue | Class |
|---|---|---|
| 2026-04-23 | D-Alpha-7.5 Phase 1 ships (`naked` restriction, S1 rule) | OK — interim guard works |
| 2026-04-24 | Per-instruction precondition database evaluated and deferred | Recognized maintenance burden early |
| 2026-04-25 | Option C committed — universal precondition categories + System #30 | Architectural pivot toward generic framework |
| 2026-04-26 | F1a category framework, F2 build-time probe pipeline | Infrastructure investment |
| 2026-04-29 | Sub-extension architecture validated 3-arch | Empirical foundation |
| 2026-05-02 | F4/F5/F6 instruction tables vendored (53+37+30 entries) | 112 entries done |
| 2026-05-02 | F7-light LR/SC pairing shipped | Works |
| 2026-05-02 | F7-full Step 2 constraints shipped (NONZERO/COMPOUND/ALIGNED/BOUNDED) | Works |
| 2026-05-02 | Session G Phase 1+2 (ordering plumbing + classification) shipped | Plumbing only |
| 2026-05-02 | Session G Phase 3 attempted, ABANDONED | False-positive on libpmem CLWB+SFENCE idiom |
| 2026-05-12 | This pivot decision | Architectural reassessment |

### 3.2 The maintenance projection

If we continued the original direction, the projected work over the next 5 years:

| Year | Expected ISA events | Maintenance impact |
|---|---|---|
| 2026 | Session G Phase 5 (CFG OrderingState) | ~30-40 hours |
| 2026 | Z9/Z10/Z13 forward-compat | ~50 hours |
| 2027 | AVX-10.2 release expected | ~100-200 instruction entries |
| 2027 | ARM v10 SVE 3.0 | ~150-200 entries |
| 2028 | RISC-V V 1.1 + new Zb*/Zk* extensions | ~100 entries |
| 2028 | Intel AVX-512 successor (probable) | ~200+ entries |
| 2029 | ARM SME 2.0 | ~150 entries |
| 2030 | Various vendor extensions | ~200+ entries |

Cumulative over 5 years: **~1,000-1,500 instruction entries to add, vet,
classify, integrate, test**. Plus ongoing per-instruction bug fixes as
edge cases emerge (the Session G Phase 3 abandonment is the canonical
example of "we tried, real code broke, had to revert").

### 3.3 The realization

The per-instruction approach commits ZER to **perpetual maintenance of a
domain that is fundamentally not ZER's domain**. Silicon evolution is
Intel/ARM/RISC-V's responsibility. They publish manuals; GCC integrates
the assembler; ZER doesn't need to know what every instruction does.

The asm_plan direction implicitly assumed: "since we want to make asm
safe, we need to know what asm does." This is **wrong**. The correct
inversion: "since we want to make asm safe, we need users to declare
what their asm does (operand structure + preconditions), and we enforce
those declarations via existing generic infrastructure."

The user owns the instruction manual. The compiler owns the safety
infrastructure. The asm code is the contract between them, expressed
through structured operand annotations.

### 3.4 The Session G Phase 3 lesson

The Phase 3 attempt was the empirical signal that the per-instruction-
knowledge approach has fundamental issues:

**The bug:** Same-block check rejected the canonical Intel libpmem
persistent-memory idiom (CLWB issued in one block, SFENCE in another). The
"correct" check requires CFG-aware analysis (Phase 5) which is another
30-40 hours of work and a 700+ line addition to `zercheck_ir.c`.

**The deeper lesson:** Even with perfect per-instruction knowledge, the
COMPILER-SIDE analysis to enforce that knowledge is complex and error-prone.
Phase 3 had correct per-instruction data; the analysis logic still got
real code wrong. CFG-aware versions are expected to discover their own
edge cases ("won't false-positive on libpmem" doesn't mean "won't
false-positive on the next exotic pattern").

In contrast, **user-declared annotations** make the analysis trivial:
- User declares `opens_state: persistence_fence_pending` on CLWB
- User declares `closes_state: persistence_fence_pending` on SFENCE
- Generic state-machine pattern (same as F7-light LR/SC) enforces pairing
- If user's CFG-spanning code is correct, the state machine just works
- If user forgets to declare the dependency, well — it's their declared
  contract; same as any other user assertion in any language

The compiler doesn't have to be smart about libpmem semantics. The user
already knows; they declare via annotations.

### 3.5 Convergent evidence: Rust didn't do this either

Rust's `core::arch` ships ~5,000 intrinsics. Rust's `asm!()` checks operand
types and call interface — period. No per-instruction UB detection in the
asm macro. No CFG-aware analysis of asm contents.

When Rust users write asm requiring instruction-specific preconditions, they
use `// SAFETY:` comments documenting their reasoning, and the unsafe block
is the boundary. The compiler trusts the human declaration.

Rust made this call after extensive design discussion (RFC 2873, Rust
Inline Assembly reference). They explicitly chose NOT to do what ZER's
asm_plan was attempting. Their reasoning: maintenance burden too high,
real benefit too small, escape hatch (raw asm in unsafe blocks) is
sufficient for the small population of users who write raw asm.

ZER's pivot aligns with Rust's design, plus the additional ZER-specific
advantage: **per-arch register validation, CPU feature gating, and
~100-classics implicit preconditions** that Rust doesn't have. ZER ends up
slightly safer than Rust's asm at zero ongoing cost.

---

## 4. The Architectural Principle Being Applied

### 4.1 The principle (one sentence)

**Compilers should be generic algorithms over annotated languages, not
databases of facts about specific entities.**

### 4.2 Where ZER already follows this

ZER's existing safety architecture rigorously applies this principle:

| Per-item information | Where it lives | Why this works |
|---|---|---|
| Which fields are volatile? | Language: `volatile *u32 reg` annotation | Compiler enforces generically (qualifier preservation, no field-specific knowledge) |
| Which params are kept? | Language: `keep *Handler h` annotation | Compiler enforces generically (escape analysis, no parameter-specific knowledge) |
| Which addresses are MMIO? | Language: `mmio 0x4000..0x4FFF;` declaration | Compiler enforces generically (range check, no per-device knowledge) |
| Which structs auto-lock? | Language: `shared struct C { }` annotation | Compiler enforces generically (auto-lock around accesses, no per-struct logic) |
| What's a handle? | Language: `Handle(T) h` type | Compiler enforces generically (state machine, no per-handle knowledge) |
| Which structs transfer? | Language: `move struct F { }` declaration | Compiler enforces generically (HS_TRANSFERRED state, no per-struct logic) |
| Which functions can yield? | Computed: `Symbol.props.can_yield` (FuncProps) | Generic recursive analysis, no per-function annotation |
| Which params are integers? | Language: `u32 x` type annotation | Compiler enforces generically (type system, no parameter-specific knowledge) |

**None of these require the compiler to have a database of specific structs,
specific addresses, specific drivers, or specific operations.** The
language carries the per-item information; the compiler runs generic
algorithms.

### 4.3 Where ZER could violate the principle (avoided)

| Tempting per-item database | Why we avoided it | Generic alternative |
|---|---|---|
| Per-struct alignment rules | Would need updates per struct definition | `_Alignas` or computed alignment |
| Per-driver lint rules | Linux's checkpatch.pl is maintenance hell | Generic safety properties + annotations |
| Per-function purity facts | Would freeze API decisions | Generic FuncProps analysis |
| Per-API channel safety | Each new IPC pattern would need rules | Generic shared struct + Ring(T) |
| Per-protocol parser rules | Each protocol = new compiler knowledge | Generic comptime + type system |

ZER consistently chose the generic-algorithm-over-annotated-language path.

### 4.4 Where asm_plan accidentally violated the principle

The original asm_plan direction broke this pattern by encoding per-instruction
facts in the compiler:

| Per-instruction fact | Encoded as | Maintenance cost |
|---|---|---|
| "BSR has UB on zero" | Constraint in instruction table | Update if BSR semantics ever change (rare) |
| "MOVAPS needs 16-byte alignment" | Constraint in instruction table | Update if MOVAPS specs change (rare) |
| "LR must precede SC" | State-machine class in instruction table | Update if RISC-V atomic model changes |
| "MFENCE produces full-memory barrier" | Ordering metadata in instruction table | Update if x86 memory model changes |
| "ldtilecfg must precede tileload" | State-machine class in instruction table | Update if AMX semantics evolve |
| ...× ~4,000 more facts | Per-instruction entries | Compounds with every ISA extension |

These facts aren't going to change (mostly). But the **set** of relevant
facts grows with every new instruction. ARM SVE 3.0 has ~200 new
instructions; some have new UB classes; the compiler must learn them.

**The right answer** is the same as everywhere else in ZER: let the user
declare via annotations, enforce via generic infrastructure.

```zer
asm_inst {
    op: "bsr",
    reads_reg: { name: "eax", value: x, requires: nonzero },
    writes_reg: { name: "ecx" },
}
```

`requires: nonzero` is the annotation. VRP (System #12) is the generic
infrastructure. No per-instruction knowledge needed.

For ergonomics, the implicit-precondition table provides auto-application
for ~100 well-known classics — same as how `type_is_optional()` internally
unwraps `TYPE_DISTINCT` so users don't have to call `type_unwrap_distinct()`
everywhere. Ergonomic helper, not architectural commitment.

### 4.5 The general lesson

If you find yourself adding a database to a compiler about specific items
(specific structs, specific functions, specific API calls, specific
instructions, specific addresses), **STOP**. The right answer is:

1. Identify the property the database encodes (alignment requirement,
   purity, ownership rule, UB condition).
2. Design a LANGUAGE-LEVEL annotation that expresses the property.
3. Write a GENERIC algorithm that enforces the annotation.
4. Optionally maintain a tiny stable table for common ergonomic auto-fill.

Done. The compiler stays generic; the world can change without the compiler
changing.

This is why:
- **Rust's lifetime annotations** work (generic) and ad-hoc per-function
  borrow rules wouldn't (database).
- **C's `volatile`** works (generic) and "compiler knows about this register"
  wouldn't (database).
- **Haskell's type classes** work (generic) and per-function monadic
  declarations wouldn't (database).
- **ZER's `shared struct`** works (generic) and per-struct lock rules
  wouldn't (database).

ZER's existing 28 of 29 safety systems follow the principle. The asm_plan
direction was the 29th system trying to violate it. The pivot brings asm
into alignment with the rest of the architecture.

---

## 5. Journey of the Discussion

This document captures a multi-day architectural discussion. The
reasoning trail matters as much as the conclusion. Preserved here so
fresh sessions can pick up the context.

### 5.1 Starting point (2026-05-12 morning)

User asked: "where are we on asm_plan.md?"

Answer at the time: D-Alpha-7.5 Phase 2 in progress. F4-F7 instruction
tables shipped. Session G Phase 3 abandoned. Session G Phase 5 (CFG
OrderingState) next at ~30-40 hours. Z9/Z10/Z13 forward-compat blocked
on S1 relaxation.

### 5.2 First framing question (user)

> "we did so complex... in OSes / baremetal, when people use asm directly,
> they usually just use ASM directly instead of going through C / higher
> level. Knowing this, my thought is: since we going so raw asm and making
> saw, might just well better to make raw asm unsafe, but use
> function/intrinsics fully that supported every architecture every machine
> related thing... what do you think?"

The user noticed the asm-safety work had ballooned in scope and was
asking whether the right move is to:
- Make raw asm "unsafe" (no validation of contents)
- Push everything users need through intrinsics

**Initial response:** Argued the original 2026-04-01 design (in
`ASM_ZER-LANG.md`) said exactly this: ~80% of kernel asm replaced by
intrinsics; remaining 20% is raw asm in naked functions, contents not
validated. We drifted between 2026-04-23 and 2026-05-02 by adding
per-instruction validation. Recommended: stop extending, ship what's done.

### 5.3 Second framing question (user)

> "is just zerdata and stuff... should it do intrinsic way but intrinsic
> means we need to SUPPORT every arch with no exception, and even sub arch
> like extension AVX and special machine precondition also need to be
> supported"

The user pushed back: "intrinsic way" requires supporting every architecture
forever, including sub-extensions. Is that feasible?

**Response:** Pure-intrinsic approach is infeasible. ~4,400 instructions
across 3 archs today, growing with every ISA extension. Even Intel's own
intrinsic guide is incomplete. The current hybrid (intrinsics + raw asm +
partial data-table validation) is actually the correct middle ground —
but we should STOP EXTENDING the data-table side.

### 5.4 The "ASM is just abstraction" framing (user)

> "ASM is just abstraction to machine language, and even asm that
> currently people learning on are just mostly emulation unless they run
> through C binary or any compiler... using intrinsic, like cannot fully
> but just common, I want full 100% usability, I don't have time
> constraint, what about ASM IR, ASM to machine, technically the language
> A,B,C, A is from ASM, B is from A, C is from B, bootstrapping, just
> higher representation, but we can make everything safe by AST/IR, what
> do you think"

The user proposed: treat asm as a first-class language with its own
AST/IR/checker pipeline, like Vale or LLVM MC. Make everything safe via
the same kind of structured representation ZER uses for high-level code.

**Response:** This is the architecturally correct answer (matches Vale,
JasMin, CompCert, LLVM MC, Sail). Full effort: 5-7 years. Phases ASM-IR-1
through ASM-IR-3 (~3 years) for structured asm without formal proofs;
Phases 4-5 (~2-4 more years) for Vale-tier formal semantics.

### 5.5 The maintenance critique (user)

> "But the problem is our code shouldn't like keep being maintain like
> today new chip come out and stuff need to update spec and stuff we
> shouldn't do that, but it should be safe cause it is IR based, how can
> we achieve that even with subarch like AVX, or other similar things"

The user identified the actual constraint: solution must be
maintenance-free as silicon evolves. New chips shouldn't force compiler
updates.

**Response (key breakthrough):** The trick is **operand-kind validation,
not instruction validation**. Frozen 10-category enum (GPR_READ,
GPR_WRITE, VEC_READ, VEC_WRITE, MEM_READ, MEM_WRITE, IMM,
BRANCH_TARGET, FLAG_READ, FLAG_WRITE). AVX-1024 in 2035 still uses these
categories. Compiler validates operand structure, not instruction
semantics. ~100 well-known UB classics in stable implicit-precondition
table. Zero per-ISA-extension maintenance.

### 5.6 The scope clarification (user)

> "our safety is not algos or logic error, just safe ASM fully safe one,
> Vale is technically need to proof the logic, like Coq, I mean
> operand-kind is fully safe? like exception of course runtime stuff, and
> do asm is already safe let say someone doesn't know specs or mmio, they
> wrote bad range, breaking boundary or illogical, we can't prevent that
> right, what are preventing in asm?"

The user wanted precise scope: operand-kind IR is "fully safe" within
ZER's normal safety scope (memory/type/concurrency/MMIO/provenance/
qualifier), NOT algorithm correctness. What CAN'T be caught: user-declared
specs being wrong, algorithm/logic errors, microarchitectural effects,
runtime CPU state.

**Response:** Explicit list of what IS prevented (~13 safety properties,
all covered) and what IS NOT prevented (5 trust-boundary categories,
same as ZER's overall scope). Operand-kind IR is "fully safe" in the
same sense ZER overall is "fully safe": within the documented scope,
with the same compile-time/runtime split and the same trust boundary at
user declarations.

### 5.7 The "Manual is fine" insight (user)

> "these are fine, we eliminated so much things but this operand kind IR
> vs our current asm-plan if fully done which are more effective, of
> course I know caveat of what we currently doing is way too much of
> maintenance probing and ZERdata"

User asked: assuming both approaches were fully completed, which catches
more bugs?

**Response:** asm_plan fully-done would catch maybe 1-2% more obscure
instruction UB (per-instruction lookup). But this is closable with the
tiny implicit-precondition table. Effective coverage: identical. Maintenance:
order of magnitude difference. Operand-kind IR wins.

### 5.8 The "Manual annotations are enforced" clarification (user)

> "these manual we can introduce language complexity like annotations or
> ownership or something else to solve this or you already assume
> annotation to solve this thats why you put manual, meaning it would
> throw error which means its safe"

User noticed "Manual" in the comparison table was misleading — it
suggested less safe. They asked: when user declares `requires: nonzero`,
does compiler ENFORCE it? Is it a compile error if precondition can't
be proven?

**Response:** YES. Annotations are compile-time CONTRACTS. Compiler
enforces via existing safety infrastructure (VRP, alignment, state
machines, context flags). Compile errors are identical to "auto" mode.
"Manual" = same safety, different ergonomics.

### 5.9 The smart-language insight (user)

> "yeah as long everything is just one time and no maintenance it means
> it is fully good, our asm plan one did this by making compiler smart
> not by adding complexity correct which is not good which became
> maintenance hell"

User articulated the architectural principle: smart compiler = brittle.
Smart language + generic compiler = robust. asm_plan made the compiler
smart; should have made the language expressive instead.

**Response:** Confirmed. This is exactly the principle ZER already follows
for the other 28 safety systems. asm_plan was the 29th system trying to
violate it.

### 5.10 The "do we need to rewrite?" pivot (user)

> "I mean that means rewriting whole stuff right if we do that, what if
> like we use existing Z-classes since we already did it, I mean just add
> a complexity for these manual stuff or no need, just redo and make the
> IR way, instead of using high level bolt on safety class which is same
> thing, but it applies to higher level substrate which convert to ASM
> rather than ASM IR with the safety directly in the same substrate"

User identified the pragmatic question: do we need to rewrite (option 1,
operand-kind IR) or can we extend existing Z-rules (option 2)? They
distinguished bolt-on-safety-at-higher-level from integrated-safety-at-
same-substrate.

**Response:** Option 2 is viable. Z-rules are already a coarse-grained
operand-kind IR (Session B typed bindings carry register names + ZER
expressions). Need to ADD: more operand metadata, language-level
annotations, tiny implicit table. KEEP: Z1-Z12, register tables, F7-light,
F7-full Step 2. STOP: extending per-instruction database, Session G Phase
5, Z9/Z10/Z13. Net effort: ~3-4 months vs option 1's 8-10 months. Same
safety. Identical user experience.

### 5.11 The "are they the same?" question (user)

> "so both are actually the same solution right one is just implemented
> in IR level a rewrite where as one that currently is just the whole
> language overall enforces to ASM, but is there any usability risk like
> in IR it supported more case or like that or it just the same"

User asked: are options 1 and 2 functionally equivalent in user-facing
behavior? Or does option 1 (IR-level) support cases that option 2
(Z-rules + annotations) can't?

**Response:** Functionally equivalent for ZER's actual use cases. Real
differences: mental model (option 1 cleaner), future safety-system
additions (option 1 auto), composability with comptime/generics (option 1
better, mostly unrealized), CFG-aware analysis (option 1 ready). Practical
user-visible impact: zero. Decision: option 2.

### 5.12 The final confirmation (user)

> "yes main thing is just of course optimization but ASM too low already
> barely optimization are there available and of course current asm plan
> Z safety classes is easier to develop compare to IR in asm level what
> do you think?"

User identified two practical clinchers:
1. Optimization at asm level is essentially zero (GCC does all of it)
2. Z-classes are easier to develop than full IR rewrite

**Response:** Both confirmed. Option 2 wins decisively. Captured here.

---

## 6. Three Options Considered

### 6.1 Option 0: Pure intrinsic (no raw asm)

**Description:** Make every asm operation a compiler-provided intrinsic.
No raw `asm("...")` allowed. User chooses from a catalog.

**Effort:** ~years. ~4,400 intrinsics across 3 archs today, growing with
every ISA extension. Each requires implementation, testing, per-arch emit
code, documentation.

**Maintenance:** Permanent multi-engineer commitment. Even Intel's own
intrinsic guide is incomplete; ARM's ACLE has gaps; compilers (GCC/Clang)
lag silicon by 1-3 years.

**Safety:** Theoretically 100% (compiler controls all asm). In practice,
broken because users always need escape hatches for vendor-specific or
brand-new ISA features the intrinsic catalog doesn't cover.

**Why rejected:** Not feasible at ZER's scale (solo development, GCC
backend). Even Rust (with 100+ contributors) didn't take this path.
Pure-intrinsic doesn't match how kernel/firmware devs actually work.

### 6.2 Option 1: Full operand-kind IR rewrite

**Description:** Rewrite asm to first-class structured form in ZER's AST/IR:

```zer
asm_inst {
    op: "movaps",
    reads_mem: { addr: buf, size: 16, align: 16 },
    writes_reg: { name: "xmm0", class: vec, bits: 128 },
    cpu_feature: sse,
}
```

Operands ARE regular ZER expressions. Compiler's main safety pipeline
(VRP, alignment, escape, provenance, MMIO, etc.) runs directly on operands.
No Z-rules needed — safety falls out of normal type checking.

**Effort:** ~8-12 months. Phases:
1. Design structured asm AST (~1 month)
2. Extend IR with asm-specific nodes (~1-2 months)
3. Operand-kind validation framework (~1-2 months)
4. Reconnect with existing safety systems (~1 month)
5. Migrate F4-F7 data into new shape (~1 month)
6. Reimplement F7-light state machine in new framework (~1 month)
7. Migration path for existing asm tests (~1 month)
8. Tests + validation + regression suite (~2-3 months)

**Maintenance after done:** Near-zero. Frozen 10-category operand-kind enum
covers everything. Generic safety pipeline auto-applies through asm.

**Safety:** ~99% (same as option 2 with implicit table).

**Why rejected:** 3x more work than option 2. Throws away ~60% of existing
Session A/B/F4-F7 code. Higher regression risk during migration. Marginal
architectural cleanness improvement doesn't justify the cost. ZER's
optimization story at asm level is essentially zero (GCC handles it), so
option 1's structural advantages don't translate to user-visible wins.

### 6.3 Option 2: Extend current design + annotations + tiny implicit table (CHOSEN)

**Description:** Build on existing Session A/B asm syntax. Add operand
metadata fields (size, align, role). Add language-level precondition
annotations (`requires:`, `align:`, `opens_state:`, `closes_state:`). Add
tiny implicit-precondition table for ~100 well-known UB classics. Keep
existing Z1-Z12 wiring. Stop extending per-instruction database.

**Effort:** ~3-4 months. Phases:
1. Operand metadata extensions to Session B syntax (~2-3 weeks)
2. Annotation processing in NODE_ASM checker (~3-4 weeks)
3. Tiny implicit-precondition table (~2 weeks)
4. State-machine annotation generalization (~2-3 weeks)
5. Tests + validation (~3-4 weeks)
6. Documentation (~1-2 weeks)

**Maintenance after done:** Near-zero. Per-instruction DB frozen at ~100
implicit-precondition entries. Z-rules stable (Z1-Z12 don't change
frequently). Operand-kind framework frozen.

**Safety:** ~99% (identical to option 1).

**Why chosen:**
- Same safety as option 1
- 3x less work
- Reuses ~95% of existing code
- Lower regression risk (incremental vs rewrite)
- Optimization advantage of option 1 is moot at asm level (GCC handles)
- Z-classes are easier to develop than IR rewrite (proven pattern, 10/13
  already shipped)
- Builds on solid foundation (Session A/B, F4-F7) instead of replacing

### 6.4 Comparison table (all three options)

| Property | Option 0 (Pure intrinsic) | Option 1 (Full IR rewrite) | Option 2 (Extend current) |
|---|---|---|---|
| Effort | Years | 8-12 months | 3-4 months |
| Existing code reused | ~10% | ~40% | ~95% |
| Safety coverage | ~99% (in theory) | ~99% | ~99% |
| User experience | Restrictive (no raw asm) | Flexible (structured asm) | Flexible (structured asm) |
| Maintenance per ISA extension | Massive (hundreds intrinsics) | Zero | Zero |
| Optimization advantage | N/A | Theoretical, mostly unrealized | N/A |
| Risk profile | Implementation backlog | Big rewrite regression risk | Incremental, low risk |
| Architectural cleanness | High but inflexible | Highest | Slightly layered |
| Backward compat | None | Migration needed | Backward compatible |
| Matches Rust | More restrictive | Stricter | Slightly stricter (annotations + implicit table) |

Option 2 dominates on every PRACTICAL axis.

---

## 7. What We're Keeping from asm_plan.md

The asm_plan.md work is **not wasted**. Significant infrastructure stays:

### 7.1 The 130 intrinsics (D-Alpha-1 through D-Alpha-14)

All 130 intrinsics shipped between 2026-04-23 and 2026-04-24 stay
exactly as they are:

- D-Alpha-1: 7 atomic intrinsics (xchg, nand, *_fetch × 5)
- D-Alpha-2: 10 barrier/bit-query/hint intrinsics
- D-Alpha-3: 5 interrupt control intrinsics (privileged)
- D-Alpha-4: 4 context switch intrinsics
- D-Alpha-9: 10 MSR/CR/XCR0 intrinsics
- D-Alpha-10: 10 inspection intrinsics (non-privileged)
- D-Alpha-11: 5 power management intrinsics
- D-Alpha-12: 6 privileged mode transition intrinsics
- D-Alpha-13: 20 Linux-scale x86 essentials (FS/GS, port I/O, XSAVE, etc.)
- D-Alpha-14: 12 final misc intrinsics (CPUID, EOI, cache control, etc.)

These cover ~80% of common kernel/firmware asm patterns. They're the
"primary safe path" for users — write `@atomic_add(&counter, 1)` instead
of raw `lock xadd` asm.

### 7.2 Z-rules Z1 through Z8, Z11, Z12 (10 of 13 wired)

Already-shipped Z-rules stay:

| Z-rule | What it does | Location |
|---|---|---|
| Z1 | UAF check at asm operand boundary | `zercheck_ir.c` IR_NOP (NODE_ASM) |
| Z2 | Move struct transfer at asm operand | `zercheck_ir.c` IR_NOP |
| Z3 | VRP range invalidation on asm output | `checker.c` NODE_ASM |
| Z4 | Provenance type clear on asm output | `checker.c` NODE_ASM |
| Z5 | Local-derived pointer rejection with memory clobber | `checker.c` NODE_ASM |
| Z6 | Asm-in-defer/async ban (forward-compat) | `checker.c` NODE_ASM |
| Z7 | MMIO range check on asm memory operand | `checker.c` NODE_ASM (via existing @inttoptr) |
| Z8 | Qualifier preservation on asm output | `checker.c` NODE_ASM |
| Z11 | Non-keep pointer param + memory clobber rejected | `checker.c` NODE_ASM |
| Z12 | scan_frame walker recurses into structured asm operands | `checker.c` scan_frame |

These work. They've been tested. They don't need changes. They cover the
core safety dimensions (memory/type/concurrency/MMIO/provenance/qualifier)
through the asm boundary.

**Z9, Z10, Z13 are NOT shipped and won't be** (covered in section 8).

### 7.3 Register tables (F2/F5/F6)

Per-arch register validation tables stay:
- `src/safety/asm_register_tables_x86_64.c` — 105 entries
- `src/safety/asm_register_tables_x86_64_avx512f.c` — 56 entries (AVX-512)
- `src/safety/asm_register_tables_aarch64.c` — 58 entries
- `src/safety/asm_register_tables_riscv64.c` — 126 entries
- `src/safety/asm_register_lookup.c` — dispatch
- `src/safety/asm_register_tables.h` — declarations

These are **auto-probed from GCC** via `scripts/gen_register_tables.sh`.
Updates are mechanical: rerun script, review diff, commit. Cost: ~30
seconds per ISA-extension rollout. Total maintenance burden: minimal.

**These provide real value:** reject `rax` on ARM, gate `xmm16` on
`--target-features=avx512f`, catch obvious register typos. Worth keeping.

### 7.4 CPU feature gating (F4 dispatch)

The `--target-features=` CLI flag wires CPU features (avx512f, sse4_2,
aes_ni, etc.) into the checker for both register validation (e.g., zmm
registers require avx512f) and instruction availability.

This is **valuable build-config infrastructure** that operand-kind IR
needs anyway. Keep as-is.

Baseline x86_64 features: SSE | SSE2 (everything else opt-in via flag).
Extensible: adding a new feature is one enum value + one CLI flag entry.

### 7.5 F7-light state machine (LR/SC pairing)

The state-machine pattern for paired ops stays. Originally implemented for
RISC-V LR/SC, ARM LDXR/STXR, x86 MONITOR/MWAIT pairs across the same asm
block.

Pattern: per-block state-machine table that tracks which "states" are
open. Hardcoded mnemonic macros per arch detect opens (LR, LDXR) and
closes (SC, STXR). Mismatch → compile error.

In option 2's design, this becomes a **REUSABLE GENERIC INFRASTRUCTURE**
that user annotations (`opens_state:` / `closes_state:`) dispatch to. The
F7-light implementation is the proof-of-concept; we generalize the
dispatch to be annotation-driven instead of mnemonic-hardcoded.

### 7.6 F7-full Step 2 constraints (NONZERO/COMPOUND/ALIGNED/BOUNDED)

The per-operand constraint plumbing in `checker.c` NODE_ASM dispatch:

- **NONZERO** (Step 2a): operand must be provably nonzero via VRP. Used by
  BSR/IDIV. Wired to existing System #12 VRP.
- **COMPOUND** (Step 2b): same as NONZERO but with INT_MIN/-1 backstop.
  Used by signed IDIV.
- **ALIGNED** (Step 2c): operand address must be provably N-byte aligned.
  Used by MOVAPS. Wired to existing alignment infrastructure.
- **BOUNDED** (Step 2d): operand value must be in range. Wired and ready,
  no instructions use it yet.

The constraint kinds become the **vocabulary** that user annotations map
to. `requires: nonzero` dispatches to NONZERO. `align: 16` dispatches to
ALIGNED. The plumbing is already done; option 2 reuses it.

### 7.7 C8 instruction classification (vendored ordering metadata)

CLWB / CLFLUSHOPT / LDAR / STLR / SFENCE / etc. classified with ordering
metadata (PRODUCES Acquire, REQUIRES_AFTER StoreStore, etc.). Used by
Session G Phase 1+2 (data plumbing).

**This data stays** but its USE is limited to the implicit-precondition
table for paired operations (LDAR/STLR pairing, CLWB/SFENCE persistence
hints, MFENCE produces full barrier).

We're NOT building Session G Phase 5 (CFG-aware OrderingState). The data
informs the implicit-precondition table; that's it.

### 7.8 Structured asm syntax (Session A)

The `asm { instructions: "..." safety: "..." }` form from D-Alpha-7.5
Session A stays. The mandatory `safety:` documentation string (≥30 chars)
stays. This is the user-facing entry point.

### 7.9 Typed operand bindings (Session B)

The `inputs: { "reg" = expr }`, `outputs: { "reg" = lvalue }`, `clobbers:
[...]` form from Session B stays. We **extend** these with operand
metadata fields (size, align, role) and inline precondition annotations
— this is the core of option 2's work.

### 7.10 Sub-extension architecture validation

The 3-arch end-to-end empirical proof from 2026-04-29 stays. The
decisions locked in there:
- `.zerdata` is compiler-internal, vendored, NOT user-extensible
- Build-time-gen + vendored output pattern (matches LLVM TableGen, ICU, etc.)
- GCC probe scripts per arch
- Intrinsic-only register classes intentional (AMX, SVE, SME)

These architectural decisions are sound and don't change in option 2.

### 7.11 Naked-only restriction (S1)

Phase 1 verified rule: `zer_asm_allowed_in_context(in_naked)` enforces
asm only inside `naked` functions. This is the v1.0 interim guard and
the foundational structural rule.

Stays. Probably permanent (the "naked attribute migration" mentioned in
asm_plan was always speculative).

---

## 8. What We're Stopping or Deferring

### 8.1 Session G Phase 5 — IR-level CFG OrderingState (DEFERRED)

**What it was:** CFG-aware acquire/release pairing and persistence-barrier
tracking. ~30-40 hours of work. Would add OrderingState to `zercheck_ir.c`
that tracks barriers from BOTH asm blocks AND `@atomic_*` intrinsics, with
set-intersection joins.

**Why deferred:**
- 5 of the 10 acquire/release pairings can be expressed as user
  annotations (`opens_state: persistence_barrier`, `closes_state:
  persistence_barrier`) and validated via the existing state-machine
  pattern.
- The remaining 5 are niche (persistent-memory, atomic-ordering edge
  cases). Affect ~0.1% of users.
- Session G Phase 3 (the same logic at block-level) was abandoned because
  it false-positived on real code (libpmem CLWB+SFENCE idiom). CFG-aware
  versions likely have their own edge cases.
- 30-40 hours of permanent complexity in `zercheck_ir.c` (which we're
  trying to clean up via refactor_ir.md).
- User-annotation-driven approach handles the same cases users actually
  care about, at zero compiler complexity cost.

**If we ever need it:** User annotations + implicit-precondition table
will be working first. If real users hit ordering bugs that annotations
don't catch, revisit then. Don't pre-build infrastructure for hypothetical
needs.

### 8.2 Z9 / Z10 / Z13 forward-compat Z-rules (DEFERRED)

**What they were:** Three remaining Z-rules blocked on S1 (naked-only)
relaxation:
- Z9: privilege escalation through asm
- Z10: ISR-context bans through asm
- Z13: thread-local access through asm

**Why deferred:**
- All blocked on relaxing S1 (allowing asm in non-naked functions). We're
  NOT relaxing S1 — naked-only is the interim guard AND the permanent
  guard.
- Their value is gated on a context that doesn't exist (asm in regular
  functions).
- ~50 hours of work for forward-compatibility we don't need.

**If we ever relax S1:** revisit. But the asm_plan.md design itself notes
S1 is restrictive enough that the additional Z-rules aren't needed.
Probably permanently deferred.

### 8.3 Adding entries to F4/F5/F6 instruction tables

**What it was:** Continuing to vendor more per-instruction safety data
(constraint kinds, ordering metadata, privilege levels, etc.).

**Why stopped:**
- The per-instruction database approach is fundamentally maintenance-heavy.
- We have ~120 entries today; another 4,000+ exist that would need
  classification.
- New ISA extensions add 50-200 entries each.
- The pivot's principle: language carries information, compiler is generic.
- Implicit-precondition table (~100 well-known classics) captures most
  practical value at fixed maintenance cost.

**What stays:** Existing 120 entries (already vendored, work). The
~100-classics implicit table is conceptually different — it's frozen at
well-known UB-prone classics (BSR, IDIV, MOVAPS, etc.) that haven't
changed semantics since the 1980s.

### 8.4 C5 explicit kernel-context model (NOT NEEDED)

**What it was:** Explicit modeling of kernel/user/hypervisor context for
privileged-instruction checking.

**Status:** Already covered by S1 (naked-only restriction). The naked
constraint is interim AND permanent enough that explicit kernel-context
modeling is redundant.

If user wants finer-grained context tracking, they can use:
- `@critical` blocks (existing, banned for spawn/alloc)
- Function-level attributes (existing `interrupt` keyword)
- User-declared `requires: kernel_context` annotation (option 2 addition)

### 8.5 Naked-attribute migration (INDEFINITELY DEFERRED)

**What it was:** Migrate from current `naked` function attribute to
something cleaner.

**Why deferred:** Requires asm-test rewrite (~20 hours). No clear win.
`naked` is sound, MISRA-compliant, well-understood. Migration would be
churn for marginal cleanness. Defer indefinitely.

### 8.6 Probe-script extensions for new GCC features

**What it would be:** Extending `scripts/gen_register_tables.sh` to handle
new GCC clobber probe scenarios (AVX-1024 hypothetical, RISC-V V 2.0, etc.).

**Status:** Already handled by existing script architecture. Adding new
candidate registers to `scripts/candidates_*.txt` and rerunning is mechanical.
No structural changes needed. Stays as-is, runs when needed.

### 8.7 Multi-backend support (LLVM, etc.)

**Status:** GCC-only is the locked-in decision (asm_plan.md decision 2).
No plans to support other backends. If we ever did, option 2's annotation
infrastructure transfers cleanly (annotations are backend-agnostic).

---

## 9. The Concrete Design

This section specifies the additions option 2 makes to existing asm
infrastructure.

### 9.1 Operand metadata extensions

**Current Session B syntax:**

```zer
asm {
    instructions: "movaps %0, %1"
    outputs: { "xmm0" = result }
    inputs:  { "rax" = buf }
    safety: "Load aligned vector from buf into xmm0"
}
```

The operand bindings (`"xmm0" = result`, `"rax" = buf`) bind register
names to ZER expressions. Compiler validates: register name valid for
arch (via vendored table), expression type matches register width.

**Option 2 extension:** Add metadata fields to operand bindings:

```zer
asm {
    instructions: "movaps %0, %1"
    outputs: { 
        "xmm0" = result,
        class: vec,         // NEW: register class (gpr/vec/mask/fp)
        bits: 128,          // NEW: explicit width
        role: write,        // NEW: read/write/read_write
    }
    inputs: { 
        "rax" = buf,
        class: gpr,
        bits: 64,
        role: address,      // NEW: address operand for memory access
        mem_size: 16,       // NEW: memory access size
        mem_align: 16,      // NEW: required alignment
    }
    safety: "Load aligned vector from buf into xmm0"
}
```

The metadata is **optional**. If not provided, the compiler infers from
the register name (existing behavior). If provided, the compiler enforces
the additional constraints:
- `class: vec` + register name `"rax"` → error (class mismatch)
- `bits: 128` + register name `"al"` → error (width mismatch)
- `mem_align: 16` + expression `buf` not provably 16-byte aligned → error

This is **additive**: existing asm code without metadata keeps working;
new code can opt into stricter validation.

### 9.2 Operand-level precondition annotations

Add four new annotation fields to operand bindings:

```zer
asm {
    instructions: "bsr %0, %1"
    outputs: { "ecx" = result, class: gpr, bits: 32 }
    inputs:  { 
        "eax" = mask, 
        class: gpr, 
        bits: 32,
        requires: nonzero,        // NEW: precondition annotation
    }
    safety: "Find highest set bit in mask (must be nonzero)"
}
```

#### 9.2.1 `requires:` — value precondition

Validates a property about the operand's value. Dispatched to existing
safety system:

| Annotation | Validation | Existing system |
|---|---|---|
| `requires: nonzero` | VRP must prove operand expression > 0 (signed) or ≠ 0 (unsigned) | System #12 VRP |
| `requires: positive` | VRP must prove operand expression > 0 | System #12 VRP |
| `requires: in_range(min, max)` | VRP must prove operand within range | System #12 VRP |
| `requires: kernel_context` | Function must be in kernel-context scope | Context flags |
| `requires: aligned(N)` | Operand address provably N-byte aligned | Alignment infrastructure |
| `requires: mmio_range(name)` | Operand address within declared `mmio` range | System #19 MMIO |
| `requires: nonnull` | Pointer operand provably non-NULL | Existing optional-unwrap |

#### 9.2.2 `align:` — alignment precondition

Validates memory operand alignment:

```zer
inputs: { 
    "rax" = buf, 
    align: 16,    // shorthand for requires: aligned(16)
}
```

Dispatched to alignment infrastructure (same one that handles `_Alignas`,
`@aligned_alloc`, MMIO alignment checks).

#### 9.2.3 `opens_state:` / `closes_state:` — paired operation state

Validates state-machine pairings across asm blocks (same scope as
F7-light LR/SC, generalized):

```zer
naked void atomic_inc(*u32 p) {
    asm {
        instructions: "lr.w %0, (%1)"
        outputs: { "t0" = old_val }
        inputs:  { "t1" = p, role: address, mem_size: 4 }
        opens_state: rv_lrsc       // NEW: opens state machine
        safety: "..."
    }
    asm { instructions: "addi %0, %0, 1" outputs: ... inputs: ... safety: "..." }
    asm {
        instructions: "sc.w %0, %1, (%2)"
        outputs: { "t2" = sc_result }
        inputs:  { "t0" = new_val, "t1" = p }
        closes_state: rv_lrsc      // NEW: must match opens_state
        safety: "..."
    }
}
```

If `opens_state` declared but no matching `closes_state` in same block:
compile error "LR without matching SC".

If `closes_state` declared but no preceding `opens_state` in same block:
compile error "SC without preceding LR".

Generic mechanism. Works for:
- RISC-V LR/SC pairs (`rv_lrsc`)
- ARM LDXR/STXR pairs (`arm_ldxr`)
- x86 MONITOR/MWAIT pairs (`x86_monitor`)
- AMX TILECONFIG/TILELOAD ordering (`amx_config`)
- Persistent-memory CLWB+SFENCE pairs (`pmem_persist`)
- Future paired operations (just add a new state name)

#### 9.2.4 `requires_after:` — ordering precondition

For ordering-dependent operations where the prior state isn't strictly
paired but must have occurred:

```zer
asm {
    instructions: "clwb (%0)"
    inputs: { "rax" = buf, role: address }
    safety: "..."
}
asm {
    instructions: "sfence"
    requires_after: any_clwb     // NEW: SFENCE must follow at least one CLWB
    safety: "..."
}
```

Generic state-tracking infrastructure.

### 9.3 Memory operand role declarations

For memory operands (input expressions used as addresses), the role
field declares what kind of memory access happens:

| Role | Meaning |
|---|---|
| `read` | Asm reads the register/operand value (no memory access) |
| `write` | Asm writes the register/operand value (no memory access) |
| `read_write` | Asm reads then writes (e.g., `inc` instruction) |
| `address` | Operand is an ADDRESS for a memory access; `mem_size`/`mem_align`/`mem_read`/`mem_write` mandatory |

For address operands, additional sub-fields:

```zer
inputs: {
    "rax" = buf,
    role: address,
    mem_size: 16,        // bytes accessed
    mem_align: 16,       // required alignment
    mem_read: true,      // asm reads from this address
    mem_write: false,    // asm doesn't write to this address
}
```

The compiler validates:
- `buf` is a valid pointer (existing escape/provenance/UAF checks via Z1, Z2, Z11)
- `buf` is provably aligned to `mem_align` (existing alignment infrastructure)
- Access size fits within `buf`'s allocated extent (existing bounds checking)
- If address is in declared `mmio` range: existing MMIO validation (System #19)
- If `mem_write: true`: existing volatile/const check (System #20)

All via existing infrastructure. Zero new logic.

### 9.4 Wire-up to existing safety systems

Each annotation dispatches to exactly one existing safety system:

| Annotation | Dispatches to |
|---|---|
| `requires: nonzero` | `find_var_range(c, expr)`, check min > 0 or known_nonzero |
| `requires: in_range(a, b)` | Same, check range inclusion |
| `requires: aligned(N)` | `expr_alignment(c, expr)`, check >= N |
| `requires: kernel_context` | `c->in_kernel_context` or function attribute |
| `requires: nonnull` | `type_is_optional(t)` check |
| `align: N` (on address) | `expr_alignment` |
| `opens_state: X` / `closes_state: X` | Per-block state-machine table (extends F7-light) |
| `requires_after: X` | Same state-machine table with "occurred" flag |
| `mem_size: N` | Bounds checking infrastructure |
| `mem_read`/`mem_write: bool` | Volatile/const qualifier check |
| `class: gpr/vec/mask/fp` | Register class table lookup (vendored) |
| `bits: N` | Register width validation (vendored) |

**No new safety system needed.** Every annotation routes to a system that
already exists for regular ZER code.

### 9.5 Error message structure

Errors follow ZER's standard format:

```
file.zer:42: asm operand precondition not satisfied:
    operand '"eax" = mask' declared 'requires: nonzero'
    but VRP cannot prove 'mask' is provably nonzero at this point.
    
    Either:
    - Add an explicit check: 'if (mask == 0) { return; }' before the asm block
    - Use a literal nonzero value
    - Use @relax_check(NONZERO) if you have external proof
    
   42 |         "eax" = mask, requires: nonzero
      |                       ^^^^^^^^^^^^^^^^^
```

Pattern matches existing ZER error messages (caret underline, suggestions,
escape hatch reference).

### 9.6 Backward compatibility

Existing asm code without operand annotations continues to work
unchanged. The annotations are **additive** — opt-in for new code.

If user wants strict mode where ALL operands MUST declare metadata, they
can use a CLI flag like `--strict-asm-operands` (or per-block
`@strict_asm`). Defaults to permissive: annotations are recommended but
not required.

For implicit-precondition table entries, the auto-applied annotations
fire WITHOUT requiring users to write them. So BSR-on-zero is caught
automatically even in legacy-style asm code.

---

## 10. The Tiny Implicit-Precondition Table

The cornerstone of option 2: ergonomic auto-application of preconditions
for ~100 well-known UB-prone instructions.

### 10.1 Why ~100 entries and not more

The instructions covered are **textbook UB classics** that:
- Predate every modern ISA extension (most are 1980s-1990s instructions)
- Have stable, well-documented UB semantics
- Are commonly written by kernel/firmware developers
- Have well-known mitigation (annotation)

These don't grow with ISA evolution. AVX-10.2 in 2026 won't add a new
"BSR-like" instruction; it adds vector compute instructions whose UB is
either well-defined or out-of-scope.

The ~100 figure comes from cataloguing:
- Bit-search: BSR, BSF, LZCNT, TZCNT, POPCNT (5)
- Integer division: DIV, IDIV (signed/unsigned, 8/16/32/64-bit) (~10)
- Aligned memory: MOVAPS, MOVAPD, MOVDQA, vector loads requiring alignment (~15)
- Shift operations: SHL/SHR with potentially-out-of-range count (~10)
- Atomic primitives: LR/SC pairs across archs (~10)
- Monitor/mwait pairs (2 mnemonics × archs)
- AMX TILECONFIG/TILELOAD ordering (~5)
- Cache management: CLFLUSH, CLWB, CLFLUSHOPT, INVD, WBINVD (~10)
- Other classics: RDMSR/WRMSR privilege, IRET context, etc. (~20)

Total: ~85-100 entries. Frozen. Updates: maybe 5 entries per decade if a
genuinely new well-known UB class emerges (extremely rare).

### 10.2 Table schema

```c
typedef enum {
    IMPL_REQUIRES_NONZERO  = 1 << 0,
    IMPL_REQUIRES_ALIGNED  = 1 << 1,
    IMPL_OPENS_STATE       = 1 << 2,
    IMPL_CLOSES_STATE      = 1 << 3,
    IMPL_REQUIRES_KERNEL   = 1 << 4,
    IMPL_REQUIRES_AFTER    = 1 << 5,
} ImplicitFlag;

typedef struct {
    const char *mnemonic;       /* "bsr", "movaps", "lr.w", etc. */
    uint32_t flags;             /* bitmask of ImplicitFlag */
    int op_index;               /* which operand the constraint applies to (0-based) */
    int constraint_value;       /* e.g., alignment value, state ID */
    const char *citation;       /* Intel SDM, ARM ARM, RISC-V spec reference */
} ImplicitPrecond;

static const ImplicitPrecond implicit_table[] = {
    /* Bit-search instructions — UB on zero input */
    { "bsr",     IMPL_REQUIRES_NONZERO,  0, 0, "Intel SDM Vol 2 BSR" },
    { "bsf",     IMPL_REQUIRES_NONZERO,  0, 0, "Intel SDM Vol 2 BSF" },
    
    /* Integer division — UB on zero divisor */
    { "div",     IMPL_REQUIRES_NONZERO,  0, 0, "Intel SDM Vol 2 DIV" },
    { "idiv",    IMPL_REQUIRES_NONZERO,  0, 0, "Intel SDM Vol 2 IDIV" },
    
    /* Aligned memory access — UB on unaligned address */
    { "movaps",  IMPL_REQUIRES_ALIGNED,  0, 16, "Intel SDM Vol 2 MOVAPS" },
    { "movapd",  IMPL_REQUIRES_ALIGNED,  0, 16, "Intel SDM Vol 2 MOVAPD" },
    { "movdqa",  IMPL_REQUIRES_ALIGNED,  0, 16, "Intel SDM Vol 2 MOVDQA" },
    
    /* RISC-V LR/SC pairing */
    { "lr.w",    IMPL_OPENS_STATE,       0, RV_LRSC_STATE, "RISC-V Atomic Ext A" },
    { "sc.w",    IMPL_CLOSES_STATE,      0, RV_LRSC_STATE, "RISC-V Atomic Ext A" },
    { "lr.d",    IMPL_OPENS_STATE,       0, RV_LRSC_STATE, "RISC-V Atomic Ext A" },
    { "sc.d",    IMPL_CLOSES_STATE,      0, RV_LRSC_STATE, "RISC-V Atomic Ext A" },
    
    /* ARM LDXR/STXR pairing */
    { "ldxr",    IMPL_OPENS_STATE,       0, ARM_LDXR_STATE, "ARM ARM A2.7" },
    { "stxr",    IMPL_CLOSES_STATE,      0, ARM_LDXR_STATE, "ARM ARM A2.7" },
    
    /* x86 MONITOR/MWAIT pairing */
    { "monitor", IMPL_OPENS_STATE,       0, X86_MONITOR_STATE, "Intel SDM Vol 2 MONITOR" },
    { "mwait",   IMPL_CLOSES_STATE,      0, X86_MONITOR_STATE, "Intel SDM Vol 2 MWAIT" },
    
    /* x86 AMX TILECONFIG/TILELOAD ordering */
    { "ldtilecfg", IMPL_OPENS_STATE,     0, X86_AMX_STATE, "Intel SDM Vol 2 LDTILECFG" },
    { "tileload",  IMPL_REQUIRES_AFTER,  0, X86_AMX_STATE, "Intel SDM Vol 2 TILELOAD" },
    { "tilestore", IMPL_REQUIRES_AFTER,  0, X86_AMX_STATE, "Intel SDM Vol 2 TILESTORE" },
    
    /* Privileged instructions — kernel-context required */
    { "rdmsr",   IMPL_REQUIRES_KERNEL,   -1, 0, "Intel SDM Vol 2 RDMSR" },
    { "wrmsr",   IMPL_REQUIRES_KERNEL,   -1, 0, "Intel SDM Vol 2 WRMSR" },
    { "lgdt",    IMPL_REQUIRES_KERNEL,   -1, 0, "Intel SDM Vol 2 LGDT" },
    { "lidt",    IMPL_REQUIRES_KERNEL,   -1, 0, "Intel SDM Vol 2 LIDT" },
    
    /* Cache management — kernel-context required for most */
    { "invd",    IMPL_REQUIRES_KERNEL,   -1, 0, "Intel SDM Vol 2 INVD" },
    { "wbinvd",  IMPL_REQUIRES_KERNEL,   -1, 0, "Intel SDM Vol 2 WBINVD" },
    /* CLFLUSH/CLWB are user-mode, no implicit constraint */
    
    /* ... continued — total ~85-100 entries ... */
};
```

See Appendix C for the full enumeration.

### 10.3 How auto-application works

In `checker.c` NODE_ASM handler, after parsing operand metadata:

```c
/* Look up mnemonic in implicit table */
const char *mnemonic = parse_first_word(asm_node->instructions);
const ImplicitPrecond *implicit = implicit_table_lookup(mnemonic);

if (implicit && !user_overrode_precondition(asm_node, implicit)) {
    /* Apply implicit precondition as if user wrote it */
    apply_implicit_precondition(c, asm_node, implicit);
}
```

`apply_implicit_precondition` does the same thing as user-written
annotations — dispatches to VRP / alignment / state-machine / etc.

### 10.4 User override

If user has reason to bypass an implicit precondition (external proof,
hand-verified, edge case), they can:

```zer
asm {
    instructions: "bsr %0, %1"
    outputs: { "ecx" = result }
    inputs:  { 
        "eax" = mask, 
        @relax_check(NONZERO),    // skip implicit precondition
    }
    safety: "Caller guarantees mask is nonzero (asserted upstream)"
}
```

Or with explicit alternative:

```zer
asm {
    instructions: "bsr %0, %1"
    outputs: { "ecx" = result }
    inputs:  { 
        "eax" = mask, 
        requires: in_range(1, 0xFFFFFFFF),    // explicit alternative
    }
    safety: "..."
}
```

This is the **escape hatch** for legitimate edge cases. Audit-grep-able
(`@relax_check` is a marker).

### 10.5 Why this is genuinely maintenance-free

The implicit table covers **frozen well-known classics**. Adding a new
entry happens when:
- A new genuinely well-known UB class emerges (rare, maybe once per
  decade)
- User community surfaces a recurring confusion that an implicit
  precondition would fix

Future ISA extensions (AVX-10, SVE 3.0, etc.) DON'T add new UB classics
in the same sense. They add new vector/matrix compute instructions whose
UB is either well-defined or out-of-scope for "language safety."

**Maintenance projection:**
- 2026-2030: 0-5 new entries (if any)
- 2030-2035: 0-5 new entries
- 2035-2040: 0-5 new entries

The table stays at ~100 entries indefinitely. **Truly frozen.**

---

## 11. State Machine Annotation Generalization

The F7-light implementation (RISC-V LR/SC, ARM LDXR/STXR) shipped with
mnemonic-hardcoded state tracking. Option 2 generalizes this to be
annotation-driven.

### 11.1 Current F7-light architecture

In `checker.c` NODE_ASM:

```c
/* F7-light: detect LR/SC pairing for RISC-V */
if (target_arch == TARGET_RISCV64) {
    if (mnemonic_is_lrsc_open(mnemonic)) {
        block_state.lrsc_open = true;
        block_state.lr_line = line;
    } else if (mnemonic_is_lrsc_close(mnemonic)) {
        if (!block_state.lrsc_open) {
            checker_error(c, line, "sc.w without preceding lr.w");
        }
        block_state.lrsc_open = false;
    }
}

/* Similar hardcoded blocks for ARM LDXR/STXR and x86 MONITOR/MWAIT */
```

This works but is hardcoded per pair. Adding a new pair (AMX config/load)
requires a new code block.

### 11.2 Generalized state-machine infrastructure

Replace with a generic state-machine table:

```c
typedef struct {
    const char *state_name;     /* "rv_lrsc", "arm_ldxr", "amx_config" */
    int state_id;               /* unique per state */
    bool is_open;               /* current state in this block */
    int open_line;              /* line where state was opened */
} StateMachineEntry;

typedef struct {
    StateMachineEntry *states;
    int count;
    int capacity;
} BlockStateMachine;

/* Per-asm-block state tracking */
static BlockStateMachine *current_block_state;
```

Annotations dispatch to this:

```c
/* When parsing operand annotation: */
if (annotation_is_opens_state(annot)) {
    int state_id = lookup_state_id(annot.state_name);
    state_machine_open(current_block_state, state_id, asm_node->line);
}

if (annotation_is_closes_state(annot)) {
    int state_id = lookup_state_id(annot.state_name);
    if (!state_machine_is_open(current_block_state, state_id)) {
        checker_error(c, asm_node->line, 
            "operation with closes_state: %s but no preceding opens_state: %s",
            annot.state_name, annot.state_name);
    }
    state_machine_close(current_block_state, state_id);
}
```

Same dispatch works for ALL pairs. F7-light's hardcoded logic becomes
implicit-table entries that dispatch to this generic infrastructure.

### 11.3 At-end-of-block balance check

When asm block scope ends, check that all opened states are closed:

```c
for (int i = 0; i < current_block_state->count; i++) {
    if (current_block_state->states[i].is_open) {
        checker_error(c, line,
            "opens_state: %s at line %d not followed by closes_state in same block",
            current_block_state->states[i].state_name,
            current_block_state->states[i].open_line);
    }
}
```

Catches LR-without-SC, LDXR-without-STXR, MONITOR-without-MWAIT,
TILECONFIG-without-TILELOAD, etc. — all with the same code.

### 11.4 Cross-block state machines (DEFERRED, not in option 2)

Some state machines genuinely span basic blocks (e.g., persistence
fences). These would need CFG-aware tracking (what Session G Phase 5 was
attempting).

**Option 2 defers this.** Cross-block state machines are uncommon and
have edge cases (the libpmem CLWB+SFENCE issue). User annotations work
within-block; cross-block is handled by:
- User asserting the contract (existing `@relax_check`)
- User restructuring code to keep paired ops in same block
- Possibly adding a future v2.x cross-block mode if real users hit this

Not in scope for option 2.

### 11.5 State name namespace

States are addressed by string name (`"rv_lrsc"`, `"arm_ldxr"`, etc.).
Reserved state names live in the implicit table:

| State name | Meaning | Used by |
|---|---|---|
| `rv_lrsc` | RISC-V LR/SC paired atomic | `lr.w`, `lr.d`, `sc.w`, `sc.d` |
| `arm_ldxr` | ARM exclusive load/store | `ldxr`, `stxr`, `ldaxr`, `stlxr` |
| `x86_monitor` | x86 MONITOR/MWAIT | `monitor`, `mwait`, `umonitor`, `umwait` |
| `amx_config` | AMX tile config + load | `ldtilecfg`, `tileload`, `tilestore` |

Users can also define custom state names for their own pairings (e.g.,
spinlock-held). The state machine is generic; the names are just
identifiers.

---

## 12. Integration with Existing Safety Systems

This section maps each annotation/operand metadata to the existing ZER
safety system that enforces it. The pattern: option 2 doesn't introduce
new safety logic, just exposes existing logic via the asm operand interface.

### 12.1 VRP integration (System #12)

Annotations using VRP:
- `requires: nonzero` — calls `find_var_range(c, expr)`, checks `known_nonzero` or `range.min > 0` (signed) / `range.min != 0` (unsigned)
- `requires: in_range(a, b)` — same, checks `range.min >= a && range.max <= b`
- `requires: positive` — checks `range.min > 0`

VRP already runs on regular ZER code. The asm annotation just runs it on
the operand expression at NODE_ASM check time.

### 12.2 Alignment integration

Annotations using alignment:
- `align: N` (on memory operand)
- `requires: aligned(N)`
- `mem_align: N`

All dispatch to existing alignment infrastructure (`expr_alignment(c, expr)`
or equivalent). Same logic that handles `_Alignas`, `@aligned_alloc`,
`@inttoptr` alignment validation.

### 12.3 Escape analysis integration (System #11)

For pointer operands:
- Local-derived pointer passed to asm with memory clobber → Z5 (existing)
- Non-keep pointer param passed to asm with memory clobber → Z11 (existing)
- Pointer flows through asm output → escape flag propagation (existing)

No new logic. Asm operand interface uses the same checks.

### 12.4 Provenance integration (Systems #3-#5)

For pointer operands:
- Provenance type cleared on asm output (Z4, existing)
- Provenance preserved through `*opaque` round-trips via @ptrcast (existing)

No new logic.

### 12.5 Handle/move tracking integration (Systems #7, #10)

For handle/move operands:
- Asm input is invalid handle → Z1 (existing)
- Asm transfers ownership of move struct → Z2 (existing, HS_TRANSFERRED state)

No new logic.

### 12.6 MMIO integration (System #19)

For memory operands with addresses:
- Address in declared `mmio` range → existing MMIO validation
- Address outside any declared range + `--no-strict-mmio` not set → error

Existing logic. Asm operand interface defers to it.

### 12.7 Qualifier preservation integration (System #20)

For pointer operands:
- Volatile-pointer input + asm writes through it → no strip (Z8, existing)
- Const-pointer input + asm writes through it → error

No new logic.

### 12.8 Context flags integration (System #24)

For state-dependent operations:
- `requires: kernel_context` → check function-level kernel attribute
- `@critical` block + asm transition → existing critical context checks

No new logic.

### 12.9 State machine integration (extends F7-light)

For paired operations:
- `opens_state:` / `closes_state:` → per-block state machine (extends F7-light pattern)
- `requires_after:` → state machine "occurred" flag

F7-light pattern generalized; not new logic, just generic dispatch.

### 12.10 Register class integration (existing vendored tables)

For all operand bindings:
- Register name valid for current arch → existing register table lookup
- Register class matches declared `class:` field → table lookup + match
- Register width matches declared `bits:` → table lookup + match
- CPU feature gating for sub-extension registers → existing F4 dispatch

No new logic.

### 12.11 Summary: zero new safety logic

Every annotation routes to an existing safety system. Option 2's work is:
- Parser changes (accept new annotation syntax)
- AST extensions (carry annotation data)
- Dispatch code at NODE_ASM (look up annotation, call existing system)
- Implicit-precondition table (data, not logic)

**No new safety algorithms.** This is what makes option 2 cheap and
low-risk.

---

## 13. Safety Coverage Matrix — Proven Equivalence

Side-by-side comparison of option 2 (this approach) vs asm_plan
fully-done vs operand-kind IR (option 1). Same input, same compile
outcomes.

### 13.1 Memory safety properties

| Property | asm_plan fully done | Option 1 (operand-kind IR) | Option 2 (this approach) |
|---|---|---|---|
| OOB through asm operand | ✓ (bounds via VRP) | ✓ (bounds via VRP) | ✓ (bounds via VRP, same code) |
| UAF through asm operand | ✓ (Z1) | ✓ (UAF check on operand) | ✓ (Z1 stays) |
| Use-after-move through asm | ✓ (Z2) | ✓ (HS_TRANSFERRED check) | ✓ (Z2 stays) |
| Escape via asm output | ✓ (Z3-Z5) | ✓ (escape analysis) | ✓ (Z3-Z5 stays) |
| Provenance through asm | ✓ (Z4-Z5) | ✓ (provenance tracking) | ✓ (Z4-Z5 stays) |
| Qualifier preservation | ✓ (Z8) | ✓ (qualifier check) | ✓ (Z8 stays) |
| Keep parameter respect | ✓ (Z11) | ✓ (keep check) | ✓ (Z11 stays) |
| MMIO range enforcement | ✓ (C6 via @inttoptr) | ✓ (MMIO check) | ✓ (existing infrastructure) |

**Result: identical coverage. All three approaches catch the same bugs.**

### 13.2 Concurrency / context properties

| Property | asm_plan fully done | Option 1 | Option 2 |
|---|---|---|---|
| Asm in defer/async ban | ✓ (Z6) | ✓ (context flag) | ✓ (Z6 stays) |
| Asm in @critical context | ✓ | ✓ | ✓ (existing) |
| Spawn-arg through asm | ✓ | ✓ | ✓ (existing) |
| ISR ban | Covered by naked | Covered by naked | Covered by naked |

**Result: identical coverage.**

### 13.3 Instruction-specific UB

| Property | asm_plan fully done | Option 1 | Option 2 |
|---|---|---|---|
| BSR/BSF on zero | Auto (per-instr table) | Auto (implicit table) OR manual | **Auto (implicit table)** OR manual |
| IDIV on zero | Auto | Auto OR manual | **Auto** OR manual |
| MOVAPS misaligned | Auto | Auto OR manual | **Auto** OR manual |
| LR/SC pairing | Auto (F7-light) | Auto (implicit table) OR manual | **Auto (implicit table)** OR manual |
| Acquire/release pairing | Auto (Session G P5) | Auto OR manual | Manual (Session G P5 deferred) |
| CLWB→SFENCE persistence | Auto (Session G P5) | Auto OR manual | Manual |
| MONITOR/MWAIT pairing | Auto (Session G P5) | Auto (implicit table) | **Auto (implicit table)** |
| AMX TILECONFIG ordering | Auto | Auto (implicit table) | **Auto (implicit table)** |
| Privilege context | Auto (C5) | Auto OR manual | **Auto (implicit table)** OR manual |

**Result: option 2 matches option 1 for items in implicit table. asm_plan
edges ahead ~1-2% for obscure ordering cases (acquire/release, CLWB persistence)
that we explicitly defer. Closable later with annotations if real users need.**

### 13.4 Register/encoding validation

| Property | asm_plan fully done | Option 1 | Option 2 |
|---|---|---|---|
| Register name valid for arch | ✓ (vendored tables) | ✓ (vendored tables) | ✓ (vendored tables, same) |
| CPU feature gating | ✓ (F4) | ✓ | ✓ (F4 stays) |
| Register class match | ✓ | ✓ | ✓ (existing, extended via class:) |
| Register width match | ✓ | ✓ | ✓ (existing, extended via bits:) |

**Result: identical coverage.**

### 13.5 Out-of-scope (all approaches)

| Property | asm_plan fully done | Option 1 | Option 2 |
|---|---|---|---|
| Wrong specification (user error) | ✗ | ✗ | ✗ |
| Algorithm correctness | ✗ | ✗ | ✗ |
| Microarchitectural (Spectre etc.) | ✗ | ✗ | ✗ |
| Runtime CPU state (CR0/CR4 bits) | ✗ | ✗ | ✗ |
| User specifies wrong asm semantics | ✗ | ✗ | ✗ |

**All three approaches have the same scope limits.** None claim algorithm
correctness; that's Vale-tier territory.

### 13.6 Effort comparison

| Property | asm_plan fully done | Option 1 | Option 2 |
|---|---|---|---|
| Effort to complete | ~80 hrs + perpetual maintenance | ~8-12 months | ~3-4 months |
| Existing code preserved | ~95% | ~40% | ~95% |
| Maintenance per ISA extension | Hundreds of entries | Zero | Zero |
| Risk profile | Maintenance treadmill | Big rewrite regression | Incremental, low risk |

**Result: option 2 dominates on effort + maintenance with no safety
loss.**

---

## 14. Production-Compiler Precedents

This section documents that option 2's design choices match established
production-compiler practice.

### 14.1 Rust's `asm!()` macro

**What Rust does:**
- Structured operand bindings: `asm!("mov {0}, {1}", out("rax") result, in("rbx") input)`
- Type checking on bound expressions
- Register name validation per target architecture
- CPU feature gating (`#[cfg(target_feature = "avx512f")]`)
- Clobber declarations
- No per-instruction UB validation
- No CFG-aware analysis of asm contents
- `// SAFETY:` comment convention (not enforced) for documentation
- `unsafe` block is the boundary

**Match with option 2:**
- Structured operand bindings ✓
- Type checking ✓
- Register validation ✓
- CPU feature gating ✓
- Clobber declarations ✓
- Option 2 adds: implicit-precondition table for ~100 classics, optional annotations
- Option 2 stricter than Rust by: ~100 well-known UB classes auto-protected

**Conclusion:** Option 2 matches Rust's pragmatic asm story with a small,
stable safety improvement (implicit precondition table). Maintenance burden
comparable.

### 14.2 Zig's `asm` keyword

**What Zig does:**
- `asm volatile (template : outputs : inputs : clobbers)` (similar to GCC)
- Type checking on expressions
- No per-instruction UB detection
- No CFG analysis
- Compile-time evaluation for known constants

**Match with option 2:** Similar shape, plus option 2's annotations. Same
overall philosophy.

### 14.3 seL4 (proven kernel)

**What seL4 does:**
- Hand-written asm with hand-proven invariants
- ISA semantics in Isabelle/HOL formal model
- No per-instruction UB detection in the compiler
- Verification happens at proof time, not compile time

**Match with option 2:** seL4's asm is small (~few hundred lines), each
hand-proven. ZER's asm safety is at compile-time level, complementary.
Different scope; neither tries to be the other.

### 14.4 GCC's inline asm

**What GCC does:**
- `__asm__ volatile (template : outputs : inputs : clobbers : labels)`
- Constraint letters (`r`, `m`, `=r`, etc.) tell GCC about operand kinds
- No per-instruction UB detection
- Operand types validated by C type system
- Backends generate correct asm from templates

**Match with option 2:** ZER's `asm { instructions: ... outputs: ... }`
form lowers directly to GCC inline asm. Option 2's metadata (`class:`,
`bits:`) maps to GCC constraint letters. Compatible by construction.

### 14.5 Vale (verified asm)

**What Vale does:**
- Structured asm with Coq semantics per instruction
- Formal proofs of algorithm correctness (AES, ChaCha20, Poly1305)
- Compiles to GCC/Clang asm
- Per-instruction semantic model

**Match with option 2:** Vale is the maximal-safety reference but with
massive effort (years of PhD-level work per primitive). Option 2 explicitly
out-of-scope for Vale-tier — algorithm correctness is opt-in via separate
`@verified_spec` mechanism (asm_plan deferred). Vale-tier is orthogonal
to operand-kind safety.

### 14.6 LLVM MC (Machine Code library)

**What LLVM MC does:**
- Structured representation of every instruction (MCInst)
- Per-target encoding tables
- Assembler/disassembler infrastructure
- No safety analysis (that's higher-level passes)

**Match with option 2:** LLVM MC is the "structured asm IR" foundation
that option 1 would build on. Option 2 doesn't need this — uses GCC's
inline-asm path. Same backend (GCC), different abstraction level.

### 14.7 Convergent design across projects

The pattern across ALL production compilers:
- **Validate the operand boundary** (types, constraints, clobbers)
- **Don't validate instruction semantics** (mostly)
- **Trust user declarations** (clobbers, safety comments)
- **Provide escape hatch** for raw asm
- **Optional formal proofs** for crypto/safety-critical

Option 2 matches this convergent pattern. The original asm_plan direction
(per-instruction semantic database) was an outlier even compared to
academic projects.

---

## 15. Effort Breakdown

Detailed effort estimate for option 2 implementation.

### 15.1 Phase 1: Operand metadata syntax extension (~2-3 weeks)

**Goal:** Parser accepts new metadata fields on operand bindings.

Work items:
- Lexer: recognize new keywords (`class`, `bits`, `role`, `mem_size`,
  `mem_align`, `mem_read`, `mem_write`)
- Parser: extend `parse_asm_operand_binding` to accept optional metadata
  fields (key-value pairs)
- AST: extend `AsmOperandBinding` struct with metadata fields
- Tests: 10-15 parser tests covering each metadata field, error cases

**Risk:** LOW. Pure parser/AST work. No safety logic changes.

### 15.2 Phase 2: Annotation processing in checker (~3-4 weeks)

**Goal:** Compiler dispatches annotations to existing safety systems.

Work items:
- Parser: recognize annotation syntax (`requires: nonzero`, `align: 16`,
  `opens_state: X`, `closes_state: X`, `requires_after: X`)
- AST: extend `AsmOperandBinding` with annotation list
- Checker: in NODE_ASM handler, iterate operand annotations and dispatch:
  - `requires: nonzero` → VRP check
  - `requires: in_range(a, b)` → VRP check
  - `requires: aligned(N)` / `align: N` → alignment check
  - `requires: kernel_context` → context flag check
  - `opens_state: X` / `closes_state: X` → per-block state machine
- Error messages: structured format matching existing ZER patterns
- Tests: 20-30 checker tests, one per annotation × success/failure case

**Risk:** LOW-MEDIUM. Each dispatch is small and isolated. Risk is in
correctly wiring to existing systems.

### 15.3 Phase 3: Implicit-precondition table (~2 weeks)

**Goal:** ~100 well-known UB classics auto-protected without user
annotation.

Work items:
- Define `ImplicitPrecond` struct and `implicit_table[]` array (see
  Appendix C for full entries)
- Implement `implicit_table_lookup(mnemonic)` (hash table for O(1)
  lookup)
- In NODE_ASM handler, after parsing operand metadata, look up first-word
  mnemonic and apply implicit preconditions
- Allow `@relax_check(...)` to bypass implicit preconditions
- Documentation: list all implicit table entries with citations
- Tests: 50+ tests, one per implicit entry (positive + negative)

**Risk:** LOW. Table is data; the dispatch reuses Phase 2's work.

### 15.4 Phase 4: State-machine annotation generalization (~2-3 weeks)

**Goal:** Generalize F7-light's hardcoded LR/SC tracking to be
annotation-driven.

Work items:
- Replace hardcoded mnemonic checks with annotation dispatch
- Per-block state-machine table (extend existing F7-light data structure)
- Open/close tracking with line attribution
- At-end-of-block balance check
- Cross-block deferral (out of scope; document limitation)
- Tests: rewrite F7-light tests to use new annotation form; verify all
  existing LR/SC tests still pass

**Risk:** MEDIUM. Touches existing F7-light code. Mitigated by extensive
existing test coverage (every LR/SC test still passes after refactor).

### 15.5 Phase 5: Tests and validation (~3-4 weeks)

**Goal:** Comprehensive test suite for option 2's additions.

Work items:
- Positive tests: every annotation × every operand kind × every safety
  system dispatch (~50-80 tests)
- Negative tests: every annotation violation produces correct error
  (~50-80 tests)
- Cross-arch tests: verify all 3 archs (x86_64, aarch64, riscv64)
- Regression tests: every existing F4-F7 test still passes
- Implicit table tests: every entry has positive + negative test
- Documentation tests: examples in docs compile and pass

**Risk:** LOW. Standard test-writing work.

### 15.6 Phase 6: Documentation and migration guide (~1-2 weeks)

**Goal:** Document the new annotation syntax for users.

Work items:
- Update `ZER-LANG.md` / `docs/reference.md` with asm safety chapter
- Update `CLAUDE.md` with annotation reference
- Examples in `examples/` directory (firmware boot, context switch,
  atomic primitives using annotations)
- Migration guide for legacy `asm("string")` users (mostly: no migration
  needed, annotations are additive)
- Update `docs/asm_plan.md` with deferral notice pointing here
- Update fresh-session onboarding

**Risk:** LOW. Documentation work.

### 15.7 Total

| Phase | Effort |
|---|---|
| 1. Operand metadata syntax | 2-3 weeks |
| 2. Annotation processing | 3-4 weeks |
| 3. Implicit-precondition table | 2 weeks |
| 4. State-machine generalization | 2-3 weeks |
| 5. Tests and validation | 3-4 weeks |
| 6. Documentation | 1-2 weeks |
| **Total** | **~13-18 weeks (~3-4 months)** |

After completion: **near-zero ongoing maintenance**. Implicit table is
frozen. Per-arch register tables auto-probe. Z-rules stable. No
per-instruction database to grow.

---

## 16. Implementation Strategy and Phasing

### 16.1 Sequencing relative to other ZER work

Option 2 doesn't block other v1.0 work. Suggested sequencing:

**Before option 2:**
- refactor_ir.md Phase A (the zercheck_ir helper layer refactor) — ~4 hrs
- Self-hosting milestone (zerc.zer compiles itself) — primary v1.0 work
- Real firmware/OS demos in ZER — informs option 2's annotation design

**Option 2 starts when:**
- v1.0 ships with current asm story (Z-rules + F4-F7 + 130 intrinsics)
- Real user feedback on which asm patterns matter
- Implicit table can be designed against real-world usage rather than speculation

**Realistic timeline:**
- 2026 Q4 – 2027 Q1: v1.0 ships
- 2027 Q1 – Q2: Option 2 implementation (~3-4 months)
- 2027 Q2+: Stabilization, real-user feedback informs table additions

### 16.2 Within-option-2 phasing

Each phase is independently testable and shippable:

```
Phase 1 (Operand metadata syntax)
    ↓
Phase 2 (Annotation processing) ────┐
    ↓                               │
Phase 3 (Implicit-precondition)     │
    ↓                               │
Phase 4 (State-machine generalize) ─┤
    ↓                               │
Phase 5 (Tests)                     │
    ↓                               │
Phase 6 (Docs)                      │
                                    ↓
                            v1.x release with full
                            option 2 implementation
```

Phases 1, 2, 3 strictly sequential. Phase 4 can overlap with 3. Phase 5
runs alongside 1-4 (test-driven). Phase 6 last.

### 16.3 Commit boundaries

Suggested commit structure:

```
commit 1-3: Phase 1 — operand metadata syntax (3 commits, one per metadata kind)
commit 4-7: Phase 2 — annotation processing (4 commits, one per annotation type)
commit 8: Phase 3 — implicit-precondition table
commit 9-10: Phase 4 — state-machine generalization (2 commits)
commit 11-15: Phase 5 — tests by category (5 commits)
commit 16-17: Phase 6 — docs (2 commits)
```

Each commit independently revertable. Full test suite runs between
commits. Total: ~17 commits over 3-4 months.

### 16.4 Backward compatibility

All option 2 additions are **additive**. Existing asm code (Session A
syntax without operand metadata) continues to work unchanged.

Implicit table entries fire automatically for legacy asm — so BSR-on-zero
is caught in legacy code without users updating to new syntax.

No migration required. Users adopt annotations at their own pace.

### 16.5 Rollback plan

If option 2 phases cause regression:
- Each commit is revertable individually
- F4-F7 stays functional throughout (not removed until option 2 fully
  ships)
- Worst case: revert to commit before option 2 started, no functionality
  lost

The pivot doesn't burn bridges. We can always extend per-instruction
database later if option 2 proves insufficient (it won't, but the option
exists).

---

## 17. Testing Strategy

### 17.1 Test categories

| Category | Count | Purpose |
|---|---|---|
| Phase 1: parser tests | ~15 | Operand metadata syntax accepted |
| Phase 2: annotation dispatch | ~50 | Each annotation correctly routes to existing system |
| Phase 3: implicit table entries | ~100 | Each implicit entry catches the right bugs |
| Phase 4: state machine | ~20 | Open/close balance across all paired ops |
| Phase 5: integration | ~30 | End-to-end asm programs compile + run |
| Phase 5: regression | ~all existing | F4-F7 / Z-rule tests still pass |
| Phase 5: cross-arch | ~30 | All 3 archs validated |

### 17.2 Test infrastructure

Reuse existing:
- `tests/test_zer.sh` — positive ZER tests (must compile + run + exit 0)
- `tests/zer_fail/*.zer` — negative tests (must fail to compile)
- `tests/test_cross_arch.sh` — 3-arch validation (already runs F4-F7 tests)
- `tools/agreement_audit.sh` — IR vs AST analyzer consistency

Add:
- `tests/zer/asm_annotated_*.zer` — positive option 2 tests
- `tests/zer_fail/asm_annotated_*.zer` — negative option 2 tests
- `tests/asm_implicit_*.zer` — one per implicit table entry

### 17.3 Mandatory tests per phase

**Phase 1 must add:**
- `tests/zer/asm_operand_class.zer` — class metadata accepted
- `tests/zer_fail/asm_operand_class_mismatch.zer` — class mismatch errors

**Phase 2 must add:**
- `tests/zer/asm_requires_nonzero_proven.zer` — VRP-provable nonzero
- `tests/zer_fail/asm_requires_nonzero_unprovable.zer` — VRP can't prove
- (and similar for align, in_range, kernel_context, etc.)

**Phase 3 must add:**
- One positive + one negative test per implicit table entry
- Total: ~200 small tests

**Phase 4 must add:**
- `tests/zer_fail/asm_lrsc_unbalanced.zer` — LR without SC
- `tests/zer_fail/asm_lrsc_orphan_sc.zer` — SC without LR
- (and similar for LDXR/STXR, MONITOR/MWAIT, AMX)

**Phase 5 must add:**
- Real-world asm patterns (boot startup, context switch, atomic primitives)
- End-to-end compile-and-run tests

### 17.4 Regression guard

Before merging option 2:
- `make docker-check` passes (all existing tests, 5,000+)
- `tools/agreement_audit.sh` shows zero disagreements
- `tools/walker_audit.sh` shows zero new walker gaps
- `tools/audit_fixed_buffers.sh` shows no new fixed-size buffers
- Cross-arch test suite passes for all 3 archs

---

## 18. Risks and Mitigations

### 18.1 Risk: Implicit table miss

**Scenario:** A well-known UB class isn't in the implicit table; users
hit it without annotation; UB at runtime.

**Probability:** LOW initially (the ~100 classics are well-cataloged), but
GROWING as new ISAs ship.

**Mitigation:**
- Document the implicit table explicitly so users know what's covered
- Encourage `requires:` annotations for safety-critical asm
- Add entries reactively when real bugs surface (not preemptively)
- The implicit table is documentation as much as protection

### 18.2 Risk: User declarations being wrong

**Scenario:** User writes `requires: nonzero` but their code logic is
wrong; compiler "proves" nonzero against wrong code path.

**Probability:** LOW. VRP is deterministic; if VRP says "provably nonzero,"
it actually is provable.

**Mitigation:**
- Annotations are contracts the COMPILER enforces. User declaring
  `requires: nonzero` doesn't make it true; compiler validates against
  VRP's actual analysis.
- If VRP CAN'T prove the declaration, compile error.
- The risk is only "user wrote wrong precondition" (e.g., declared
  `nonzero` when they should have declared `positive`). Same risk as
  any other annotation in any language — user owns their declarations.

### 18.3 Risk: Annotations are user-facing complexity

**Scenario:** Users find annotations harder to write than raw asm.

**Probability:** MEDIUM. Asm users are sophisticated but annotations add
syntax overhead.

**Mitigation:**
- Implicit table covers ~100 classics WITHOUT user typing
- Annotations are optional; legacy asm works without them
- Examples in docs show real-world usage patterns
- Error messages guide users to correct annotation form

### 18.4 Risk: F7-light state machine generalization breaks existing tests

**Scenario:** Refactoring F7-light's hardcoded LR/SC logic to annotation-
driven introduces regressions.

**Probability:** MEDIUM. State-machine code is delicate.

**Mitigation:**
- Phase 4 (generalization) is independently testable
- Existing F7-light tests all run after refactor
- If regression: rollback Phase 4, ship Phases 1-3 + 5-6 without state
  machine work (state machines stay hardcoded)
- State machine generalization is incremental win, not critical path

### 18.5 Risk: Cross-block ordering can't be enforced

**Scenario:** User writes CLWB in one block, SFENCE in another; compiler
can't track across blocks; missed bug.

**Probability:** MEDIUM. Real pattern (Session G Phase 3 abandonment).

**Mitigation:**
- Document the limitation clearly
- Encourage users to keep paired ops in same block (idiom for safety)
- Provide `@relax_check` for legitimate cross-block patterns
- If real users hit this, revisit CFG-aware extension (v2.x feature)
- Same trade-off as Rust (which doesn't try to track this either)

### 18.6 Risk: Performance regression in compile times

**Scenario:** Adding annotation processing slows down compilation.

**Probability:** LOW. Annotations dispatch to existing systems (no new
heavy analysis).

**Mitigation:**
- Each annotation check is O(1) or O(log n) (hash lookups, range checks)
- Implicit table lookup is O(1) hash
- Benchmark before/after; flag if regression > 5%

### 18.7 Risk: Documentation drift

**Scenario:** Implicit table updates aren't reflected in docs; users
don't know what's auto-protected.

**Probability:** LOW with discipline. MEDIUM without.

**Mitigation:**
- Implicit table source code IS the documentation source
- Generate docs from the table
- Audit script verifies docs match table
- Single source of truth

---

## 19. Open Questions — Decided

This document captures decisions made during the session-long discussion.
For completeness, the decisions:

### 19.1 Q: Pure intrinsic or hybrid? DECIDED: hybrid.

Pure intrinsic isn't feasible at ZER's scale (~4,400 instructions × 3
archs × growing forever). Hybrid (intrinsics + raw asm with annotations
+ tiny implicit table) is the right answer.

### 19.2 Q: Full operand-kind IR rewrite or extend current design? DECIDED: extend.

Same safety, 3x less work. Option 2 (extend) wins on every practical
axis. Option 1 (rewrite) gives theoretical cleanness but no
user-visible advantages.

### 19.3 Q: Manual annotations or per-instruction auto-detection? DECIDED: both.

Implicit table for ~100 well-known classics (auto). Annotations for
everything else (manual). User can override implicit table with
`@relax_check` or explicit annotations.

### 19.4 Q: Session G Phase 5 (CFG ordering)? DECIDED: deferred indefinitely.

Niche (~0.1% of users), complex, abandoned-Phase-3 lesson suggests
hard to get right. User annotations cover the cases users care about.
Revisit if real users hit ordering bugs annotations can't catch.

### 19.5 Q: Z9/Z10/Z13 forward-compat? DECIDED: deferred indefinitely.

Blocked on S1 relaxation we're not doing. ~50 hrs of work for context
that doesn't exist.

### 19.6 Q: Per-instruction database growth? DECIDED: frozen.

Current 120 entries stay. No further additions. Implicit table (~100
frozen classics) replaces the growing per-instruction database.

### 19.7 Q: Naked-only restriction? DECIDED: permanent.

S1 (asm only in naked functions) is interim AND permanent. Migration to
alternative attribute deferred indefinitely. Naked is well-understood
and MISRA-compliant.

### 19.8 Q: Implicit table user-extensible? DECIDED: NO.

Implicit table is compiler-internal, vendored, frozen. User adds entries
via annotations on their code, not via the table. Matches design
decision from 2026-04-29 ("user-extensible intrinsic registry rejected").

### 19.9 Q: GCC dependency? DECIDED: locked in.

GCC is the backend assembler. No multi-backend. Option 2's annotations
are backend-agnostic in principle but no plans to add LLVM/MASM support.

### 19.10 Q: Vale-tier formal proofs? DECIDED: orthogonal, opt-in v2.x+.

Algorithm correctness (is this AES?) is a separate dimension from
operand-safety (is this memory access safe?). Vale-tier opt-in via
`@verified_spec`, planned for v2.x+. Option 2 is language-safe scope
only.

---

## 20. Out of Scope (Explicitly NOT Doing)

This section enumerates what option 2 does NOT include, with rationale.
Fresh sessions tempted to expand scope should review this.

### 20.1 NOT doing: Full operand-kind IR rewrite

Specifically:
- New structured asm AST/IR distinct from current asm syntax
- Migration of all existing F4-F7 code to new framework
- Removal of Z-rules (they stay)

Why: ~5-6 extra months for no user-visible benefit. Option 2 achieves
equivalent safety. See section 6.2.

### 20.2 NOT doing: Session G Phase 5 (CFG OrderingState)

Specifically:
- CFG-aware acquire/release pairing
- Persistent-memory ordering tracking
- Cross-block barrier propagation

Why: Niche (~0.1% of users). Abandoned Phase 3 showed false-positive
risk. User annotations cover practical cases. ~30-40 hrs not worth it.

### 20.3 NOT doing: Z9/Z10/Z13 forward-compat Z-rules

Specifically:
- Z9 privilege escalation through asm
- Z10 ISR-context bans through asm
- Z13 thread-local access through asm

Why: Blocked on S1 relaxation we're not doing. ~50 hrs for unused
forward-compat.

### 20.4 NOT doing: Adding more entries to F4/F5/F6 instruction tables

Specifically:
- More constraint kinds per instruction
- More ordering metadata
- New entries per ISA extension

Why: The per-instruction database is the maintenance burden we're
pivoting away from. Implicit table (~100 frozen classics) replaces it.

### 20.5 NOT doing: User-extensible implicit table

Specifically:
- `.zerdata`-style user-defined implicit precondition entries
- Loadable instruction-specific safety rules
- Per-project custom UB classes

Why: Rejected 2026-04-29 ("user-extensible intrinsic registry"). Users
define instruction-specific safety via annotations on their own code,
not via compiler-loaded data.

### 20.6 NOT doing: Multi-backend support

Specifically:
- LLVM/Clang backend
- MASM/NASM output
- Direct machine code emission

Why: GCC-only decision locked in (asm_plan.md decision 2). Annotation
framework is backend-agnostic but no plans to use other backends.

### 20.7 NOT doing: Direct asm-byte emission

Specifically:
- Replacing GCC's assembler
- Own LLVM MC-style infrastructure
- Bytes-out compilation path

Why: GCC handles asm assembly correctly; no value in reinventing. Same
decision as 2026-04-29 validation.

### 20.8 NOT doing: Naked-attribute migration

Specifically:
- Replacing `naked` with alternative attribute
- Allowing asm in non-naked functions
- New context model

Why: Naked is interim AND permanent. Migration is churn for marginal
cleanness. Test rewrite cost (~20 hrs) not justified.

### 20.9 NOT doing: Cross-block state machines

Specifically:
- CFG-aware state-machine annotation tracking
- Persistence-fence pairing across blocks
- Lock-ordering tracking through asm

Why: Same reason as Session G Phase 5. Niche, complex, edge-case-prone.
User keeps paired ops in same block (idiom).

### 20.10 NOT doing: Compile-time asm simulation

Specifically:
- Symbolic execution of asm bytes
- Per-instruction effect tracking via comptime
- Asm code as runnable comptime expression

Why: Vale-tier territory. Massive effort. Out of language-safety scope.

### 20.11 NOT doing: Performance optimization of asm contents

Specifically:
- Register allocation optimization in inline asm
- Instruction scheduling
- Peephole optimization

Why: GCC does this. ZER doesn't reinvent. Optimization isn't ZER's job
at asm level.

---

## 21. The "Smart Language vs Smart Compiler" Principle

This section captures the architectural principle that motivated option 2.
Worth preserving for future decisions.

### 21.1 The principle

> **Compilers should be generic algorithms over annotated languages, not
> databases of facts about specific entities.**

### 21.2 What "smart" means in each direction

**Smart language** means:
- Vocabulary for users to declare properties (annotations, attributes,
  qualifiers)
- Structured forms that carry information (types, traits, contracts)
- Frozen primitives (operand kinds, type categories, capability classes)
- Generic algorithms enforce declarations

**Smart compiler** means:
- Per-item databases (specific functions, specific structs, specific
  instructions)
- Hardcoded knowledge about external entities
- Logic that knows what specific things do
- Updates required when the external world changes

### 21.3 Why smart-language wins

| Property | Smart language | Smart compiler |
|---|---|---|
| Maintenance burden | Bounded (vocabulary size) | Unbounded (entity count grows) |
| Future-proofing | User declares new things | Compiler must learn new things |
| Cross-domain reuse | Generic algorithms apply | Domain-specific code |
| Code review | Users see contracts explicitly | Knowledge hidden in compiler |
| Audit trail | Annotations grep-able | Database opaque |
| Migration cost (when external world changes) | Zero | Hours-to-years |

The smart language approach trades a fixed vocabulary design cost for
unbounded operational savings.

### 21.4 Where the principle applies

**Apply this principle when:**
1. The information is per-item (not generic)
2. The items can grow in number (not bounded by language design)
3. The items live outside the compiler's domain (hardware, libraries,
   APIs, protocols)
4. Users have authoritative knowledge of the items
5. A generic algorithm can enforce declared properties

**Don't apply this principle when:**
1. The information is genuinely generic (e.g., VRP on integer arithmetic)
2. Users can't reasonably declare the property (e.g., complex range
   inference)
3. The compiler has authoritative knowledge (e.g., builtin operators)

VRP is correctly compiler-smart because integer ranges aren't external
items; they're properties of arithmetic operations the compiler controls.
Per-instruction UB is correctly language-smart because instructions are
external items the compiler doesn't own.

### 21.5 Where ZER follows the principle (verified)

| Smart language | Smart compiler |
|---|---|
| `volatile`, `const`, `keep`, `restrict` qualifiers | Type inference |
| `mmio 0x4000..0x4FFF;` declarations | Bounds checking |
| `shared struct` annotation | Auto-lock insertion |
| `Handle(T)` type carries lifecycle contract | Generic state machine |
| `move struct` declaration | Generic HS_TRANSFERRED state |
| `naked fn` attribute | Generic asm context check |
| `comptime` annotation | Compile-time evaluator |
| `requires: nonzero` (option 2) | Generic VRP check |
| `opens_state: X` (option 2) | Generic state machine |
| `cpu_feature: avx512f` | Generic feature flag |

ZER's vocabulary (annotations, attributes, qualifiers) is the smart-language
side. ZER's safety algorithms (VRP, escape analysis, type system, state
machines) are the smart-compiler side. The two work together: language
declares, compiler enforces.

### 21.6 Where ZER could violate the principle (must avoid)

| Tempting smart-compiler addition | Why it would be wrong | Smart-language alternative |
|---|---|---|
| Per-instruction UB database | Maintenance hell, doesn't match ZER's architecture | Annotations + tiny implicit table |
| Per-driver lint rules | checkpatch.pl is the warning | Generic safety + driver-level annotations |
| Per-API channel safety rules | Each new IPC pattern = compiler change | Generic shared struct + Ring(T) |
| Per-protocol parser rules | Each protocol = compiler change | Generic comptime + types |
| Per-struct ownership rules | Ad-hoc, fragile | Generic `move struct` + lifecycle |
| Per-function purity facts | Freezes API | Generic FuncProps analysis |

The asm_plan direction was the most prominent recent violation. Option 2
fixes it.

### 21.7 The general rule

If you're tempted to add per-X knowledge to the compiler:

```
1. Identify the property you're encoding (alignment, purity, lifetime, etc.)
2. Design a language-level annotation expressing it
3. Write a generic algorithm enforcing it
4. (Optional) Maintain a small frozen table for common ergonomic auto-fill
5. STOP. Don't grow the per-X database.
```

This is the rule. It applies to asm safety. It applies to anything else
where this temptation arises.

---

## 22. Fresh-Session Onboarding Checklist

If you're a fresh session picking up the asm safety work, here's the
onboarding order:

### 22.1 Read these documents in order

1. **This document end-to-end** (~3000 lines, ~45 min)
2. `docs/ASM_ZER-LANG.md` (the original 2026-04-01 design, ~476 lines)
   — establishes baseline philosophy
3. `docs/asm_plan.md` Status section + "Sub-Extension Architecture
   Validated 2026-04-29" section — what's already shipped
4. `CLAUDE.md` "Stage 4" section — current asm status
5. `BUGS-FIXED.md` recent asm-related entries (BUG-652, BUG-653, etc.)

### 22.2 Verify current state

```bash
# Confirm 130 intrinsics implemented
grep -E "@cpu_|@atomic_|@barrier|@bswap|@popcount|@ctz|@clz" src/builtins.c | wc -l

# Confirm register tables vendored
ls src/safety/asm_register_tables_*.c

# Confirm F4-F7 instruction tables vendored
ls src/safety/asm_instruction_table_*.c

# Confirm Z-rules wired (Z1-Z12)
grep -E "^/\* Z[0-9]+:" checker.c zercheck_ir.c | wc -l

# Confirm tests pass
make docker-check
```

If any of these fail, the codebase isn't in the state this doc assumes.
Investigate before starting option 2 work.

### 22.3 Understand the architectural principle

Read section 21 of this document. The "smart language, generic compiler"
principle is the foundation for every option 2 design decision.

### 22.4 Understand what's frozen vs evolving

**Frozen (don't extend):**
- Per-instruction database (~120 entries in F4-F7)
- Z-rules count (Z1-Z12, no Z14+)
- Implicit precondition table (~100 entries, frozen at well-known
  classics)
- Operand kind enum (10 categories)

**Evolving (small, bounded):**
- Register tables (auto-probed, rare updates per ISA extension)
- CPU feature flags (~2-3 per year)
- Test suite (grows with usage)

### 22.5 Begin work

Sequence per section 16:
1. Phase 1: operand metadata syntax (~2-3 weeks)
2. Phase 2: annotation processing (~3-4 weeks)
3. Phase 3: implicit-precondition table (~2 weeks)
4. Phase 4: state-machine generalization (~2-3 weeks)
5. Phase 5: tests (~3-4 weeks)
6. Phase 6: docs (~1-2 weeks)

Total: ~3-4 months.

### 22.6 Decision checkpoints

Before committing significant work, verify these decisions haven't
changed:

- Is `naked fn` still the asm context boundary? (S1 not relaxed)
- Is GCC still the backend? (no multi-backend plans)
- Is Vale-tier still opt-in v2.x+? (algorithm correctness orthogonal)
- Is per-instruction DB still frozen? (no growth beyond current 120)

If any of these have changed, this document's design decisions need
revisiting before implementation.

### 22.7 Anti-patterns to avoid

- DON'T extend per-instruction database
- DON'T try to make implicit table user-extensible (rejected 2026-04-29)
- DON'T add Z14, Z15, etc. (annotation framework replaces these)
- DON'T build CFG-aware ordering analysis (Session G Phase 5 deferred)
- DON'T rewrite to full operand-kind IR (option 1, more work for no win)
- DON'T claim "100% asm safety" — claim what's accurate (see section 2)

---

## Appendix A: Code Samples

Illustrative examples of option 2's design. Actual implementation should
match existing codebase conventions.

### A.1 Phase 1: Operand metadata extension

```c
/* AST extension in ast.h */

typedef enum {
    OPERAND_ROLE_READ,
    OPERAND_ROLE_WRITE,
    OPERAND_ROLE_READ_WRITE,
    OPERAND_ROLE_ADDRESS,
} OperandRole;

typedef enum {
    REG_CLASS_GPR,
    REG_CLASS_VEC,
    REG_CLASS_MASK,
    REG_CLASS_FP,
    REG_CLASS_OTHER,
} RegClass;

typedef struct AsmOperandMetadata {
    bool has_class;
    RegClass class;
    bool has_bits;
    int bits;
    bool has_role;
    OperandRole role;
    bool has_mem_size;
    int mem_size;
    bool has_mem_align;
    int mem_align;
    bool mem_read;
    bool mem_write;
} AsmOperandMetadata;

typedef struct AsmOperandBinding {
    const char *register_name;
    int register_name_len;
    Node *expr;
    AsmOperandMetadata metadata;     /* NEW */
    AsmAnnotation *annotations;      /* NEW */
    int annotation_count;
} AsmOperandBinding;

/* Parser extension in parser.c */

static AsmOperandBinding *parse_asm_operand_binding(Parser *p) {
    AsmOperandBinding *binding = arena_alloc(p->arena, sizeof(*binding));
    memset(binding, 0, sizeof(*binding));
    
    /* Parse "regname" = expr (existing) */
    expect(p, TOK_STRING);
    binding->register_name = p->previous.text;
    binding->register_name_len = p->previous.len;
    expect(p, TOK_EQ);
    binding->expr = parse_expression(p);
    
    /* Parse optional metadata fields */
    while (match(p, TOK_COMMA)) {
        if (check(p, TOK_IDENT)) {
            const char *field = p->current.text;
            int flen = p->current.len;
            
            if (flen == 5 && memcmp(field, "class", 5) == 0) {
                advance(p);
                expect(p, TOK_COLON);
                binding->metadata.has_class = true;
                binding->metadata.class = parse_reg_class(p);
            } else if (flen == 4 && memcmp(field, "bits", 4) == 0) {
                advance(p);
                expect(p, TOK_COLON);
                expect(p, TOK_INT_LIT);
                binding->metadata.has_bits = true;
                binding->metadata.bits = p->previous.int_value;
            } else if (flen == 4 && memcmp(field, "role", 4) == 0) {
                /* ... similar for role, mem_size, mem_align, etc. ... */
            } else if (flen == 8 && memcmp(field, "requires", 8) == 0) {
                advance(p);
                expect(p, TOK_COLON);
                parse_asm_annotation(p, binding);
            }
            /* ... more annotation parsing ... */
        }
    }
    
    return binding;
}
```

### A.2 Phase 2: Annotation dispatch

```c
/* In checker.c NODE_ASM handler */

static void check_asm_operand_annotations(Checker *c, AsmOperandBinding *b) {
    for (int i = 0; i < b->annotation_count; i++) {
        AsmAnnotation *annot = &b->annotations[i];
        
        switch (annot->kind) {
            case ANNOT_REQUIRES_NONZERO: {
                /* Dispatch to existing VRP */
                VarRange range;
                if (!find_var_range(c, b->expr, &range)) {
                    checker_error(c, annot->line,
                        "operand precondition 'requires: nonzero' cannot be verified:\n"
                        "    VRP cannot determine value range of expression");
                    return;
                }
                if (!range.known_nonzero && range.min == 0 && range.max == 0) {
                    /* Definitely zero */
                    checker_error(c, annot->line,
                        "operand precondition violated:\n"
                        "    'requires: nonzero' but expression is provably zero");
                    return;
                }
                if (!range.known_nonzero && range.min <= 0 && range.max >= 0) {
                    /* Range includes zero */
                    checker_error(c, annot->line,
                        "operand precondition 'requires: nonzero' cannot be verified:\n"
                        "    expression range [%lld, %lld] includes zero\n"
                        "    add explicit check or use @relax_check(NONZERO)",
                        range.min, range.max);
                    return;
                }
                /* Provably nonzero — OK */
                break;
            }
            
            case ANNOT_REQUIRES_ALIGNED: {
                int required_align = annot->align_value;
                int actual_align = expr_alignment(c, b->expr);
                if (actual_align < required_align) {
                    checker_error(c, annot->line,
                        "operand precondition 'requires: aligned(%d)' violated:\n"
                        "    expression has alignment %d, required %d",
                        required_align, actual_align, required_align);
                    return;
                }
                break;
            }
            
            case ANNOT_OPENS_STATE: {
                state_machine_open(c->asm_state_machine,
                    annot->state_id, annot->line);
                break;
            }
            
            case ANNOT_CLOSES_STATE: {
                if (!state_machine_is_open(c->asm_state_machine,
                    annot->state_id)) {
                    checker_error(c, annot->line,
                        "operand precondition 'closes_state: %s' violated:\n"
                        "    no matching 'opens_state' in same asm block",
                        annot->state_name);
                    return;
                }
                state_machine_close(c->asm_state_machine, annot->state_id);
                break;
            }
            
            case ANNOT_REQUIRES_KERNEL_CONTEXT: {
                if (!c->current_func || !c->current_func->is_kernel) {
                    checker_error(c, annot->line,
                        "operand precondition 'requires: kernel_context' violated:\n"
                        "    current function not marked as kernel-context");
                    return;
                }
                break;
            }
            
            /* ... more annotation kinds ... */
        }
    }
}
```

### A.3 Phase 3: Implicit-precondition table

```c
/* In src/safety/asm_implicit_precond.c */

typedef enum {
    IMPL_REQUIRES_NONZERO  = 1 << 0,
    IMPL_REQUIRES_ALIGNED  = 1 << 1,
    IMPL_OPENS_STATE       = 1 << 2,
    IMPL_CLOSES_STATE      = 1 << 3,
    IMPL_REQUIRES_KERNEL   = 1 << 4,
    IMPL_REQUIRES_AFTER    = 1 << 5,
    IMPL_COMPOUND_CHECK    = 1 << 6,  /* INT_MIN/-1 backstop */
} ImplicitFlag;

typedef struct {
    const char *mnemonic;
    uint32_t flags;
    int op_index;              /* -1 = applies to instruction itself, not operand */
    int constraint_value;      /* alignment, state_id, etc. */
    const char *citation;
} ImplicitPrecond;

/* The frozen ~100-entry table */
static const ImplicitPrecond implicit_table[] = {
    /* Bit-search — UB on zero */
    { "bsr",     IMPL_REQUIRES_NONZERO, 0, 0, "Intel SDM Vol 2 BSR" },
    { "bsf",     IMPL_REQUIRES_NONZERO, 0, 0, "Intel SDM Vol 2 BSF" },
    
    /* Division — UB on zero */
    { "div",     IMPL_REQUIRES_NONZERO, 0, 0, "Intel SDM Vol 2 DIV" },
    { "idiv",    IMPL_REQUIRES_NONZERO | IMPL_COMPOUND_CHECK, 0, 0,
                 "Intel SDM Vol 2 IDIV" },
    
    /* Aligned vector loads — SIGSEGV on misalignment */
    { "movaps",  IMPL_REQUIRES_ALIGNED, 0, 16, "Intel SDM Vol 2 MOVAPS" },
    { "movapd",  IMPL_REQUIRES_ALIGNED, 0, 16, "Intel SDM Vol 2 MOVAPD" },
    { "movdqa",  IMPL_REQUIRES_ALIGNED, 0, 16, "Intel SDM Vol 2 MOVDQA" },
    { "vmovaps", IMPL_REQUIRES_ALIGNED, 0, 32, "Intel SDM Vol 2 VMOVAPS (AVX)" },
    
    /* RISC-V LR/SC pairing */
    { "lr.w",    IMPL_OPENS_STATE,      0, STATE_RV_LRSC, "RISC-V ISA A Ext" },
    { "lr.d",    IMPL_OPENS_STATE,      0, STATE_RV_LRSC, "RISC-V ISA A Ext" },
    { "sc.w",    IMPL_CLOSES_STATE,     0, STATE_RV_LRSC, "RISC-V ISA A Ext" },
    { "sc.d",    IMPL_CLOSES_STATE,     0, STATE_RV_LRSC, "RISC-V ISA A Ext" },
    
    /* ARM exclusive load/store */
    { "ldxr",    IMPL_OPENS_STATE,      0, STATE_ARM_LDXR, "ARM ARM A2.7" },
    { "ldaxr",   IMPL_OPENS_STATE,      0, STATE_ARM_LDXR, "ARM ARM A2.7" },
    { "stxr",    IMPL_CLOSES_STATE,     0, STATE_ARM_LDXR, "ARM ARM A2.7" },
    { "stlxr",   IMPL_CLOSES_STATE,     0, STATE_ARM_LDXR, "ARM ARM A2.7" },
    
    /* x86 MONITOR/MWAIT */
    { "monitor", IMPL_OPENS_STATE,      0, STATE_X86_MONITOR, "Intel SDM Vol 2" },
    { "mwait",   IMPL_CLOSES_STATE,     0, STATE_X86_MONITOR, "Intel SDM Vol 2" },
    { "umonitor", IMPL_OPENS_STATE,     0, STATE_X86_UMONITOR, "Intel SDM Vol 2" },
    { "umwait",  IMPL_CLOSES_STATE,     0, STATE_X86_UMONITOR, "Intel SDM Vol 2" },
    
    /* x86 AMX tile operations */
    { "ldtilecfg", IMPL_OPENS_STATE,    0, STATE_X86_AMX, "Intel SDM Vol 2" },
    { "tileloadd", IMPL_REQUIRES_AFTER, 0, STATE_X86_AMX, "Intel SDM Vol 2" },
    { "tilestored", IMPL_REQUIRES_AFTER, 0, STATE_X86_AMX, "Intel SDM Vol 2" },
    { "tilezero",  IMPL_REQUIRES_AFTER, 0, STATE_X86_AMX, "Intel SDM Vol 2" },
    
    /* Privileged instructions — kernel-only */
    { "rdmsr",   IMPL_REQUIRES_KERNEL, -1, 0, "Intel SDM Vol 2 RDMSR" },
    { "wrmsr",   IMPL_REQUIRES_KERNEL, -1, 0, "Intel SDM Vol 2 WRMSR" },
    { "lgdt",    IMPL_REQUIRES_KERNEL, -1, 0, "Intel SDM Vol 2 LGDT" },
    { "lidt",    IMPL_REQUIRES_KERNEL, -1, 0, "Intel SDM Vol 2 LIDT" },
    { "ltr",     IMPL_REQUIRES_KERNEL, -1, 0, "Intel SDM Vol 2 LTR" },
    { "lldt",    IMPL_REQUIRES_KERNEL, -1, 0, "Intel SDM Vol 2 LLDT" },
    { "invd",    IMPL_REQUIRES_KERNEL, -1, 0, "Intel SDM Vol 2 INVD" },
    { "wbinvd",  IMPL_REQUIRES_KERNEL, -1, 0, "Intel SDM Vol 2 WBINVD" },
    { "swapgs",  IMPL_REQUIRES_KERNEL, -1, 0, "Intel SDM Vol 2 SWAPGS" },
    
    /* See Appendix C for the full list */
};

const ImplicitPrecond *implicit_table_lookup(const char *mnemonic, int len) {
    /* Simple linear scan — table is ~100 entries, fast enough */
    /* For production, replace with perfect hash if profiling shows hot spot */
    for (size_t i = 0; i < ARRAY_SIZE(implicit_table); i++) {
        const ImplicitPrecond *entry = &implicit_table[i];
        size_t entry_len = strlen(entry->mnemonic);
        if (entry_len == (size_t)len &&
            memcmp(entry->mnemonic, mnemonic, len) == 0) {
            return entry;
        }
    }
    return NULL;
}
```

### A.4 Phase 4: State-machine generalization

```c
/* In checker.c — state machine infrastructure */

typedef struct {
    int state_id;
    bool is_open;
    int open_line;
} StateMachineEntry;

typedef struct {
    StateMachineEntry *states;
    int count;
    int capacity;
} StateMachineTable;

void state_machine_open(StateMachineTable *sm, int state_id, int line) {
    /* Find existing entry */
    for (int i = 0; i < sm->count; i++) {
        if (sm->states[i].state_id == state_id) {
            if (sm->states[i].is_open) {
                /* Re-opening (e.g., nested LR/SC) — typically an error,
                 * but some patterns allow it; arch-specific */
            }
            sm->states[i].is_open = true;
            sm->states[i].open_line = line;
            return;
        }
    }
    
    /* New state — add entry */
    if (sm->count >= sm->capacity) {
        sm->capacity = sm->capacity ? sm->capacity * 2 : 8;
        sm->states = realloc(sm->states, sm->capacity * sizeof(StateMachineEntry));
    }
    sm->states[sm->count].state_id = state_id;
    sm->states[sm->count].is_open = true;
    sm->states[sm->count].open_line = line;
    sm->count++;
}

bool state_machine_is_open(StateMachineTable *sm, int state_id) {
    for (int i = 0; i < sm->count; i++) {
        if (sm->states[i].state_id == state_id) {
            return sm->states[i].is_open;
        }
    }
    return false;
}

void state_machine_close(StateMachineTable *sm, int state_id) {
    for (int i = 0; i < sm->count; i++) {
        if (sm->states[i].state_id == state_id) {
            sm->states[i].is_open = false;
            return;
        }
    }
}

/* End-of-asm-block balance check */
void state_machine_check_balanced(Checker *c, StateMachineTable *sm, int block_end_line) {
    for (int i = 0; i < sm->count; i++) {
        if (sm->states[i].is_open) {
            checker_error(c, block_end_line,
                "state machine '%s' opened at line %d but not closed in same asm block",
                state_id_to_name(sm->states[i].state_id),
                sm->states[i].open_line);
        }
    }
}
```

### A.5 Full end-to-end example — RISC-V atomic increment

```zer
naked void atomic_inc_riscv(*u32 p) {
    asm {
        instructions: "lr.w t0, (a0)"
        outputs: { 
            "t0" = old_val,
            class: gpr,
            bits: 32,
        }
        inputs: { 
            "a0" = p,
            class: gpr,
            bits: 64,
            role: address,
            mem_size: 4,
            mem_align: 4,
            mem_read: true,
        }
        opens_state: rv_lrsc       /* OR auto via implicit table */
        safety: "Load-reserved word from p, opening LR/SC critical region"
    }
    asm {
        instructions: "addi t0, t0, 1"
        outputs: { "t0" = new_val, class: gpr, bits: 32 }
        inputs:  { "t0" = old_val, class: gpr, bits: 32 }
        safety: "Increment loaded value"
    }
    asm {
        instructions: "sc.w t1, t0, (a0)"
        outputs: { 
            "t1" = sc_result,
            class: gpr,
            bits: 32,
        }
        inputs: { 
            "t0" = new_val,
            class: gpr,
            bits: 32,
            "a0" = p,
            class: gpr,
            bits: 64,
            role: address,
            mem_size: 4,
            mem_align: 4,
            mem_write: true,
        }
        closes_state: rv_lrsc       /* OR auto via implicit table */
        safety: "Store-conditional, closing LR/SC region. sc_result=0 if successful, nonzero if reservation lost."
    }
    /* Compiler verifies:
     *   - p is valid 4-byte aligned pointer (existing alignment + escape)
     *   - LR/SC pairing is balanced in this naked function
     *   - new_val flows from old_val correctly (register class match)
     *   - All memory accesses respect access size + alignment
     *   - Naked function context (S1)
     */
}
```

---

## Appendix B: Annotation Reference

Complete catalog of operand annotations option 2 supports.

### B.1 Operand metadata fields

| Field | Type | Default | Description |
|---|---|---|---|
| `class` | enum | inferred from register name | Register class: `gpr`, `vec`, `mask`, `fp`, `other` |
| `bits` | integer | inferred from register name | Register width in bits |
| `role` | enum | inferred from input/output position | `read`, `write`, `read_write`, `address` |
| `mem_size` | integer | required for `role: address` | Bytes accessed via this address |
| `mem_align` | integer | optional | Required alignment of address (also written as `align: N` shorthand) |
| `mem_read` | bool | true if role=address | Whether asm reads from this address |
| `mem_write` | bool | false if role=address | Whether asm writes to this address |

### B.2 Precondition annotations

| Annotation | Operand kind | Dispatches to | Description |
|---|---|---|---|
| `requires: nonzero` | scalar value | VRP | Value provably ≠ 0 |
| `requires: positive` | scalar value | VRP | Value provably > 0 |
| `requires: in_range(a, b)` | scalar value | VRP | Value provably in [a, b] |
| `requires: aligned(N)` | address | alignment infra | Address provably N-byte aligned |
| `requires: kernel_context` | (none) | context flags | Function in kernel scope |
| `requires: nonnull` | pointer | optional-unwrap | Pointer provably non-NULL |
| `requires: mmio_range(name)` | address | MMIO system | Address in declared mmio range |
| `align: N` | address | alignment infra | Shorthand for `requires: aligned(N)` |

### B.3 State-machine annotations

| Annotation | Description | Dispatches to |
|---|---|---|
| `opens_state: X` | Opens state machine `X` at this asm | State-machine infra |
| `closes_state: X` | Must match preceding `opens_state: X` in same block | State-machine infra |
| `requires_after: X` | Must follow at least one `opens_state: X` in same block | State-machine infra |

### B.4 CPU feature gating

| Annotation | Description |
|---|---|
| `cpu_feature: avx512f` | Requires AVX-512F enabled in build (`--target-features=avx512f`) |
| `cpu_feature: sse4_2` | Requires SSE 4.2 |
| `cpu_feature: aes_ni` | Requires AES-NI |
| `cpu_feature: neon` | Requires ARM NEON |
| `cpu_feature: sve` | Requires ARM SVE |
| (existing feature flags from F4) | (existing) |

### B.5 Escape hatches

| Annotation | Description |
|---|---|
| `@relax_check(ANNOT_KIND)` | Skip a specific implicit precondition check |
| `@relax_check(*)` | Skip all implicit precondition checks (rare, audit-grep-able) |

### B.6 Reserved state names (implicit table)

| State name | Used by |
|---|---|
| `rv_lrsc` | RISC-V `lr.w`, `lr.d`, `sc.w`, `sc.d` |
| `arm_ldxr` | ARM `ldxr`, `ldaxr`, `stxr`, `stlxr` |
| `x86_monitor` | x86 `monitor`, `mwait` |
| `x86_umonitor` | x86 `umonitor`, `umwait` (user-mode wait) |
| `x86_amx` | x86 `ldtilecfg`, `tileloadd`, `tilestored`, `tilezero` |
| `pmem_persist` | Persistent-memory `clwb` + `sfence` (when CFG-aware lands) |

User-defined state names are also allowed (uppercase recommended for
custom names).

---

## Appendix C: Implicit-Precondition Table — Concrete Contents

Full enumeration of the ~100-entry implicit table. Frozen reference.

### C.1 x86_64 entries (~50)

**Bit-search (UB on zero):**
- `bsr` — requires nonzero on operand 0
- `bsf` — requires nonzero on operand 0
- (`lzcnt`, `tzcnt`, `popcnt` — well-defined on zero, no implicit constraint)

**Integer division (UB on zero, plus INT_MIN/-1 for signed):**
- `div` — requires nonzero on operand 0
- `idiv` — requires nonzero on operand 0, compound check INT_MIN/-1
- `divsd`, `divss`, `divpd`, `divps` — FP division, no integer UB

**Aligned vector loads (SIGSEGV on misalignment):**
- `movaps` — requires aligned(16) on memory operand
- `movapd` — requires aligned(16)
- `movdqa` — requires aligned(16)
- `vmovaps` — requires aligned(32) when 256-bit
- `vmovapd` — requires aligned(32)
- `vmovdqa` — requires aligned(32)
- `vmovaps` (512-bit) — requires aligned(64)
- `movntdq` — requires aligned(16), non-temporal hint
- `movntps`, `movntpd` — requires aligned(16)

**Cache management:**
- `clflush` — no implicit (user-mode, aligned address typical but not required)
- `clflushopt` — same
- `clwb` — opens_state: pmem_persist (informational, no error currently)
- `invd` — requires kernel context
- `wbinvd` — requires kernel context

**MMX (legacy, niche):**
- `emms` — closes any MMX state (informational)
- `femms` — same

**Atomic pairs:**
- `monitor` — opens_state: x86_monitor, requires kernel context (legacy)
- `mwait` — closes_state: x86_monitor, requires kernel context (legacy)
- `umonitor` — opens_state: x86_umonitor, user-mode (no kernel requirement)
- `umwait` — closes_state: x86_umonitor, user-mode

**AMX (sub-extension):**
- `ldtilecfg` — opens_state: x86_amx, requires CPU feature AMX
- `sttilecfg` — opens_state: x86_amx
- `tilereleaseall` — closes_state: x86_amx
- `tileloadd` — requires_after: x86_amx, requires CPU feature AMX
- `tilestored` — requires_after: x86_amx
- `tilezero` — requires_after: x86_amx
- (AMX compute instructions similar)

**Privileged (kernel-only):**
- `rdmsr` — requires kernel context
- `wrmsr` — requires kernel context
- `rdpmc` — requires kernel context (unless CR4.PCE=1, user-controlled)
- `lgdt` — requires kernel context
- `lidt` — requires kernel context
- `lldt` — requires kernel context
- `ltr` — requires kernel context
- `cli`, `sti` — requires kernel context (cpl-dependent in protected mode)
- `swapgs` — requires kernel context
- `iretq` — requires kernel context
- `syscall`, `sysret` — entry/exit transitions, kernel state expected
- `vmcall`, `vmlaunch`, `vmresume` — hypervisor, kernel context

**Control register access:**
- `mov cr0, *` — requires kernel context
- `mov cr3, *` — requires kernel context
- `mov cr4, *` — requires kernel context

**Debug registers:**
- `mov dr*, *` — requires kernel context

**I/O port instructions:**
- `in`, `out`, `ins`, `outs` — requires kernel context (or IOPL≥CPL)

**Hardware-protected extensions:**
- `xsave`, `xrstor` — operand requires CPU feature XSAVE
- `xsetbv` — requires kernel context (XCR0 write)

### C.2 ARM64 (aarch64) entries (~25)

**Atomic exclusive pairs:**
- `ldxr` — opens_state: arm_ldxr
- `ldxrb`, `ldxrh`, `ldxr (32/64)` — variants, all open_state: arm_ldxr
- `ldaxr`, `ldaxrb`, `ldaxrh` — load-acquire-exclusive, same state
- `stxr` — closes_state: arm_ldxr
- `stxrb`, `stxrh` — same
- `stlxr`, `stlxrb`, `stlxrh` — store-release-exclusive

**Aligned loads:**
- (ARM64 generally tolerant; specific instructions may require alignment depending on MMU config)

**Privileged:**
- `msr` (to/from system registers) — requires kernel context (EL2/EL3)
- `mrs` (from sensitive registers) — requires kernel context
- `eret` — requires kernel context
- `dret` — requires kernel context
- `smc` — secure monitor call (kernel)
- `hvc` — hypervisor call (kernel)

**Cache:**
- `dc cvac`, `dc civac` — user-mode cache maintenance (no implicit)
- `dc cvap`, `dc cvadp` — persistent memory
- `ic ialluis`, `ic iallu` — kernel context

**SVE/NEON:**
- (Most NEON/SVE instructions are well-defined; alignment optional)

### C.3 RISC-V entries (~25)

**Atomic LR/SC pairs (A extension):**
- `lr.w` — opens_state: rv_lrsc
- `lr.d` — opens_state: rv_lrsc
- `sc.w` — closes_state: rv_lrsc
- `sc.d` — closes_state: rv_lrsc

**AMO instructions:**
- `amoadd.w/d`, `amoor.w/d`, `amoand.w/d`, `amoxor.w/d`, `amoswap.w/d` —
  no implicit constraint (well-defined)
- `amomax.w/d`, `amomin.w/d` — same

**Fence instructions:**
- `fence` — no implicit (memory ordering hint)
- `fence.i` — no implicit (instruction fence)

**Privileged (M-mode / S-mode):**
- `mret` — requires M-mode kernel context
- `sret` — requires S-mode kernel context
- `wfi` — typically kernel context
- `sfence.vma` — requires kernel context
- `csrrw`, `csrrs`, `csrrc` — depends on CSR (some user, some kernel)
- `ecall` — entry-point, kernel state expected
- `ebreak` — debug

**Hypervisor (H extension):**
- `hfence.gvma`, `hfence.vvma` — requires hypervisor context
- `hret` — requires hypervisor context

### C.4 Cross-arch totals

- x86_64 entries: ~50
- aarch64 entries: ~25
- riscv64 entries: ~25
- **Total: ~100 entries**

All entries reference frozen UB classes from established ISA documentation
(Intel SDM, ARM ARM, RISC-V ISA Manual). Updates: extremely rare (maybe
once per decade for genuinely new well-known UB class).

---

## Appendix D: Mapping from asm_plan to This Approach

Cross-reference: each piece of asm_plan.md work and what option 2 does
with it.

### D.1 Things that stay

| asm_plan.md item | Status in option 2 |
|---|---|
| 130 intrinsics (D-Alpha-1 through D-Alpha-14) | **Stay as-is** |
| Z1: UAF check at asm operand boundary | **Stay** |
| Z2: Move struct transfer | **Stay** |
| Z3: VRP invalidation on output | **Stay** |
| Z4: Provenance clear on output | **Stay** |
| Z5: Memory clobber escape | **Stay** |
| Z6: Defer/async ban | **Stay** |
| Z7: MMIO range check | **Stay** |
| Z8: Qualifier preservation | **Stay** |
| Z11: Non-keep pointer + memory clobber | **Stay** |
| Z12: scan_frame asm recursion | **Stay** |
| Session A: structured asm syntax | **Stay** (base for extension) |
| Session B: typed operand bindings | **Stay** (extended) |
| F2: x86_64 register tables | **Stay** (auto-probed) |
| F5: ARM64 register tables | **Stay** (auto-probed) |
| F6: RISC-V register tables | **Stay** (auto-probed) |
| F4 CPU feature gating | **Stay** |
| F7-light LR/SC state machine | **Stay** (becomes implicit table + generalized state machine) |
| F7-full Step 2 constraints (NONZERO, ALIGNED, etc.) | **Stay** (becomes implicit table + annotation infrastructure) |
| C8 ordering metadata | **Stay** (informational; implicit table for pairing) |
| Naked-only restriction (S1) | **Stay** (permanent) |
| Sub-extension architecture | **Stay** (locked in) |

### D.2 Things that stop / defer

| asm_plan.md item | Status in option 2 |
|---|---|
| Session G Phase 3 (in-block ordering) | **Already abandoned 2026-05-02** |
| Session G Phase 5 (CFG OrderingState) | **DEFERRED** indefinitely |
| Z9: Privilege escalation through asm | **DEFERRED** (blocked on S1 relax) |
| Z10: ISR-context ban through asm | **DEFERRED** (blocked on S1 relax) |
| Z13: Thread-local access through asm | **DEFERRED** (blocked on S1 relax) |
| C5: Explicit kernel-context model | **NOT NEEDED** (S1 + implicit table covers it) |
| Adding entries to F4-F7 instruction tables | **STOPPED** at current ~120 entries |
| Naked attribute migration | **DEFERRED** indefinitely |
| `@verified_spec` Vale-tier | **DEFERRED** to v2.x+ (orthogonal) |
| Per-instruction precondition database | **NOT EXPANDED** beyond current |
| Universal precondition categories (8 categories) | **Subsumed** by annotation framework |

### D.3 Things that are added in option 2

| New in option 2 | Effort |
|---|---|
| Operand metadata fields (`class`, `bits`, `role`, `mem_size`, etc.) | Phase 1 |
| Annotation syntax (`requires:`, `align:`, `opens_state:`, etc.) | Phase 2 |
| Annotation dispatch to existing safety systems | Phase 2 |
| Implicit-precondition table (~100 entries) | Phase 3 |
| State-machine annotation generalization | Phase 4 |
| Tests + docs | Phases 5-6 |

### D.4 What public-facing documentation says

**Before option 2 (current asm_plan.md framing):**
> "100% language-safe via 13 Z-rules + 8 universal precondition categories + System #30 atomic ordering. Per-instruction preconditions enforced for ~120 instructions."

**After option 2 (this doc's framing):**
> "Rust-level operand-type safety + per-arch register validation + CPU feature gating + 130 intrinsics covering common kernel patterns + operand-level precondition annotations + ~100 well-known UB classics auto-protected. `naked fn` is the asm boundary (MISRA Dir 4.3 compliant). Raw asm contents not validated (matches Rust); use intrinsics or annotations for safety-critical code."

The new framing is honest and accurate. The old framing oversold.

---

## Appendix E: References

### E.1 ZER documents

- `docs/ASM_ZER-LANG.md` — original 2026-04-01 asm design (foundation)
- `docs/asm_plan.md` — D-Alpha-7.5 Phase 2 plan (superseded by this doc for ongoing work)
- `docs/asm_preconditions_research.md` — research artifact on 8 categories
- `CLAUDE.md` — Stage 4 asm safety status section
- `BUGS-FIXED.md` — recent asm-related bug fixes
- `docs/refactor_ir.md` — zercheck_ir helper-layer refactor (separate concern)
- `docs/compiler-internals.md` — patterns for asm safety implementation

### E.2 External references

- Intel Software Developer Manual Vol 2 — instruction-specific UB documentation
- ARM Architecture Reference Manual (ARM ARM) — ARM instruction semantics
- RISC-V Instruction Set Manual (Privileged + Unprivileged) — RISC-V semantics
- Rust Inline Assembly Reference — Rust's asm story
- Rust RFC 2873 — Rust asm! design rationale
- Zig Inline Assembly — Zig's asm syntax
- GCC Inline Assembly — GCC operand constraints reference
- Vale (Microsoft Everest) — verified asm for crypto
- JasMin (Inria) — asm-like language with formal semantics
- CompCert — verified C compiler with Mach/Asm semantics
- LLVM MC — structured machine code library
- Sail — formal ISA specification language
- seL4 — verified microkernel approach to asm

### E.3 Related ZER architectural docs

- `docs/safety_model.md` — 4-model safety architecture
- `docs/compiler-internals.md` — emitter patterns, type system, etc.
- `docs/proof-internals.md` — VST/Coq verification patterns (orthogonal)
- `docs/limitations.md` — known limitations (some asm-related)

### E.4 Key commits

- 2026-04-23 D-Alpha-7.5 Phase 1 (S1 naked-only restriction)
- 2026-04-24 Option C (universal precondition categories)
- 2026-04-25 Session A (structured asm syntax)
- 2026-04-25 Session B (typed operand bindings)
- 2026-04-25 Session D (4 universal structural rules)
- 2026-04-26 Session E1/E2/E3 (Z-rules wiring)
- 2026-04-26 Session F1a (8-category framework skeleton)
- 2026-04-26 Session F2 (x86_64 register table via probe)
- 2026-04-26 Session F7-minimum (register validation wired)
- 2026-04-29 Sub-extension architecture validation (3-arch proof)
- 2026-04-29 F4.1/F4.2 (x86_64 instruction tables)
- 2026-05-02 F5 (aarch64 instruction tables)
- 2026-05-02 F6 (riscv64 instruction tables)
- 2026-05-02 F7-light (LR/SC pairing)
- 2026-05-02 F7-full Step 2 (constraints via VRP)
- 2026-05-02 Session G Phase 1+2 (ordering plumbing)
- 2026-05-02 Session G Phase 3 abandoned
- 2026-05-12 **This document — pivot decision**

---

## End of Document

This is a planning document. Implementation should follow when:

1. v1.0 ships with current asm story (Z-rules + F4-F7 + 130 intrinsics + structured syntax)
2. refactor_ir.md Phase A is complete (zercheck_ir helper layer)
3. Real user code with raw asm has accumulated (informs annotation design)

When implementing, sequence per section 16. Use Appendices A-E as references.
The decisions in sections 19-20 are locked in unless explicitly revisited.

The architectural principle in section 21 ("smart language, generic compiler")
applies beyond asm safety — it's the rule ZER follows everywhere else. The
asm_plan direction was the violation. Option 2 brings asm safety into
alignment with the rest of ZER's architecture.

**The result is the same safety as the original asm_plan fully-done with
~3x less work and zero ongoing maintenance.** That's the win.
