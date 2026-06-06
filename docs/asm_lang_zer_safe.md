# ZER-Asm Safety — Level C: Defer to GCC, Frozen Core

**Status:** Planning document. Decision finalized 2026-05-12 (Level C).
**Architectural pivot to Level D locked in 2026-05-31 (see Section 1.6).**
**Architectural refinement to Option E — Three-Layer / No-Favored-ISA — locked in 2026-06-06 (see Section 1.7). READ SECTION 1.7 FIRST — it is the current locked architecture; Level D's `@intrinsic_def` mechanism survives but its layering and ISA-reference assumptions are refined.**
Execution pending: Level C cleanup first, then Level D mechanism on top, with Option E's no-favored-ISA factoring applied.
**Date:** 2026-05-05 (drafted), 2026-05-10 (audit), 2026-05-11 (Phase A/B split),
2026-05-12 (Level C decision), 2026-05-12 (2-layer crystallization — see section 1.5),
2026-05-31 (Level D pivot — user-extensible intrinsics, see Section 1.6),
**2026-06-06 (Option E — three-layer, no-favored-ISA, program-consequence vocabulary applied to asm, structure/semantics straddle observation — see Section 1.7).**
**Supersedes:** the extension trajectory of `docs/asm_plan.md` (Session G Phase 5,
Z9/Z10/Z13, per-instruction database growth, register-table maintenance,
CPU feature gating tracking).
**Decision:** **Level C** — drop everything ISA-specific from ZER's compile-time
asm validation. Defer to GCC. Keep only the truly frozen safety layer.

> **POST-DISCUSSION UPDATE (2026-05-12, evening):** A long architectural
> conversation explored richer layers on top of Level C (per-operand
> annotations / "Tier 2", IR region wrappers like `@atomic_sequence`,
> auto-guard emission). All explored. All **DEFERRED indefinitely** in
> favor of the simpler **2-layer model: intent intrinsics (primary safe
> path) + raw asm in naked (escape hatch)**. See **section 1.5** for
> the crystallized final design. Level C content below is unchanged and
> remains the execution baseline; 2-layer simply confirms that nothing
> richer is needed on top of it.

**Scope:** Aggressive cleanup of ZER's asm safety infrastructure. ~7,000 lines
deleted. Compile-time safety preserved via hardcoded well-known UB classics
list (~12 frozen instructions) + Z-rules + naked-only restriction + 130
intrinsics. Register validation, instruction validity, CPU feature gating
all delegated to GCC's assembler.

**Effort to complete:** ~1-2 days, 6 commits.
**Maintenance after:** **TRULY zero.** No probe scripts. No per-arch tables.
No per-ISA-extension sync events. ZER works on every architecture GCC
supports — automatically.

**Architectural certainty:** HIGH.
- Matches Rust and Zig design (they don't validate register names either)
- ZER is a transpiler to C, so any C compiler's validation suffices
- GCC dominates ZER's target audience (embedded/firmware/kernel)
- All deleted features are duplicated by GCC's assembler anyway

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
1.4. [Extended Discussion (2026-05-12 evening) — Full Context Dump](#14-extended-discussion-2026-05-12-evening--full-context-dump) ← **Full conversation context (Tier 2, regions, clobber, Rust comparison, trust gaps, explicit-intent pattern, 99% coverage goal, all deferred ideas)**
1.5. [Final Design — Two-Layer Model (crystallization, 2026-05-12 evening)](#15-final-design--two-layer-model-crystallization-2026-05-12-evening) ← **Level C baseline (still correct)**
1.6. [Level D Pivot — User-Extensible Intrinsics with Explicit Contracts (added 2026-05-31)](#16-level-d-pivot--user-extensible-intrinsics-with-explicit-contracts-added-2026-05-31) ← **Mechanism still current; layering and ISA-reference refined by 1.7**
1.7. [Option E Pivot — Three-Layer Architecture, No Favored ISA, Program-Consequence Vocabulary (added 2026-06-06)](#17-option-e-pivot--three-layer-architecture-no-favored-isa-program-consequence-vocabulary-added-2026-06-06) ← **READ FIRST — CURRENT LOCKED ARCHITECTURE FOR ASM**
2. [The Decision in One Page](#2-the-decision-in-one-page)
3. [The Transpiler Nature of ZER — Why This Works](#3-the-transpiler-nature-of-zer--why-this-works)
4. [GCC Coverage in Embedded — Reality Check](#4-gcc-coverage-in-embedded--reality-check)
5. [Why This Pivot — Bug History and Maintenance Reality](#5-why-this-pivot--bug-history-and-maintenance-reality)
6. [The Architectural Principle Being Applied](#6-the-architectural-principle-being-applied)
7. [The Three Levels Considered](#7-the-three-levels-considered)
8. [Journey of the Discussion](#8-journey-of-the-discussion)
9. [What We're Keeping at Level C](#9-what-were-keeping-at-level-c)
10. [What We're Deleting at Level C](#10-what-were-deleting-at-level-c)
11. [The Hardcoded UB Classics List](#11-the-hardcoded-ub-classics-list)
12. [Integration with Existing Safety Systems](#12-integration-with-existing-safety-systems)
13. [Safety Coverage Matrix — Level A vs B vs C](#13-safety-coverage-matrix--level-a-vs-b-vs-c)
14. [Production-Compiler Precedents](#14-production-compiler-precedents)
15. [Effort Breakdown — 6 Commits](#15-effort-breakdown--6-commits)
16. [Implementation Strategy](#16-implementation-strategy)
17. [Testing Strategy](#17-testing-strategy)
18. [Risks and Mitigations](#18-risks-and-mitigations)
19. [Open Questions Decided](#19-open-questions-decided)
20. [Out of Scope (Explicitly NOT Doing)](#20-out-of-scope-explicitly-not-doing)
21. [The "Smart Language vs Smart Compiler" Principle](#21-the-smart-language-vs-smart-compiler-principle)
22. [Fresh-Session Onboarding Checklist](#22-fresh-session-onboarding-checklist)
23. [Appendix A: Concrete File-by-File Deletion List](#appendix-a-concrete-file-by-file-deletion-list)
24. [Appendix B: The Hardcoded UB Classics — Concrete Contents](#appendix-b-the-hardcoded-ub-classics--concrete-contents)
25. [Appendix C: What ZER's Public Safety Claim Becomes](#appendix-c-what-zers-public-safety-claim-becomes)
26. [Appendix D: Mapping from asm_plan to Level C](#appendix-d-mapping-from-asm_plan-to-level-c)
27. [Appendix E: References](#appendix-e-references)

---

## 1. Executive Summary

ZER asm safety reached a decision point in May 2026. The original direction
(`docs/asm_plan.md`, D-Alpha-7.5 Phase 2) committed to building per-instruction
safety knowledge into the compiler: vendored instruction tables, classification
rules, register-name validation, CPU feature gating, and a CFG-aware
OrderingState pass (Session G).

By 2026-05-02, ~225 hours had been invested and ~80 hours remained (Session G
Phase 5 + Z9/Z10/Z13 + ongoing per-ISA-extension maintenance).

**Through a multi-day architectural discussion (2026-05-12), three increasingly
aggressive cleanup levels were considered:**

- **Level A** (initial proposal): Stop extending. Keep register tables, F7-light,
  F7-full Step 2, CPU feature gating. Add annotations + small implicit table.
  ~3-4 months. Some ongoing GCC-coupled maintenance.

- **Level B**: Drop register tables additionally. Keep CPU feature gating + Z-rules.
  Defer register name validation to GCC. ~3-4 months.

- **Level C** (CHOSEN): Drop register tables AND CPU feature gating AND probe
  scripts AND 8-category framework AND instruction tables. Defer everything
  ISA-specific to GCC. Keep only the frozen core (Z-rules, naked-only,
  intrinsics, ~12 hardcoded UB classics). **~1-2 days execution, truly zero
  maintenance forever.**

**Level C was selected because:**

1. ZER is a transpiler to C — the user's C compiler validates ISA-specific
   things anyway. Register tables and CPU feature gating duplicate work GCC
   already does.

2. ZER targets embedded/firmware/kernel developers — GCC is essentially universal
   in this audience. Deferring to GCC means ZER works on every architecture GCC
   supports, automatically, with zero ZER-side effort.

3. ZER is sole-developer-maintained. Every ongoing-maintenance dependency is a
   real cost. Level A/B keep small maintenance burdens that compound over years.
   Level C eliminates them entirely.

4. Production-compiler precedent — Rust and Zig don't validate register names
   either. They defer to LLVM/GCC's assembler. Level C matches their pattern.

5. "Fit all architecture" is actually STRONGER at Level C. Level A commits to
   3 archs (x86_64, aarch64, riscv64). Level C inherits GCC's full architecture
   matrix (15+ archs including AVR, MSP430, Xtensa, PowerPC, MIPS, etc.).

**The result:**

| Property | Original asm_plan (fully done) | Level C (this plan) |
|---|---|---|
| Compile-time safety coverage | ~99% in scope | ~99% in scope (same) |
| ZER-side maintenance burden | Perpetual (~1-2 events/year) | Zero |
| Architecture coverage | 3 archs maintained | Every arch GCC supports |
| Per-instruction database | ~120 entries growing | None |
| Probe scripts | 2 (register + instruction) | None |
| Register tables | 4 vendored files | None |
| CPU feature enum | ~14 entries managed | None — defer to GCC -m flags |
| Effort remaining | ~80 hours + perpetual | ~1-2 days one-time |
| Code surface | ~7,000 lines of asm-safety infrastructure | ~600 lines of asm-safety code |

**Net effect:** same safety, dramatically less code, ZERO ongoing work,
broader architecture support.

---

## 1.4. Extended Discussion (2026-05-12 evening) — Full Context Dump

This section records the multi-hour architectural conversation that took
place after the Level C decision was made, which explored richer designs
on top of Level C and ultimately rejected them in favor of the 2-layer
crystallization in section 1.5. Captured here so fresh sessions don't
re-litigate the same ground or accidentally revive a deferred idea.

### 1.4.1. Where we started — Level C was already decided

Going into the evening discussion, Level C was already committed:
- Delete instruction tables, register tables, probe scripts, CPU feature enum
- Keep 130 intrinsics, Z-rules, naked-only, hardcoded ~12 UB classics
- Defer everything ISA-specific to GCC
- ~1-2 days execution, zero ongoing maintenance

The discussion was about whether richer safety mechanisms should be added
on top of Level C to push beyond what GCC's assembler validates.

### 1.4.2. The "Tier 2 annotations" exploration

**The idea:** add user-facing per-operand annotations on raw asm operand
bindings to declare semantic preconditions. Example:

```zer
asm {
    instructions: "bsr %1, %0"
    outputs: { "rax" = result }
    inputs:  { "rbx" = mask, requires: nonzero }    // ← Tier 2 annotation
    safety: "..."
}
```

The compiler would dispatch `requires: nonzero` to the existing VRP system.
If VRP can prove `mask` is nonzero at the call site, the asm compiles. If
not, compile error. **No theorem prover, no SMT, no formal proof — just
existing dataflow analyses run through asm operand boundaries.**

**Annotation vocabulary explored:**
1. `requires: nonzero` → VRP check
2. `requires: range(a, b)` → VRP check
3. `requires: aligned(N)` / `align: N` → alignment infrastructure
4. `requires: nonnull` → optional-unwrap
5. `requires: kernel_context` → context flag
6. `opens_state: X` → state machine (user-defined name)
7. `closes_state: X` → state machine
8. `requires_after: X` → state machine

Eight kinds, frozen vocabulary. Compiler-side maintenance: zero growth
beyond initial implementation.

**Why explored:** would let users get compile-time precondition checks for
asm patterns not in the intrinsic catalog (AMX, SVE, future ISA extensions,
custom user operations) without writing a full intrinsic.

**Why DEFERRED:**

1. **Redundancy with intrinsics.** For the common case (~95% of asm),
   intrinsics already encode preconditions. Tier 2 only helps the residual
   ~5% of raw asm in niche cases.
2. **Two-ways-to-express problem.** Same precondition declared two ways:
   either as an intrinsic body or as a Tier 2 annotation. Violates "one
   obvious way to do it."
3. **User must know to add the annotation.** Same meta-trust problem as
   intrinsic selection — if user is "dumb" and doesn't add the annotation,
   no compile-time benefit. Tier 2 doesn't help dumb users.
4. **Caller-side guards are equivalent.** A regular `if (mask == 0) trap;`
   before calling a `naked` function gets elided by VRP if provable —
   same result, no new language surface.
5. **~3-4 weeks of implementation work** for marginal benefit on niche
   cases that ~1-2% of users hit.

**Verdict:** annotations defer to post-v1.0 if real demand emerges from
production use.

### 1.4.3. The "IR region wrappers for asm" exploration

**The idea:** structured block wrappers for multi-block asm patterns:

```zer
@atomic_sequence {
    asm { instructions: "lr.w t0, (a0)" /* ... */ }
    asm { instructions: "addi t0, t0, 1" /* ... */ }
    asm { instructions: "sc.w t1, t0, (a0)" /* ... */ }
}
```

The region carries shared invariants (LR/SC pairing, memory ordering, etc.)
that the compiler tracks across all blocks in the region body. Same pattern
as existing `@critical { }`, `defer { }`, `@once { }`.

**Region keywords proposed:**
- `@atomic_sequence { }` — LR/SC, MONITOR/MWAIT pairings
- `@cache_sync_region { }` — CLWB+SFENCE persistence sequences
- `@dma_buffer_op(buf, len) { }` — DMA setup/transfer/complete with auto-emit cache flush + invalidate
- `@transaction { }` — future transactional memory support

**Why explored:** regions are conceptually consistent with ZER's existing
safety-scope patterns (naked, @critical, defer, @once, shared struct, comptime),
and they cleanly handle multi-block coordination that flat annotations
struggle with.

**Why DEFERRED:**

1. **Composite intrinsics handle the same patterns cleaner.** `@atomic_cas(...)`
   does what `@atomic_sequence { lr.w + ... + sc.w }` does — and the intrinsic
   form is simpler from the user's perspective. The region exposes implementation
   detail (manual LR/SC) that the intrinsic abstracts away.
2. **Adds language surface without unlocking new safety.** Anything a region
   would track, a well-designed composite intrinsic tracks more cleanly.
3. **The cases regions would handle (LR/SC pairing) are already covered.**
   F7-light state machine + composite intrinsics together cover the existing
   real cases.
4. **"User wraps niche asm in a region just to get safety" is a worse UX
   than "user calls an intrinsic that has safety baked in."**
5. **~2-3 weeks of implementation work** for marginal benefit.

**Verdict:** asm-specific regions deferred. General-purpose regions like
`@critical`, `defer`, `@once` stay — they're language features, not asm-safety
features.

### 1.4.4. The "auto-guard emission" exploration

**The idea:** for Tier 2 annotations where VRP can't prove the precondition
at compile time, emit a software runtime check (`if (!cond) _zer_trap(...)`)
before the asm. This would eliminate hardware-dependence: wrong precondition
always produces a deterministic ZER-controlled trap, regardless of CPU.

Example: user declares `requires: aligned(16)` on a MOVAPS operand. If VRP
can't prove the address is 16-byte aligned, ZER emits:

```c
if ((uintptr_t)addr & 15) {
    _zer_trap("asm MOVAPS operand alignment precondition failed");
}
__asm__ __volatile__ ("movaps (%0), %%xmm0" : : "r"(addr));
```

**Why explored:** the hardware trap fallback is hardware-dependent:
- Hosted Linux/BSD/macOS user mode: ~95% reliable (SIGSEGV, SIGFPE, SIGILL)
- Bare-metal Cortex-M0 / no-MMU: hardware doesn't trap on misalignment
- Boot code pre-exception-handler: undefined behavior (no trap path)

Auto-guard would close this gap by emitting software checks ZER controls.

**Why DEFERRED:**

1. **Requires Tier 2 to exist.** Auto-guard is meaningful only if users can
   declare preconditions on raw asm. If Tier 2 is deferred, auto-guard has
   nothing to emit guards for.
2. **Intrinsics already emit their own guards.** `@bit_scan_reverse(mask)` can
   check `mask != 0` internally before emitting `bsr`. Layer 1 already does
   this — auto-guard would be parallel infrastructure for the rare Layer 2
   case.
3. **Performance cost on tight loops.** Auto-emitted runtime checks ~2-4
   instructions per asm block. Acceptable for safety-critical builds,
   noise in hot paths. Without opt-in, defeats the point of inline asm.

**Verdict:** auto-guard deferred along with Tier 2.

### 1.4.5. The clobber gap discussion

A long subthread explored whether ZER can detect missing clobber declarations
in raw asm. The conclusion: **no production language detects this without
either per-instruction database (maintenance hell) or formal proof per
asm block (Vale-tier manual work).**

**The mechanism of the bug:**

```
Wrong precondition (e.g., BSR on zero):
  → asm instruction TRIES to execute
  → CPU exception (#DE/#GP/#AC)
  → SIGFPE/SIGSEGV/SIGBUS
  → LOUD failure, debuggable

Wrong clobber (e.g., user forgot to declare rdx clobbered by DIV):
  → asm instruction DOES execute successfully
  → register rdx silently contaminated with leftover from DIV
  → next ~50 instructions run with garbage in rdx
  → eventually flows into a load address → SIGSEGV
  → BUT debugger points to wrong location, far from cause
  → SILENT failure, hard to debug
```

**Why this is "the effect system gap":**

Languages track *some* effects (allocation, mutation, lifetimes, types). They
don't track *all* effects. Register state is an effect that lives outside
every mainstream language's type system. The contract between user-asm and
GCC is **invisible to the language**.

**Category:** invisible-contract effect bug. The clobber gap is the
asm-shaped instance of a general phenomenon. Other instances in safe code:

| Category instance | Invisible contract | Wrong tool = silent bug |
|---|---|---|
| Wrong clobber list (asm) | "rdx preserved" | Corrupted register downstream |
| Wrong atomic memory ordering (safe Rust) | Happens-before edges | Stale load on weak-memory hardware |
| Wrong Mutex lock order (multi-lock) | Global lock ordering | Deadlock at runtime |
| Wrong drop-order reliance | Field declaration order | Behavioral shift on refactor |
| Wrong Cell vs RefCell vs Mutex choice | "Do I need runtime borrow checks?" | Panic at runtime or perf cost |
| Wrong cinclude C function signature | C ABI | Silent ABI corruption |
| Wrong MMIO range declaration | Hardware address layout | OS faults or wrong device |

In every case: compiler accepts syntactically-correct code; user picked
wrong tool; failure manifests downstream as silent consequence. **Same
shape across domains.**

### 1.4.6. The Rust comparison

A subthread compared Rust's safety story to ZER's. Key findings:

**Both pass the same test at the asm boundary.** Rust's `unsafe { asm!() }`
has zero clobber validation, zero precondition checks beyond operand types.
Same gap as ZER. Same gap as C. Rust's safety claim is **NOT** that asm is
safe — it's that the **safe subset** is checked and **unsafe is bounded**.

**Both implement the "composition" safety model.** Most code is in the safe
subset (genuinely checked). Escape hatches are explicit, isolated, audited.
The boundary between safe and unsafe is the type system's (or in ZER's case,
the 29 tracking systems') job.

**Where ZER catches more by default (the "smart compiler + dumb user" model):**

| Bug class | Rust mechanism | ZER mechanism |
|---|---|---|
| Reference cycle leak | User picks `Weak` for back-references | Handle is index, cycles impossible by construction |
| Lock ordering deadlock | User maintains global lock order manually | Same-statement multi-shared-type access = compile error |
| Uninitialized memory | User picks `MaybeUninit::assume_init` correctly | Everything auto-zeroed at declaration |
| Lifetime annotations | User writes `<'a, 'b>` on every fn | Escape analysis walks field/index chains automatically |
| Atomic ordering | User picks `Acquire`/`Release`/`SeqCst` correctly | Single SeqCst choice — no wrong choice possible |
| Stack overflow | User hopes for the best | `--stack-limit N` + recursion detection via DFS |
| Lock primitive choice | User picks `Mutex` vs `RwLock` vs `Cell` vs `RefCell` | `shared struct` auto-locks; `shared(rw) struct` auto-rwlocks |
| Spawn data race | Auto-traits `Send`/`Sync` (user may `unsafe impl` wrong) | Spawn-target body scanned at compile time |
| Channel size choice | User picks buffer size, wrong = OOM or lockstep | `Ring(T, N)` always bounded; unbounded not available |
| Drop order reliance | Field declaration order matters silently | `defer` makes order explicit; no implicit drop ordering |
| Pin for self-referential | User knows when to pin futures | Async compiles to flat state machine; no Pin needed |
| Bounds proof | User picks `.get()` vs `[]` | VRP proves indices; auto-guard only if unprovable |
| Sign/width conversion | User picks `as` vs `try_into()` | Implicit narrowing rejected; `@truncate`/`@saturate` explicit |

**The architectural insight:**

```
Rust:  Smart user × Medium-smart compiler = safe
       (user encodes invariants in types; borrow checker enforces type discipline)

ZER:   Dumb user × Very-smart compiler = safe
       (compiler infers invariants from dataflow; user writes plain code)
```

Both reach roughly equivalent safety. Rust pushes complexity into the user's
brain (lifetimes, Send/Sync, Pin, MaybeUninit, Cell-family choice, atomic
ordering). ZER pushes complexity into the compiler (29 tracking systems doing
per-function dataflow inference).

**Where Rust catches more (honest):**

The narrow real gap is **API expressiveness**, not safety coverage:
- Rust's signature `fn foo<'a>(x: &'a T) -> &'a U` declares "result tied to
  arg lifetime" at the type level. ZER has no such surface syntax.
- Rust's trait bounds (`T: Hash`) constrain generic APIs. ZER's
  monomorphization defers to per-stamp type-checking.
- Rust's `&mut` aliasing rule prevents some patterns expressible only with
  lifetimes.

**But:** these are API design gaps, not safety gaps. The underlying bugs Rust
would catch via these features, ZER catches via dataflow analysis anyway. The
user-visible difference is "you can't write a library API that statically
refuses aliasing in ZER" — but if your code has the bug, ZER still catches it.

**Net:** ZER catches every bug class Rust catches in the safe subset. The
mechanisms differ. The user experience differs (less expertise required for
ZER). The audit surface and escape-hatch model are equivalent.

### 1.4.7. The trust gap pattern (universal across ZER)

A subthread identified that the clobber gap (user declares, compiler trusts,
wrong declaration causes silent issue) is a **universal pattern** across ZER,
not unique to asm. Every safety feature that bridges to external reality
has the same shape:

| Feature | User declares | Compiler trusts | If wrong, what happens |
|---|---|---|---|
| `mmio 0x4000..0x4FFF;` | Hardware address layout | Address valid | OS fault at runtime |
| `volatile *u32` | Hardware requires unoptimized access | Optimizer leaves accesses | Optimizer removes essential reads silently |
| `shared struct` | Data accessed across threads | Auto-lock generated | Silent race on bypass |
| `threadlocal u32` | Per-thread storage | Each thread has own copy | Lost updates if actually shared |
| `move struct` | Resource has unique ownership | Tracking enforced | Over-restrictive (false positives) |
| `keep` parameter | Pointer may be stored globally | Storage allowed | Escape analysis fires false-positive |
| `@ptrcast(*T, opaque)` | Memory layout is T | Type assertion accepted | Type confusion silent for cross-language |
| `cinclude` C signatures | Function signature matches actual C | Type-checked against decl | ABI silent corruption if mismatch |
| Asm clobber list | Registers modified by asm | Trusted verbatim | Silent register corruption |
| Asm precondition (if Tier 2 existed) | Operand semantic | Dispatched to dataflow | Wrong type of check applied |

**Pattern:** ZER (and every safe language) trusts user declarations about
external reality. The compiler enforces ZER-side consequences of those
declarations. External reality is the user's responsibility.

**Mitigation strategy across ZER (universal):**
1. Multiple overlapping annotations (defense in depth)
2. Runtime/OS/hardware traps for hardware-related cases
3. Audit-ability (all annotations greppable)
4. Documentation conventions (`safety:` strings, header comments)
5. For safety-critical: opt-in formal verification (`@verified_spec` v2.x+)

**No additional core complexity is going to fix this universally** because
fixing it requires either per-item databases (maintenance hell) or formal
proofs (Vale-tier manual work). ZER's design accepts this universal trust
gap and uses the same mitigation strategy across all instances.

### 1.4.8. The hardware-dependent trap reality check

A subthread examined whether the "CPU trap catches wrong precondition" claim
is actually reliable. Conclusion: **conditional on context, not universal.**

```
Hardware trap fallback reliability:

  Linux/BSD/macOS user mode (hosted):       ~95% reliable (loud crash)
  Linux kernel module:                       ~90% reliable
  Modern RTOS (Zephyr, FreeRTOS):           ~85% reliable
  Bare-metal Cortex-M with handlers:        ~70% reliable
  Bare-metal boot code (pre-handler):        ~30% reliable
  Cortex-M0 / AVR / no-MMU MCU:              ~40% reliable
  Inside an exception handler:               unreliable (double-fault risk)
```

Specific cases where hardware does NOT trap:
- Misaligned scalar access on x86 (EFLAGS.AC off by default in user mode)
- Misaligned access on ARM Cortex-A (SCTLR_EL1.A off by default in user mode)
- Misaligned access on Cortex-M0/M0+ (no alignment check exists)
- RDTSC in user mode (works without trap even though it reads privileged time)
- Wrong arithmetic instruction (computes wrong value, no trap)
- Undeclared register clobbers (silent register corruption)
- Wrong FP rounding mode (slightly wrong float)

**Implication:** the public safety claim about wrong-asm-traps-at-runtime
needs to be honest about hardware/OS dependence. ZER's actual guarantee is
at the operand boundary (Z-rules), not for asm body content. Hardware trap
is "usually clean" property of the runtime environment, not a ZER guarantee.

This is the same scope every safe language has for inline asm. Honest.

### 1.4.9. The three tiers of contracts (clarification)

A subthread distinguished three tiers of "contract" mechanisms to clarify
what ZER does and doesn't claim:

**Tier 1: Documentation comments (no enforcement)**
- `// SAFETY: caller must ensure mask != 0` in Rust
- `safety: "..."` string in ZER's asm syntax
- Convention only, zero compiler check

**Tier 2: Annotations checked by existing static analyses (no proofs)**
- User declares a precondition; compiler dispatches to existing dataflow
  analysis (VRP for ranges, alignment infrastructure, optional unwrapping)
- Compile error if analysis can't satisfy the precondition
- **Deterministic, no SMT solver, no theorem prover, no manual proof work**
- ZER already does this for hardcoded UB classics (~12 frozen entries).
- Tier 2 user-facing extension was explored and deferred (see 1.4.2).

**Tier 3: Annotations checked by theorem provers / SMT solvers (formal proofs)**
- SPARK Ada's `Pre`/`Post`/`Global` clauses
- Frama-C ACSL's `\valid`, `\forall`, etc.
- Vale's Coq proof obligations
- Compiler generates proof obligations sent to SMT/theorem prover
- May succeed, fail, or time out; user may need to write proofs manually
- **This is what "formal verification" means in industry**

**ZER's position:**
- Tier 1 always: `safety:` strings on every asm block (≥ 30 chars via S4)
- Tier 2 partial: hardcoded UB classics (the ~12 frozen entries)
- Tier 2 user-facing: explored, **deferred** (see 1.4.2)
- Tier 3: out of core scope; `@verified_spec` opt-in for v2.x+, niche audience

**Critical clarification:** "contracts" in the SPARK/ACSL/Vale sense ARE
proof systems (Tier 3). ZER's hardcoded UB classics dispatch and any future
Tier 2 are NOT proof systems — they're existing dataflow analyses dispatched
through annotation surface. **No mathematical proof is involved.** No SMT
solver is in the toolchain. No Coq is required.

### 1.4.10. The "smart language vs smart compiler" principle (final)

```
                  SAFE DEFAULT FOR EVERYDAY USER
                            ↑
                            |
ZER's design ─────────► Compiler is smart (dataflow infers properties)
                            |
                            |
                            |
                            ↓
                  USER MUST KNOW INVARIANTS
                            ↑
                            |
Rust's design ─────► User encodes invariants in types
                            |
                            ↓
                  REQUIRES SOPHISTICATION
```

Both reach equivalent safety. The dial that differs is **expected user
sophistication.**

ZER's choice: assume the user is a mid-level C programmer who has never
heard of borrow checker. Compiler does the work. Safety guaranteed without
expertise.

Rust's choice: assume the user is a senior systems programmer who reads
the standard library. User provides expertise. Compiler enforces what user
encoded.

**For ZER's audience (embedded/firmware/kernel developers transitioning
from C), the "smart compiler + dumb user" model is the right design.**

### 1.4.11. Industry precedent — final confirmation

The 2-layer model (intrinsics primary + raw asm escape) is the **converged
industry pattern** for production kernel/RTOS development:

- **Hubris RTOS** (Oxide Computer): intrinsics for everything; raw `asm!()`
  in tiny `kernel/boot` crate only. 99% of code never touches asm.
- **Linux kernel**: `readl`/`writel`/`atomic_*`/`barrier` intrinsics
  everywhere; `.S` files only in `arch/<X>/boot/`, `entry/`, `head/`.
- **Zephyr RTOS**: intrinsics in headers; asm in `core/locore.S` and
  `arch/<X>/core/aarch32/cortex_m/exc_exit.S` etc.
- **FreeRTOS**: intrinsics + `portasm.S` per port.
- **STM32 / CMSIS**: `__DMB()`, `__WFI()`, `__NOP()` intrinsic macros; raw
  asm rare and confined to startup files.
- **Rust core::arch**: ~5000 intrinsics covering AVX/SSE/SVE/NEON; `asm!()`
  is the explicit unsafe escape.
- **GCC's own kernel headers** (`<x86intrin.h>`, `<arm_neon.h>`): intrinsics
  as the documented path; `__asm__` as the escape.

**Every production kernel/RTOS converged on this model. ZER's 2-layer
crystallization joins the converged norm.**

### 1.4.12. What "fully safe" means after 2-layer

The honest safety scope claim post-2-layer:

```
ZER guarantees:
  ✓ Memory safety through asm operands (Z-rules)
  ✓ Type safety on operand bindings (existing type system)
  ✓ Concurrency safety through operands (Z6)
  ✓ MMIO range validation (Z7 + mmio decl)
  ✓ Provenance tracking (Z4, Z5)
  ✓ Qualifier preservation (Z8)
  ✓ Move/transfer semantics (Z2)
  ✓ Hardcoded UB classics (~12 frozen, via AST mnemonic detection)
  ✓ LR/SC pairing (F7-light hardcoded state machine)
  ✓ Naked-fn isolation (MISRA Dir 4.3)
  ✓ Intent intrinsic preconditions (via existing dataflow at intrinsic body)
  ✓ Cross-architecture support (every arch GCC supports, ~15+ archs)

ZER does NOT guarantee:
  ✗ Algorithm correctness (Vale-tier territory, opt-in v2.x+)
  ✗ Microarchitectural attacks (hardware vendor problem)
  ✗ Clobber list completeness for raw asm (universal asm limit)
  ✗ cinclude signature correctness (C interop boundary)
  ✗ User-declared facts about external hardware/OS (trust boundary)
  ✗ Hardware-independence of trap behavior on raw asm (context-dependent)

Out of scope (acceptable, matches every safe language):
  ✗ Wrong precondition on niche raw asm instruction (CPU trap usually catches;
     ZER auto-guard would have helped but Tier 2 was deferred)
```

**This is the honest, audit-able safety claim.** No marketing inflation.
Matches Rust, Zig, Linux kernel, Hubris, every production safe language at
this tier.

### 1.4.13. Maintenance picture — final crystallization

```
ZER-side ongoing maintenance after one-time Level C cleanup:

  Intrinsic catalog (~130 today):
    Growth rate: ~1-2 entries / year (real demand only)
    Per-entry cost: ~30-50 lines if using GCC builtin (most cases)
                    ~100-200 lines if needs per-arch inline asm wrapper
    Total catalog code: ~3,000-5,000 lines, bounded forever

  Hardcoded UB classics list (~12 frozen entries):
    Growth rate: ~1 entry / decade (genuinely new well-known UB)
    Per-entry cost: ~3-5 lines in checker.c

  Z-rules infrastructure (Z1-Z8, Z11, Z12):
    Frozen. Zero growth. One-time setup.

  Per-arch register tables:        DELETED. Zero maintenance.
  Per-arch instruction tables:     DELETED. Zero maintenance.
  Probe scripts:                   DELETED. Zero maintenance.
  CPU feature enum:                DELETED. Zero maintenance.
  8-category framework:            DELETED. Zero maintenance.
  Session G ordering plumbing:     DELETED. Zero maintenance.

  Sub-extension support (AVX-512, AMX, SVE, SME, etc.):
    Inherited from GCC automatically. Zero ZER work.

  New architecture support (any arch GCC adds):
    Inherited from GCC automatically. Zero ZER work.

  Tier 2 annotation framework:     NOT BUILT. Deferred.
  IR region wrappers for asm:      NOT BUILT. Deferred.
  Auto-guard emission for Tier 2:  NOT BUILT. Deferred.

TOTAL ZER-side ongoing burden: ~1-2 hours per year, bounded forever.
NOT hellish. NOT continuous. NOT compounding.
```

### 1.4.14. The execution decision

The 6-commit cleanup plan in **section 16** remains correct and proceeds
as originally specified. The 2-layer crystallization in **section 1.5** is
a clarification of what Level C means in practice, NOT a plan revision.

**Optional follow-on (post-Level-C):** if real demand emerges for niche
intrinsics not in the current 130-entry catalog (AMX, SVE specifics,
cache-management wrappers), they can be added incrementally one at a time,
each as a small ~30-50 line addition. Not blocking, not urgent, demand-driven.

**Never blocking:** Tier 2 annotations, IR regions for asm, auto-guard
emission, per-instruction database, additional structural complexity. All
explored, all deferred indefinitely.

### 1.4.15. The "explicit intent via separate intrinsics" pattern

A late-conversation insight crystallized the architectural pattern ZER uses
to handle **sibling operations with identical input types but different
semantic intent.** This is the same shape as the clobber gap (invisible
contract, user must know intent) but the resolution is structural rather
than checked.

**The pattern: don't ship one silent operator; ship distinct intrinsics
with explicit names.**

| Domain | C's silent default (BUG-PRODUCING) | ZER's explicit-intent split |
|---|---|---|
| Width conversion | `(u32)big_u64` silently truncates | `@truncate(u32, val)` — explicit |
| Range conversion | `(i8)big_int` silently wraps | `@saturate(i8, val)` — explicit clamp |
| Bit reinterpretation | `*(u32*)&float_val` silently aliases | `@bitcast(u32, val)` — explicit |
| Integer → pointer | `(int*)addr` silently makes a pointer | `@inttoptr(*u32, addr)` — explicit, MMIO-validated |
| Pointer → integer | `(uintptr_t)ptr` silently | `@ptrtoint(ptr)` — explicit |
| Pointer type cast | `(B*)a_ptr` silent reinterpretation | `@ptrcast(*B, ptr)` — explicit, provenance-checked |
| Container choice | `malloc` for everything | `Pool / Slab / Ring / Arena` — distinct names |
| Atomic ordering | `atomic_load(ptr, ordering)` | **SeqCst only** — family collapsed |
| Lock type | User picks `Mutex` vs `RwLock` vs `Cell` | `shared struct` / `shared(rw) struct` — compiler picks |
| Bit search | One `bsr` mnemonic, UB on zero | `@bit_scan_reverse(mask)` — internal nonzero check |
| Cache ops (future, if added) | One asm `clflushopt` or `clwb` string | `@cache_flushopt` vs `@cache_writeback` — distinct names |
| MSR read/write | Same opcode prefix, different semantic | `@cpu_read_msr` vs `@cpu_write_msr` — distinct sigs |

**The principle:**

```
C model:    ONE operator (cast) picks "truncate" silently
            → user didn't know they were truncating
            → silent value corruption

ZER model:  THREE intrinsics (@truncate/@saturate/@bitcast)
            → user MUST type one
            → typing it IS the intent declaration
            → no silent path exists
            → forgetting = compile error, not silent bug
```

**Why this is a safety mechanism, not just ergonomics:**

The act of typing the intrinsic name **is** the intent declaration. There's
no fallback the compiler picks. The user can't "forget to think about overflow
semantics" because forgetting means the code doesn't compile. The language
literally cannot pick wrong for the user — the user picks, and picking is
logged in the source code as a permanent audit trail.

**Two distinct resolution strategies for sibling families:**

1. **Collapse the family** when there's a single "right default":
   - Atomic ordering → SeqCst only (5 orderings → 1)
   - Lock type → `shared struct` auto-picks (4+ primitives → 1 declaration)
   - Memory layout → auto-zeroing (no `MaybeUninit` family needed)

2. **Split into named siblings** when operations are genuinely different:
   - `@truncate` vs `@saturate` vs `@bitcast` — all valid, different intent
   - `Pool` vs `Slab` vs `Ring` vs `Arena` — different allocation semantics
   - `@cache_flushopt` vs `@cache_writeback` — different cache semantics

**The dumb-user safety floor:**

> User cannot silently pick wrong because the language has no silent path.
> They must type the intent. Once typed, the consequence follows from the
> declared intent.

**This pattern has a name in language design:**
- Zig: *"no hidden control flow, no hidden allocations, no hidden casts"*
- Rust: *"explicit is better than implicit"* (no implicit numeric conversion)
- Ada/SPARK: *"strong typing with explicit conversions"*
- Python's Zen: *"explicit is better than implicit"*
- ZER (implicit via design): **"every conversion is a named intrinsic"**

All converge on the same insight: **silent operators are bug-class generators.**
Named operators with distinct intent are safer because the picker is
mechanically forced to declare which operation they want.

**Relationship to the clobber gap (1.4.5):**

| Gap shape | Clobber (raw asm) | Sibling intrinsic |
|---|---|---|
| Invisible contract | "rdx preserved" | "this is truncate semantics" |
| User must know intent | Yes | Yes |
| Compiler can verify? | No (needs per-instruction DB) | Often yes (type system / VRP) |
| Resolution | None automatic (intrinsics absorb 95%) | Explicit naming forces intent declaration |
| Silent failure? | Yes (register corruption downstream) | No (named operator = audit trail) |

**Key insight:** the intrinsic-sibling case is structurally better than the
clobber case because the explicit name converts the bug from "silent semantic
corruption" into "code review can see which operation was picked." Even when
the compiler can't verify intent, the source code records it.

**Status:** this pattern is already practiced throughout ZER's existing 130
intrinsics + container builtins + conversion intrinsics. Not a new design
addition — a documentation crystallization of the principle already in use.
Future intrinsic additions should follow the same rule: **if two operations
are semantically different, give them different names. Never ship one
operator that silently picks.**

### 1.4.16. The 99% intrinsic coverage goal (zero `.S` files needed)

**Goal stated by the user 2026-05-12 evening:** intrinsics should be expansive
enough that ZER firmware/kernel projects need **zero standalone `.S` assembly
files**. GCC handles all per-arch codegen via intrinsic bodies; raw asm in
`naked` functions covers the irreducible ~1% (boot stubs, vector tables,
hand-tuned hot loops).

**This goal is achievable and matches the converged industry pattern.**
Hubris RTOS (Oxide Computer, Rust-based, in production hardware) hit
exactly this target: 99% of their kernel is intrinsic-based; ~200 LOC of
raw asm in a tiny `boot` crate; no standalone `.S` files in their codebase.

This subsection maps the path from ZER's current ~130-intrinsic catalog
(covering ~95% of typical kernel/firmware needs) to the ~150-170-intrinsic
target (covering ~99%).

#### Current coverage map (what .S files traditionally cover vs ZER today)

| Traditional `.S` use case | ZER current coverage | Status |
|---|---|---|
| Atomic ops, CAS, fetch-add | `@atomic_*` (15 intrinsics, D-Alpha-1) | ✓ Covered |
| Memory barriers | `@barrier_*`, `@barrier_acq_rel` | ✓ Covered |
| Bit ops (popcount, clz, ctz, ffs, parity) | `@popcount`, `@ctz`, `@clz`, `@ffs`, `@parity` (D-Alpha-2) | ✓ Covered |
| Byte swap | `@bswap16/32/64` (D-Alpha-2) | ✓ Covered |
| MSR / CR / XCR0 access | `@cpu_read_msr`, `@cpu_write_cr*`, `@cpu_*_xcr0` (D-Alpha-9) | ✓ Covered |
| Port I/O (in/out) | `@port_in8/16/32`, `@port_out8/16/32` (D-Alpha-13) | ✓ Covered |
| Interrupt enable/disable/wait | `@cpu_disable_int`, `@cpu_enable_int`, `@cpu_wait_int` (D-Alpha-3) | ✓ Covered |
| Interrupt state save/restore | `@cpu_save_int_state`, `@cpu_restore_int_state` (D-Alpha-3) | ✓ Covered |
| Cache management (CLFLUSHOPT/CLWB/MOVNTI) | `@cache_flushopt`, `@cache_writeback`, `@nt_store` (D-Alpha-13) | ✓ Covered |
| CPU feature detection | `@cpu_cpuid`, `@cpu_cpuid_ecx`, `@cpu_vendor_id`, `@cpu_feature_bits` (D-Alpha-10/14) | ✓ Covered |
| Context save/restore (callee-saved) | `@cpu_save_context`, `@cpu_restore_context`, `@cpu_*_fpu` (D-Alpha-4) | ✓ Covered |
| Extended state save/restore (XSAVE) | `@cpu_xsave`, `@cpu_xrstor`, `@cpu_fxsave`, `@cpu_fxrstor` (D-Alpha-13/14) | ✓ Covered |
| Privileged mode transitions | `@cpu_syscall`, `@cpu_sysret`, `@cpu_iret` (D-Alpha-12) | ✓ Covered |
| Hypercalls / firmware calls | `@cpu_hypercall`, `@cpu_sbi_call`, `@cpu_smc_call` (D-Alpha-12/13) | ✓ Covered |
| Debug registers (DR0-DR7) | `@cpu_read_dr`, `@cpu_write_dr` (D-Alpha-13) | ✓ Covered |
| Performance counters | `@cpu_read_pmc` (D-Alpha-13) | ✓ Covered |
| Stack / thread pointer / flags read | `@cpu_read_sp`, `@cpu_read_tp`, `@cpu_read_flags` (D-Alpha-10) | ✓ Covered |
| Power management (sleep, idle, monitor/mwait) | `@cpu_deep_sleep`, `@cpu_idle_hint`, `@cpu_mwait`, `@cpu_umwait` (D-Alpha-11/14) | ✓ Covered |
| Privilege query | `@cpu_get_priv_level` (D-Alpha-12) | ✓ Covered |
| Control-flow integrity | `@cpu_endbr` (CET-IBT) (D-Alpha-14) | ✓ Covered |
| FS/GS segment bases (FSGSBASE) | `@cpu_read_fsbase`, `@cpu_write_fsbase`, GS variants (D-Alpha-13) | ✓ Covered |
| Cache disable/enable (privileged) | `@cpu_cache_disable`, `@cpu_cache_enable` (D-Alpha-14) | ✓ Covered |
| End-of-interrupt | `@cpu_eoi` (D-Alpha-14) | ✓ Covered |
| Page fault address | `@cpu_read_cr2` (D-Alpha-14) | ✓ Covered |
| Legacy FPU init | `@cpu_fpu_init` (D-Alpha-14) | ✓ Covered |
| **Subtotal current intrinsics** | **~130 (D-Alpha-1 through D-Alpha-14)** | **~95% of typical use** |

The 130-intrinsic catalog was deliberately built for kernel/firmware needs
across the 14 D-Alpha batches. Coverage today already exceeds what most
embedded RTOS projects need.

#### The gap — ~15-20 intrinsics to push coverage from ~95% to ~99%

These are remaining `.S`-file use cases that the current catalog doesn't
fully address. Adding them is **optional follow-on work, not blocking
Level C execution**.

| Gap area | Proposed intrinsic | Implementation strategy |
|---|---|---|
| **Boot stack setup** (before C runtime) | `@cpu_set_stack(addr)` | Per-arch inline asm wrapper in intrinsic body |
| **Vector table install** | `@cpu_set_vector_table(addr)` | x86: LIDT; ARM: VTOR write; RISC-V: stvec |
| **Unconditional jump (no return)** | `@cpu_jump_to(addr)` | Per-arch JMP/B/JR; marked `__attribute__((noreturn))` |
| **Full context save** (incl. SP/PC/PSR) | `@cpu_save_full_context(*u8 buf)` | Beyond callee-saved subset (D-Alpha-4 has callee-saved only) |
| **Full context restore** | `@cpu_restore_full_context(*u8 buf)` | Inverse of above |
| **IRQ vector entry prologue** | `@irq_entry()` | Stack alignment + register save per arch ABI |
| **IRQ vector exit epilogue** | `@irq_exit()` | Restore + IRET / RFI / SRET |
| **Atomic max/min via CAS loop** | `@atomic_fetch_max`, `@atomic_fetch_min` | Composite intrinsic (CAS loop) |
| **Aligned vector load/store** | `@vec_load_aligned_128/256/512`, store versions | GCC vector builtins |
| **Saturating arithmetic** | `@sat_add`, `@sat_sub`, `@sat_mul` | GCC `__builtin_*_overflow` + clamp where available |
| **Volatile memcpy/memset** (MMIO) | `@memcpy_volatile`, `@memset_volatile` | Cannot be optimized away — for memory-mapped regions |
| **Cache flush range** | `@cache_flush_range(*u8 addr, usize len)` | Loop wrapper over CLFLUSHOPT / DC CVAC |
| **Cache invalidate range** | `@cache_invalidate_range(*u8 addr, usize len)` | Loop wrapper over CLFLUSH+invalidate / DC IVAC |
| **DMA buffer prep/complete** | `@dma_buffer_prep`, `@dma_buffer_complete` | Cache flush + memory barrier composite |
| **TLB invalidation** | `@tlb_flush_all`, `@tlb_flush_page(addr)` | INVLPG / TLBI / SFENCE.VMA |
| **CPU pause / spin-loop hint** | `@cpu_pause()` | PAUSE / YIELD (spin-loop optimization hint) |
| **Random number** (RDRAND / RDSEED) | `@cpu_rdrand`, `@cpu_rdseed` | x86 hardware RNG with retry loop |
| **Crypto primitives** (if AES-NI present) | `@aes_enc_round`, `@aes_dec_round` (optional) | AES-NI intrinsics already in GCC |
| **SHA primitives** (if SHA-NI present) | `@sha1_msg1/2`, `@sha256_msg1/2` (optional) | SHA-NI intrinsics already in GCC |
| **Carryless multiply** (PCLMULQDQ) | `@clmul_64x64` (optional) | For CRC, GCM mode etc. |

**~15-20 additions** depending on how many crypto helpers are bundled.
After this phase: **~150-170 intrinsics, ~99% coverage of typical kernel/
firmware needs**.

#### The irreducible minimum (~1% raw asm in naked fn, cannot be intrinsics)

Even with maximum intrinsic coverage, three narrow categories will always
need raw asm in `naked` functions. These are the same categories Hubris,
Tock, seL4, Linux, and every production kernel maintain as small,
hand-audited asm:

**Category 1: Boot entry before C runtime exists** (~50-100 LOC per project)
```zer
naked void _start() {
    asm {
        instructions: "
            ldr sp, =_stack_top    // set stack ptr
            ldr r0, =_bss_start    // BSS zeroing
            ldr r1, =_bss_end
        1:  cmp r0, r1
            beq 2f
            str r2, [r0], #4
            b 1b
        2:  bl main                 // jump to C runtime
            b .                     // hang if main returns
        "
        safety: "Boot entry: set SP, zero BSS, call main. Pre-runtime."
    }
}
```
**Why irreducible:** before the boot stub runs, the C runtime doesn't
exist. Cannot call a function until SP is set. Cannot zero BSS until
SP is set. The first ~10 instructions of every embedded system are
fundamentally pre-language. Same for kernel boot, hypervisor entry, etc.

**Category 2: Vendor-specific instructions before ZER catalog catches up**
(~0-50 LOC per project, temporary)
```zer
naked void custom_vendor_op() {
    asm {
        instructions: "tdpbssd %tmm0, %tmm1, %tmm2"
        safety: "Intel AMX matmul step; @amx_tdpbssd intrinsic planned v0.5.1"
    }
}
```
**Why irreducible (temporarily):** ZER's catalog grows ~1-2 entries/year.
Brand-new ISA features (AVX-10.2 in 2027, ARM SME2 in 2028, etc.) may
temporarily need raw asm until intrinsics ship. Migrates to intrinsics
over time.

**Category 3: Hand-tuned hot loops where exact scheduling matters**
(~0-200 LOC per project, rare)
```zer
naked void aes_inner_loop(...) {
    asm {
        instructions: "
            // ~50 lines of hand-scheduled AES rounds
            // Compiler reordering would hurt cache/pipeline
        "
        safety: "Hand-tuned for Skylake µarch; matches OpenSSL reference."
    }
}
```
**Why irreducible:** crypto authors and high-perf inner loops want
*exact* instruction order. Even GCC builtins may reorder. This is rare
(<1% of <1% of code) but legitimate.

**Total irreducible asm per typical project:** ~100-300 LOC across all
three categories. Auditable in one sitting. Same scale as Hubris, Linux's
`arch/<X>/boot/`, Zephyr's `core/locore.S`.

#### Implementation path — phased, demand-driven

```
PHASE 0 (today):
  Status: ~130 intrinsics shipped (D-Alpha-1 through D-Alpha-14)
  Coverage: ~95% of typical kernel/firmware needs
  Raw asm % in typical project: ~3-5%

PHASE 1 (post-Level-C cleanup, ~3-4 weeks, OPTIONAL):
  Action: Add ~15-20 gap intrinsics from table above
  Priority order:
    1. Boot helpers (@cpu_set_stack, @cpu_set_vector_table, @cpu_jump_to)
    2. Full context save/restore (priv levels)
    3. IRQ entry/exit wrappers
    4. Cache range ops
    5. DMA buffer helpers
    6. TLB ops
    7. @cpu_pause spin hint
    8. Vector aligned load/store
    9. Saturating arithmetic
    10. Optional: crypto/SHA/CLMUL (if real demand)

  Implementation: each intrinsic = ~30-100 lines, can ship one at a time
  Coverage after: ~99% of typical needs
  Raw asm % in typical project: ~1%

PHASE 2 (ongoing, demand-driven, ~1-2 entries/year):
  Action: Add intrinsics when real users hit specific gaps
  Source: user issue tracker, vendor docs, new ISA extensions

  Implementation: incremental, never blocking
  Coverage after: bounded growth, asymptotic ~99%+

PHASE 3 (steady state):
  ~150-170 intrinsics in catalog
  ~1% raw asm in typical project (boot + vectors + rare hot loop)
  Zero standalone `.S` files in user projects
  GCC handles all per-arch codegen via intrinsic bodies
  ZER's source files are always `.zer` — no asm file extension needed
```

#### Industry validation — this exact target has been hit before

| System | Intrinsic count | Raw asm in kernel | `.S` files needed |
|---|---|---|---|
| **Hubris RTOS** (Oxide) | ~250 via `core::arch` + custom wrappers | ~200 LOC in `kernel/boot` crate | Zero |
| **Tock OS** (academic + embedded) | ~300 intrinsics + arch-specific | ~150 LOC per port | Zero |
| **seL4** | Similar pattern, Isabelle proofs on top | ~500 LOC verified asm | Zero — even Isabelle proofs treat asm as inline |
| **Zephyr RTOS** | Headers with intrinsics | ~300 LOC in `arch/<X>/core/` | Few (`locore.S`) — could be eliminated |
| **Linux kernel** | Thousands of `readl`/`writel`/etc. intrinsics | Substantial — but spread across thousands of files | Some (e.g., `arch/x86/entry/entry_64.S`) — historically grew, could be modernized |
| **ZER target** | ~150-170 intrinsics | ~100-300 LOC raw asm in naked fns | Zero (goal) |

**ZER's target falls between Hubris and Zephyr in catalog size. Achievable.**
Hubris specifically validated that 99% intrinsic coverage with raw asm
escape works in production embedded hardware (Oxide rack switch firmware).

#### What this means for marketing / public claim

After Phase 1 lands, ZER's public-facing claim about asm safety can include:

> "ZER firmware projects typically have zero standalone `.S` assembly files.
> 99% of CPU-level operations are expressed via the ~150-intrinsic catalog
> (atomic, MSR, port I/O, cache, context, etc.) — each with safety baked in
> at the catalog level. The remaining ~1% (boot stubs, vector tables, the
> rare hand-tuned hot loop) lives in `naked` functions with explicit raw
> asm + `safety:` audit string. GCC handles all per-arch codegen via
> intrinsic bodies. No probe scripts, no per-arch tables, no per-instruction
> database — works on every architecture GCC supports automatically."

This is honest, audit-able, and matches what Hubris/Tock can claim today.

#### Relationship to other sections

- **Section 1.5 (Final Design — Two-Layer Model):** the 99% goal is the
  Layer 1 catalog's stretch target. Layer 2 (raw asm in naked) handles
  the irreducible 1%. Doesn't change the architecture — just expands the
  catalog over time.
- **Section 9 (What We're Keeping):** intrinsic catalog is part of "Keep."
  Phase 1 additions go here.
- **Section 16 (Implementation Strategy):** the 6-commit Level C cleanup
  ships first. Phase 1 catalog expansion is post-Level-C optional follow-on.
- **Section 20 (Out of Scope):** "Algorithm correctness" and
  "Microarchitectural" remain out of scope even at 99% intrinsic coverage.

**Status:** goal documented, path defined, no commitments made beyond
Level C execution. Phase 1 catalog expansion can be deferred indefinitely
or executed incrementally based on user demand.

---

## 1.5. Final Design — Two-Layer Model (crystallization, 2026-05-12 evening)

**This section supersedes any ambiguity in later sections about additional
language complexity on top of Level C.** A multi-day architectural discussion
on 2026-05-12 explored richer designs (Tier 2 operand annotations, IR region
wrappers, auto-guard emission) and **rejected them all** in favor of the
simpler two-layer model below. Level C is the execution baseline; this
section confirms that nothing richer gets layered on top.

### The two layers

```
┌──────────────────────────────────────────────────────────┐
│ LAYER 1 — INTENT INTRINSICS (primary safe path)           │
│                                                            │
│   ~130 today, room for ~20 more if real demand emerges.    │
│                                                            │
│   @bit_scan_reverse, @atomic_*, @cpu_read_msr, @port_*,    │
│   @cache_*, @barrier_*, @cpu_*, @vec_load_aligned, etc.   │
│                                                            │
│   → Compiler-controlled emission. Safety baked in.         │
│   → ~80% wrap GCC builtins (zero per-arch ZER code).       │
│   → ~20% wrap per-arch inline asm (frozen, audited once).  │
│   → User-facing experience: call function, get safety.     │
│   → Covers 95%+ of typical kernel / firmware / embedded.   │
└──────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────┐
│ LAYER 2 — RAW ASM IN NAKED FN (escape hatch, "unsafe")    │
│                                                            │
│   naked fn boot() {                                        │
│       asm {                                                │
│           instructions: "..."                              │
│           safety: "Audit string ≥ 30 chars (S4)"          │
│       }                                                    │
│   }                                                        │
│                                                            │
│   → No Tier 2 annotations. No IR regions. Plain asm.       │
│   → MISRA Dir 4.3 satisfied via mandatory `naked`.         │
│   → User responsibility: clobbers, preconditions, intent.  │
│   → AST mnemonic still auto-applies ~12 UB classics.       │
│   → Z-rules still apply through operand bindings.          │
│   → Covers boot stubs, ctx switch, hand-tuned crypto.      │
└──────────────────────────────────────────────────────────┘
```

That's it. **Two layers. Nothing else.**

### What's EXPLICITLY DEFERRED indefinitely

These ideas were explored in depth during the 2026-05-12 discussion and
**rejected** as adding complexity without commensurate safety gain over the
2-layer model. Re-opening these requires fresh user-visible motivation:

| Deferred idea | Why explored | Why rejected |
|---|---|---|
| **Tier 2 per-operand annotations** (`requires: nonzero`, `align: 16`, etc. on asm operand bindings) | Would let users get compile-time precondition checks on raw asm without writing an intrinsic | Redundant with intrinsics for common cases; rarely useful for niche cases; intent intrinsics + caller-side guards cover the same ground without adding annotation vocabulary; "one obvious way to do it" principle |
| **IR region wrappers for asm safety** (`@atomic_sequence { }`, `@cache_sync_region { }`, `@dma_buffer_op { }`) | Multi-block coordination patterns get clean structural scope | Composite intrinsics (`@atomic_cas`, `@cache_flush_range`, etc.) handle the same patterns with simpler syntax; regions add language surface without unlocking new safety; existing regions like `@critical`/`defer`/`@once` already exist for general scope needs |
| **Auto-guard emission for unprovable Tier 2 preconditions** | Would make wrong-precondition behavior hardware-independent (loud ZER trap instead of CPU-dependent trap) | Only useful if Tier 2 annotations exist (which they don't); intrinsics emit their own internal guards already |
| **State-machine annotations on individual blocks** (`opens_state:` / `closes_state:`) | Would let user-defined multi-block patterns be checked | F7-light hardcoded LR/SC state machine covers the one real case; user-defined patterns either become intrinsics or live in raw asm |
| **Generic asm linter / per-instruction database for clobber completeness** | Would catch missing clobber declarations | Per-instruction database = maintenance hell (the whole reason Level C exists); intrinsics absorb 95%+ of clobber audits at the catalog level instead |
| **`unsafe asm` keyword** (vs current bare `asm`) | Symmetry with Rust | Cosmetic only; `naked` already isolates asm structurally; rename happened in opposite direction (2026-04-25 dropped `unsafe asm` → bare `asm`) |

### What stays from Level C, unchanged

Section 2 ("The Decision in One Page") and section 9 ("What We're Keeping")
are still correct. The 2-layer model is just a more precise framing of what
Level C already says:

- 130 intent intrinsics (catalog) — Layer 1
- Z-rules Z1-Z8 + Z11/Z12 through asm operands — applies to Layer 2
- Naked-only restriction (S1) — defines Layer 2 boundary
- Structured asm syntax (`asm { instructions: ... safety: ... }`) — Layer 2 syntax
- Hardcoded UB classics (~12 frozen) via AST mnemonic detection — applied to Layer 2
- F7-light LR/SC pairing — applies to Layer 2 only (LR/SC also available via composite intrinsic)
- GCC delegation for ISA-level (registers, instructions, CPU features, sub-extensions, future ISAs)

### The honest "what we give up vs Rust"

Rust's `core::arch::*` has ~5000 intrinsics. ZER's catalog has ~130-150. For
truly esoteric ops Rust users find a pre-made intrinsic; ZER users either
wait for a catalog addition or drop to Layer 2 raw asm. **In practice the
~150 catalog covers what kernel / firmware / RTOS developers actually use.**
The 5000-intrinsic gap is mostly SIMD variants used by numerical computing —
not ZER's primary audience.

### Maintenance picture — final

```
ZER-side ongoing maintenance after one-time implementation:

  Intrinsic catalog growth:          ~1-2 entries / year (real demand only)
  Hardcoded UB classics list:         ~1 entry / decade (genuinely new UB class)
  Per-arch register tables:           ZERO — deleted, GCC handles
  Per-arch instruction tables:        ZERO — deleted, GCC handles
  CPU feature enum:                   ZERO — deleted, GCC -m flag passthrough
  Probe scripts:                      ZERO — deleted
  Sub-extension support:              ZERO — GCC inherits automatically
  New architecture support:           ZERO — GCC inherits automatically
  Tier 2 annotation framework:        ZERO — not built (deferred)
  IR region wrappers:                 ZERO — not built (deferred)

TOTAL ZER-side ongoing: ~1-2 hours per year. Bounded. Not hellish.
```

### Industry precedent for this exact model

The 2-layer "intrinsics primary + raw asm escape" model is the **industry
default for production kernel/RTOS development**:

- **Hubris RTOS** (Oxide Computer, Rust-based): intrinsics for everything,
  raw `asm!()` only in tiny `kernel/boot` crate. 99% of code never touches asm.
- **Linux kernel**: `readl`/`writel`/`atomic_*`/`barrier` intrinsics everywhere;
  `.S` files only in `arch/<X>/{boot,entry,head}.S`.
- **Zephyr RTOS**: same pattern, intrinsics in headers, asm in `core/locore.S`.
- **FreeRTOS**: same pattern.
- **STM32 / CMSIS**: `__DMB()`, `__WFI()`, `__NOP()` intrinsic macros; raw asm rare.
- **Rust stdlib**: `core::arch::*` is the intrinsic catalog; `asm!()` is the
  unsafe escape.

**Every production system that takes asm safety seriously uses this exact
2-layer pattern.** ZER's design matches the converged industry norm.

### Conformance — MISRA Dir 4.3 is sufficient

MISRA C:2023 Dir 4.3 (Mandatory): *"Assembly language shall be encapsulated
and isolated."*

ZER's `naked` requirement satisfies this fully. No additional ZER-side
machinery (Tier 2 annotations, region wrappers, contract systems) is
required to claim MISRA conformance for asm encapsulation. **Bare 2-layer
model passes the MISRA test.**

Higher safety-critical standards (DO-178C Level A, ISO 26262 ASIL D, IEC
62304 Class C) require formal proof / exhaustive testing — Vale/SPARK
territory, opt-in via `@verified_spec` v2.x+, out of core ZER scope.

### Execution path remains unchanged

The 6-commit cleanup plan in **section 16** is correct and proceeds as
specified. The 2-layer crystallization above is a clarification, not a
plan revision. Nothing new gets built beyond what section 16 lists.

If intrinsic catalog growth is desired (the optional ~20 new intent
intrinsics for UB-prone operations not currently covered), that's a
**post-Level-C optional follow-on**, ~3-4 weeks, bounded scope, can ship
incrementally one intrinsic at a time. Not blocking.

---

## 1.6. Level D Pivot — User-Extensible Intrinsics with Explicit Contracts (added 2026-05-31)

> **REFINED BY OPTION E (Section 1.7, added 2026-06-06).** Level D's `@intrinsic_def` mechanism, demand/promise asymmetry rule (§1.6.17), and missing-arch propagation rule (§1.6.18) survive unchanged. The layering described in §1.6.16 ("blessed @core::* with per-ISA dispatch authored by ZER team in core") is refined by Option E: ZER core ships the schema (closed category vocabulary) + the semantic operation taxonomy + the `@intrinsic_def` / `@bind` mechanism, with **zero per-ISA bindings in language core**. Per-ISA bindings (including for operations Level D would have called "blessed @core::*") live in Layer 2 libraries, structurally peer to community libraries. The packaging question of whether ZER team maintains a canonical Layer 2 binding library (e.g., `zer-asm-x86`) is separable from the safety architecture and decidable empirically. See §1.7 for the locked layering, the structure/semantics straddle observation, and the closed-vocabulary kind-difference from SPARK elevated to load-bearing thesis. The program-consequence vs hardware-consequence vocabulary locked in `CLAUDE.md` and `firmware_safety_extensions.md` applies to all of Level D's claims; "wrong contract poisons verifier reasoning about callers" (§1.6.7) is restated as hardware-consequence floor at the `@bind` declaration site, with the closed-vocabulary discipline (§1.7.7) bounding the trust-gap shape.

**This section ADDS to Level C, does not remove it.** Level C remains the
correct execution baseline. Level D is an architectural upgrade on top of
Level C that resolves the one remaining maintenance leak: the ~1-2
intrinsics/year ZER team additions for new ISA features.

**Status:** Architecture LOCKED IN as of 2026-05-31. Implementation pending. **REFINED by Option E as of 2026-06-06 — see §1.7.**

**Pivot summary:** Level C constrains Y (operations) and delegates Z
(ISA-specific details) to GCC, but still leaves Y_intrinsic catalog growth
on ZER team's desk (~1-2 additions/year forever). Level D externalizes the
authorship of Y_intrinsic to users/libraries while keeping the verifier
(X, Z-rules, safety class registry) internal and frozen. ZER team's
ongoing work converges to a fixed point: new safety classes appear only
when fundamentally novel hardware-precondition kinds emerge (~once per
decade), not when new instructions or extensions appear.

> **READ FIRST:** This section captures the full design discussion that
> produced the Level D pivot. The architecture decisions here are LOCKED.
> Future sessions should not re-litigate. Reference sections 1.6.1
> through 1.6.13 for the complete context.

### 1.6.1. The mathematical formalization

Following a long architectural discussion, the asm safety architecture
can be formalized precisely:

```
X = { safety classes }                              finite, bounded set
Y = { all possible ASM operations }                 potentially infinite
Z = X expanded across all (arch, instr, operand)    |Z| = unbounded
```

**X is frozen.** Z-rules Z1-Z8/Z11/Z12 + UB classics (~12) + naked-only
(S1) + operand types + 8 safety categories = ~33 elements. These don't
grow with hardware evolution.

**Z is unbounded.** Every new ISA extension, every new instruction, every
new sub-architecture adds entries to Z. Growth rate: ~1000-1500 entries
over 5 years.

**The X ↔ Z relationship:**
- Every element of Z reduces to membership in some subset of X
- Every safety class in X has potentially infinitely many instances in Z
- Direct enforcement via X alone is incomplete (compressed over per-instruction details)
- Direct enforcement via Z is impossible in practice (|Z| unbounded — maintenance hell)

**The resolution: constrain Y.**

Make Y a controlled subset rather than the full unbounded space:

```
Y_zer = Y_intrinsic ∪ Y_naked

Y_intrinsic: operations expressible through ZER's intrinsic catalog
             class(intrinsic) ⊆ X pre-computed at intrinsic-definition time
             
Y_naked: operations expressible through raw asm in naked functions
         with explicit safety: annotation
         user takes responsibility for safety classes that apply
```

**Coverage equation (Level C):**

```
Coverage(Y_zer) = Coverage(Y_intrinsic) + Coverage(Y_naked) + Delegation(GCC)

Where:
  Coverage(Y_intrinsic) ≈ 99% with complete X enforcement
  Coverage(Y_naked) ≈ 1% with partial X enforcement + user-declared audit
  Delegation(GCC) = all Z-level concerns (register validation, instruction
                    validity, CPU features, sub-extension support)
```

### 1.6.2. The two levers identified

The asm design uses two distinct architectural levers, applied in sequence:

**Lever 1 (Level C): Constrain Y so finite X covers it. Delegate Z to GCC.**

This bounds ZER's burden to |X| + |Y_intrinsic|. |X| is frozen. But
|Y_intrinsic| still grew on ZER team's desk (~130 today, ~1-2/year additions
forever).

**Lever 2 (Level D): Externalize the authorship of Y_intrinsic while
keeping the verifier internal and frozen.**

This is the NEW move. Not "constrain Y" again — it's "constrain Y, then
externalize who authors Y while ZER retains the verification engine."

```
Lever 1 (Level C):
  ZER team owns:    catalog (Y_intrinsic), verifier (X)
  Delegated:        Z (register/instruction validity → GCC)
  Burden:           1-2 intrinsics/year forever
  
Lever 2 (Level D):
  ZER team owns:    verifier (X), mechanism (intrinsic_def syntax)
  Delegated:        Z (→ GCC), catalog authorship (→ user/library ecosystem)
  Burden:           ~0 — new safety class ~once per decade
```

The lever differences matter:
- Lever 1: constrain Y to bounded set
- Lever 2: open Y back to unbounded, but make authorship distributed and contract-explicit

ZER's work converges to a fixed point with Lever 2. That's the actual
freedom Level C was trying to reach but couldn't fully achieve.

### 1.6.3. The intrinsic_def mechanism

User (or library) defines intrinsics in ZER source code:

```
@intrinsic_def @bit_scan_reverse(mask: u32) -> u32 {
    arch x86_64: {
        instructions: "bsr %1, %0"
        inputs:  { "in"  = mask }
        outputs: { "out" = result }
        clobbers: [ "flags" ]
    }
    arch aarch64: {
        instructions: "clz %0, %1"
        inputs:  { "in"  = mask }
        outputs: { "out" = result }
        clobbers: [ ]
    }
    requires:    mask != 0
    safety_class: [ C1_nonzero ]
    effect:       result_in_range(0, 31)
    safety:       "Bit-scan reverse; UB on zero input per Intel SDM Vol 2 BSR"
}
```

User code anywhere in ZER then calls it:

```
u32 idx = @bit_scan_reverse(mask);
// Compiler enforces: mask != 0 via existing VRP infrastructure
// Compiler enforces: declared safety_class membership via existing systems
```

**Compiler responsibilities:**
1. Verify operand types match declared input/output types
2. Verify safety_class assignments dispatch to existing safety infrastructure
3. Verify `requires` clause uses operands correctly (references valid operand names)
4. Verify operand binding consistency (declared names match asm template references)
5. Pass per-arch dispatch to GCC for assembly-time validation

**Author responsibilities (the seam — see Section 1.6.6):**
1. Asm string actually does what the contract claims
2. Clobber list is complete
3. `requires` precondition matches hardware reality
4. `safety_class` assignments are correct for the actual operation
5. Per-arch implementations are semantically equivalent

### 1.6.4. The maintenance picture after Level D

```
ZER-side ongoing maintenance after Level D ships:

  Safety class registry (X):              ~0-1 entries per DECADE
                                          (only for fundamentally novel
                                           hardware-precondition kinds)
  
  Z-rules infrastructure (Z1-Z8, Z11, Z12): FROZEN, no growth
  
  Hardcoded UB classics (~12):            FROZEN, ~1 entry per decade
  
  intrinsic_def syntax/parser:            FROZEN after initial ship
  
  Verification engine:                    FROZEN after initial ship
                                          (uses existing safety systems)
  
  Blessed catalog (@core::*):             STABLE at current ~130
                                          (slow-grow via promotion from
                                           Layer 2, NOT frozen forever)
  
  Per-arch register tables:               DELETED (Level C) — GCC handles
  Per-arch instruction tables:            DELETED (Level C) — GCC handles
  CPU feature enum:                       DELETED (Level C) — GCC handles
  Probe scripts:                          DELETED (Level C) — gone
  
  User-defined intrinsics (@<lib>::*):    UNBOUNDED growth in user space
                                          ZER team NOT involved
```

**The honest fixed-point claim — distinguish X from Layer 1:**

Earlier framing said "Layer 1 FROZEN forever, per-decade fixed point."
That conflated two distinct things. The honest split:

**X (safety class registry) — genuinely per-decade fixed point:**
- New fundamentally novel hardware-precondition kinds only
- Example: CHERI capabilities introduction in early 2020s (one event)
- Example: hypothetical "atomic ordering across heterogeneous memory"
- Frequency: per-decade, possibly per-multi-decade
- This IS the actual fixed point

**Layer 1 (blessed catalog @core::*) — slow-moving, NOT frozen:**
- Starts at ~130 (current state)
- Grows by PROMOTION from Layer 2 when a pattern proves universal
  - Trigger: three or more libraries hand-roll the same pattern against
    the same asm — that's ecosystem signal it should be blessed
- Each promotion: ZER team audits the candidate, then it joins core
- Rate: handful per several years, not zero
- Not a flaw — healthy ecosystem feedback loop
- "Frozen forever" denies this can happen and locks in a claim that
  will be falsified by ZER's own success

**Why "per-year for everything" was also wrong:**

The doc previously stated "~1-2 intrinsics/year on real demand." That
was the Level C figure where ZER team owned all catalog authorship.
Level D externalizes Y_intrinsic authorship, so:

- New instructions → Y growth → user library territory (NOT ZER's work)
- New ISA extensions → Y growth → user library territory (NOT ZER's work)
- New vendor opcodes → Y growth → user library territory (NOT ZER's work)
- New architectures → Y growth → user libraries + GCC delegation (NOT ZER)
- Pattern promotion to Layer 1 → ZER team audit per promotion
  - Rate: handful per several years
- New fundamentally novel safety class → X growth → ZER's work
  - Rate: per-decade

**The two growth rates, separated:**

```
ZER-team ongoing work:
  X (safety classes):    ~per-decade           (true fixed point)
  Layer 1 (blessed):     ~handful per several years (slow promotion)
  Z-rules / UB classics: frozen
  Mechanism / verifier:  frozen after ship
  
Library/ecosystem work:
  Y_user_defined:        unbounded growth in user space (not ZER's work)
```

TOTAL ZER-side burden: a few hours per several years (Layer 1 promotion
audit), ~1-10 hours per decade (new safety class). NOT zero, but bounded
and ecosystem-driven rather than ZER-pushed.

Honest framing: bounded, slow-growth, not perpetual treadmill.
NOT 1-2/year (Level C figure). NOT zero forever (overcorrection).
Actually: slow promotion of Layer 1 (years), plus per-decade X.

### 1.6.5. The bootstrap argument

```
ZER compiler ships ONCE with:
  - Safety class registry X (~33 frozen elements)
  - intrinsic_def syntax (one-time language feature)
  - Verification engine (uses existing safety infrastructure)
  - Operand type checking (already exists)
  
After this ships, ZER compiler is DONE with intrinsic-related work.

The catalog grows in user/library space:
  - Vendor SDKs ship intrinsic libraries for their hardware
  - Domain libraries ship intrinsic_def for their patterns (crypto, DSP, etc.)
  - Project-specific intrinsics live in project code
  - Each contributor audits their own contracts
  
End state:
  Blessed standard library: ~30-50 (minimum) or ~130 (current, frozen)
  User libraries: thousands of domain-specific intrinsics
  ZER team work: minimal, per-decade safety class additions
```

This is "bootstrap ASM" — ASM provides the operations, intrinsic_def
provides the contracts, ZER's existing safety infrastructure provides
the verification. Same pattern as how Rust's standard library ships
`asm!()` macro once, and the crates ecosystem provides every specific
use case (raw-cpuid, x86, cortex-m, etc.).

ZER's version is FIRST-CLASS (not a macro hack) — definitions are
language-level constructs with structured contracts, not text substitution.
Contracts get compiler verification against safety classes.

### 1.6.6. The seam — wrong contract at definition site

**The honest critical analysis: Level D reintroduces Definition B's
failure mode at the intrinsic_def boundary.**

ZER previously rejected Definition B (SPARK-style contracts) in Section 2:

> "If the user's contract is wrong, the compiler verifies a wrong contract
> and produces false confidence. The bug ships anyway, but with a
> 'verified' wrong contract."

intrinsic_def is structurally identical at the definition boundary:
- User declares contract (operands, requires, safety_class, clobbers)
- Compiler verifies code-against-contract
- Wrong contract → silent bug

**This is the seam. It must be named explicitly, not obscured.**

What ZER CAN verify about a user-defined intrinsic_def:

| Property | Verifiable? | How |
|---|---|---|
| Operand types | ✓ Yes | Existing type checker |
| Safety class invocation correctness | ✓ Yes | Pattern check against class definition |
| Operand binding consistency | ✓ Yes | Existing parser |
| `requires` references valid operands | ✓ Yes | Symbol resolution |
| `safety_class` enum membership | ✓ Yes | Lookup in X registry |
| Per-arch dispatch syntactic correctness | Partial | GCC validates at assembly time |
| Whether asm string actually does declared op | ✗ No | Needs per-instruction DB (rejected) |
| Whether clobber list is complete | ✗ No | Needs per-instruction DB |
| Whether `requires` matches actual hardware precondition | ✗ No | Needs hardware spec |
| Whether effect statement is semantically correct | ✗ No | Needs semantic equivalence checker |
| Cross-arch implementations are equivalent | ✗ No | Needs cross-arch semantic checker |

**The seam summary:**

ZER's intrinsic_def verification covers structural consistency (operand
types, safety class invocation, operand binding) but NOT semantic
correctness against silicon (asm-vs-contract, clobber completeness,
hardware-precondition correctness).

**This is the SAME boundary as:**
- Rust's `unsafe impl Send` (user declares Send-ness, compiler trusts)
- Rust's `unsafe { asm!() }` (user declares contract via comments, compiler trusts)
- ZER's `cinclude` (user declares C function signature, compiler trusts)
- ZER's raw `asm { ... }` in naked (user declares via `safety:` string)

Every safe systems language has this boundary somewhere. Level D moves
ZER's boundary from "every asm use site" (current Level C) to "every
intrinsic_def site" (Level D).

### 1.6.7. Why the seam is BETTER than the alternatives

Despite the seam, Level D has real advantages over alternatives:

```
Comparison of contract-authoring locations:

Rust's unsafe { asm!() }:
  Audit point per: every use site
  Contract structure: free-form `// SAFETY:` comments
  Compiler enforcement: minimal — operand types only
  Audit burden: many sites, unstructured

C's asm:
  Audit point per: every use site
  Contract structure: none
  Compiler enforcement: none beyond compilation
  Audit burden: every use site, no help

ZER Level C (current):
  Audit point per: every blessed intrinsic (one-time, ZER team)
  Contract structure: encoded in catalog entry
  Compiler enforcement: complete via catalog (no user input)
  Coverage: limited to catalog (~130 entries)

ZER Level D (NEW):
  Audit point per: every intrinsic_def (one-time, author)
  Contract structure: structured (operand types, requires, safety_class)
  Compiler enforcement: complete against structured contract
  Coverage: unbounded via user libraries
```

**Level D vs Rust unsafe — DIFFERENT TRADE, NOT STRICT IMPROVEMENT.**

Earlier framing claimed Level D was "BETTER than Rust unsafe." That was
overclaim — it stated only the flattering half. Both halves are true:

**Level D ergonomics: BETTER than Rust unsafe**
- Contracts are STRUCTURED (machine-checkable against X, not free-form `// SAFETY:` comments)
- Audit happens ONCE per def, reused many times (not per call site)
- Greppable per category (`grep intrinsic_def` vs `grep unsafe`)

**Level D blast radius when wrong: WORSE than Rust unsafe**
- Rust's `unsafe` cannot lie to the borrow checker about other code —
  the unsafety is contained at the block boundary
- Level D's `intrinsic_def` FEEDS the verifier with structured claims
  (`safety_class:`, operand types, effects) that the verifier uses
  to reason about CALLERS
- A wrong `safety_class` declaration tells the verifier to enforce
  the wrong invariant at every call site
- Worse: a wrong claim about what an intrinsic SATISFIES can cause
  the verifier to certify surrounding code as safe on a false premise
- Blast radius = the verifier's reasoning about callers, not just the
  local intrinsic body
- This is a poisoned input to the trusted verifier, not a contained escape

**Honest framing:** Level D is a DIFFERENT trade-off than Rust unsafe,
not a strict improvement. Better ergonomics and structured auditability;
worse blast radius when a contract is wrong. The choice between them is
about which failure mode is preferable for the audience.

ZER picks Level D because:
1. ZER's audience prefers structured contracts over free-form comments
2. The blast radius is mitigable via the demand/promise asymmetry rule
   (see Section 1.6.17) — user intrinsics can DEMAND obligations freely
   but cannot silently DISCHARGE obligations downstream relies on
3. Greppable per-category auditing matches ZER's existing escape boundaries
   (`cinclude`, `naked fn`, now `intrinsic_def`)

The blast-radius cost is real and named. The mitigation is the
asymmetry rule, not the structured nature of the contracts.

**Level D advantages over Level C alone:**
- Coverage extends to any hardware (not catalog-limited)
- No ZER-team bottleneck for new ISA features
- Library ecosystem can flourish
- ZER team converges to fixed point on X (safety classes)

**Level D trade vs Level C:**
- Soundness claim becomes CONDITIONAL on author contract correctness
- Wrong contract poisons verifier reasoning about callers
  (worse blast radius than the Level C catalog-only approach)
- More compiler complexity (verification engine for intrinsic_def)
- Mitigated by demand/promise asymmetry (Section 1.6.17)

### 1.6.8. The blessed namespace mechanism

To distinguish ZER-audited intrinsics from user-defined ones, Level D
introduces namespace convention:

```
@core::atomic_cas         ← blessed (ZER team audited, frozen)
@core::bit_scan_reverse   ← blessed
@core::cpu_disable_int    ← blessed

@user::custom_op          ← user-defined (author owns contract)
@my_vendor::amx_tdpbssd   ← vendor library
@app::project_specific    ← project-internal

Compiler treats `@core::*` as special:
  - Cannot be redefined by user code
  - Frozen at language version
  - ZER team audited

User namespaces:
  - Defined by module path / package name
  - `@<library>::<intrinsic>` resolves via standard import system
  - Each library author owns their namespace
```

**Implementation:** ~50-100 lines reusing existing module/namespace
infrastructure. No new mechanism needed beyond namespace prefix
convention enforced at intrinsic_def site.

**Why namespace prefix vs other options:**

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| Namespace prefix (`@core::*`) | Visual distinction, greppable, reuses module path | Verbose | **CHOSEN** |
| Module path import | Cleaner imports | Requires resolution | Alternative |
| Reserved marker (`@intrinsic_def_blessed`) | Explicit | New mechanism | Heavier |
| Convention only (no language distinction) | Simplest | No enforcement | Too weak |

Recommended: namespace prefix. Already aligns with ZER's existing
module path syntax. No new language mechanism.

### 1.6.9. The conditional soundness claim (4-level breakdown)

Level D's safety story is honestly conditional. ZER's claim splits into
four levels:

```
LEVEL 1 — Pure ZER (no intrinsic_def, no cinclude):
  Unconditionally sound. ZER-team-owned correctness.
  Memory safety, type safety, concurrency safety all guaranteed.
  
LEVEL 2 — ZER + blessed intrinsics (@core::*):
  Unconditionally sound. ZER team has audited these one-time, frozen.
  
LEVEL 3 — ZER + user-defined intrinsics (@<lib>::*):
  Sound CONDITIONAL on author's contract correctness.
  Compiler verifies "code matches declared contract" structurally.
  Compiler does NOT verify "declared contract matches silicon."
  Same conditional boundary as cinclude / Rust unsafe.
  
LEVEL 4 — ZER + cinclude (C interop):
  Sound CONDITIONAL on C code correctness.

Greppable audit boundaries (decentralized auditability):
  grep "@<lib>::"        → audit user-defined intrinsic uses
  grep intrinsic_def     → audit intrinsic definitions
  grep cinclude          → audit C interop boundary points
  grep "naked fn"        → audit raw asm escape points
```

**The honest framing principle:** Each level explicitly names what it
depends on. No hidden assumptions. No "mostly safe" framing.

This is stronger than Rust's claim because:
- intrinsic_def contracts are STRUCTURED (machine-checkable)
- Audit boundaries are GREPPABLE per category
- Conditional dependencies are EXPLICIT per level

It's more honest than languages that obscure the boundary (e.g., claims
of "memory safe" without specifying which code, "race-free" without
specifying which concurrency model).

### 1.6.10. The triage of Level D concerns

In the design discussion, the following concerns were raised about Level D:

| Concern | Whose problem | Resolution |
|---|---|---|
| No mechanical verification asm string matches contract | Design by intention | NOT a gap — author declares; ZER trusts. Same as Definition A philosophy applied at def site instead of use site. |
| Library quality variation | Library author | Library author owns their contracts. Same as every package ecosystem (npm, cargo, pip). ZER provides mechanism; ecosystem provides quality. |
| Versioning / namespace collisions | Library / build system | Module path resolution. Standard package management concern. NOT language concern. |
| Cross-library composition trust transitivity | Library author | Library author publishes contracts; downstream trusts published interface. Same as every package ecosystem. |
| No "blessed library" mechanism in language | ZER | RESOLVED via `@core::*` namespace convention (Section 1.6.8). |
| Soundness claim becomes conditional | ZER (documentation) | RESOLVED via 4-level honest breakdown (Section 1.6.9). |

**Concerns 1, 2, 3, 4 are not ZER's problem — they're ecosystem concerns
that every package-based language handles via convention.**

**Concerns 5 and 6 are ZER's work** — both bounded one-time implementations:
- #5: namespace convention (~50-100 lines parser/checker)
- #6: documentation update (README, reference.md, marketing claim)

### 1.6.11. The mathematical formalization extended

```
After Level D:

Y_zer = Y_blessed ∪ Y_user_defined ∪ Y_naked
        ↑           ↑                  ↑
        frozen      grows in user-space  ~0.1% escape, naked-only

|Y_blessed| = FROZEN at ~130 (current) or trimmed to ~30-50 minimum
|Y_user_defined| = grows with hardware evolution, but in user space
|Y_naked| = bounded ~0.1% (boot stubs, hand-tuned crypto)

class(Y_blessed) ⊆ X         → audited by ZER team (one-time, frozen)
class(Y_user_defined) ⊆ X    → declared by author, structurally verified
                                  by compiler against X
class(Y_naked) ⊆ X           → user-declared via safety annotation

Coverage(Y_zer) = Coverage(Y_blessed) + Coverage(Y_user_defined) +
                  Coverage(Y_naked) + Delegation(GCC)

ZER-team-side maintenance burden:
  |X| growth: ~0-1 per decade
  |Y_blessed| growth: 0 (frozen)
  intrinsic_def mechanism: 0 (one-time ship)
  Verification engine: 0 (one-time ship)
  
Hardware evolution touches:
  |Y_user_defined|: user library territory
  GCC delegation (Z): user's C compiler

ZER team's work converges to a fixed point after Level D ships.
```

### 1.6.12. Comparison to industry practice

```
System              Catalog approach            Maintenance burden
─────────────────────────────────────────────────────────────────────────
Hubris RTOS (Rust)   core::arch + library       Rust-team for core,
                                                  community for crates
                                                  
Linux kernel        readl/writel + arch/*.S    Kernel maintainers
                                                  
Zephyr RTOS         intrinsics in headers      Vendor + community
                                                  
STM32 / CMSIS       __DMB(), __WFI() macros    ARM + ST vendors
                                                  
Rust stdlib         core::arch + asm! macro    Rust team for core,
                                                  crates ecosystem for rest
                                                  
ZER Level C         Built-in catalog (~130)    ZER team (1-2/year)
                                                  
ZER Level D (NEW)   Built-in core + user-def   ZER team for core (frozen),
                                                  community for rest
                                                  Per-decade for ZER team
```

Level D brings ZER into alignment with the converged industry pattern:
**small blessed core + extensible mechanism + community catalog growth.**

This is exactly how every successful systems language handles the
"unbounded catalog" problem. ZER's contribution: STRUCTURED contracts
verified against existing safety infrastructure, not free-form
documentation.

### 1.6.13. Implementation scope for Level D

**What ships with Level D (one-time work):**

```
1. Parser support for intrinsic_def syntax (~200-400 lines)
   - Token recognition for new keyword
   - AST nodes for intrinsic definitions
   - Per-arch dispatch parsing
   - Operand binding parsing
   - Contract clause parsing (requires, safety_class, effect)

2. Verification engine (~500-1000 lines)
   - Operand type validation
   - Safety class invocation dispatch
   - Cross-architecture consistency checks (best-effort)
   - Integration with existing Z-rules

3. Namespace convention (~50-100 lines)
   - @core::* reservation
   - Module path resolution for @<lib>::*
   - Collision detection

4. Documentation (~few hundred lines)
   - 4-level safety claim in docs/reference.md
   - intrinsic_def syntax reference
   - Author's guide for writing safe intrinsic_def
   - Ecosystem conventions

5. Migration (optional)
   - Existing 130 intrinsics become @core::*
   - Verify they're frozen and audited
   - Document the freezing

Total: ~750-1500 lines of compiler code + documentation
Estimated effort: ~4-8 weeks for one developer
```

**What does NOT ship (per Level C decision):**

- No per-instruction database (Z stays in GCC)
- No register validation tables (GCC handles)
- No CPU feature enum (GCC -m flags)
- No probe scripts (deleted in Level C)
- No 8-category framework infrastructure beyond X registry

**Pre-Level-D prerequisite:**

Level C cleanup (Section 16, 6 commits, 1-2 days) should ship FIRST.
This removes the per-instruction database and register tables that
would conflict with the user-extensible approach.

Then Level D adds intrinsic_def on the clean Level C foundation.

### 1.6.14. The architecture locks

> **AUGMENTED BY §1.7.18 (Option E locks).** The 12 locks below remain valid; Option E adds: (13) no per-ISA asm in Layer 1 of language core; (14) closed-vocabulary discipline must stay hard — no free-form predicates in Layer 2; (15) structure/semantics straddle is the architectural forcing argument for the layering; (16) packaging of canonical Layer 2 libraries is separable from safety architecture; (17) program-consequence vs hardware-consequence vocabulary is locked across the asm domain identical to the firmware domain. Read §1.7.18 for the complete locked-decisions list.

**Locked in (do not re-litigate in future sessions):**

1. **The lever is "externalize authorship"** — different from "constrain Y"
2. **Per-decade growth for ZER team applies to X (safety classes)** — Layer 1 is slow-grow via promotion, not frozen forever
3. **The seam IS real** — wrong contract at def site is the failure mode; blast radius is LARGER than Rust unsafe (verifier poisoning)
4. **The mitigation IS greppable trusted-once boundary** — same shape as cinclude, Rust unsafe — PLUS the demand/promise asymmetry rule (lock #11)
5. **The safety claim splits 4 ways** — pure ZER, blessed, user-def, cinclude
6. **Blessed namespace is `@core::*`** — convention enforced at intrinsic_def parsing
7. **Verification engine reuses existing safety infrastructure** — no new Z-rules
8. **|X| stays per-decade fixed point** — ~33 elements, ~0-1 additions per decade
9. **|Y_blessed| is STABLE not frozen** — starts at ~130, grows by promotion from Layer 2 when patterns prove universal (handful per several years)
10. **Library ecosystem owns |Y_user_defined|** — ZER not involved
11. **Demand/promise asymmetry — DIRECTIONAL classification, not syntactic** — see Section 1.6.17. User declarations that cause the verifier to do LESS checking on surrounding code are PROMISES (must be checkable or explicitly taint caller's soundness). Declarations that cause MORE checking are DEMANDS (free, can only over-reject). Classify by effect on verifier workload, not by syntactic slot.
12. **Missing-arch propagation rule** — see Section 1.6.18. Call to intrinsic with no arch-clause for compile target = compile error at call site. Supported-archs is part of the intrinsic's contract; caller's portability set = intersection of supported-archs across all intrinsics transitively used. GCC delegation does NOT cover this.

**Future sessions:** If a fresh session proposes:
- "Let's add another safety class for X" → check if it's genuinely novel; usually no
- "Let's add intrinsic Y to blessed catalog" → check if it qualifies for promotion from Layer 2 (handful per several years rate)
- "Let's verify asm strings against contracts" → that's per-instruction DB, REJECTED
- "Let's audit user libraries" → that's ecosystem, not ZER
- "Let's let user intrinsics claim arbitrary safety_class provides" → VIOLATES lock #11 (promise must be checkable or tainting)
- "Let's silently fallback on missing arch" → VIOLATES lock #12 (must be compile error)

Reference Section 1.6 instead of re-deriving.

### 1.6.15. The complete pivot summary

> **REFINED BY §1.7.16 (Option E comparison matrix).** The "Level D end state" below describes a "Frozen blessed catalog @core::* (~130 or trimmed to ~30-50)" with implicit per-ISA dispatch authored by ZER team in core. Under Option E, the catalog of ~130 intrinsics is refactored: the operation declarations move to Layer 1 (taxonomy in core, no asm bodies); the per-ISA dispatch moves to a separately-maintained Layer 2 binding library (e.g., `zer-asm-x86`). Architectural commitment: ZER core has no per-ISA bindings, ever. Packaging commitment (separable, decidable empirically): ZER team likely maintains one canonical Layer 2 library to avoid universal-constant fragmentation. See §1.7.16 for the complete comparison matrix across Levels C, D, the rejected Option C, and the locked Option E.

```
What ZER had at Level C:
  - Bounded catalog (~130 intrinsics, growing ~1-2/year)
  - Y_naked escape hatch for the 1%
  - GCC delegation for Z
  - Maintenance: ~1-2 intrinsics/year forever

What ZER has at Level D (after pivot):
  - Frozen blessed catalog (@core::*, ~130 or trimmed to ~30-50)
  - Frozen verification engine
  - User-extensible intrinsic_def mechanism (@<lib>::*)
  - Frozen Y_naked for ~0.1% irreducible cases
  - GCC delegation for Z (unchanged)
  - 4-level conditional safety claim
  - Maintenance: ~per-decade safety class additions

The bottom-line architectural lock:
  ZER COMPILER intrinsic work converges to a fixed point.
  Catalog growth happens in user/library space.
  Safety claim is honestly conditional, greppably auditable.
  Hardware evolution doesn't touch ZER team's work.
```

**This is the final architecture for ZER's asm safety.** Locked in
2026-05-31. Level C cleanup ships first, then Level D adds intrinsic_def
on top.

### 1.6.16. The corrected 2-layer architecture (with escape hatch excluded)

> **REFINED BY §1.7.3 (Option E).** The 2-layer model below ("blessed @core::* + user-defined @<lib>::*") is restructured under Option E into a 3-layer model: **Layer 1 (ZER core: schema + operation taxonomy + mechanism, NO per-ISA bindings) + Layer 2 (per-ISA binding libraries, structurally peer, including for operations Level D would have called blessed) + Layer 3 (firmware/application code: import and call)**. The escape hatch (raw asm in naked) remains structurally separate from the layered model. The framing below — "Layer 1 blessed primitives, ZER-team-audited, with internal per-ISA dispatch authored by ZER team" — is the part Option E refines: ZER core does not author per-ISA asm bodies; those live in Layer 2 libraries (potentially ZER-team-maintained as a separate canonical library — a packaging decision, not an architectural one). The conceptual content of "Layer 1 is foundational + Layer 2 is built on top + escape hatch is parallel" carries forward, just at one more level of factoring. Read §1.7.3 for the locked 3-layer model.

Clarification on the architectural layering for Level D — the safety
story is a clean 2-layer model. Raw asm in naked functions is an
ESCAPE HATCH, not a layer.

```
LAYER 1 — Blessed @core::* intrinsics
  ZER-team-audited, FROZEN at ~130 (or trimmed to ~30-50)
  Universal foundational primitives (atomics, barriers, common CPU ops)
  Internal per-ISA dispatch (ZER-team-authored)
  Unconditionally sound

LAYER 2 — User-defined @<lib>::* intrinsics
  Library author-defined, contract-explicit
  Unbounded — grows in user/library space
  Internal per-ISA dispatch (library-author-authored)
  Conditionally sound (depends on author's contract correctness)

[ESCAPE HATCH — not a layer]
  Raw asm in naked functions
  ~0.1% irreducible (boot stubs, hand-tuned crypto)
  Outside the layered scheme — explicit unsafe boundary
  Same conceptual category as cinclude / Rust unsafe blocks
```

**Why raw asm is excluded from layering:**

Raw asm doesn't compose with Layers 1-2:
- It's not a contract-bearing primitive
- It's not in the verification engine's scope
- It's a deliberate escape from the safety story

Treating it as "Layer 3" or "Layer 0" wrongly implies it's part of the
safe architecture. It isn't. It's the explicit-unsafe boundary, same
shape as `cinclude` and Rust's raw `unsafe { asm!() }`.

**The clean 2-layer picture:**

```
ZER's safe ASM architecture:

  LAYER 1: blessed primitives (small, frozen, ZER-audited)
  LAYER 2: user-defined intrinsics (unbounded, library-authored,
                                    contract-verified)
  
  Both layers:
    - Same intrinsic_def mechanism
    - Same verification engine
    - Same call-site experience (just call the intrinsic)
    - Internal per-ISA dispatch (per-intrinsic_def)
    
  Layers differ in:
    - Who authored (ZER team vs library author)
    - Soundness (unconditional vs conditional on contract correctness)
    - Scope (universal foundational vs domain-specific)

[Separately, NOT a layer]
  Raw asm escape hatch for the 0.1% irreducible
```

**Why your framing might tempt 3 layers (and why we don't):**

A natural intuition would be:
- Layer 1: blessed primitives
- Layer 2: user intrinsics building on blessed
- Layer 3: raw asm escape

But this is wrong for two reasons:

1. **User intrinsics don't STRICTLY build on blessed primitives.** Each
   user intrinsic_def is self-contained with its own per-ISA dispatch.
   Library author writes the per-ISA asm directly, not through Layer 1.
   They MAY compose blessed primitives (e.g., calling @core::atomic_cas
   inside their intrinsic body), but this is COMPOSITION not LAYERING-
   FOR-VERIFICATION.

2. **Raw asm is escape, not a layer.** Layers are part of the safety
   story (compose with verification, follow Definition A). Raw asm is
   explicit-unsafe boundary, parallel to cinclude. Calling it Layer 3
   wrongly implies it's verified by the language; it isn't.

The honest model: **2 layers in the safety story + 1 escape hatch
parallel to other unsafe boundaries.**

**Per-ISA dispatch happens INSIDE each intrinsic_def, not BETWEEN layers:**

```
@intrinsic_def @my_lib::aes_enc_round(state: u128, key: u128) -> u128 {
    arch x86_64: {
        instructions: "aesenc %2, %1"     ← per-ISA INSIDE the def
    }
    arch aarch64: {
        instructions: "aese %0.16b, %1.16b; aesmc %0.16b, %0.16b"
    }
    arch riscv64: {
        // not supported — library author can omit
    }
    requires: ...
    safety_class: [ ... ]
}
```

Library author writes per-ISA implementations. Same shape as how ZER
team writes per-ISA for Layer 1 blessed intrinsics. The "for all ISA"
property is internal to each intrinsic_def, not a separate layer.

**Growth pattern under the corrected layering:**

```
Layer 1: FROZEN (no growth — ZER team converged at fixed point)
Layer 2: GROWS in user/library space (no ZER team involvement)
Escape:  BOUNDED at ~0.1% (boot stubs, hand-tuned crypto only)

Per-ISA dispatch: happens INSIDE each intrinsic_def
GCC: handles Z-level concerns (register/instruction/feature validation)
ZER team work: ~per-decade safety class additions to X registry
```

**Cross-reference to other sections:**

- The 4-level conditional soundness claim (Section 1.6.9) refers to
  SOUNDNESS levels (pure ZER, blessed, user-def, cinclude), NOT to
  architectural layers
- The 2-layer architecture here refers to ZER's ACTIVE safety story
  for asm operations (Layer 1 blessed + Layer 2 user-defined)
- The escape hatch (raw asm in naked) is the SAME thing as
  Section 1.5's "Layer 2 — RAW ASM IN NAKED FN" — at Level D it gets
  renamed to "escape hatch" to make its non-layer nature explicit

**Locked terminology going forward:**

| Term | Meaning |
|---|---|
| Layer 1 | Blessed @core::* intrinsics (ZER-audited, frozen) |
| Layer 2 | User-defined @<lib>::* intrinsics (library-authored, conditional) |
| Escape hatch | Raw asm in naked (~0.1% irreducible, explicit unsafe) |
| Soundness Level 1-4 | Conditional soundness claim breakdown (separate concept) |
| Per-ISA dispatch | Internal to each intrinsic_def, not a layer |

When future sessions reference "the layering," they mean the 2-layer
model + escape hatch from this subsection. The 4-level soundness claim
is a SEPARATE axis describing what's conditionally vs unconditionally
sound.

### 1.6.17. The demand/promise asymmetry — DIRECTIONAL classification

**The rule:** Any user declaration that causes the verifier to do LESS
checking on surrounding code is a **PROMISE** and must be either
checkable or explicitly taint the caller's soundness level. Any
declaration that causes the verifier to do MORE checking is a
**DEMAND** and is free (can only over-reject, never under-protect).

**Classify by effect on the verifier's workload, NOT by which syntactic
slot the declaration uses.**

This is the asymmetry that closes the wrong-contract seam (Section 1.6.6).
Without it, a wrong `safety_class` or `effect:` declaration silently
poisons the verifier's reasoning about callers. With it, user intrinsics
have full freedom in the safe direction and are blocked in the unsafe one.

**Why directional, not syntactic:**

A natural but WRONG formulation: "`requires:` = safe demand; `effect:` /
`safety_class:` = unsafe promise." This formulation misses the
load-bearing case — **output-type declarations that look like demands
but act like promises.**

Example of promise-in-demand's-clothing:

```
@intrinsic_def @my_lib::aligned_load(addr: usize) -> *aligned u32 {
    arch x86_64: { ... }
    // No `requires:`, no `effect:`, no `safety_class:` declared
    // BUT: the output type carries `*aligned`
}

Downstream code:
  *aligned u32 p = @my_lib::aligned_load(addr);
  // Compiler sees `*aligned` and SKIPS its own alignment check
  // If aligned_load's asm doesn't actually produce aligned output,
  // downstream code is silently unsafe — verifier did LESS checking
  // based on the declared output type
```

The output type `*aligned u32` is in an operand-type slot (looks like
a demand) but functions as a promise (downstream relaxes its own
checks based on it). Syntactic classification misses this case
entirely.

**The correct directional rule:**

```
For each user declaration in an intrinsic_def, ask:
  "Does this declaration cause the verifier to do LESS checking on
   code that calls or uses this intrinsic?"
  
  YES → it is a PROMISE
  NO  → it is a DEMAND
  
Promises must be EITHER:
  (a) Checkable by the verifier from existing safety infrastructure
      (e.g., compiler can prove the asm produces aligned output)
  (b) Explicitly taint the caller's soundness level
      (caller drops from "blessed-sound" to "user-conditional-sound")

Demands are always free — user can declare as many preconditions /
operand types / safety_class requirements as they want. Worst case
is false rejection (annoying, not unsafe).
```

**Examples classified directionally:**

| Declaration | Verifier effect | Classification |
|---|---|---|
| `requires: mask != 0` | More checking on callers (must prove nonzero) | DEMAND (free) |
| `inputs: { "rax" = x: u32 }` (operand type) | More checking on callers (must pass u32) | DEMAND (free) |
| `safety_class_requires: [C2_alignment]` | More checking on callers | DEMAND (free) |
| `outputs: { "rax" = result: u32 }` (plain type) | No relaxation downstream | DEMAND (free) |
| `outputs: { "rax" = result: *aligned u32 }` | Downstream skips alignment check | **PROMISE** (must be checkable or tainting) |
| `outputs: { "rax" = result: non_null *u8 }` | Downstream skips null check | **PROMISE** (must be checkable or tainting) |
| `effect: result_is_aligned_to(16)` | Downstream skips alignment check | **PROMISE** (must be checkable or tainting) |
| `safety_class_provides: [C2_alignment]` | Downstream skips alignment-class check | **PROMISE** (must be checkable or tainting) |

The same `outputs:` slot can hold a demand (plain `u32`) or a promise
(`*aligned u32`). Slot ≠ classification.

**Tie to keep:**

This is the same sound-but-conservative discipline ZER uses for `keep`:
- `keep` lets the user ADD obligations (parameter can be stored persistently)
- `keep` does NOT let the user DISCHARGE obligations downstream relies on
- Wrong `keep` annotation = false rejection (over-restricting)
- Same direction: free to demand, blocked from silently promising

**Implementation hint:**

The verifier infrastructure already exists for the demand direction —
existing safety systems (VRP, alignment, escape, MMIO, qualifier
preservation) enforce additional checks at use sites. Promise checking
requires either:
- The verifier reasoning about asm semantics (usually impossible)
- An explicit `taint_caller: <soundness_level>` annotation on the
  intrinsic_def declaring that callers drop to a lower soundness level
- A `verify_promise: <existing_check>` hook that runs an existing
  safety check on the intrinsic's output before allowing the promise

Default behavior for unverified promises: REJECTED at intrinsic_def
parse time. User must either prove checkable or explicitly taint.

**Catch the easy mistake:**

If a fresh session writes the rule as "`requires:` is safe, `effect:`
is unsafe," that's the syntactic formulation. It misses the output-type
case. Reject and reformulate directionally.

### 1.6.18. Missing-arch propagation rule

**The rule:** A call to an intrinsic with no arch-clause for the
current compile target is a **compile error at the call site**.
Supported-archs is part of the intrinsic's contract; a caller's
portability set is the intersection of supported-archs across all
intrinsics it transitively uses.

**The hole this closes:**

```
@intrinsic_def @my_lib::aes_enc_round(state: u128, key: u128) -> u128 {
    arch x86_64: { instructions: "aesenc %2, %1" ... }
    arch aarch64: { instructions: "aese ..." ... }
    // riscv64 omitted
}

Compile target: riscv64
User code: u128 result = @my_lib::aes_enc_round(state, key);

Without the rule, three possibilities and the plan didn't pick one:
  (a) compile error "intrinsic not available on this arch"   ← CORRECT
  (b) silent fallback to nothing                              ← UNSOUND
  (c) GCC catches it                                          ← IMPOSSIBLE
```

GCC delegation **cannot cover this case** because:
- GCC validates emitted asm bytes
- If no arch-clause matches, NO asm is emitted for this intrinsic on
  this target
- GCC has nothing to validate — failure is absence-of-emission, not
  presence-of-wrong-emission

**The required behavior:**

```
At intrinsic_def parse time:
  Collect supported_archs = { arch : intrinsic has arch-clause for arch }
  Attach supported_archs to intrinsic's symbol

At call site:
  current_target_arch = compile target
  if current_target_arch not in callee.supported_archs:
    COMPILE ERROR: "intrinsic @<name> not supported on <arch>;
                    supported archs: {arch1, arch2, ...}"

At function/library level:
  supported_archs(fn) = ∩ supported_archs(intrinsic)
                        for all intrinsics fn transitively uses
  This propagates upward — caller portability is bounded by
  intersection of all transitively-called intrinsics' arch support
```

**Why supported-archs propagation matters:**

Without explicit propagation, callers can compile on arch X but fail
silently when retargeted to arch Y because some transitive intrinsic
lacks a Y clause. With propagation, the caller's signature explicitly
encodes its arch portability, and porting to a new arch surfaces all
the missing-arch failures at compile time.

**Implementation:**

```
1. intrinsic_def parser: collect supported_archs from arch-clauses
2. Symbol table: store supported_archs on each intrinsic
3. Call-site checker: verify current_target in callee.supported_archs
   → emit error with directive listing missing arch
4. FuncSummary: propagate supported_archs upward as intersection
5. Library publication: supported_archs is part of public contract
```

Estimated: ~100-200 lines in checker.c + parser support.

**Catch the easy mistake:**

If a fresh session writes "GCC will catch missing-arch via assembly
failure," that's wrong — GCC can't validate absent emission. Need
explicit ZER-side check at the call site. Reject and add the
propagation rule.

---

## 1.7. Option E Pivot — Three-Layer Architecture, No Favored ISA, Program-Consequence Vocabulary (added 2026-06-06)

**This section ADDS to Levels C and D, does not remove them.** Level C is the deletion baseline (Z stays in GCC). Level D's `@intrinsic_def` mechanism is the user-extensibility primitive. Option E is the architectural refinement that:

1. Applies the **program-consequence vs hardware-consequence** vocabulary (locked in `firmware_safety_extensions.md` and `CLAUDE.md`) to the asm domain.
2. Refines the layering from Level D's "blessed-vs-user-defined" split into a cleaner **three-layer** model: **Layer 1 (ZER core: schema + operation taxonomy + mechanism, no ISA)** / **Layer 2 (per-ISA bindings: library territory)** / **Layer 3 (firmware/application: import and call)**.
3. Locks the **no-favored-ISA-in-core** decision: ZER's language core ships the closed category vocabulary + the semantic operation taxonomy + the `@intrinsic_def`/`@bind` mechanism, and ships **zero** per-ISA bindings in core. Per-ISA bindings — including x86 — are user/community library territory.
4. Identifies the **structure/semantics straddle** as the architectural reason asm uniquely requires the binding mechanism while every other ZER construct sits cleanly on one side of the program-domain/hardware-domain boundary.
5. Elevates the **closed verifier-native vocabulary** from "one of four mitigations" to "the load-bearing kind-difference from SPARK," and locks the discipline that the contract language must stay closed (no free-form `requires:` predicates).

**Status:** Architecture LOCKED IN as of 2026-06-06. Mechanical confirmation (write one `@bind` binding by hand against the schema) pending. Implementation pending; the work order is Level C cleanup → Level D mechanism → Option E factoring applied at the layering and ISA-reference points.

> **READ FIRST.** This section is the current locked architecture for ZER's asm safety. It is the lens through which Section 1.6 (Level D) should be read. Where 1.7 and 1.6 disagree on layering or on whether ZER core ships per-ISA bindings, **1.7 supersedes**. 1.6's `@intrinsic_def` mechanism, demand/promise asymmetry rule (§1.6.17), and missing-arch propagation rule (§1.6.18) carry forward unchanged.

### 1.7.1. The vocabulary locked elsewhere, applied here

The program-consequence vs hardware-consequence vocabulary was locked in `CLAUDE.md` (top of file, "ZER's Goal" section) and `docs/firmware_safety_extensions.md` (§1, §1a, §22). The full locked statement:

> ZER guarantees 100% program-consequence coverage: every wrong use of a value in ZER source code is caught at the use site. This includes embedded/firmware data — once a value crosses into ZER through a typed boundary (intrinsic return, MMIO read, cinclude function, linker symbol, asm output, source literal), it is program data and ZER verifies every program-level operation on it. Hardware-consequence — peripheral side effects, datasheet-specific value correctness, silicon behavior — is floor, out of scope for any language.

The vocabulary discipline that must not equivocate:

- **Program-consequence** — what happens when a value is used wrongly inside ZER source. Caught at 100%. The use is in the program, so ZER owns it.
- **Hardware-consequence** — what happens when a hardware fact is wrong relative to user belief (peripheral doesn't actually clear on read, baud value is wrong-for-this-board, asm instruction doesn't actually have the categories declared). Floor. The fact lives outside the program, never enters it, so ZER has nothing to verify.

Applied to asm:

- **Program-consequence (ZER total, 100%):** Every call site of every intrinsic — `@arith_add_with_carry(a, b, c_in)`, `@load_acquire(ptr)`, `@barrier_full()` — is verified against the operation's declared categories. Operand types, escape, provenance preservation through outputs, qualifier preservation through inputs, clobber accounting, ordering constraints, context permissions. Wrong use at the call site = compile error. This holds regardless of which ISA binding the caller compiles against, because the verifier reasons over the schema, not over asm strings.
- **Hardware-consequence (floor, surfaced at the `@bind` declaration site):** Whether the asm body inside a `@bind` declaration actually has the categories the declaration claims. If a binding declares `clobbers_flags + no_memory_effect` but the asm secretly writes memory, ZER cannot detect it. This is the floor for asm just as datasheet-correctness is the floor for MMIO declarations and signature-correctness is the floor for cinclude. Same floor pattern, different boundary location.

The phrase to never let collapse:

> "100% program-consequence" must never read as "ZER verifies the asm body matches the contract."

It doesn't. ZER verifies that every program-domain operation on values passing through the intrinsic boundary respects the declared categories. The declaration's correctness against silicon is the floor, the same floor that exists for every typed boundary into hardware-domain facts.

### 1.7.2. Why Option E refines Level D's layering

Level D's layering (§1.6.16, "the corrected 2-layer architecture") split intrinsics into:

- **Layer 1:** Blessed `@core::*` intrinsics, ZER-team-audited, frozen, **with internal per-ISA dispatch authored by ZER team**.
- **Layer 2:** User-defined `@<lib>::*` intrinsics, library-authored, conditional, with internal per-ISA dispatch authored by library author.
- **Escape hatch:** Raw asm in naked.

The Option E refinement: the "internal per-ISA dispatch authored by ZER team" part of Layer 1 is the architectural commitment Option E removes. ZER core does NOT author per-ISA asm bodies. ZER core ships the schema (closed category vocabulary) + the operation taxonomy (the set of semantic operations) + the `@intrinsic_def` / `@bind` mechanism, and stops there. Per-ISA bindings live in libraries — including x86, including the operations Level D would have called "blessed."

This is a structural refinement, not a contradiction of Level D. Level D's `@intrinsic_def` mechanism is preserved verbatim. The demand/promise asymmetry rule (§1.6.17) is preserved. The missing-arch propagation rule (§1.6.18) is preserved. The 4-level conditional soundness claim (§1.6.9) is refined: the "blessed" level (Level 2 of the conditional soundness breakdown) is now bound to the schema correctness, not to a ZER-team-shipped per-ISA catalog.

### 1.7.3. The three layers in dependency order

The locked three-layer model:

```
LAYER 1 — ZER core (frozen, shipped once + rare release extensions)
─────────────────────────────────────────────────────────────────
  Lives in: language core, compiler binary
  Authored by: ZER team
  Frequency of change: rare (per-decade for category vocabulary,
                       handful per several years for operation taxonomy)

  Contents:
    - Closed CATEGORY VOCABULARY (the schema)
        clobbers_flags, clobbers_register(name),
        reads_mem(width, ordering), writes_mem(width, ordering),
        requires_aligned(n), requires_nonzero(operand),
        requires_in_range(operand, lo, hi),
        memory_barrier(acquire | release | seqcst),
        produces_carry, consumes_carry,
        changes_privilege(from, to),
        control_flow(returns | jumps_to | calls),
        provenance_clear_on_output(operand),
        value_in_value_out,
        no_memory_effect, no_flag_effect,
        ... [closed set, ZER-owned, extended only via release]

    - SEMANTIC OPERATION TAXONOMY (the named operations)
        @arith_add_wrap(a, b) -> T
        @arith_add_with_carry(a, b, c_in) -> {result, c_out}
        @arith_sub_wrap, @arith_sub_with_borrow,
        @arith_mul_wide, @arith_div_unchecked,
        @bit_and, @bit_or, @bit_xor, @bit_shift_left, @bit_shift_right,
        @bit_rotate_left, @bit_rotate_right,
        @bit_scan_forward, @bit_scan_reverse, @bit_count_leading_zeros,
        @bit_count_trailing_zeros, @bit_population_count,
        @load_aligned(ptr), @load_unaligned(ptr),
        @load_acquire(ptr), @load_relaxed(ptr),
        @store_aligned(ptr, val), @store_unaligned(ptr, val),
        @store_release(ptr, val), @store_relaxed(ptr, val),
        @atomic_cas(ptr, expected, new),
        @atomic_swap, @atomic_fetch_add, @atomic_fetch_sub,
        @atomic_fetch_and, @atomic_fetch_or, @atomic_fetch_xor,
        @barrier_acquire, @barrier_release, @barrier_full,
        @barrier_compiler_only,
        @cpu_disable_int, @cpu_enable_int, @cpu_wait_int, @cpu_pause,
        @cpu_syscall, @cpu_sysret, @cpu_iret,
        @cpu_read_msr(idx), @cpu_write_msr(idx, val),
        @cpu_read_cr(n), @cpu_write_cr(n, val),
        @cpu_cache_invalidate(addr), @cpu_cache_writeback(addr),
        @cpu_dma_start(buf, channel),
        ... [closed-after-design set, ZER-owned, extended via release]

    - MECHANISM
        @intrinsic_def     (declare a new semantic operation in core or library)
        @bind              (provide a per-ISA implementation for a declared operation)
        verifier dispatch  (operate on categories, dispatch to existing Z-rules
                            + existing safety analyses; never on asm strings)

    - DELEGATIONS
        Z-level concerns (register validity, instruction validity, CPU feature
        gating) → GCC, as per Level C
        Hardware-consequence (whether silicon honors the binding's declared
        categories) → engineer territory at the @bind declaration site

  What Layer 1 does NOT contain:
    - No per-ISA asm bodies
    - No "x86 reference" or "ARM reference" or any ISA-specific catalog
    - No free-form predicate language (closed vocabulary only)
    - No user-extensibility of categories or operations
      (extensions go through ZER core release with review)

LAYER 2 — Per-ISA binding library (community / per-vendor / per-project)
───────────────────────────────────────────────────────────────────────
  Lives in: external libraries imported by user firmware
  Authored by: library maintainers — community, vendor, or project-local
  Frequency of change: per (operation, ISA) pair, authored once,
                       frozen-after-review

  Contents:
    - @bind declarations mapping each semantic operation in Layer 1's
      taxonomy to per-ISA asm
    - Per-ISA asm bodies (instructions, operand constraints, clobbers)
    - Category SELECTION (not invention) from Layer 1's vocabulary
    - safety: annotations for review (one declaration ↔ one paragraph
      explaining why the categories match what this asm does on this ISA)

  Example library structure:
    zer-asm-x86            — x86_64 bindings for Layer 1 operations
    zer-asm-arm-cortex-m   — ARM Cortex-M bindings
    zer-asm-arm-cortex-a   — ARM Cortex-A bindings (different memory model)
    zer-asm-riscv          — RISC-V bindings (including no-flag emulation
                              sequences for operations like add_with_carry)
    zer-asm-xtensa-lx7     — Xtensa LX7 bindings
    zer-asm-msp430         — TI MSP430 bindings
    ...

  Each library:
    - Is structurally peer to every other Layer 2 library under the
      Layer 1 schema
    - Has no privileged position
    - Authored by whoever brings up that ISA
    - Reviewed against the closed-checklist (do declared categories
      match what this asm does on this ISA?)
    - Frozen after review for that (operation, ISA) pair

  What Layer 2 does NOT do:
    - Cannot invent new categories (must select from Layer 1's vocabulary)
    - Cannot invent new operations (must bind operations Layer 1 declared)
    - Cannot publish free-form requires: predicates
    - If a binding cannot be expressed in Layer 1's vocabulary, the fix
      is a Layer 1 schema-extension request via ZER core release
      (NEVER a Layer 2 freedom)

LAYER 3 — Firmware / application (the actual program)
──────────────────────────────────────────────────────
  Lives in: user firmware source code
  Authored by: firmware/application engineer (the audience that matters
               for adoption ergonomics)
  Frequency of activity: continuous (this is the regular coding loop)

  What this person does:
    1. Imports a Layer 2 binding library for their target ISA:
         import zer_asm_arm_cortex_m;
    2. Imports Layer 1's operation taxonomy (transitively via Layer 2,
       or explicitly):
         use @core::ops;
    3. Calls operations as typed functions:
         let cur = *GPIOA_ODR;
         let next = @arith_xor(cur, 0x1);
         *GPIOA_ODR = next;
         @barrier_release();
    4. That is the entire workflow.

  What this person does NOT do:
    - Does not write asm
    - Does not author @bind declarations
    - Does not declare or select categories
    - Does not need to read an ISA manual to use operations
    - Does not need to know what asm the binding emits

  Verification at this layer:
    - Every call site is verified against the operation's declared
      categories (Layer 1's contract)
    - Verifier dispatches to existing analyses: Z-rules, escape,
      provenance preservation, qualifier preservation, alignment,
      ordering, context flags
    - Program-consequence: 100% over every call site, locked
```

This is the locked three-layer architecture. The pattern parallels firmware (mmio + @inttoptr in core, per-chip addresses in user space, firmware code that just uses typed pointers), the pattern parallels cinclude (mechanism in core, vendor HAL in user space, firmware code that just calls typed wrappers), and the pattern parallels every other domain in ZER where structural primitives live in core and substrate details live in user space.

### 1.7.4. The locked Layer 1 schema design rules

Layer 1's contents are closed. The discipline:

**Category vocabulary rules:**
1. Categories describe **structural behavior kinds** that exist across ISAs (clobbers a flag register class, reads memory at width W with ordering O, etc.), not ISA-specific instruction properties.
2. Categories are added only via ZER core release with review.
3. Categories support an "N/A on this ISA" disposition — for example, `clobbers_flags` is N/A on RISC-V because no flag register exists. The verifier handles N/A correctly (does not require the binding to declare flag categories on ISAs that lack flags).
4. Categories must be verifier-native: each category dispatches to an existing safety analysis. No category that requires a brand-new analysis.
5. No free-form predicates. If a category-shaped concept cannot be expressed as a selection-from-finite-set, it does not enter the vocabulary.

**Operation taxonomy rules:**
1. Operations are named for what they MEAN, not for any ISA's mnemonic. `@arith_add_with_carry` not `@x86_adc`.
2. Each operation has a fixed category profile that applies to every ISA binding of that operation. If the category profile differs structurally between ISAs (e.g., x86 ADD clobbers flags, ARM plain ADD doesn't), those are different semantic operations (`@arith_add_wrap` vs `@arith_add_with_carry`) — not one operation with per-ISA category variation.
3. Operations are added via ZER core release. The rate is bounded by genuinely-new semantic operation kinds appearing in hardware (per-several-years for vector extensions, novel atomic primitives, new memory ordering modes).
4. Operations are typed at the ZER level (operand types, return types). Per-ISA bindings cannot change the typed signature.
5. Operation taxonomy is closed-after-design. Extensions go through ZER core release.

**Mechanism rules:**
1. `@intrinsic_def` declares a new operation (Layer 1 release) or a new core-blessed wrapper. Used by ZER team and by library authors of new operations they're seeking promotion.
2. `@bind` provides a per-ISA implementation of an existing operation. Used by Layer 2 library authors.
3. Per-ISA dispatch is internal to a `@bind` (one binding per (operation, ISA) pair).
4. Verifier reasons over categories only. Never over asm strings.
5. GCC handles Z-level concerns (register validity, instruction validity, CPU feature gating) as per Level C.

### 1.7.5. The structure/semantics straddle — the architectural reason this factoring is forced

This is the deepest architectural observation in the asm safety thread, and it should be preserved as the load-bearing reason for Option E's shape.

**Every other ZER construct sits cleanly on one side of the program-domain / hardware-domain boundary:**

- A `u32` value, regardless of origin (hardware read, file, network, source literal): program-domain at every operation, ZER owns 100%.
- An `mmio` address declaration: hardware-domain claim at the declaration site (does this address really name the peripheral?), program-domain at every downstream use through the typed pointer. The boundary is crossed cleanly at the declaration.
- A `cinclude` function signature: hardware-domain claim at the signature declaration, program-domain at every call site. Same clean cross.
- A linker symbol extern: same pattern.
- A typed register access: structure in program, criterion in datasheet — both sides exist but they live in separate tokens (the typed pointer vs the address value).

**An asm instruction is the exception. It straddles.**

`adc $0, $1` is one token that simultaneously:
- Carries operand structure (program-domain — Z-rules apply to inputs and outputs, escape analysis applies to register clobbers, provenance clearing applies to outputs, qualifier preservation applies through the boundary, ordering constraints apply, context flags apply)
- Carries semantic meaning (hardware-domain — what flags get clobbered, what state mutates, what precondition is required for correctness, what the instruction actually does to the machine — and this varies per ISA)

You cannot separate these by reading the token. The instruction's structural connections and its hardware semantics are fused in one mnemonic. That fusion is unique to asm among ZER constructs.

**Why this forces Option E specifically:**

1. Asm's semantics are per-ISA hardware facts (the ISA manual is the datasheet for instruction meaning).
2. Any safety system that wants to reason about asm meaning must import that meaning from outside the program text.
3. "Outside the program, per-ISA" is by definition a binding (a `@bind` declaration).
4. ZER core cannot ship "what `adc` means" without committing to an ISA's semantics in the language core — which would make ZER a "portable language" wearing a costume of universality while actually carrying a favored-ISA assumption.
5. The only factoring that preserves both "ZER reasons about asm safety structurally" AND "ZER does not commit to an ISA" is: structure-half stays in core (universal, ISA-less, frozen — Z-rules, operand boundaries, the category vocabulary, the operation taxonomy), semantics-half imported per-ISA via `@bind` (user-authored, hardware-consequence floor surfaced at the binding site).

That is Option E. It is not "the cleanest of three options" — it is the unique architecture that respects the structure/semantics straddle without either committing to one ISA or refusing to reason about asm meaning at all.

**Any alternative either:**
- (A) commits ZER core to one ISA's semantics — breaking architecture-agnostic positioning and making ZER a "<favored-ISA> language" in disguise; or
- (B) refuses to reason about asm meaning at all — abandoning the asm safety claim and dropping back to "asm is just text we hand to GCC."

Option E is what's left after rejecting both. The structure/semantics straddle dictates the shape; the choice was made by the fusion in the token, not by architectural preference.

### 1.7.6. The closed-vocabulary kind-difference from SPARK — load-bearing, not bullet-point

In earlier framings, four "mitigations" were listed for the SPARK-shape trust gap in `@intrinsic_def`:

1. Frozen category vocabulary
2. Per-operation contract, not per-call
3. Audit visibility at use site
4. Per-author trust (library author authors once, users consume)

Three of these are **quantitative** mitigations — they make the SPARK trap smaller (fewer contracts, more visible, authored once). They are real but they are differences of degree. SPARK could in principle adopt all three and still be SPARK.

The first one is the only **kind-difference**, and it is the load-bearing distinction from SPARK. It must be elevated from bullet point to thesis:

> **SPARK contracts are arbitrary predicates; `@intrinsic_def` / `@bind` contracts can only select from a closed, ZER-owned category vocabulary that the verifier already knows how to reason about.**

Why this is a different failure topology, not just a smaller surface:

- A SPARK contract can be wrong in **unbounded ways**, because the predicate language is unbounded. You can assert any logical claim, including ones that are subtly self-inconsistent, vacuously discharged by the prover, or true on the implementation but false on the surrounding program.
- A `@bind` contract can be wrong in **exactly one way**: the declared categories do not match what the asm body actually does on this ISA. It cannot be wrong about anything else, because there is nothing else to declare — the contract language consists only of selections from the closed category vocabulary.

This makes the trust gap **auditable against a fixed checklist** instead of against an open-ended logical claim:

| To review a SPARK contract | To review a `@bind` declaration |
|---|---|
| Understand the predicate language | Skim the closed category vocabulary list |
| Decide whether the predicate is well-formed | (Not a concern — categories are syntactic) |
| Decide whether the predicate is true of the implementation | For each declared category, check the asm body |
| Open-ended reasoning task | Finite checklist |
| Surface grows with predicate complexity | Surface is fixed by the schema |

**This is genuinely stronger than SPARK** — not because the trust gap is absent (it isn't; the other Claudes in the design discussion were correct to push back on any framing that suggested otherwise), but because the gap has a **fixed, finite shape**, so verifying-the-contract-against-the-asm is a mechanical checklist task instead of a proof obligation.

SPARK's gap is open-shaped. ZER's gap is closed-shaped. Same existence; different reviewability.

### 1.7.7. The hard line: closed vocabulary or the kind-difference collapses

The closed-vocabulary discipline is the whole differentiator. The moment Layer 2 is allowed to carry a free-form `requires:` predicate (an arbitrary logical precondition) or a free-form `effect:` clause or any other open-shape declaration, the kind-difference dissolves and ZER becomes "tidier SPARK," not a structurally distinct architecture.

The rules that must hold:

1. **`@bind` declarations select from the Layer 1 category vocabulary.** They do not invent categories.
2. **`@bind` declarations bind operations Layer 1 declared.** They do not invent operations.
3. **No free-form predicate language in Layer 2.** Specifically:
   - `requires:` clauses must select from a finite precondition vocabulary (e.g., `requires_nonzero(operand)`, `requires_aligned(operand, n)`, `requires_in_range(operand, lo, hi)`, `requires_msr_writable(msr_idx)`). Never an arbitrary logical predicate.
   - `effect:` clauses must select from a finite postcondition vocabulary (e.g., `effect_in_range(result, lo, hi)`, `effect_aligned(result, n)`, `effect_provenance_cleared(output)`). Never an arbitrary logical predicate.
4. **If a binding cannot be expressed in the existing schema, that is a Layer 1 schema-extension request via ZER core release.** It is NEVER a Layer 2 freedom to open the vocabulary locally.
5. **Demand/promise asymmetry rule from §1.6.17 still applies.** Promises that cause the verifier to do less checking on callers must either be verifier-checkable or explicitly taint the caller's soundness level. The closed-vocabulary discipline narrows the set of expressible promises but does not relax the asymmetry rule.

The discipline implies a maintenance commitment on ZER team: the schema design must be expressive enough that real-world bindings can be expressed without users feeling the need for free-form predicates. If users keep hitting "the vocabulary is missing X" cases, that is a signal that the schema is incomplete — the fix is to extend the schema (Layer 1 release with review), never to open Layer 2.

**This is the work.** Not "ship x86 reference or not" — that's a packaging decision, separable from the architecture. The real work is making the Layer 1 schema rich enough to handle structurally different machine models (flag-bearing vs flagless ISAs, weak vs TSO memory models, vector vs scalar register classes, CHERI vs non-CHERI capability models) without escape into open-shape predicates. Get this right once and the schema is stable for the long term.

### 1.7.8. The safety vs governance register separation

A critical observation from the design discussion: the question of **"who maintains canonical Layer 2 binding libraries"** is a packaging/governance question, separable from the safety architecture. They live in two distinct registers and must not be conflated.

**Safety register (locked by Option E):**
- Does program-consequence stay 100% under this layering? **Yes.**
- Is binding-correctness floor cleanly named at the `@bind` site? **Yes.**
- Is the closed-vocabulary kind-difference preserved? **Yes.**
- Is the structure/semantics straddle respected? **Yes.**

The safety architecture holds **regardless of who ships the x86 binding library, whether the ZER team blesses one, whether the community owns it, or whether vendor-specific libraries proliferate.**

**Governance / packaging register (open, decidable empirically):**
- Who maintains the canonical x86 binding library?
- Does ZER core's release process include blessing one Layer 2 library per supported ISA?
- Does the community own ISA-bring-up entirely?
- Is there a `zer-asm-x86` repo maintained by the ZER team, structurally peer to community libraries but receiving extra review attention?

These are real ecosystem-coordination questions. They affect onboarding friction, fragmentation risk, default-library competition, and version-skew management. They do NOT affect the safety claim, because Layer 1 (the language core, where safety is established) makes no commitment about which Layer 2 library is canonical.

**Why this separation matters:**

The governance question is reversible. Whether the canonical x86 binding library ships:
- As part of `zer-core` binary,
- As a separately-maintained `zer-asm-x86` repo authored by ZER team,
- Or as a community library that becomes de-facto standard,

these are packaging choices that can change later without breaking any user code, because the user-facing API (the operation taxonomy + categories in Layer 1) is locked at the schema layer.

So you do not have to decide the governance question now to lock the safety architecture. The safety architecture locks at Option E. The governance question is independent and can be decided empirically — by writing one Layer 2 binding for a common operation (the x86 `adc` for `@arith_add_with_carry`, or the RISC-V no-flag carry-emulation sequence) and seeing what packaging model makes sense given the artifact's size, complexity, and review needs.

**The honest framing on packaging:**

A genuine pushback worth recording: under Option E with no x86 reference in core, every single user/library reimplements bindings that are completely identical across all users. `adc` on x86 is `adc` for everyone — it is a universal constant, not a per-user fact. Delegating a universal constant to N users means either fragmentation (N incompatible binding libraries) or de-facto standardization without official governance (one community library becomes load-bearing while ZER team has no formal commitment to it).

This is a real coordination concern. It does not break the safety architecture, but it is a real ecosystem-design question.

The middle-path resolution the design discussion landed on: **ship the Layer 1 schema in core, no per-ISA bindings in language core, AND maintain a small canonical Layer 2 binding library (e.g., `zer-asm-x86`) as a separately-maintained ZER-team-owned repo that is structurally peer to community libraries but receives extra review attention.** This preserves Option E's "no favored ISA in the language" while applying the "universal constant ships once, governed" insight at the library layer instead of the core layer.

This is not C and it is not E-as-stated; it is E's architecture with one canonical Layer 2 library maintained by the same team. The architectural commitment is locked at E. The packaging decision about who maintains x86 is a Layer 2 governance question that can be settled empirically.

### 1.7.9. The full workflow per layer

To make the layering concrete, here is the end-to-end workflow for each layer.

**Layer 3 workflow — firmware/application author (the audience that decides ZER's adoption):**

This person's workflow is **not** extremely different from regular ZER coding. They:

1. Import a per-ISA binding library matching their target:
   ```
   import zer_asm_arm_cortex_m;
   ```

2. Declare their firmware-side things (mmio regions, linker symbols, vector table — using the firmware extension primitives locked in `firmware_safety_extensions.md`):
   ```
   mmio 0x40020000..0x40020FFF;
   volatile *u32 GPIOA_ODR = @inttoptr(volatile *u32, 0x40020014);
   ```

3. Write code calling semantic operations as typed functions:
   ```
   fn toggle_led() {
       let cur = *GPIOA_ODR;                  // volatile read
       let next = @arith_xor(cur, 0x1);       // semantic op
       *GPIOA_ODR = next;                     // volatile write
       @barrier_release();                    // memory barrier
   }
   ```

4. That is the entire workflow.

For this person, asm is **invisible**. They call `@arith_xor(...)` and `@barrier_release()` and the compiler routes through whichever Layer 2 binding library was imported. They never see `eor`, `dmb`, or any other ISA-specific instruction. The cognitive model is identical to calling any other typed function.

**No primitive declarations, no contract authoring, no schema awareness. Same ergonomics as importing any ZER library.**

This is the load-bearing usability property. The architecture is radical in Layer 2, but Layer 3 is ergonomically normal — which is what matters for the audience that decides whether ZER gets adopted.

**Layer 2 workflow — per-ISA binding library author (a smaller audience, but the place where the SPARK-family work lives):**

This person's workflow IS different from writing C inline asm. They:

1. Pick a semantic operation from Layer 1's taxonomy:
   ```
   @arith_xor(a: T, b: T) -> T
       where T in {u8, u16, u32, u64}
       categories: { no_memory_effect, no_flag_effect, value_in_value_out }
   ```

2. Author a `@bind` declaration mapping the operation to per-ISA asm:
   ```
   @bind arith_xor isa: arm_cortex_m {
       asm { instructions: "eor $0, $1, $2"
             inputs: [a, b]
             outputs: [result]
             safety: "ARM EOR: bitwise xor, plain form does not affect
                      flags; categories declared in @arith_xor's
                      Layer 1 entry (no_memory_effect, no_flag_effect,
                      value_in_value_out) all hold on this ISA." }
   }
   ```

3. Select categories from Layer 1's vocabulary. **Never invent.** If a category needed for this binding does not exist in the vocabulary, submit a Layer 1 schema-extension request via ZER core release.

4. Get the `@bind` declaration reviewed against the closed checklist:
   - For each declared category, does the asm body honor it?
   - Are all operand widths and types consistent with the operation's Layer 1 signature?
   - Are clobbers complete (no flag-clobbering asm with `no_flag_effect` declared)?
   - Are ordering constraints honest (no acquire/release mismatches)?
   - Does the safety: annotation explain the binding clearly enough for future review?

5. Once reviewed, the binding is frozen for that (operation, ISA) pair.

This is mechanical to author (selection from a finite menu, not predicate authorship) and mechanical to review (finite checklist against a closed schema, not open proof obligation). It is genuinely different from C-style "write asm, hope it works" — but it is bounded by the schema, which is the whole point of Option E.

**Layer 1 workflow — ZER core / schema author (the smallest audience, the slowest cadence):**

This is design work, not coding work. It happens:

1. **At initial schema design** — enumerate the closed category vocabulary, identifying every category kind needed to express the structural-behavior properties of asm operations across all machine models the architecture will support (flag-bearing, flagless, weak-memory, strong-memory, CHERI, non-CHERI, vector, scalar). Get this right once and most extensions never happen.

2. **At operation taxonomy design** — enumerate the set of semantic operations the language ships in its closed-after-design taxonomy. Bias toward generality: prefer fewer, more general operations over many specialized ones.

3. **At rare schema extensions** — when a Layer 2 library author or community demand identifies a category kind or operation kind the existing schema cannot express, and the extension is genuinely structural (not a workaround for missing operations), go through ZER core release with review and add it.

4. **At per-decade evolutions** — when a fundamentally novel hardware-precondition kind emerges (the CHERI capability model is the canonical example), extend the schema to accommodate it. This is rare by design — the schema should be stable for many years between such extensions.

This is the work the ZER team owns. It is bounded, slow-cadence, and the design discipline is locked: closed vocabulary, no free-form predicates, structural categories only, verifier-native dispatch only.

### 1.7.10. Concrete examples of each operation in Layer 1

To make the schema design intent concrete, here are illustrative shape sketches for several Layer 1 operations and their per-ISA bindings. **These are illustrative, not implementation specifications.** The exact category vocabulary and operation signatures will be locked at schema design time.

**Example 1 — Arithmetic with carry (the canonical ADD/flags case):**

```
// Layer 1 (ZER core, frozen)
@intrinsic_def arith_add_with_carry(a: T, b: T, c_in: u1) -> {result: T, c_out: u1}
    where T in {u8, u16, u32, u64}
    categories: {
        no_memory_effect,
        produces_carry,
        consumes_carry,
        value_in_value_out,
        clobbers_flags    // declared at Layer 1; binding chooses whether
                          // this category applies via the ISA-N/A mechanism
    }
    safety: "Add with carry-in producing carry-out. The clobbers_flags
             category is N/A on ISAs that lack a flag register;
             flagless ISAs must emulate via GPRs while preserving the
             carry-in/carry-out semantics."

// Layer 2 (zer-asm-x86)
@bind arith_add_with_carry isa: x86_64 {
    asm { instructions: "adc $b, $a"
          inputs:  [a, b, c_in => CF]
          outputs: [result => a, c_out => CF]
          clobbers: ["flags"]
          safety:  "x86 ADC: a := a + b + CF, sets CF based on result.
                    All declared categories hold: no memory effect,
                    produces and consumes carry, value_in_value_out,
                    and clobbers_flags is honored (CF/ZF/SF/OF
                    affected as documented in Intel SDM Vol 2 ADC)." }
}

// Layer 2 (zer-asm-arm-cortex-m)
@bind arith_add_with_carry isa: arm_cortex_m {
    asm { instructions: "adcs $0, $1, $2"
          inputs:  [a, b, c_in => C]
          outputs: [result => $0, c_out => C]
          clobbers: ["NZCV"]
          safety:  "ARM ADCS: rd := rn + op2 + C, sets NZCV.
                    All declared categories hold; clobbers_flags
                    honored via NZCV update." }
}

// Layer 2 (zer-asm-riscv)
@bind arith_add_with_carry isa: riscv64 {
    asm { instructions: "add $r, $a, $b\n"
                        "sltu $t1, $r, $a\n"
                        "add $r, $r, $c_in\n"
                        "sltu $t2, $r, $c_in\n"
                        "or $c_out, $t1, $t2"
          inputs:  [a, b, c_in]
          outputs: [result => $r, c_out]
          clobbers: ["$t1", "$t2"]
          isa_disposition: {
              clobbers_flags: N/A  // RISC-V has no flag register
          }
          safety:  "RISC-V emulates carry-add via SLTU sequence.
                    No flag register exists; clobbers_flags marked
                    N/A. Categories produces_carry, consumes_carry,
                    value_in_value_out, no_memory_effect all hold." }
}
```

The same Layer 1 operation has structurally different per-ISA realizations. On flag-bearing ISAs the binding is small and uses the flag register; on flagless ISAs the binding is a multi-instruction emulation sequence. Both bindings honor the same Layer 1 contract. Layer 3 firmware code calls `@arith_add_with_carry(a, b, c)` and the verifier checks every call site against the Layer 1 contract regardless of which binding compiles.

**Example 2 — Acquire load (memory ordering):**

```
// Layer 1 (ZER core, frozen)
@intrinsic_def load_acquire(ptr: *T) -> T
    where T in {u8, u16, u32, u64}
    categories: {
        reads_mem(sizeof(T), acquire),
        requires_aligned(ptr, alignof(T)),
        no_flag_effect,
        value_in_value_out
    }
    safety: "Acquire-ordered load. Reads from ptr with acquire ordering
             semantics — subsequent reads and writes cannot be reordered
             before this load. Requires aligned pointer."

// Layer 2 (zer-asm-x86)
@bind load_acquire isa: x86_64 {
    asm { instructions: "mov ($1), $0"
          inputs:  [ptr]
          outputs: [result => $0]
          clobbers: []
          safety:  "x86 has acquire semantics for ordinary loads under
                    TSO. The MOV instruction satisfies acquire ordering
                    without explicit barrier. Aligned access requirement
                    holds via declared category." }
}

// Layer 2 (zer-asm-arm-cortex-a)
@bind load_acquire isa: arm_cortex_a {
    asm { instructions: "ldar $0, [$1]"
          inputs:  [ptr]
          outputs: [result => $0]
          clobbers: []
          safety:  "ARMv8 LDAR: load-acquire register. Hardware
                    enforces acquire ordering. Aligned access required
                    by ISA and declared category." }
}

// Layer 2 (zer-asm-riscv)
@bind load_acquire isa: riscv64 {
    asm { instructions: "lw $0, ($1)\n"
                        "fence r,rw"
          inputs:  [ptr]
          outputs: [result => $0]
          clobbers: []
          safety:  "RISC-V acquire load: ordinary LW followed by
                    fence r,rw. Acquire ordering: subsequent reads
                    and writes cannot reorder before the load.
                    Aligned access required." }
}
```

**Example 3 — Memory barrier:**

```
// Layer 1 (ZER core, frozen)
@intrinsic_def barrier_full()
    categories: {
        memory_barrier(seqcst),
        no_memory_effect,
        no_flag_effect,
        value_in_value_out
    }
    safety: "Full sequential-consistency memory barrier.
             Prevents any reordering of memory operations across
             this point."

// Layer 2 (zer-asm-x86)
@bind barrier_full isa: x86_64 {
    asm { instructions: "mfence"
          inputs: []
          outputs: []
          clobbers: []
          safety: "x86 MFENCE: full memory barrier, seqcst." }
}

// Layer 2 (zer-asm-arm-cortex-a)
@bind barrier_full isa: arm_cortex_a {
    asm { instructions: "dmb ish"
          inputs: []
          outputs: []
          clobbers: []
          safety: "ARMv8 DMB ISH: data memory barrier, inner shareable
                    domain. Provides seqcst ordering across the
                    inner shareable domain." }
}

// Layer 2 (zer-asm-riscv)
@bind barrier_full isa: riscv64 {
    asm { instructions: "fence rw,rw"
          inputs: []
          outputs: []
          clobbers: []
          safety: "RISC-V FENCE rw,rw: full memory barrier." }
}
```

These examples illustrate the pattern: Layer 1 declares the semantic operation with its category profile; Layer 2 provides per-ISA realizations that honor the contract; Layer 3 firmware code calls the operation without seeing the asm. The verifier's reasoning is over the Layer 1 categories — every call site of `@barrier_full()` is treated as a seqcst memory barrier regardless of which ISA binding compiles, so subsequent operations are correctly ordered.

### 1.7.11. The mechanical confirmation test — write one binding by hand

The whole design verifies under one experiment, which is the artifact-decides discipline applied at the right granularity:

**Write one Layer 2 `@bind` by hand against the schema. Write one Layer 3 call site that uses it. Check the result.**

Specifically:

1. Write the x86 binding for `@arith_add_with_carry` against the schema (the `adc` instruction).
2. Write the RISC-V binding for the same operation (the SLTU-based carry emulation sequence).
3. Write one Layer 3 firmware function that uses `@arith_add_with_carry` — for example, a multi-precision addition routine.
4. Compile it for both targets. Inspect:
   - Does the Layer 3 code know anything about which binding compiles? It should not.
   - Could a firmware author who has never read an x86 manual or a RISC-V manual use `@arith_add_with_carry` correctly by just importing the relevant binding library?
   - Is the x86 `@bind` declaration tiny (one instruction line plus the safety annotation)? It should be.
   - Is the RISC-V `@bind` declaration substantial (a real authored sequence with intermediate registers)? It should be.
   - Does the category vocabulary express both bindings honestly, including the "clobbers_flags N/A on RISC-V" disposition?

If yes to all: the three layers are clean. Layer 2 fully absorbed the per-ISA complexity. Layer 3 ergonomics is normal. The schema is expressive enough.

If Layer 3 leaks — if the firmware author has to understand the binding to use the operation correctly — Layer 2 is not doing its job, or the operation taxonomy is wrong (operations should be self-describing through categories alone). The fix is at Layer 1 (refine the operation signature or category profile), not at Layer 2 (do not add ad-hoc workarounds in bindings).

If the binding cannot be expressed in the existing category vocabulary — if the author keeps reaching for a free-form `requires:` predicate — that is the signal the schema is incomplete. The fix is a Layer 1 schema-extension request through ZER core release. Not a Layer 2 freedom.

**This experiment is the only confirmation that does not drift.** Architecture discussion can ratify the framing; only the artifact (one binding written, one call site compiled) confirms the layering actually delivers what the design claims.

### 1.7.12. The honest scope under Option E — what ZER claims, what it does not

The full claim, stated with the locked vocabulary:

**Program-consequence coverage at 100%.**

Every call site of every Layer 1 operation in Layer 3 firmware code is verified against the operation's declared categories. The verifier dispatches to existing safety analyses — Z-rules (operand boundary checks for asm-produced values), escape analysis (no asm-output value escapes through a memory-clobbering binding to a typed pointer outside its scope), provenance preservation (asm outputs receive provenance_clear and require explicit `@ptrcast` round-trip before re-typing), qualifier preservation (volatile passes through every binding boundary without strip), alignment checks (when categories declare `requires_aligned` the call site is verified or runtime-trapped), ordering enforcement (memory barriers declared by category profile honor the ordering at the verifier level even if the underlying asm uses different instructions per ISA), context propagation (asm-bearing bindings called inside `interrupt`-marked or `@critical`-marked context inherit the context's ban list).

This holds **regardless of which Layer 2 binding library is imported**. The verifier reasons over Layer 1 categories. Layer 2's asm strings are opaque to the verifier; only the declared categories matter.

This holds **regardless of whether the binding's declared categories actually match the asm semantics on this ISA**. That is the hardware-consequence floor, surfaced at the `@bind` site, library-author's responsibility, reviewable as a finite checklist against the closed schema. The program-consequence claim does not depend on binding-correctness; it depends on the schema being honored at every call site, which is structural verification.

**Hardware-consequence (the floor, named):**

What ZER does NOT verify:

- Whether the asm body inside a `@bind` declaration actually has the categories the declaration claims. If a binding declares `clobbers_flags + no_memory_effect` and the asm secretly writes memory, ZER cannot detect it. Library author owns this; reviewer with knowledge of the ISA checks via the finite checklist.
- Whether the asm produces semantically-correct output for the operation. If a binding for `@arith_add_with_carry` actually computes `a XOR b XOR c_in` due to a typo, ZER cannot detect it. The operation's typed signature is honored; the semantic equivalence is not verified.
- Whether the per-ISA implementations of a single Layer 1 operation are semantically equivalent across ISAs. Cross-ISA equivalence is library-author and code-review responsibility. ZER verifies that each binding honors its declared categories on its own ISA; cross-ISA equivalence is the author's contract to the user.
- Whether the ISA's actual hardware honors the declared categories (the silicon matches the ISA manual). This is the deepest floor — even GCC trusts the ISA manual.

These are not gaps. They are the hardware-domain floor that exists for every memory-safe systems language at the asm boundary. The structural property that makes ZER's floor better than alternatives is the closed-vocabulary discipline: the floor has a fixed, finite shape, so review is bounded.

**The architectural distinction from SPARK, stated precisely:**

SPARK and ZER both have a hardware-consequence trust gap at the contract boundary between user declarations and silicon behavior. The gap exists by physics; no language eliminates it. The distinctions:

| Property | SPARK | ZER (Option E) |
|---|---|---|
| Contract language | Arbitrary predicates | Closed category vocabulary |
| Trust gap shape | Open (anything expressible can be wrong) | Closed (only "categories vs asm semantics" can be wrong) |
| Review surface | Open-ended logical task | Finite checklist |
| Contract count per program | Unbounded (per-call) | Bounded (per-operation-per-ISA, frozen-after-review) |
| Audit visibility | Contracts in spec files, sometimes separated from use | Intrinsic name at use site; binding at well-known library location |
| Trust factoring | Contract author per call site | Binding author per (operation, ISA) |
| Hardware floor | Real and per-call | Real and per-(operation, ISA) — smaller, frozen |

ZER is stronger than SPARK on the contract-language axis (closed vs open) and on the bounded-vs-unbounded contract-count axis. ZER is at the same floor as SPARK on the underlying physics (contracts can be wrong about silicon; no language can verify the silicon). Both are honest; ZER's is smaller and more structurally bounded.

### 1.7.13. Cross-reference to firmware safety vocabulary

The same program-consequence vs hardware-consequence vocabulary applies uniformly across ZER's safety domains. The locked statements:

**For firmware (from `firmware_safety_extensions.md` §1):**

> Once a value crosses into ZER through a typed boundary (intrinsic return, MMIO read, cinclude function, linker symbol, asm output, source literal), it is program data and ZER verifies every program-level operation on it. Hardware-consequence — peripheral side effects, datasheet-specific value correctness, silicon behavior — is floor, out of scope for any language.

**For asm (Option E, this section):**

> Every call site of every Layer 1 operation in Layer 3 firmware code is verified against the operation's declared categories. Hardware-consequence — whether the asm body inside a `@bind` declaration actually has the categories declared, whether the ISA semantics match the binding's claims, whether the silicon honors the ISA manual — is floor, out of scope for any language.

The two statements are the same structural commitment applied to different boundaries. Firmware's boundary is at value-entry through typed declarations (mmio, cinclude, linker symbols). Asm's boundary is at the `@bind` declaration site. In both cases, ZER owns program-consequence at 100% and names hardware-consequence as the floor.

The structure/semantics straddle observation (§1.7.5) is the reason asm uniquely requires the binding mechanism while firmware's other primitives do not — but the resulting safety story has the same shape: program-consequence total, hardware-consequence floor at the declaration site.

### 1.7.14. Anti-patterns — drifts to refuse

The session-level drift patterns documented in `firmware_safety_extensions.md` §23 apply to asm work too. Specifically, the following drifts must be refused regardless of how sympathetically they are framed:

**Anti-pattern 1: Collapsing program-consequence and hardware-consequence under one word.**

The word "consequence" must never carry both meanings in the same claim. If you find yourself writing "ZER catches every consequence at 100%," check whether you mean program-consequence (correct, locked) or hardware-consequence (false, floor). The split must be explicit. The discipline from `firmware_safety_extensions.md` §1 applies: program-consequence is caught, hardware-consequence is floor, and "consequence" by itself is ambiguous and forbidden in external claims.

**Anti-pattern 2: Opening Layer 2's contract language.**

The closed-vocabulary kind-difference is the load-bearing distinction from SPARK. The moment Layer 2 is allowed to carry free-form `requires:` predicates, free-form `effect:` clauses, or any other open-shape declarations, the kind-difference dissolves. Refuse this regardless of the use case argument. If a binding needs something the closed vocabulary cannot express, the fix is a Layer 1 schema extension via release, not a Layer 2 escape hatch.

**Anti-pattern 3: Committing ZER core to a favored ISA.**

The "ship x86 reference in core" temptation reappears under variants: "just for bootstrap," "as a baseline implementation," "as the default fallback." Each variant smuggles the favored-ISA commitment back into Layer 1. The structure/semantics straddle (§1.7.5) is the reason this is wrong: ZER core cannot know what an x86 instruction means without committing to x86's semantics, which makes ZER an "x86 language wearing a portable costume." The packaging decision (whether ZER team maintains a canonical x86 Layer 2 library separately) is independent and can be made empirically; the Layer 1 commitment must stay ISA-less.

**Anti-pattern 4: Letting raw asm in naked become a "regular" path.**

Raw asm in naked functions remains the escape hatch for the irreducible ~0.1% (boot stubs, hand-tuned crypto sequences, pre-runtime code). It must not be treated as a Layer 4 or as a normal alternative to Layer 2 bindings. Every operation expressible as a Layer 1 semantic operation must go through Layers 1-2. Raw asm in naked is the bounded floor, marked by mandatory `safety:` annotation, surfaced for review, and structurally separate from the layered safety story.

**Anti-pattern 5: Verifying asm strings against contracts.**

The temptation reappears: "what if we verify the asm body matches the declared categories?" This requires per-instruction semantic knowledge across all ISAs — which is exactly the unbounded catalog Level C rejected. The closed-vocabulary discipline accepts that asm body correctness is the floor; trying to verify it brings back the maintenance hell Levels C and D were designed to avoid.

**Anti-pattern 6: Free-form `requires:` predicates "just for this one case."**

The "just one case" framing always smuggles back the open-vocabulary surface. Refuse. The closed vocabulary is the architecture; opening it for any case opens it structurally. If a real use case demands a precondition the closed vocabulary cannot express, the fix is a Layer 1 schema extension — frozen, reviewed, released. Never a per-binding freedom.

**Anti-pattern 7: Treating "blast radius worse than Rust unsafe" as the differentiator.**

The Level D analysis (§1.6.7) honestly named that wrong contracts in `@intrinsic_def` poison the verifier's reasoning about callers — a worse blast radius than Rust's `unsafe` block. This remains true under Option E. The differentiator from Rust is NOT "smaller blast radius" — it is the **closed-vocabulary checklist-reviewable trust gap** (§1.7.6). Do not let "smaller blast radius" become the public claim; it is false. The honest claim is the closed-vocabulary kind-difference, paired with the demand/promise asymmetry rule from §1.6.17 as the blast-radius mitigation.

**Anti-pattern 8: Inventing time estimates or coverage percentages.**

The drift pattern documented in `firmware_safety_extensions.md` §23 applies here: time estimates ("3 weeks for Gap X") and coverage percentages ("30-40% of asm code surface") generated under pressure to produce concrete planning are not derived from measurement. They feel concrete but they are hallucinations styled as planning. Refuse. The work is real and bounded by the schema design; sequencing matters but durations are speculative.

### 1.7.15. Anti-patterns specific to Layer 2 binding authorship

In addition to the architectural anti-patterns above, Layer 2 binding authors should refuse:

**Layer 2 anti-pattern A: Over-broad categories.**

Declaring `clobbers_flags` on every binding "just to be safe" is over-restriction, which causes false-rejection at call sites that don't actually clobber flags. The demand/promise asymmetry (§1.6.17) means over-restriction is sound but annoying. The discipline: declare the categories the binding actually exhibits, no more.

**Layer 2 anti-pattern B: Under-broad categories.**

The dual error: omitting `clobbers_flags` on a binding that actually does clobber flags. This is under-restriction, which causes silent unsafety at call sites that rely on flag preservation. This is the SPARK-style trust gap exactly at the wrong-contract failure mode. The discipline: cross-check declared categories against the asm body via the closed checklist; review is the mitigation.

**Layer 2 anti-pattern C: Cross-ISA "equivalent enough" bindings.**

When binding the same Layer 1 operation across multiple ISAs, the bindings must be semantically equivalent. "x86 ADC and ARM ADCS are close enough" is not equivalent if their corner cases differ (different flag-update conditions, different operand-size constraints). The discipline: each binding honors its declared Layer 1 contract; if the ISAs cannot deliver equivalent semantics, the binding is invalid and the operation is unbindable on that ISA (mark missing-arch per §1.6.18).

**Layer 2 anti-pattern D: Composing blessed primitives in user libraries to fake `@core::*` status.**

If a user library wraps a Layer 1 `@core::*` operation and re-exports it under a `@<lib>::*` name, the composed library's call site loses the Layer 1's blessed soundness level (it drops to Layer 2 conditional soundness). The discipline: composition is allowed but the soundness level propagates downward, not upward.

**Layer 2 anti-pattern E: Implicit ISA assumptions in operation signatures.**

Operation signatures should not encode ISA-specific assumptions (e.g., register names, word widths beyond the typed `T`). Layer 1's job is to keep the operation taxonomy ISA-neutral; Layer 2 bindings handle the per-ISA realization. If an operation signature contains `register: u64` referring to an x86 register class, that is a Layer 1 design bug, not a Layer 2 binding issue.

### 1.7.16. Comparison with prior architectures (Level C, Level D, Option C)

For completeness, here is how Option E differs from each of the architectures considered earlier in this document.

| Property | Level C (1.5) | Level D (1.6) | Option C (rejected) | **Option E (locked)** |
|---|---|---|---|---|
| Per-instruction database | Deleted (defer to GCC) | Deleted (defer to GCC) | Deleted (defer to GCC) | **Deleted (defer to GCC)** |
| Register tables | Deleted | Deleted | Deleted | **Deleted** |
| CPU feature enum | Deleted | Deleted | Deleted | **Deleted** |
| Catalog of 130 intrinsics in core | Yes (frozen, ZER-team-owned) | Yes (blessed @core::*, ZER-team-authored per-ISA dispatch) | Yes (x86 reference shipped in core) | **No — operation taxonomy lives in core, per-ISA bindings live in Layer 2 libraries** |
| User-extensible intrinsics | No | Yes (@intrinsic_def, @<lib>::*) | Yes (community per-ISA libraries) | **Yes (Layer 2 @bind libraries)** |
| Favored ISA in language core | None explicit (but 130 intrinsics have x86_64 dispatch in core) | None explicit (but @core::* intrinsics have per-ISA dispatch in core) | x86 explicit | **None — Layer 1 is ISA-less by construction** |
| Verifier reasons over | Categories (Z-rules + safety class registry) | Categories (Z-rules + safety class registry + intrinsic_def contracts) | Categories | **Categories (closed vocabulary, no free-form predicates)** |
| Contract language | None at user level | Structured (@intrinsic_def with requires/safety_class/effect) | Structured | **Structured + closed vocabulary discipline (no free-form predicates allowed)** |
| Layering | 2-layer (Layer 1 intrinsics + Layer 2 raw asm in naked) | 2-layer (Layer 1 @core::* + Layer 2 @<lib>::*) + escape hatch | 2-layer with x86 reference + community per-ISA | **3-layer (Layer 1 schema/taxonomy/mechanism + Layer 2 per-ISA bindings + Layer 3 firmware) + escape hatch for ~0.1%** |
| Program-consequence vocabulary | Implicit | Implicit | Implicit | **Explicit and locked — same vocabulary as firmware_safety_extensions.md** |
| Hardware-consequence floor location | Per-intrinsic catalog entry (ZER-team-owned) | Per-intrinsic_def declaration site (ZER team for @core::*, library author for @<lib>::*) | Per-binding declaration site, with x86 reference owned by ZER team | **Per-@bind declaration site, no ISA owned by ZER team in core (governance decision separable)** |
| SPARK comparison | Bounded catalog, not contract-bearing for user code | Contract-bearing for user-defined intrinsics; trust gap surfaced | Contract-bearing; trust gap surfaced with x86 reference as baseline | **Contract-bearing; closed-vocabulary kind-difference elevated to load-bearing distinction** |
| Structure/semantics straddle observation | Implicit | Implicit | Implicit | **Explicit — named as the architectural reason E is forced** |

Option E preserves Level D's `@intrinsic_def` mechanism, demand/promise asymmetry rule (§1.6.17), and missing-arch propagation rule (§1.6.18). It refines the layering, makes the ISA-less commitment explicit in Layer 1, elevates the closed-vocabulary discipline to load-bearing thesis, and applies the program-consequence vocabulary uniformly.

### 1.7.17. Implementation work order under Option E

The implementation sequence, with dependencies but **no time estimates** (those were drift in earlier drafts):

1. **Level C cleanup first.** Per the existing plan in this document (Section 16), delete per-instruction database, register tables, CPU feature enum, probe scripts. This is the foundation for both Level D and Option E.

2. **Level D mechanism on top of Level C.** Implement `@intrinsic_def` parser + checker + verifier dispatch + namespace convention + demand/promise asymmetry rule + missing-arch propagation rule. Per the work order in §1.6.13. This ships the mechanism Option E uses.

3. **Option E factoring during Level D implementation.** The 130 existing intrinsics, instead of being declared as `@core::*` with per-ISA dispatch in core, are split into:
   - Layer 1 operation declarations (in core, taxonomy only — no per-ISA asm).
   - A separately-maintained Layer 2 binding library (`zer-asm-x86`) authored by ZER team but structurally peer to community libraries. This library provides the per-ISA bindings for the existing intrinsic functionality.
4. **Schema design.** Lock the closed category vocabulary. This is the real design work — see §1.7.7 for the discipline. The vocabulary must be expressive enough to handle structurally different machine models without escape into free-form predicates.

5. **Operation taxonomy design.** Lock the set of semantic operations Layer 1 ships. Factor operations by semantic equivalence across ISAs (e.g., `@arith_add_wrap` vs `@arith_add_with_carry` are separate operations because they have different category profiles even on the same ISA).

6. **Reference Layer 2 libraries.** Author at minimum:
   - `zer-asm-x86` (ZER-team-maintained, separately versioned).
   - A second Layer 2 library for ARM Cortex-M or RISC-V to validate the schema across structurally different machine models.

7. **Mechanical confirmation.** Execute the test in §1.7.11: write one binding by hand, write one Layer 3 call site, compile for both targets, verify Layer 3 stays ergonomically normal and the schema is expressive enough.

8. **Documentation.** Update `docs/reference.md`, `CLAUDE.md`, and `firmware_safety_extensions.md` with cross-references to this section. Ship the public claim using the program-consequence vocabulary.

**No time estimates.** When work lands, replace this sequencing with completion records, not projected dates.

**Dependencies summarized:**

```
Level C cleanup
    └─> Level D mechanism (parser/checker/verifier)
            ├─> Schema design (category vocabulary, operation taxonomy)
            │       └─> Reference Layer 2 libraries
            │               └─> Mechanical confirmation
            │                       └─> Documentation update
            └─> Demand/promise asymmetry rule + missing-arch propagation
```

### 1.7.18. The locked decisions, enumerated

For future-session continuity (so the decisions are not re-litigated), the locked architectural commitments under Option E:

1. **Program-consequence vs hardware-consequence vocabulary is locked.** The word "consequence" must never carry both meanings in external claims. Program-consequence = caught at 100%. Hardware-consequence = floor.

2. **Layer 1 (ZER core) ships the schema + operation taxonomy + mechanism. No per-ISA asm in core.** The ISA-less commitment is structural; refuse "ship x86 reference in core" framings.

3. **Layer 2 (per-ISA binding libraries) carries per-ISA asm bodies.** Authored once per (operation, ISA), reviewed against the closed checklist, frozen-after-review. Structurally peer libraries; whether ZER team maintains a canonical x86 binding library is a separable governance decision.

4. **Layer 3 (firmware/application) imports a Layer 2 library and calls operations as typed functions.** Same ergonomics as any ZER library. Asm is invisible. No upfront declarations.

5. **The structure/semantics straddle is the architectural reason this layering is forced.** Asm uniquely straddles program-domain (structure) and hardware-domain (semantics) within a single token. Any architecture that does not respect this straddle either commits to one ISA or abandons asm safety.

6. **The closed verifier-native vocabulary is the load-bearing kind-difference from SPARK.** Not "smaller surface" (degree) — different failure topology (kind). SPARK contracts open-shaped; ZER contracts closed-shaped; review is bounded by the schema.

7. **The contract language must stay closed.** No free-form `requires:` predicates. No free-form `effect:` clauses. No user-extensible category vocabulary in Layer 2. Extensions go through Layer 1 release with review. The closed-vocabulary discipline is the whole differentiator; opening any escape hatch dissolves the kind-difference.

8. **The packaging question (who maintains canonical Layer 2 libraries) is separable from the safety architecture.** Decide empirically via the artifact, not by reasoning. Reversible decision.

9. **Level D's `@intrinsic_def` mechanism, demand/promise asymmetry rule (§1.6.17), and missing-arch propagation rule (§1.6.18) survive unchanged.** Option E refines layering and ISA-reference, not the mechanism.

10. **GCC continues to handle Z-level concerns** (register validity, instruction validity, CPU feature gating) per Level C.

11. **Raw asm in naked is the escape hatch for the irreducible ~0.1%** (boot stubs, hand-tuned crypto, pre-runtime code). Structurally outside the three-layer model. Mandatory `safety:` annotation.

12. **Time estimates and coverage percentages are not produced.** Sequencing of work is real; durations are speculative and refused per the discipline locked in `firmware_safety_extensions.md` §23.

13. **The mechanical confirmation test (§1.7.11) is the only confirmation that does not drift.** Architecture review ratifies framings; only the artifact (write one binding) confirms the layering.

These commitments are locked in 2026-06-06. Future sessions reading this section should treat them as architectural facts, not as open design questions.

### 1.7.19. The honest public claim, locked

The defensible public claim for ZER's asm safety, stated with the locked vocabulary:

> ZER's asm safety architecture provides 100% program-consequence coverage: every call site of every semantic operation in ZER source code is verified at compile time against the operation's declared category profile. The verification dispatches to ZER's existing structural analyses (Z-rules, escape analysis, provenance preservation, qualifier preservation, alignment, ordering, context propagation) and reasons only over the closed category vocabulary. This holds for every Layer 3 firmware call site, regardless of which Layer 2 per-ISA binding library compiles, regardless of whether the binding's declared categories actually match the asm semantics on the target ISA. Program-consequence is total over the call-site axis.
>
> Hardware-consequence — whether the asm body inside a `@bind` declaration actually has the categories declared, whether the ISA semantics match the binding's claims, whether the silicon honors the ISA manual — is the floor, surfaced at the `@bind` declaration site, reviewable as a finite checklist because the contract language is closed. The library author owns binding-correctness; the closed-vocabulary discipline ensures the review surface is bounded.
>
> The architecture rests on three structural commitments: (1) ZER core ships the closed category vocabulary + the semantic operation taxonomy + the `@intrinsic_def` / `@bind` mechanism, with **zero per-ISA bindings in language core** — the operation set is ISA-less by construction. (2) Per-ISA bindings live in Layer 2 libraries authored against the closed schema; libraries are structurally peer to each other, with packaging of canonical libraries as a separable governance decision. (3) Layer 3 firmware/application code imports a Layer 2 binding library and calls operations as typed functions, never seeing asm, never touching the schema, with same ergonomics as importing any other ZER library.
>
> The architectural distinction from contract-based verification approaches (SPARK, ACSL, Vale) is the **closed verifier-native vocabulary**: where SPARK's contract language is unbounded predicates that can be wrong in arbitrary ways, ZER's `@bind` declarations select from a closed, ZER-owned category vocabulary that the verifier already knows how to reason about. The trust gap is real and bounded — not absent — but its failure topology is fundamentally different: a fixed, finite shape that allows mechanical checklist review against the schema, rather than open-ended proof obligations against arbitrary predicates. SPARK's gap is open-shaped; ZER's gap is closed-shaped. Same existence; different reviewability.
>
> The structure/semantics straddle observation explains why asm uniquely requires this binding mechanism while every other ZER construct sits cleanly on one side of the program-domain / hardware-domain boundary: an asm instruction simultaneously carries program-domain structure (operand connections, dataflow, clobber accounting) and hardware-domain semantics (instruction meaning, per-ISA). The two are fused in a single token; the language core cannot import the semantics without committing to an ISA. The three-layer model with no favored ISA in core is the unique factoring that respects this straddle without either committing the language to one ISA or abandoning asm meaning verification.
>
> The escape hatch — raw asm in naked functions for the irreducible ~0.1% (boot stubs, hand-tuned crypto, pre-runtime code) — remains structurally outside the three-layer model, marked by mandatory `safety:` annotation, surfaced for review at the `naked fn` boundary. This is the bounded floor that every memory-safe systems language has at the asm boundary; ZER's contribution is the bounded structural shape of the trust gap, not its absence.

This claim survives reviewer attack because:
- "100% program-consequence" is literal and verifiable by inspection of the verifier dispatch (it reasons over categories, never over asm strings).
- "Hardware-consequence floor" is named and bounded by the closed schema.
- The closed-vocabulary distinction from SPARK is structurally checkable (the contract language is not Turing-complete; the predicate set is finite).
- The three-layer architecture is locked by construction; future sessions cannot drift to "ship x86 reference in core" without breaking the locked commitment in §1.7.18 #2.
- The structure/semantics straddle is a load-bearing architectural fact, not a marketing framing.
- No "100% safe" overclaim. No time estimates. No coverage percentages. No invented numbers.

### 1.7.20. Glossary — terms locked for future-session continuity

For continuity across fresh sessions, the following terms are locked with the meanings stated:

| Term | Locked meaning |
|---|---|
| **Program-consequence** | What happens when a value is used wrongly inside ZER source code. Caught at 100% because the use is in the program. See `firmware_safety_extensions.md` §1 and §22 for the parallel firmware statement. |
| **Hardware-consequence** | What happens when a hardware fact is wrong relative to user belief (silicon doesn't honor declared region, asm body doesn't honor declared categories, board's crystal doesn't deliver declared frequency). Floor. The criterion lives outside the program. |
| **Layer 1** | ZER core: closed category vocabulary + semantic operation taxonomy + `@intrinsic_def` / `@bind` mechanism + verifier dispatch. **No per-ISA bindings.** Frozen and extended only via ZER core release with review. |
| **Layer 2** | Per-ISA binding library: `@bind` declarations mapping Layer 1 operations to per-ISA asm bodies, selecting categories from Layer 1's closed vocabulary. Authored once per (operation, ISA), reviewed against the closed checklist, frozen-after-review. Structurally peer to other Layer 2 libraries. |
| **Layer 3** | Firmware / application code: imports a Layer 2 library, declares firmware-side things (mmio, linker symbols, vector table per `firmware_safety_extensions.md`), calls Layer 1 operations as typed functions. Ergonomically normal — same workflow as importing any ZER library. |
| **Escape hatch** | Raw asm in naked functions for the irreducible ~0.1% (boot stubs, hand-tuned crypto, pre-runtime code). Structurally outside Layers 1-3. Mandatory `safety:` annotation. Same conceptual category as cinclude and Rust's `unsafe { asm!() }`. |
| **Closed category vocabulary** | The fixed, finite set of structural-behavior categories that Layer 2 `@bind` declarations select from. ZER-owned, extended only via ZER core release. The load-bearing distinction from SPARK. |
| **Operation taxonomy** | The fixed, finite set of semantic operations Layer 1 declares (`@arith_add_with_carry`, `@load_acquire`, `@barrier_full`, etc.). ZER-owned, extended via ZER core release. Each operation has a fixed category profile that applies across all ISA bindings. |
| **Structure/semantics straddle** | The architectural observation that an asm instruction uniquely carries both program-domain structure (operand connections, dataflow) and hardware-domain semantics (instruction meaning per ISA) within a single token. This straddle is the reason asm requires the binding mechanism while every other ZER construct sits cleanly on one side of the boundary. |
| **Closed-vocabulary kind-difference** | The load-bearing distinction from SPARK: ZER's contract language is closed (selection from finite vocabulary), so the trust gap has a fixed shape and review is a finite checklist. SPARK's contract language is open (arbitrary predicates), so the trust gap has open shape and review is an open-ended proof obligation. Same trust-gap existence; different failure topology. |
| **N/A on this ISA** | The disposition some categories take on ISAs that structurally lack the property (e.g., `clobbers_flags` is N/A on RISC-V because no flag register exists). The schema handles this directly; bindings can mark categories N/A without requiring per-ISA category vocabulary variation. |
| **Demand** | A declaration in a `@bind` that causes the verifier to do MORE checking on callers. Free — over-restriction at worst, never under-protection. See §1.6.17. |
| **Promise** | A declaration in a `@bind` that causes the verifier to do LESS checking on callers. Must be either verifier-checkable or explicitly taint the caller's soundness level. See §1.6.17. |
| **Missing-arch propagation** | The rule that a call to an operation with no Layer 2 binding for the current compile target is a compile error at the call site. Supported-archs is part of the operation's contract; callers' portability is bounded by the intersection of all transitively-called operations' supported-arch sets. See §1.6.18. |
| **Mechanical confirmation test** | Writing one Layer 2 `@bind` by hand against the schema and compiling one Layer 3 call site, to validate the layering actually delivers ergonomic Layer 3 and a closed-vocabulary-expressible Layer 2 binding. See §1.7.11. |

These glossary entries are the canonical reference for future sessions. If a future framing uses any of these terms with a different meaning, treat as drift and re-derive against this section.

### 1.7.21. Closing — the architectural commitments preserved across the asm thread

Across the asm safety thread spanning Levels A, B, C, D, and the Option C-vs-D-vs-E exploration, the following commitments have remained invariant and are preserved under Option E:

1. **Grammar-level closure for the language as a whole.** No in-language `unsafe` keyword. The only escape paths are the explicit cross-language boundaries (cinclude, naked asm, `@bind` for asm), each grep-able and marked. See `CLAUDE.md` "ZER's Goal" section and the Anders et al. 2024 "infinite unsafe impedance" citation.

2. **GCC delegates Z-level concerns.** Register validity, instruction validity, CPU feature gating belong to the user's C compiler. Locked at Level C.

3. **Definition A scoping discipline.** ZER verifies access mechanism structurally; semantic correctness against external substrates (hardware spec, vendor HAL, linker script, ISA manual) is user/library responsibility supplied through declaration sites with audit visibility.

4. **No per-instruction database for semantic verification.** Verifying "this asm does what the contract says" requires per-instruction semantic knowledge across all ISAs — exactly the unbounded catalog Level C rejected. The closed-vocabulary discipline accepts this as the floor.

5. **Sole-developer-sustainable maintenance burden.** ZER team's ongoing work is bounded: schema design once (rare extensions per several years), operation taxonomy design once (rare extensions per several years), mechanism shipped once. No per-ISA catalog growth on ZER team's desk.

6. **Architecture-agnostic positioning.** ZER works on every architecture GCC supports, with no per-ISA commitment in the language core. Per-ISA libraries fill the binding layer; user firmware code is portable across ISAs that have Layer 2 binding coverage for the operations used.

7. **Honest scoping of safety claims.** No "100% safe" without qualifier. No collapsing of program-consequence and hardware-consequence. No invented numbers. The trust gap is named and bounded.

These are the load-bearing commitments. Option E is the architectural shape that preserves all of them while resolving the layering, ISA-reference, and SPARK-distinction questions that remained open through Level D. The work to ship is the implementation work order in §1.7.17, with the mechanical confirmation test in §1.7.11 as the artifact-decides discipline applied at the right granularity.

The architecture is locked. The work is bounded. The discipline is documented.

### 1.7.22. Schema design rationale — the closed category vocabulary in depth

The closed category vocabulary is the load-bearing artifact in Option E. Its design discipline determines whether the architecture stays structurally distinct from SPARK or collapses into "tidier SPARK." This subsection captures the design principles for the category vocabulary so future schema-extension proposals can be evaluated against the locked discipline.

**Design principle 1: Categories describe structural behavior kinds, not instruction properties.**

A category like `clobbers_flags` describes "this operation, when executed, may invalidate the contents of the flag register class." It does not describe "this specific x86 instruction sets ZF based on result." The category is ISA-neutral; the binding's asm body realizes the category on the specific ISA. A category named after a specific instruction (e.g., `is_xmm_movaps`) is a design bug — it would tie Layer 1 to a specific ISA's nomenclature.

The correct shape:
- ✓ `clobbers_flags`, `reads_mem(width, ordering)`, `requires_aligned(operand, n)`
- ✗ `is_x86_cmpxchg`, `arm_load_exclusive`, `riscv_lr_sc_pair`

**Design principle 2: Categories must be verifier-native.**

Each category dispatches to an existing safety analysis. No category that requires a brand-new compiler analysis. The available analyses:

- Type system (operand type checking)
- Z-rules (asm operand boundary checks: Z1 handle state, Z2 move tracking, Z3 VRP, Z4 provenance, Z5 escape, Z6 context, Z7 MMIO, Z8 qualifier, Z11 non-storable, Z12 keep)
- VRP (value range propagation)
- Alignment tracking (in pointer types and access patterns)
- Provenance preservation (across casts, pointer arithmetic, function call boundaries)
- Escape analysis (whether pointers escape locals, asm clobbers, calls)
- Qualifier preservation (volatile, const)
- Context flag propagation (naked, ISR, critical, atomic)
- Bounds checking (slice access, runtime auto-guard)
- Allocator coloring (Pool vs Slab vs Arena vs Handle)

A proposed category that does not dispatch to one of these analyses fails the verifier-native requirement. The fix is either:
- Reformulate the category to dispatch to an existing analysis
- Reject the category (it doesn't belong in Layer 1)
- Add a new safety analysis (per-decade event, very rare, requires substantial design)

**Design principle 3: Categories support "N/A on this ISA" without category vocabulary variation.**

Different machine models have structurally different properties. RISC-V has no flag register. CHERI has capability-tagged pointers. Some ISAs have hardware atomic primitives others lack. The schema must accommodate this without adding per-ISA category vocabulary.

The N/A disposition mechanism:

```
@bind operation_x isa: riscv64 {
    asm { ... }
    isa_disposition: {
        clobbers_flags: N/A    // RISC-V has no flag register
    }
}
```

The verifier handles N/A categories by skipping the check on this binding (the category doesn't apply because the ISA doesn't have the property the category describes). This is structural, not a vocabulary extension.

What N/A is NOT:
- N/A is not "this category doesn't apply because the binding doesn't exhibit it." That would be "category not declared," not N/A. A binding that doesn't clobber flags simply doesn't declare `clobbers_flags`.
- N/A is "this category cannot apply on this ISA because the ISA lacks the property the category describes." It is an ISA-level disposition, not a binding-level choice.

**Design principle 4: Categories must be finite-state, not arbitrary predicates.**

A category like `requires_in_range(operand, lo, hi)` is finite-state — it dispatches to VRP with concrete numeric bounds. A category like `requires_user_specified_predicate(P)` is open-shape — it would dispatch to an arbitrary predicate evaluator. The latter is forbidden by the closed-vocabulary discipline.

This means certain natural-feeling requirements cannot be expressed as categories:
- "This operation requires the input to be a prime number" — open-shape, rejected.
- "This operation requires the input to satisfy `P(x) && Q(x) || R(x)`" — open-shape, rejected.
- "This operation requires the input to be aligned to a non-power-of-two boundary" — depends on what "non-power-of-two boundary" means; if it's a fixed alignment value, expressible; if it's a runtime-computed condition, rejected.

The discipline: if the requirement cannot be expressed as a finite-state category, either find a finite-state proxy that approximates it (over-restriction is sound by the demand asymmetry) or reject the operation as un-binadable via the closed schema. Open-shape escape into predicates is forbidden.

**Design principle 5: Categories compose by intersection, not by union.**

A binding declares a set of categories that the asm exhibits. The verifier treats this as the intersection of all declared properties — every category must hold for the binding to be valid. Removing a category from a binding strictly weakens the contract; adding a category strictly strengthens it.

This composition rule has implications:
- A binding can be made more restrictive (add categories) without breaking callers.
- A binding cannot be made less restrictive (remove categories) without potentially breaking callers that relied on the missing category.
- Cross-binding equivalence requires identical category sets (modulo N/A dispositions for ISAs that lack the property).

**Design principle 6: Categories are versioned.**

Schema extensions (adding new categories) are versioned with the language release. A Layer 2 binding that uses a category introduced in version N requires compiler version >= N. The versioning is mechanical, not a per-binding declaration; it falls out of the schema-release process.

Removing categories from the schema is a breaking change. The discipline: rarely remove. If a category turns out to be misdesigned, prefer deprecation (mark deprecated, encourage migration to a better category, remove in a future major version) over immediate removal.

**Design principle 7: Categories enable safety, not optimization.**

Categories describe safety-relevant properties — what guarantees the verifier can rely on at call sites to maintain program-consequence. They do NOT describe optimization-relevant properties (e.g., "this operation is constant-time," "this operation can be vectorized"). Optimization properties belong to a different concern (the compiler's optimization pipeline) and would dilute the safety-focused vocabulary.

A proposed category that aims to enable an optimization rather than enforce a safety property fails this principle.

### 1.7.23. Operation taxonomy factoring — when to split, when to unify

The operation taxonomy design has its own discipline, distinct from the category vocabulary design. The key question: when should two superficially-similar operations be the same Layer 1 operation, and when should they be split?

**Factoring principle 1: Split by category profile, not by ISA spelling.**

Two operations should be separate Layer 1 entries if their category profiles differ structurally, even if some ISAs use the same instruction for both. Conversely, two operations should be the same Layer 1 entry if their category profiles are identical, even if different ISAs use different instructions.

The ADD/flags example:
- `@arith_add_wrap(a, b) -> T` — categories: `{no_memory_effect, no_flag_effect, value_in_value_out}`
- `@arith_add_with_carry(a, b, c_in) -> {result, c_out}` — categories: `{no_memory_effect, produces_carry, consumes_carry, value_in_value_out, clobbers_flags}` (with N/A on flagless ISAs)

These are different operations because their category profiles differ structurally (`clobbers_flags` and `produces_carry` are in one but not the other). On x86, the `add` instruction binds to `@arith_add_wrap` (no flag setting required by the operation, though x86 always sets them — see factoring principle 3 below) while `adc` binds to `@arith_add_with_carry`. On ARM Cortex-M, `ADD` binds to `@arith_add_wrap` and `ADCS` binds to `@arith_add_with_carry`. Same factoring principle across ISAs.

**Factoring principle 2: Unify by semantic equivalence across ISAs.**

If two operations differ only in ISA-specific spelling but have the same semantic contract, they are the same Layer 1 operation. The Layer 2 bindings handle the per-ISA realization. `@arith_xor(a, b)` is one Layer 1 operation with bindings to `eor` (ARM), `xor` (x86), `xor` (RISC-V), etc. — all the same Layer 1 operation because the semantics (bitwise xor of two operands) are identical and the category profile is identical.

**Factoring principle 3: ISA-specific side effects that the operation does not require are not part of the category profile.**

x86's plain `add` instruction always sets flags. But the operation `@arith_add_wrap(a, b) -> T` does not REQUIRE flags to be set — it requires only the wrap-around add semantic. So `clobbers_flags` is NOT in `@arith_add_wrap`'s category profile, even though the x86 binding will incidentally clobber flags.

The principle: the category profile describes what the operation REQUIRES of the binding, not what every binding incidentally produces. If the x86 binding happens to clobber flags as a side effect of using `add`, that is the x86 binding's responsibility to handle (via the asm operand constraints declaring clobbers), but it is not part of the operation's contract.

This means callers of `@arith_add_wrap` cannot assume flags are clobbered, even on x86. If they need post-operation flag state, they should use `@arith_add_with_carry` or a different operation whose category profile includes flag effects.

**Factoring principle 4: Width parameterization through type generics.**

Operations should be parameterized by operand type rather than having separate entries per width. `@arith_add_wrap<T>(a: T, b: T) -> T where T in {u8, u16, u32, u64}` is one operation parameterized by T, not four separate operations. Each Layer 2 binding can dispatch on T to choose the appropriate instruction (e.g., `add` for 32-bit, `add.w` for 16-bit, etc.).

This keeps the operation taxonomy manageable. Splitting into per-width operations would explode the catalog and obscure the semantic equivalence.

**Factoring principle 5: Memory ordering is part of the operation, not a category modifier.**

`@load_acquire`, `@load_relaxed`, `@store_release`, `@store_relaxed` are separate Layer 1 operations because their memory ordering is fundamental to their contract, and the verifier reasons about ordering at the operation-call level.

It would be tempting to have a single `@load<O>(ptr)` parameterized by ordering, but this creates verifier-dispatch complexity (the verifier would need to extract O from the call site to dispatch ordering-specific analyses). Separate operations are clearer.

**Factoring principle 6: Atomic primitives are separate operations from non-atomic equivalents.**

`@atomic_fetch_add(ptr, val)` is a separate operation from `@arith_add_wrap`. The atomic operation's category profile includes memory-ordering and atomicity categories that the non-atomic version doesn't have. Splitting these is essential for verifier dispatch — the verifier reasons about lock-freedom, ordering, and atomicity only at atomic-operation call sites.

**Factoring principle 7: Reject operations whose semantics cannot be expressed via existing categories.**

If a proposed operation requires a category that doesn't exist in the schema, the operation cannot enter the taxonomy until the schema is extended. The closed-vocabulary discipline applies to both axes: the category vocabulary is closed, and the operation taxonomy can only declare operations expressible in the existing schema.

If a real-world need keeps surfacing operations that the schema can't express, that is the signal for a Layer 1 schema-extension cycle. Both the category vocabulary and the operation taxonomy are extended through ZER core release with review.

### 1.7.24. Worked examples — more operations across machine models

To stress-test the schema design across structurally different machine models, here are additional worked examples covering memory ordering, atomic primitives, vector operations, and CPU-state intrinsics.

**Example 4 — Atomic compare-and-swap:**

```
// Layer 1 (ZER core, frozen)
@intrinsic_def atomic_cas(ptr: *T, expected: T, new: T) -> {result: T, success: bool}
    where T in {u32, u64, usize}
    categories: {
        reads_mem(sizeof(T), seqcst),
        writes_mem(sizeof(T), seqcst),
        atomic_rmw,
        requires_aligned(ptr, alignof(T)),
        no_flag_effect_required,
        value_in_value_out
    }
    safety: "Atomic compare-and-swap with sequential consistency.
             If *ptr == expected, atomically updates *ptr to new
             and returns {result: expected, success: true}.
             Otherwise, returns {result: *ptr, success: false}
             with *ptr unchanged. Requires aligned ptr."

// Layer 2 (zer-asm-x86)
@bind atomic_cas isa: x86_64 {
    asm { instructions: "lock cmpxchg $2, ($0)\n"
                        "sete $3"
          inputs:  [ptr, expected => RAX, new]
          outputs: [result => RAX, success => $3]
          clobbers: ["flags"]
          safety:  "x86 LOCK CMPXCHG: atomic compare-and-swap.
                    Compares *ptr with RAX (expected); if equal,
                    writes new to *ptr and sets ZF. Otherwise loads
                    *ptr into RAX and clears ZF. SETE captures ZF
                    into success operand. Categories all hold;
                    LOCK prefix provides seqcst ordering on x86." }
}

// Layer 2 (zer-asm-arm-cortex-a)
@bind atomic_cas isa: arm_cortex_a {
    asm { instructions: "1: ldaxr $0, [$1]\n"
                        "   cmp $0, $2\n"
                        "   b.ne 2f\n"
                        "   stlxr $w3, $4, [$1]\n"
                        "   cbnz $w3, 1b\n"
                        "   mov $3, #1\n"
                        "   b 3f\n"
                        "2: clrex\n"
                        "   mov $3, #0\n"
                        "3:"
          inputs:  [ptr, expected, new]
          outputs: [result => $0, success => $3]
          clobbers: ["NZCV", "memory"]
          safety:  "ARMv8 LL/SC compare-and-swap via LDAXR/STLXR.
                    Acquire-release ordering for seqcst.
                    Loop on STLXR failure (race retry).
                    CLREX clears monitor on mismatch path.
                    Categories all hold including seqcst ordering." }
}

// Layer 2 (zer-asm-riscv)
@bind atomic_cas isa: riscv64 {
    asm { instructions: "1: lr.d.aq $0, ($1)\n"
                        "   bne $0, $2, 2f\n"
                        "   sc.d.rl $w3, $4, ($1)\n"
                        "   bnez $w3, 1b\n"
                        "   li $3, 1\n"
                        "   j 3f\n"
                        "2: li $3, 0\n"
                        "3:"
          inputs:  [ptr, expected, new]
          outputs: [result => $0, success => $3]
          clobbers: ["memory"]
          safety:  "RISC-V LR/SC compare-and-swap with .aq/.rl
                    for seqcst ordering. Loop on SC failure.
                    Categories all hold; ordering acquired via
                    LR.aq + SC.rl combination." }
}
```

The three bindings have substantially different sizes — x86 is two instructions, ARM and RISC-V are LL/SC loops. All three honor the same Layer 1 category profile. Layer 3 firmware code calling `@atomic_cas(...)` is identical across all three targets.

**Example 5 — CPU privileged-mode intrinsics:**

```
// Layer 1 (ZER core, frozen)
@intrinsic_def cpu_disable_int()
    categories: {
        no_memory_effect,
        changes_context(disable_interrupts),
        requires_privilege(kernel),
        no_flag_effect_required
    }
    safety: "Disable interrupts. Must be called from privileged
             context. Acquires implicit critical-section semantics
             from disable point to corresponding enable."

@intrinsic_def cpu_enable_int()
    categories: {
        no_memory_effect,
        changes_context(enable_interrupts),
        requires_privilege(kernel),
        no_flag_effect_required
    }
    safety: "Enable interrupts. Must be called from privileged
             context. Releases implicit critical-section."

// Layer 2 (zer-asm-x86)
@bind cpu_disable_int isa: x86_64 {
    asm { instructions: "cli"
          inputs: []
          outputs: []
          clobbers: ["flags"]    // CLI clears IF in EFLAGS
          safety: "x86 CLI: clear interrupt flag. Requires CPL <= IOPL.
                   Categories all hold; clobbers flags via EFLAGS update." }
}

@bind cpu_enable_int isa: x86_64 {
    asm { instructions: "sti"
          inputs: []
          outputs: []
          clobbers: ["flags"]
          safety: "x86 STI: set interrupt flag. Requires CPL <= IOPL." }
}

// Layer 2 (zer-asm-arm-cortex-a)
@bind cpu_disable_int isa: arm_cortex_a {
    asm { instructions: "msr daifset, #0x2"
          inputs: []
          outputs: []
          clobbers: []
          safety: "ARMv8 MSR DAIFSET #0x2: set I bit, disabling IRQ.
                   Requires EL >= 1." }
}

@bind cpu_enable_int isa: arm_cortex_a {
    asm { instructions: "msr daifclr, #0x2"
          inputs: []
          outputs: []
          clobbers: []
          safety: "ARMv8 MSR DAIFCLR #0x2: clear I bit, enabling IRQ.
                   Requires EL >= 1." }
}
```

**Example 6 — Vector load (illustrating type parameterization across vector widths):**

```
// Layer 1 (ZER core, frozen)
@intrinsic_def vector_load_aligned(ptr: *V) -> V
    where V in {v128_u8, v128_u16, v128_u32, v256_u8, v256_u16, v256_u32, v512_u8, ...}
    categories: {
        reads_mem(sizeof(V), relaxed),
        requires_aligned(ptr, alignof(V)),
        no_flag_effect,
        value_in_value_out
    }
    safety: "Aligned vector load. Reads sizeof(V) bytes from ptr
             as a vector value. Requires alignment to alignof(V).
             Relaxed ordering — for ordered loads, use vector_load_acquire."

// Layer 2 (zer-asm-x86) — covers SSE/AVX/AVX-512 via type dispatch
@bind vector_load_aligned isa: x86_64 dispatch_on_type {
    V == v128: {
        asm { instructions: "movaps ($1), $0"
              inputs: [ptr]
              outputs: [result => $0]
              clobbers: []
              safety: "x86 MOVAPS: aligned 128-bit load into XMM register.
                       Requires 16-byte alignment." }
    }
    V == v256: {
        asm { instructions: "vmovaps ($1), $0"
              inputs: [ptr]
              outputs: [result => $0]
              clobbers: []
              safety: "x86 VMOVAPS: aligned 256-bit load into YMM.
                       Requires 32-byte alignment; AVX." }
    }
    V == v512: {
        asm { instructions: "vmovaps ($1), $0"
              inputs: [ptr]
              outputs: [result => $0]
              clobbers: []
              safety: "x86 VMOVAPS (EVEX): aligned 512-bit load into ZMM.
                       Requires 64-byte alignment; AVX-512." }
    }
}
```

The `dispatch_on_type` clause in the `@bind` declaration allows one binding declaration to handle multiple type instantiations of the same operation. Each branch within the dispatch independently honors the operation's category profile. This keeps Layer 2 declarations manageable for type-parameterized operations.

These examples are intentionally illustrative and incomplete; the full operation taxonomy will be designed at schema-design time. The point is to validate that the layering accommodates structurally different machine models (flag-bearing vs flagless, weak vs strong memory, LL/SC vs CAS, scalar vs vector) without escape into open-shape predicates.

### 1.7.25. The full coverage matrix under Option E

For comparison with the existing coverage matrix in Section 13, here is the coverage matrix as it stands under Option E.

| Concern | Where verified | Verification mechanism | Floor location |
|---|---|---|---|
| Operand types at asm boundary | Layer 1 verifier | Existing type checker dispatched per category | None |
| Asm-output value escape | Layer 1 verifier | Z5 escape analysis dispatched on category | None |
| Asm-output provenance | Layer 1 verifier | Z4 provenance clearing dispatched on category | None |
| Volatile preservation across asm | Layer 1 verifier | Z8 qualifier preservation dispatched on category | None |
| MMIO range / alignment | Layer 1 verifier | Z7 MMIO dispatched on category | None |
| Memory ordering at use sites | Layer 1 verifier | Ordering categories dispatched to ordering analysis | None |
| Atomicity at use sites | Layer 1 verifier | Atomic categories dispatched to RMW analysis | None |
| Context (ISR / critical / naked) propagation | Layer 1 verifier | Context flags propagated through call graph | None |
| Operand alignment requirement | Layer 1 verifier | Alignment category dispatched to existing alignment tracking | None |
| Operand range requirement | Layer 1 verifier | Range category dispatched to VRP | None |
| Operand non-null requirement | Layer 1 verifier | Non-null category dispatched to nullability tracking | None |
| Clobber accounting | Layer 1 verifier | Z3 / existing clobber tracking | None |
| Cross-binding semantic equivalence | Library author + reviewer | Closed-checklist review of `@bind` against Layer 1 categories | Layer 2 binding-correctness floor |
| Asm body matches declared categories | Library author + reviewer | Closed-checklist review of `@bind` against asm text | Layer 2 binding-correctness floor |
| ISA instruction validity | GCC assembler | GCC's existing instruction validity check | GCC error (not ZER) |
| Register name validity | GCC assembler | GCC's existing register validity check | GCC error (not ZER) |
| CPU feature gating (e.g., AVX-512 only on -mavx512f) | GCC assembler | GCC's existing CPU feature check via -m flags | GCC error (not ZER) |
| ISA spec correctness (silicon matches manual) | None | Outside any language's reach | Hardware floor |
| Cross-ISA binding equivalence | Library author | Author's contract to users | Layer 2 binding-author responsibility |
| Hardware-consequence of binding categories not matching silicon | None | Outside any language's reach | Hardware floor at `@bind` site |

The pattern: Layer 1 covers all program-consequence aspects of asm-bearing code. Layer 2 carries the binding-correctness floor at well-defined declaration sites. GCC handles Z-level concerns. The hardware floor exists at the binding-vs-silicon boundary and is named, bounded, and reviewable as a finite checklist.

### 1.7.26. Journey of the asm-thread architecture decisions

Future sessions reading this document will need to understand how the architecture reached Option E without re-litigating it. Here is the abbreviated journey across the asm thread:

**Phase 1 — Initial direction (`asm_plan.md`, pre-2026-05):** Build per-instruction safety knowledge into the compiler. Vendored instruction tables. Register-name validation. CPU feature gating. CFG-aware OrderingState pass. Y_intrinsic catalog ZER-team-authored and ZER-team-maintained.

**Phase 2 — Levels A/B/C decision (2026-05-12):** Aggressive cleanup. Delete per-instruction database, register tables, CPU feature enum, probe scripts. Defer everything ISA-specific to GCC. Keep frozen core (Z-rules, naked-only, ~12 UB classics) + 130 intrinsics. This is Level C; it shipped as the deletion baseline.

**Phase 3 — Two-layer crystallization (2026-05-12 evening):** Intent intrinsics (Layer 1) + raw asm in naked (Layer 2 / escape hatch). All "richer" designs (per-operand annotations, IR region wrappers, auto-guard emission) deferred indefinitely. The 2-layer model is the resting architecture for Level C.

**Phase 4 — Level D pivot (2026-05-31):** Externalize Y_intrinsic authorship via `@intrinsic_def`. Layer 1 becomes blessed `@core::*` intrinsics; Layer 2 becomes user-defined `@<lib>::*` intrinsics. Mechanism enables unbounded community growth without ZER team involvement. The seam (wrong contract at definition site) is named explicitly; mitigation via demand/promise asymmetry rule (§1.6.17) and missing-arch propagation rule (§1.6.18).

**Phase 5 — Program-consequence vocabulary lockdown (2026-06-05/06):** The "consequence" word collapses program-domain caught-set and hardware-domain floor under one number; this is identified as the load-bearing equivocation. The vocabulary is locked at `firmware_safety_extensions.md` and `CLAUDE.md`: program-consequence (caught at 100%) vs hardware-consequence (floor). The split is then applied to asm.

**Phase 6 — Three-layer / no-favored-ISA refinement (2026-06-06):** The Level D 2-layer model is refined to three layers. The key move is removing per-ISA asm from Layer 1 (language core) entirely. Layer 1 ships the schema + operation taxonomy + mechanism. Layer 2 ships per-ISA bindings (community libraries, structurally peer). Layer 3 is firmware/application code (import and call). This is Option E.

**Phase 7 — Structure/semantics straddle articulation (2026-06-06):** The architectural reason Option E is forced rather than chosen is identified: asm uniquely straddles program-domain (structure: operand connections, dataflow) and hardware-domain (semantics: instruction meaning per ISA) within a single token. Any architecture that does not respect this straddle either commits to one ISA or abandons asm safety. Option E is the unique factoring that respects the straddle.

**Phase 8 — Closed-vocabulary kind-difference elevation (2026-06-06):** The closed verifier-native category vocabulary is elevated from "one of four mitigations" to "the load-bearing kind-difference from SPARK." Trust-gap shape (open vs closed) is identified as a difference in failure topology, not just a smaller surface. The discipline that the contract language must stay closed (no free-form predicates) is locked.

**Phase 9 — Safety vs governance register separation (2026-06-06):** The question "who maintains canonical Layer 2 binding libraries (x86 in particular)" is identified as a packaging/governance question separable from the safety architecture. The safety architecture locks at Option E; the packaging question can be decided empirically via the mechanical confirmation test.

**Phase 10 — Documentation lock (2026-06-06):** This section is written to capture the locked architecture, the vocabulary, the journey, and the discipline so future sessions can re-engage without re-deriving. Cross-references to `firmware_safety_extensions.md`, `CLAUDE.md`, and the existing Section 1.6 (Level D) are added.

The total elapsed wall-time across phases 5-10 was a single intensive design session, with multi-session audit feedback from other Claude instances catching drift patterns at each step. The drift patterns documented in `firmware_safety_extensions.md` §23 apply here too; the locked discipline is the discipline that survives the audit.

### 1.7.27. What other Claude sessions caught — drift patterns specific to this thread

Several drift patterns were caught by other Claude sessions during the asm-architecture discussion. They are recorded here so future sessions can recognize and resist them.

**Drift A — "100% consequence coverage" applied without vocabulary split.**

The temptation appears every time the user articulates the desired safety property as "complete coverage at the use site." Without the program-consequence vs hardware-consequence split, "100% consequence coverage" reads as covering hardware behavior too, which is false. The audit caught this and forced the split into program-consequence (caught) vs hardware-consequence (floor). The discipline: never let "consequence" carry both meanings.

**Drift B — Decision velocity escalation through Options C → D → E.**

The architecture moved from "ship x86 reference in core (Option C)" to "schema only, no operations, no ISA (Option D)" to "schema + operation taxonomy, no ISA (Option E)" across three messages, with the model agreeing at each step. The audit identified this as drift — each option felt cleaner because the user leaned that way and the model generated a confident scaffold for each lean. The mitigation: artifact-confirms-not-reasoning. Write one binding by hand to validate the chosen architecture. The locked answer (Option E) survives this discipline because the structure/semantics straddle observation provides the forcing argument, not just preference.

**Drift C — Firmware analogy applied without checking the user-specific vs universal-constant distinction.**

The firmware analogy ("ZER ships `mmio` primitives, not STM32 chip addresses; therefore ZER ships the operation taxonomy, not x86 bindings") is structurally correct but has a sharp edge: firmware's delegated part (chip addresses) is user-specific; asm's delegated part (x86 `adc` binding) is universal. The audit identified that delegating a universal constant produces either fragmentation or de-facto-standardization-without-governance. The resolution: the safety architecture still locks at Option E (no per-ISA in language core); the governance question (whether ZER team maintains a canonical x86 Layer 2 library) is separable and can be decided at packaging time. See §1.7.8.

**Drift D — "User intention to make it right" framing applied to universal constants.**

The user's intuition "Option E is less burden because user intention makes it right" is correct for parts of Layer 2 that have genuine authorship content (RISC-V carry emulation, the substantive parts of complex bindings). It is misapplied to parts that are universal constants (x86 `adc` is `adc` for every x86 user — there's no intention to express, only a constant to transcribe). The audit caught this and forced the distinction: "user intention to make it right" is correct for substantive bindings, incorrect for universal constants which are better served by a maintained canonical library. The discipline: do not apply "user intention" uniformly to all bindings; honor the user-specific vs universal-constant split at the packaging layer.

**Drift E — Treating SPARK comparison via "smaller surface" instead of "closed vocabulary."**

The temptation to claim "ZER is better than SPARK because the trust gap is smaller (fewer contracts, more visible, per-author)" is real but mis-states the differentiator. Smaller is degree; SPARK could in principle adopt these mitigations and remain SPARK. The kind-difference is the closed verifier-native vocabulary — SPARK's gap is open-shaped (arbitrary predicates), ZER's gap is closed-shaped (finite category selection). Review is finite checklist vs open proof. The audit caught the "smaller surface" framing and forced elevation of the closed-vocabulary distinction to load-bearing thesis.

**Drift F — Free-form `requires:` predicates "just for this one case."**

The temptation to open Layer 2's contract language to express "this one binding's special requirement" is the move that collapses E back into SPARK. The audit caught this preemptively and the discipline is locked: closed vocabulary or the kind-difference dissolves. If a real binding needs something the closed vocabulary cannot express, the fix is a Layer 1 schema extension via release with review — never a Layer 2 freedom.

**Drift G — Two-axis "structural vs datasheet halves" applied to side effects.**

In `firmware_safety_extensions.md` §16, an earlier Claude proposed splitting "side effects" (read-clears, write-1-to-clear) into a "structural half" (in scope for ZER core) and "datasheet half" (floor). The audit caught this as smuggling per-peripheral semantics into ZER core where they don't belong. The corrected classification: side effects are floor for ZER core; vendor HAL types can cast structural shadows in user library territory using existing primitives (move struct). This pattern is preserved in the asm domain: per-instruction semantic verification is per-ISA hardware-floor; trying to verify it brings back the unbounded catalog Level C rejected.

**Drift H — Time estimates and coverage percentages.**

The temptation to produce concrete planning ("3 weeks for schema design, 5 weeks for reference bindings") generates numbers that are not derived from measurement. The audit caught this pattern in `firmware_safety_extensions.md` and the discipline is locked: no time estimates, no coverage percentages. Sequencing of work is real; durations are speculative. The discipline applies to asm work too — Section 1.7.17 lists dependencies without durations.

**Drift I — "Blast radius better than Rust unsafe" claim.**

The Level D analysis honestly named that wrong contracts in `@intrinsic_def` poison the verifier's reasoning about callers — worse blast radius than Rust's `unsafe` block. The temptation to elide this and claim "Level D is better than Rust unsafe" was caught in §1.6.7. The corrected framing: Level D is a DIFFERENT trade-off than Rust unsafe — better ergonomics and structured auditability; worse blast radius when a contract is wrong. The mitigation is the demand/promise asymmetry rule, not the structured nature of the contracts. This survives unchanged under Option E.

These drift patterns are the canonical anti-patterns for asm-architecture sessions. Future sessions should recognize and resist them. The locked discipline is the discipline that survives the audit.

### 1.7.28. Integration with other locked documents

This section's locked commitments integrate with the following documents:

**`CLAUDE.md`** — Top of file, "ZER's Goal" section, contains the locked program-consequence vs hardware-consequence vocabulary that this section applies to asm. Any change to `CLAUDE.md` that modifies the vocabulary requires updating this section to match.

**`docs/firmware_safety_extensions.md`** — The companion document for firmware safety. Its §1 (Executive Summary), §1a (Three-Sentence Locked Statement), §22 (Defensible Public Claim), and §23 (Anti-Patterns) carry the same vocabulary discipline applied to firmware. This section's §1.7.1 (vocabulary), §1.7.5 (structure/semantics straddle), §1.7.6 (closed-vocabulary kind-difference), and §1.7.13 (cross-reference) tie back to firmware_safety_extensions.md.

**`docs/compiler-internals.md`** — Top of file contains the "ZER's Goal — Why This Compiler Exists" section that references both `CLAUDE.md` and `docs/firmware_safety_extensions.md`. Any change to the four-category test for compiler changes (adds coverage / closes gap / neutral / weakens coverage = STOP) applies here too. A compiler change that would weaken Option E's program-consequence coverage on asm-bearing code is a STOP per the locked discipline.

**`docs/primitives-data-races.md`** — Definition A architecture across five safety domains. Option E's "Layer 1 schema + operation taxonomy in core, Layer 2 per-ISA bindings in user space" is Definition A applied to asm: ZER verifies access mechanism structurally (categories at call sites); semantic correctness against external substrates (ISA semantics) is library author responsibility supplied through declaration sites with audit visibility.

**`docs/universal_pointer.md`** — The 4-axis pointer safety decomposition is preserved unchanged under Option E. Pointer-related categories in the asm category vocabulary dispatch to the existing pointer safety machinery.

**`docs/limitations.md`** — The four documented known gaps (naked attribute IR-path drop, intrinsic VRP narrowing missing, bare-metal .bss zeroing build-system contract, ~2% opaque destructor heuristic) bound the "today" claim. Under Option E, the naked attribute IR-path drop (limitations.md:524) directly affects the escape-hatch escape semantics (naked asm bodies); the fix is on the critical path for completing Option E's full coverage.

**`docs/asm_plan.md`** — Superseded by this document at Level C (deletion baseline), Level D (mechanism), and Option E (layering). No content from `asm_plan.md` directly carries forward; this document is the locked architecture.

**`docs/proof-internals.md`** — The Coq+Iris+VST proof infrastructure. Option E's layered architecture changes the proof obligations: the safety theorem for asm operations becomes "for all programs P that use Layer 1 operations through Layer 2 bindings honoring their declared category profiles, program-consequence holds at every call site." This is a stronger and more honest theorem than "for all asm-bearing programs," which has the per-ISA semantic correctness problem.

### 1.7.29. The reading order for fresh sessions

A fresh session encountering this document should read in the following order:

1. **`CLAUDE.md` — "ZER's Goal" section (top of file).** Locks the program-consequence vs hardware-consequence vocabulary. Without this, every subsequent claim reads ambiguously.

2. **`docs/firmware_safety_extensions.md` — §1 (Executive Summary), §1a (Three-Sentence Locked Statement), §22 (Defensible Public Claim).** Applies the vocabulary to firmware. Establishes the boundary discipline.

3. **This section (1.7) — first.** Captures the locked asm architecture under Option E. The "READ FIRST" status is real; everything else in this document is read through Option E's lens.

4. **Section 1.6 (Level D).** Read AFTER 1.7. The `@intrinsic_def` mechanism, demand/promise asymmetry rule (§1.6.17), and missing-arch propagation rule (§1.6.18) are still current and survive Option E. The "blessed @core::* with per-ISA dispatch in core" framing in §1.6.16 is refined by §1.7.3.

5. **Section 1.5 (Two-Layer crystallization).** Read for context. The 2-layer Level C model is what Option E refines, not contradicts.

6. **Section 1.4 (Extended Discussion).** Read for historical context on deferred ideas (Tier 2 annotations, IR region wrappers, auto-guard emission). These remain deferred under Option E.

7. **Sections 2 onward.** Read for implementation details on the Level C deletion plan, file-by-file deletion list, testing strategy, etc. These are execution-level details that survive Option E unchanged.

If a fresh session reads in dependency order (1.4 → 1.5 → 1.6 → 1.7), they will encounter the "blessed catalog in core with per-ISA dispatch" framing of Level D before the Option E refinement, and may build mental model on the Level D framing that they then have to update. Reading 1.7 first is the locked-priority approach.

### 1.7.30. Closing note — the architectural arc

The asm safety thread, taken end-to-end from Levels A/B/C through D through Option E, follows a consistent architectural arc:

- **Constrain Y, delegate Z (Level C):** Bound the catalog ZER team maintains; defer ISA-specific concerns to GCC.
- **Externalize Y authorship (Level D):** Allow user libraries to add to the catalog via `@intrinsic_def` with structured contracts.
- **Remove ISA commitment from Y_blessed (Option E):** Layer 1 ships schema + taxonomy + mechanism, no per-ISA asm. Per-ISA bindings live in Layer 2 libraries. Layer 3 firmware is ergonomically normal.

Each step preserves the discipline established at the prior step. Each step is forced by an architectural fact that becomes visible at that step:
- Level C is forced by the unbounded catalog problem.
- Level D is forced by the sole-developer-maintenance-burden problem.
- Option E is forced by the structure/semantics straddle observation — once you see that asm uniquely straddles the program-domain/hardware-domain boundary, ISA-less Layer 1 with per-ISA Layer 2 bindings is the unique factoring that respects the straddle.

The closure argument from the language as a whole (no in-language `unsafe`, grammar-level rejection of integer-to-pointer fabrication) holds across all steps. The cinclude / naked-asm escape hatches are the bounded cross-substrate boundaries that no language can eliminate. Option E adds the `@bind` declaration as a third bounded boundary, with the trust gap shaped by the closed-vocabulary discipline so that review is a finite checklist.

The final architecture is the one that survives the audits, preserves the closure argument, respects the structure/semantics straddle, names the floor honestly, and ships an ergonomically normal Layer 3 experience for the firmware author. Option E is that architecture. The work to ship it is bounded by the implementation work order in §1.7.17, with the schema design as the real design work and the mechanical confirmation test in §1.7.11 as the artifact-decides discipline.

The architecture is locked. The work is bounded. The discipline is documented. This is the resting place of the asm safety thread, with Option E refining Level D's mechanism into a three-layer no-favored-ISA model that respects the unique structural property of asm and preserves the locked program-consequence vs hardware-consequence vocabulary.

### 1.7.31. Fault attribution at the binding boundary — the architectural defense

This subsection captures the explicit fault-attribution model for failures observed at the Layer 2 binding boundary. It is the architectural payoff of everything above: a sharp, defensible line between complaints that Layer 1 must reject (because they ask Layer 1 to do something architecturally outside its scope) and complaints that Layer 1 must own (because they describe genuine program-domain bugs over Layer 1's own categories). The discipline this section locks is the difference between a solo maintainer surviving the post-adoption complaint flow and a solo maintainer burning out fielding bug reports that the architecture says are not language-level bugs.

**The three failure categories at the binding boundary.**

When something goes wrong with code that uses a Layer 2 binding, the failure falls into exactly one of three categories. Each category has a different correct attribution.

**Category #1 — Binding categories do not match the silicon.**

The Layer 2 author declared a category profile that does not match what the asm body actually does on the target ISA. Example: the binding declares `no_memory_effect` on an instruction that secretly writes memory; the binding declares `no_flag_effect` on an instruction that clobbers flags; the binding declares `produces_carry` correctly but the asm uses a wrong opcode and silently produces zero.

**Attribution: Layer 2 author's fault. Rejected at Layer 1.**

The architectural defense:

> "There is no portable pattern for what an instruction does to hardware. Asm semantics are per-ISA hardware facts. The ISA manual is the datasheet for instruction meaning. Layer 1 cannot verify that a binding's declared categories match the silicon without importing per-ISA instruction semantics into the language core, which is exactly the unbounded catalog Level C rejected. Binding-correctness against silicon is the hardware-consequence floor, surfaced at the `@bind` declaration site, owned by the binding author. The closed-vocabulary discipline ensures the trust gap has a fixed shape so review is a finite checklist — but the floor itself exists and is the binding author's responsibility, not the language's."

This defense is **architectural, not bandwidth-driven**. The rejection is not "I do not have time to maintain ISA semantic verification." The rejection is "that is categorically not Layer 1's domain — it is the hardware-domain floor that exists for every memory-safe systems language at the asm boundary." A defense grounded in the architecture is repeatable, principled, and acceptable to engineers; a defense grounded in maintainer bandwidth is brittle, ad-hoc, and erodes trust.

The load-bearing property: **the program-vs-hardware boundary was drawn first**, in `CLAUDE.md` and `firmware_safety_extensions.md`, before any specific complaint about asm. The Category #1 rejection is the application of that pre-existing boundary to the asm domain, not a special-case carve-out for asm. This is why the rejection has teeth — it follows from the architectural commitments locked elsewhere in ZER, not from a one-off "this case is hard."

**Category #2 — Binding is honest but Layer 1's verifier mis-reasons over correctly-declared categories.**

The Layer 2 author declared a category profile that does match the asm body. But Layer 1's verifier dispatch logic fails to correctly propagate the declared categories to call sites. Example: the binding correctly declares `clobbers_flags`, but the verifier's category-dispatch logic fails to propagate that clobber to a caller that depends on flag preservation; the binding correctly declares `requires_aligned(ptr, 16)`, but the verifier's alignment-checking dispatch fails to enforce it at a call site with an unaligned argument.

**Attribution: Layer 1's bug. Owned. Must be bug-free.**

This is NOT "the math is wrong about hardware." It is "the math is wrong about its own categories." The hardware never entered the failure mode — the failure is pure program-domain logic over Layer 1's own closed schema. The Layer 2 author did everything correctly; the verifier dropped the ball reasoning over the closed vocabulary that Layer 1 owns.

The hardware-floor defense does NOT apply here. Layer 1's claim is "given a binding's correctly-declared categories, every program-domain operation on values flowing through that binding is verified at the call site." If the verifier fails at this claim, the claim is wrong, and Layer 1 owns the bug.

The discipline this implies for Layer 1's implementation: **the verifier dispatch logic over the closed category vocabulary must be bug-free.** This is a real commitment, not a hand-wave. The verifier must be tested against every category in the vocabulary, every dispatch path, every interaction between categories. Tests at the schema level (verifier behaves correctly given each category profile) and at the integration level (binding A combined with binding B in a call sequence behaves correctly) are mandatory.

The good news: **this commitment is bounded by the closed schema.** The category vocabulary is finite. The operation taxonomy is finite. The dispatch paths are finite. Verifier correctness over a finite schema is a finite testing surface, not an open-ended one. The math Layer 1 commits to is bounded by the schema design, which is itself bounded by the closed-vocabulary discipline. Solo maintenance of this surface is feasible because the surface does not grow with hardware evolution — it grows only with rare schema extensions through release.

**Category #3 — Category vocabulary cannot express the instruction honestly.**

The Layer 2 author has an instruction whose actual behavior cannot be honestly expressed by selecting from the existing category vocabulary. Either the relevant category does not exist in the schema, or two categories that should be distinct have merged into one and the author cannot distinguish their binding's behavior, or the schema is incomplete in some other structural way.

**Attribution: Layer 1's bug. Owned. Fixed through schema-extension release.**

This is a schema-completeness issue. The author is being honest (they cannot lie about hardware because the vocabulary does not let them express the truth), and the schema has a gap that prevents honest expression. The fix is a Layer 1 schema-extension cycle: add the missing category (frozen, reviewed, released), or split the merged categories (frozen, reviewed, released), and the author can then re-express the binding honestly.

This is the slow-cadence work the schema design discipline (§1.7.22) accommodates. Schema extensions are rare by construction — categories describe structural behavior kinds that exist across ISAs, and the set of such kinds is bounded by hardware-software interaction physics, not by per-instruction growth. Real-world schema-completeness issues should surface during the initial schema design (rigorous enumeration of category kinds across the supported machine models) and then arrive at low rate (per-several-years cadence) thereafter.

**Why owning #2 and #3 cleanly makes the #1 rejection credible.**

The architectural defense for Category #1 has teeth only if Layer 1 owns Categories #2 and #3 cleanly. If Layer 1 reflexively rejects every complaint with "hardware floor, your problem," engineers learn the defense is reflexive and stop trusting it. Friends who got excited about ZER hit a Category #2 bug, report it correctly, and watch it get waved off as "your binding's problem" — and they conclude the defense was a dodge all along.

The discipline:

- **Category #1 complaints get the architectural rejection, firmly and repeatably.** The rejection is grounded in the program-vs-hardware boundary locked elsewhere in ZER, not in maintainer bandwidth.
- **Category #2 bugs get owned, fixed, and shipped.** No deflection. The verifier dispatch over the closed schema is Layer 1's math, and the math must be correct.
- **Category #3 schema gaps get owned, evaluated, and resolved through release.** Either the gap is real (schema extension shipped) or the proposed binding can be re-expressed using existing categories (author guided through the closed vocabulary).

The credibility of the #1 rejection depends on the visible owning of #2 and #3. If a maintainer reflexively applies the hardware-floor defense to everything, the defense becomes a dodge. If a maintainer owns the bugs that are genuinely Layer 1's and rejects the ones that are not with a principled architectural reason, the rejection is legitimate and engineers accept it.

This is the difference between sustainable solo maintenance and burnout. The architecture provides the structure; the maintainer applies the structure consistently.

**The two-responsibility, one-boundary statement.**

Stated precisely, with the locked vocabulary:

> Layer 1 owns category-reasoning. Given a Layer 2 binding's correctly-declared category profile, Layer 1 must propagate the implications of those categories to every call site, every time, without bugs. This is program-domain logic over Layer 1's own closed schema. The math must be correct; the verifier must be tested; this commitment is bounded by the finite schema and the closed vocabulary, and it is solo-maintainable.
>
> Layer 1 does NOT own category-truth-against-silicon. Given a Layer 2 binding's declared category profile, Layer 1 trusts the declaration as the binding-correctness contract supplied by the library author. Whether the asm body actually has the declared categories on the target ISA is the hardware-consequence floor, surfaced at the `@bind` declaration site, library author's responsibility, reviewable as a finite checklist because the contract language is closed.
>
> The boundary between Layer 1's owned responsibility (category-reasoning) and the binding author's owned responsibility (category-truth-against-silicon) is the same program-consequence vs hardware-consequence boundary that ZER applies uniformly across all safety domains. Applying this boundary to fault attribution at the binding layer: program-domain failures are Layer 1's; hardware-domain failures are the binding author's. Both responsibilities are real; both are bounded; one is solo-maintainable and the other is library-author-owned by design.

**Why this works for solo maintenance.**

The Category #1 rejection is the load-bearing solo-maintainer survival mechanism. Without it, the language core's maintenance burden grows with every ISA extension, every new instruction, every vendor extension — which is exactly the unbounded catalog problem Levels C, D, and Option E were designed to avoid.

The Category #2 ownership is the load-bearing credibility mechanism. Without it, the #1 rejection sounds like a dodge. The bounded nature of the #2 surface (finite schema, finite dispatch paths, finite test matrix) makes #2 ownership feasible for a solo maintainer in a way that ISA-semantic verification (Category #1's territory) never would be.

The Category #3 schema-extension process is the load-bearing schema-evolution mechanism. Without it, the closed-vocabulary discipline becomes brittle (real expressivity gaps cannot be fixed) and the architecture's kind-difference from SPARK dissolves (authors reach for free-form predicates as workarounds, opening the closed vocabulary). The slow-cadence release process keeps the schema closed in practice while allowing genuine extensions to land when needed.

Together, the three responsibilities partition the failure space cleanly. Solo maintenance is feasible because:

- Category #1 is architecturally outside Layer 1's scope (no maintenance burden at all).
- Category #2 is bounded by the closed schema (finite, testable, manageable maintenance burden).
- Category #3 is slow-cadence (extension events per several years, not per release).

The combined ongoing burden is bounded, slow-growing, and architecturally well-defined. Option E is sustainable for one person because the architecture draws the responsibility boundary in the right place — at program-vs-hardware — and the boundary maps cleanly to fault attribution.

**The drift to refuse.**

The drift that breaks the model is reflexive #1-style rejection of #2 or #3 complaints. "My binding declared `clobbers_flags` but the caller is not seeing the clobber" is a #2 complaint, owned by Layer 1. Rejecting it as "your binding's problem, hardware floor" is the wrong attribution and corrodes credibility.

The dual drift is owning #1 complaints as if they were #2 or #3. "My binding declared `no_memory_effect` but the asm actually writes memory and ZER didn't catch the inconsistency" is a #1 complaint, rejected at Layer 1. Accepting it as a Layer 1 bug — "we should add per-instruction semantic verification for this case" — opens the unbounded catalog and breaks the architecture.

Both drifts must be refused. The discipline is the boundary itself: program-domain ↔ Layer 1 owns; hardware-domain ↔ binding author owns. The boundary is the same line ZER draws uniformly; applying it consistently at the binding layer is the architectural commitment.

**One more note on false positives from category clash.**

A practical question raised during the design discussion: what about false positives from category clash — where two operations have superficially similar category profiles and the verifier fails to distinguish them, producing a wrong rejection at a call site?

This is **not** automatically a #1 (binding author's) fault. The correct attribution depends on the root cause:

- If two operations have genuinely identical category profiles and the verifier cannot distinguish them, that may be a **schema-completeness issue (#3)** — the vocabulary is too coarse to express the structural difference, and a schema extension is the fix.
- If two operations have distinct category profiles and the verifier mis-dispatches between them, that is a **verifier bug (#2)** — Layer 1's reasoning over the closed schema has a flaw, owned and fixable.
- Only if the category clash arises from a Layer 2 author declaring categories incorrectly (e.g., copy-pasting from another binding without verifying applicability) is it a binding-author error — and even then, it is closer to a binding-correctness issue than a category-clash issue.

The discipline: when a false positive surfaces from category clash, diagnose the root cause before defending. Reflexively classifying it as the binding author's problem is the same credibility-erosion drift as misattributing #2 as #1.

**The closing on fault attribution.**

The architectural defense Layer 1 provides for Category #1 is the structural reason Option E is sustainable for one person, and its credibility depends on the visible owning of Categories #2 and #3. Stated as the locked discipline:

- Category #1 (binding-vs-silicon mismatch): architecturally outside Layer 1's scope. Rejected with the program-vs-hardware boundary defense. Repeatable, principled, accepted by engineers because it follows from the architecture, not from bandwidth.
- Category #2 (verifier mis-reasoning over correctly-declared categories): Layer 1's bug. Owned, tested, fixed, shipped. Bounded by the closed schema.
- Category #3 (vocabulary incompleteness): Layer 1's bug, schema-extension class. Owned through release process. Slow cadence, bounded by hardware-software interaction physics.

The boundary between #1 and {#2, #3} is the same program-consequence vs hardware-consequence boundary that ZER applies uniformly. The boundary's consistent application at the binding layer is what makes Option E architecturally coherent, ethically defensible to library authors and firmware engineers, and sustainable for a solo maintainer over the long term.

This is the architectural payoff of the entire Option E design. The closed-vocabulary discipline (§1.7.6, §1.7.7), the three-layer structure (§1.7.3), the structure/semantics straddle observation (§1.7.5), the safety-vs-governance separation (§1.7.8), and the program-consequence vs hardware-consequence vocabulary (§1.7.1) together produce a fault-attribution model that is principled, repeatable, and solo-maintainable. The architecture provides the structure; the maintainer applies the structure consistently. That is the model.

---

## 2. The Decision in One Page

**Keep (already shipped, working, truly frozen):**
- All 130 intrinsics from D-Alpha-1 through D-Alpha-14
- Z-rules Z1 through Z8, Z11, Z12 (10 of 13 wired into NODE_ASM)
- Naked-only restriction (S1) — asm only in `naked` functions
- Structured asm syntax (Session A: `asm { instructions: ... safety: ... }`)
- Typed operand bindings (Session B: `inputs:`, `outputs:`, `clobbers:`)
- F7-light LR/SC pairing (hardcoded mnemonic state machine, ~50 lines)
- Hardcoded UB classics dispatch (NEW: ~30 lines, ~12 frozen instructions)
- Operand type validation via existing ZER type system

**Delete (all ISA-coupled maintenance burden):**
- `src/safety/asm_register_tables_*.c` (4 files, 519 lines) — register name tables
- `src/safety/asm_register_lookup.c` (81 lines) — dispatch
- `src/safety/asm_register_tables.h` (116 lines) — schema
- `src/safety/asm_instruction_table_*.c` (3 files, 186 lines) — instruction tables
- `src/safety/asm_instruction_table.h` (188 lines) — schema
- `src/safety/asm_categories.{c,h}` (252 lines) — 8-category framework skeleton
- `scripts/gen_register_tables.sh` (124 lines) — register probe
- `scripts/gen_instruction_table.sh` (373 lines) — instruction probe
- `scripts/candidates_*.txt` (3 files, 468 lines) — register candidate lists
- `arch_data/*.zerdata` (3 files + SCHEMA, 1,532 lines) — per-instruction data
- F7-full Step 2 dispatch code in checker.c (~80 lines, replaced by hardcoded list)
- Session G Phase 1+2 plumbing (`ZerBarrierKind`, `ZerOrderingRole` enums, ~50 lines)
- CPU feature enum + flag mapping in checker.c (~30 lines, replaced by -m flag passthrough)
- Obsolete planning docs in asm_plan.md (Session G Phase 5 plan, re-audit rounds 2-6, ~3,000 lines)

**Defer to GCC:**
- Register name validation per arch (`rax` on ARM = GCC error)
- Instruction validity per arch (`bsr` on ARM = GCC error)
- CPU feature gating (`xmm16` needs `-mavx512f` = GCC error)
- Operand class enforcement (GCC constraint letters)
- Assembly syntax correctness
- Sub-extension support (AMX, SVE, SME, AVX-10, future ISAs)
- New architecture support (any arch GCC adds)

**Public-facing safety claim becomes:**

> "ZER's asm safety: operand-type checking (via existing ZER type system) +
> Z-rules (memory/type/concurrency/MMIO safety through asm operands) +
> 130 intrinsics for common kernel patterns + hardcoded protection for
> well-known UB classics (BSR-on-zero, IDIV-on-zero, MOVAPS-misaligned,
> LR/SC pairing) + naked-fn isolation (MISRA Dir 4.3). Register validation,
> CPU feature gating, instruction validity all handled by your C compiler's
> assembler — works on every architecture GCC supports."

This is honest and accurate. No marketing inflation. No commitment to
perpetual maintenance.

---

## 3. The Transpiler Nature of ZER — Why This Works

ZER is a **transpiler to C**, not a compiler that emits machine code:

```
ZER source → zerc → C source → C compiler → executable
                    ↑                ↑
            (we build this)    (anyone's compiler)
```

This is the same architecture as Nim, Vala, original C++ "Cfront", Haxe.
The user provides their own C compiler. ZER doesn't ship a code generator.

### Why this matters for asm safety

Since the user's C compiler is the actual gateway to machine code, any
ISA-specific validation we do at the ZER level is **duplicating work** the
user's C compiler will do anyway:

| Validation | Where it happens at Level A | Where it happens at Level C |
|---|---|---|
| Register name valid for arch | ZER checker (using vendored tables) AND GCC | GCC only |
| CPU feature flag matches | ZER checker (vendored enum) AND GCC | GCC only (`-m` flags) |
| Instruction valid for arch | ZER checker (vendored tables) AND GCC | GCC only |
| Operand encoding fits | ZER (partial) AND GCC | GCC only |

Both validations produce a compile-time error. The user can't run the broken
code in either case. The only difference is **which tool emits the error
message** — ZER's formatted version, or GCC's.

For a sole developer with no maintenance budget, deferring to GCC is the
right call. The error message difference is cosmetic; the safety difference
is zero.

### Why this works for multiple C compilers

GCC and Clang accept essentially the same C. The C we emit uses GNU C
extensions (`__auto_type`, `__typeof__`, statement expressions, `__atomic_*`,
`__builtin_*`, AT&T inline asm syntax) — all of which Clang also supports.

```bash
# These both work today, no changes needed:
zerc main.zer --emit-c -o main.c
gcc main.c -o main_gcc
clang main.c -o main_clang
```

### What this means for the future

If we ever want to support MSVC, Keil, IAR, or other compilers:
- Add an emit mode: `--emit-target=msvc` produces different C (no statement expressions, `_Interlocked*` instead of `__atomic_*`, etc.)
- Or pre-processing layer that converts GNU C → portable C
- Not planned today; not needed for ZER's audience

**Level C makes this future flexibility easier** — fewer ZER-side dependencies
on GCC-specific things (probe scripts, vendored data) means switching
backends is more contained.

### Concrete consequence for asm safety

**We do not need to be in the ISA-validation business.** Every C compiler
already is. Our job is the safety story (memory/type/concurrency/MMIO);
the C compiler's job is making sure the asm assembles. They don't overlap.

This is the architectural insight that makes Level C correct.

---

## 4. GCC Coverage in Embedded — Reality Check

ZER's target audience is kernel/firmware/embedded developers. Does GCC
actually cover this audience? Yes, overwhelmingly.

### Open-source / hobbyist / maker (100% GCC)

- **Linux kernel** — GCC canonical for 30+ years (Clang support recent, alternative)
- **BSDs** (FreeBSD, OpenBSD, NetBSD) — GCC and Clang
- **STM32 firmware** — `arm-none-eabi-gcc` (Cortex-M0/M0+/M3/M4/M7/M33/M55)
- **ESP32** — `xtensa-esp32-elf-gcc` (Espressif's GCC fork) + RISC-V GCC for newer chips
- **Arduino** — `avr-gcc` + `arm-none-eabi-gcc`
- **Raspberry Pi Pico** — `arm-none-eabi-gcc` (RP2040 Cortex-M0+)
- **Zephyr RTOS** — GCC primary
- **FreeRTOS** — GCC
- **RT-Thread, NuttX, Mbed OS** — GCC
- **OpenWrt routers** — GCC for MIPS, ARM, x86
- **Yocto/Buildroot embedded Linux** — GCC

### Vendor toolchains that ARE GCC underneath

- **Xilinx Vitis** (FPGA soft cores)
- **NXP MCUXpresso**
- **Nordic nRF Connect SDK**
- **Espressif IDF**
- **Renesas e² studio** (some chips)
- **TI MSP430** (msp430-gcc)

### Architectures GCC supports (matters for Level C's "fit all archs")

- x86, x86_64
- ARM (32-bit ARMv4 through ARMv8.x, all Cortex-A/R/M variants)
- AArch64 (ARM64) with SVE/SVE2/SME
- RISC-V (32-bit and 64-bit, all extensions)
- AVR (Arduino, ATtiny, ATmega)
- MSP430 (TI low-power)
- Xtensa (ESP32, Tensilica DSPs)
- PowerPC (32-bit/64-bit, big/little endian)
- MIPS (32-bit/64-bit, big/little endian)
- SPARC, s390x (IBM Z), m68k, SuperH
- ARC, NIOS2, MicroBlaze (FPGA soft cores)
- PIC (via Microchip GCC fork)
- Blackfin, ColdFire (legacy DSPs)

**Level C inherits this entire matrix for free.**

### The exceptions (not ZER's audience)

| Platform | Compiler | ZER target? |
|---|---|---|
| macOS / iOS apps | Clang exclusive | NO |
| Windows kernel drivers | MSVC | NO |
| Apple Silicon bare-metal | Clang | Rare |
| Certified medical/automotive | Keil ARM, IAR (ISO 26262/IEC 61508 cert) | Niche commercial |
| CUDA GPU | nvcc (LLVM-based) | NO |
| AMD ROCm | LLVM | NO |
| Intel oneAPI HPC | Intel compilers (LLVM) | NO |

**For ZER's actual audience: these don't matter.** People writing ZER are
GCC users by definition (open-source/hobbyist/embedded firmware).

### What this means for Level C

When we say "defer to GCC," we mean: ZER targets the universe of programmers
who use GCC, which is essentially the entire embedded/firmware/kernel
open-source ecosystem. The "loss" of not supporting Keil/IAR/MSVC isn't
real because those aren't ZER users.

The "gain" is enormous: ZER works on every architecture GCC supports, with
zero ZER-side maintenance. New architectures show up in GCC; ZER works on
them automatically.

---

## 5. Why This Pivot — Bug History and Maintenance Reality

### 5.1 The bug rate signal

Over the 6-week period from 2026-04-23 to 2026-05-02, the asm-safety work
shipped progressively more infrastructure:

| Date | Item shipped |
|---|---|
| 2026-04-23 | D-Alpha-7.5 Phase 1 (`naked` restriction, S1 rule) |
| 2026-04-25 | Session A (structured asm syntax) |
| 2026-04-25 | Session B (typed operand bindings) |
| 2026-04-26 | F1a category framework |
| 2026-04-26 | F2 build-time probe pipeline (x86_64 register table) |
| 2026-04-26 | F7-minimum (register validation wired) |
| 2026-04-29 | Sub-extension architecture validated 3-arch |
| 2026-04-29 | F4.1+F4.2 x86_64 instruction tables (53 entries) |
| 2026-05-02 | F5 aarch64 instruction tables (37 entries) |
| 2026-05-02 | F6 riscv64 instruction tables (30 entries) |
| 2026-05-02 | F7-light LR/SC pairing |
| 2026-05-02 | F7-full Step 2 constraints (NONZERO/COMPOUND/ALIGNED/BOUNDED) |
| 2026-05-02 | Session G Phase 1+2 (ordering plumbing) |
| 2026-05-02 | Session G Phase 3 attempted, ABANDONED |
| 2026-05-12 | Pivot decision (Level C) |

### 5.2 The maintenance projection

Continuing the original direction would commit to:

| Year | Expected ISA events | Maintenance impact |
|---|---|---|
| 2026 | Session G Phase 5 (CFG OrderingState) | ~30-40 hours |
| 2026 | Z9/Z10/Z13 forward-compat | ~50 hours |
| 2027 | AVX-10.2 release expected | ~100-200 instruction entries |
| 2027 | ARM v10 SVE 3.0 | ~150-200 entries |
| 2027 | New register names per chip rollout | ~20-30 entries |
| 2028 | RISC-V V 1.1 + new Zb*/Zk* extensions | ~100 entries |
| 2028 | Intel AVX-512 successor (probable) | ~200+ entries |
| 2029 | ARM SME 2.0 | ~150 entries |
| 2030+ | Various vendor extensions, new architectures | ~200+ entries/year |

Cumulative over 5 years: **~1,000-1,500 instruction entries to add, vet,
classify, integrate, test.** Plus ongoing per-instruction bug fixes as edge
cases emerge (Session G Phase 3 abandonment is the canonical example —
"we tried, real code broke, had to revert").

**For a sole developer, this is unsustainable.** Even at Level A (keep
register tables), we'd carry ~1-2 GCC-coupled sync events per year forever.
Level C eliminates them all.

### 5.3 The realization

The per-instruction approach commits ZER to **perpetual maintenance of a
domain that is fundamentally not ZER's domain**. Silicon evolution is
Intel/ARM/RISC-V's responsibility. They publish manuals; GCC integrates
the assembler; ZER doesn't need to know what every instruction does.

The asm_plan direction implicitly assumed: "since we want to make asm
safe, we need to know what asm does." This is **wrong**. The correct
inversion: "since we want to make asm safe, we need users to declare
what their asm does (operand structure), and we enforce existing safety
invariants on those operands (UAF, escape, MMIO, etc.). Everything else
is the C compiler's job."

The user owns the instruction manual. The C compiler owns ISA validation.
ZER owns memory/type/concurrency safety. Three clean responsibilities,
no overlap.

### 5.4 The Session G Phase 3 lesson

The Phase 3 attempt was the empirical signal:

**The bug:** Same-block check rejected the canonical Intel libpmem
persistent-memory idiom (CLWB issued in one block, SFENCE in another).
The "correct" check requires CFG-aware analysis (Phase 5), another 30-40
hours of work and a 700+ line addition to `zercheck_ir.c`.

**The deeper lesson:** Even with perfect per-instruction data, the
COMPILER-SIDE analysis to enforce that data is complex and error-prone.
Phase 3 had correct classification data; the analysis logic still got
real code wrong.

**Level C eliminates the entire class of problems** by removing both the
data AND the analysis. GCC handles the ISA-specific stuff. ZER handles
the safety story.

### 5.5 Convergent evidence: Rust didn't do this either

Rust's `asm!()` macro:
- Validates operand types ✓
- Validates register CLASSES (`out("reg") x` = "any GPR") ✓
- Does NOT validate specific register names per arch
- Does NOT validate instruction semantics
- Does NOT do CFG-aware analysis of asm contents

When Rust users write asm requiring instruction-specific preconditions, they
use `// SAFETY:` comments and `unsafe` block boundary. The compiler trusts
human declaration.

Rust made this call after extensive design discussion (RFC 2873). They
explicitly chose NOT to do what ZER's asm_plan was attempting.

**Level C brings ZER into alignment with Rust's pragmatic approach** —
plus a small bonus (~12 hardcoded UB classics protection) that even Rust
doesn't have.

---

## 6. The Architectural Principle Being Applied

### 6.1 The principle (one sentence)

**Compilers should be generic algorithms over annotated languages, not
databases of facts about specific entities.**

### 6.2 Where ZER already follows this

ZER's existing safety architecture rigorously applies this principle:

| Per-item information | Where it lives | Why this works |
|---|---|---|
| Which fields are volatile? | Language: `volatile *u32 reg` | Compiler enforces generically |
| Which params are kept? | Language: `keep *Handler h` | Compiler enforces generically |
| Which addresses are MMIO? | Language: `mmio 0x4000..0x4FFF;` | Compiler enforces generically |
| Which structs auto-lock? | Language: `shared struct C { }` | Compiler enforces generically |
| What's a handle? | Language: `Handle(T) h` | Compiler enforces generically |
| Which structs transfer? | Language: `move struct F { }` | Compiler enforces generically |
| Which functions can yield? | Computed: `Symbol.props.can_yield` | Generic FuncProps analysis |

**None require the compiler to have a database of specific structs,
addresses, drivers, or operations.** The language carries per-item info;
the compiler runs generic algorithms.

### 6.3 Where asm_plan accidentally violated the principle

The original asm_plan direction encoded per-instruction facts in the compiler:

| Per-instruction fact | Encoded as | Maintenance cost |
|---|---|---|
| "BSR has UB on zero" | Constraint in instruction table | Stable but in growing DB |
| "MOVAPS needs 16-byte alignment" | Constraint in instruction table | Stable but in growing DB |
| "LR must precede SC" | State-machine class in instruction table | Stable but in growing DB |
| "MFENCE produces full-memory barrier" | Ordering metadata in instruction table | In growing DB |
| ...× ~4,000 more facts (potential) | Per-instruction entries | Compounds with every ISA extension |
| "Which registers are GPRs vs vec?" | Per-arch register table | Sync with GCC capability |
| "Which CPU features exist?" | Enum, growing | ~2-3/year additions |

**The right answer (Level C):**

- Hardcode the ~12 well-known UB classics directly in checker.c (`bsr`,
  `bsf`, `div`, `idiv`, `movaps`, `movapd`, `movdqa`, `lr.w`, `lr.d`,
  `sc.w`, `sc.d`, `ldxr`, `stxr`) — these are 1980s-1990s classics, frozen.

- Defer register tables to GCC. User's C compiler errors if `rax` used on ARM.

- Defer CPU feature gating to GCC. User passes `-mavx512f` flag; GCC errors
  if AVX-512 instructions used without it.

- Generic safety (Z-rules) applies to operand expressions via existing
  systems (VRP, alignment, escape, MMIO, etc.).

The compiler stays generic. The world can change without the compiler changing.

### 6.4 The general lesson

If you find yourself adding a database to a compiler about specific items
(specific structs, functions, API calls, instructions, addresses), **STOP**.
The right answer is:

1. Identify the property the database encodes
2. Either: design a language-level annotation expressing it (smart language)
3. Or: defer to a more authoritative tool that already validates it (smart delegation)
4. Don't grow the per-item database

Level C applies BOTH inversions to asm safety:
- Smart language: Z-rules, operand types, hardcoded UB classics (small fixed set)
- Smart delegation: register names, CPU features, instruction validity → GCC

---

## 7. The Three Levels Considered

The 2026-05-12 discussion went through three increasingly aggressive cleanup
levels:

### 7.1 Level A — Keep existing infrastructure, add annotations

**Description:** Build on existing Session A/B asm syntax. Keep register
tables, F7-light, F7-full Step 2, CPU feature gating. Add operand metadata
fields, language-level precondition annotations, tiny implicit-precondition
table (~100 entries). Stop extending per-instruction database beyond current
state.

**What it preserves:**
- Per-arch register validation at compile time
- CPU feature gating in ZER's checker
- ZER-formatted error messages
- Implicit table for ~100 well-known UB classics

**Effort:** ~3-4 months one-time + ongoing maintenance (~1-2 events/year
for register tables, ~2-3/year for CPU feature flags).

**Why ultimately rejected:**
- "Low maintenance" isn't zero
- Sole-developer-unfriendly perpetual sync
- Doesn't fully apply the smart-language principle
- Duplicates work the C compiler does anyway

### 7.2 Level B — Drop register tables, keep CPU feature gating

**Description:** Like Level A but ALSO drops the register tables. Keeps
CPU feature flag enum. Specific register validation defers to GCC.

**What it preserves:**
- CPU feature gating in ZER's checker (small enum, ~14 entries)
- Register class validation (via operand annotations like `class: vec`)
- Hardcoded UB classics

**Effort:** ~3-4 months (similar to Level A; register-table work was small fraction).

**Why rejected:**
- Still has ~2-3 CPU feature additions per year (GCC-coupled)
- Half-measure: if we're dropping ISA-specific stuff, drop ALL of it
- Inconsistent: validates CPU features but not specific registers

### 7.3 Level C — Drop everything ISA-specific (CHOSEN)

**Description:** Drop register tables AND CPU feature gating AND probe scripts
AND instruction tables AND 8-category framework. Defer everything ISA-specific
to GCC. Keep only the frozen safety layer.

**What it preserves:**
- Naked-only restriction (S1)
- Z-rules Z1-Z8, Z11, Z12
- 130 intrinsics
- F7-light LR/SC pairing (hardcoded mnemonics, frozen)
- Hardcoded UB classics (~12 instructions, frozen)
- Session A/B asm syntax
- Operand type checking via existing ZER type system

**Effort:** ~1-2 days one-time. **TRULY zero maintenance after.**

**Why selected:**
1. Sole developer wants zero maintenance forever
2. ZER is a transpiler — duplicating GCC's validation is unnecessary
3. "Fit all architecture" is STRONGER at Level C (inherit GCC's full matrix)
4. Production-compiler precedent (Rust/Zig don't validate register names)
5. Smallest possible code surface
6. Cleanest architectural division (ZER does safety; GCC does ISA)

### 7.4 Side-by-side comparison

| Property | Level A | Level B | Level C (CHOSEN) |
|---|---|---|---|
| Effort | ~3-4 months | ~3-4 months | ~1-2 days |
| Code reduction | ~5,600 lines | ~6,700 lines | ~7,000 lines |
| Register table | KEEP (vendored, probed) | DELETE (defer to GCC) | DELETE (defer to GCC) |
| CPU feature enum | KEEP (~14 entries) | KEEP (~14 entries) | DELETE (defer to GCC -m flags) |
| Probe scripts | KEEP (register probe) | KEEP (register probe) | DELETE all |
| Instruction tables | Stop extending | DELETE | DELETE |
| Hardcoded UB classics | ~100 implicit table entries | ~100 implicit table entries | ~12 hardcoded in checker.c |
| Annotations | Add | Add | Not needed |
| F7-light LR/SC | Keep + generalize | Keep + generalize | Keep as-is (hardcoded) |
| F7-full Step 2 | Keep dispatch | Keep dispatch | Refactor to hardcoded |
| Ongoing maintenance | Low but nonzero | Low but nonzero | **TRULY zero** |
| Architecture support | 3 archs maintained | 3 archs (registers via GCC) | Every arch GCC supports |
| ZER error vs GCC error | ZER errors for everything | ZER for features, GCC for registers | GCC for everything ISA-specific |
| Match production compilers | Stricter than Rust | Closer to Rust | Matches Rust (+ classics bonus) |

Level C dominates on every practical axis for ZER's sole-developer
multi-architecture goal.

---

## 8. Journey of the Discussion

The decision was reached through a long architectural discussion across
multiple sessions. The journey matters as much as the conclusion, preserved
here for fresh sessions.

### 8.1 Starting point (2026-05-12 morning)

User asked: "where are we on asm_plan.md?"

Answer at the time: D-Alpha-7.5 Phase 2 in progress. F4-F7 instruction
tables shipped. Session G Phase 3 abandoned. Session G Phase 5 next at
~30-40 hours. Z9/Z10/Z13 forward-compat blocked.

### 8.2 The "make it less complex" framing

> "we did so complex... should we make raw asm unsafe but push everything
> through intrinsics that supported every architecture every machine
> related thing"

**Response:** Pure-intrinsic infeasible (~4,400 instructions × 3 archs ×
growing forever). Hybrid (intrinsics + raw asm + partial validation) is
the right shape — but stop extending the validation side.

### 8.3 The maintenance critique

> "our code shouldn't keep being maintained like today new chip come out
> and stuff need to update spec... but it should be safe cause it is IR
> based, how can we achieve that even with subarch like AVX?"

**Response (key breakthrough):** Operand-kind validation, not instruction
validation. Frozen 10-category enum. AVX-1024 in 2035 still uses these
categories. No per-ISA-extension maintenance.

### 8.4 The "ASM is just abstraction" framing

> "ASM is just abstraction to machine language... using intrinsic, like
> cannot fully but just common, I want full 100% usability, I don't have
> time constraint, what about ASM IR, ASM to machine?"

**Response:** Operand-kind IR (Option 1). Full structured asm AST with
generic safety pipeline. ~8-12 months effort.

### 8.5 The "do we need rewrite?" question

> "I mean that means rewriting whole stuff right if we do that, what if
> we use existing Z-classes since we already did it, just add a complexity
> for these manual stuff"

**Response:** Option 2 (extend current) is viable. Same safety as Option 1,
3x less work. Add operand metadata + annotations + tiny implicit table to
existing Session A/B syntax. Keep Z-rules.

### 8.6 The "are they the same?" verification

> "so both are actually the same solution right, one is just implemented
> in IR level a rewrite whereas the other is whole language enforces to
> ASM... is there any usability risk?"

**Response:** Functionally equivalent for ZER's actual use cases. Different
internal architecture, identical user experience. Decision: Option 2
(extend current design + annotations + tiny implicit table). Estimated
~3-4 months. This became Level A.

### 8.7 The "is this really good?" challenge

> "is this really good refactor?"

**Response:** Honestly evaluated. The plan is good but over-scoped. The 6
helpers split into 3 high-value items and 3 speculative items. Recommended
Phase A (~4 hrs) + defer Phase B. (Note: this was about refactor_ir, but
the same instinct applies to asm work.)

### 8.8 The "Manual is fine" insight

> "these manual we can introduce language complexity like annotations or
> ownership or something else to solve this... meaning it would throw
> error which means its safe"

**Response:** Annotations are compile-time CONTRACTS. Compiler enforces
via existing safety infrastructure. Same safety as auto detection.

### 8.9 The "smart language" principle articulation

> "our asm plan one did this by making compiler smart not by adding
> complexity correct which is not good which became maintenance hell"

**Response:** This is the architectural principle. ZER's 28 of 29 safety
systems follow it. asm_plan was the violation. The fix: smart language
(annotations + frozen primitives) + generic compiler (existing safety
systems) = no per-item database.

### 8.10 The "registry table is must?" question (Level A→B→C inflection)

> "low doesn't mean like good also right... I think the registry table
> still need or no need or that dependent on GCC?"

This was the pivotal question. Re-examining the "LOW maintenance"
classifications honestly:
- Register tables: GCC-dependent, ~1-2 sync events/year
- CPU feature enum: ~2-3 additions/year
- F7-light LR/SC: truly frozen
- Hardcoded UB classics: truly frozen

The user noticed "low" wasn't "zero." If the goal is true zero, drop the
GCC-coupled stuff entirely.

**Response:** Presented Level A vs B vs C. Level A still has nonzero
maintenance. Level B drops register tables but keeps CPU features. Level C
drops everything ISA-specific. Recommended Level C for sole-developer
zero-maintenance goal.

### 8.11 The "GCC covers everything" verification

> "GCC already supported like full everything right cause EVERYTHING is
> ran by GCC whole shit industry, exception is of course LLVM is option
> in software level, but embedded is GCC?"

**Response:** Verified extensively. GCC dominates ZER's audience (embedded/
firmware/kernel open-source). Exceptions (Keil/IAR/MSVC) are commercial
niches that aren't ZER's audience. Level C inherits GCC's full architecture
matrix.

### 8.12 The "transpiler nature" verification

> "even if these diff compiler the way we wrote can easily just switch
> right... GCC next day can just switch to CLang, MSVC we are not making
> our compiler we are transpiling into C correct?"

**Response:** Correct. ZER → C → user's choice of C compiler. GCC ↔ Clang
~95% compatible (Clang accepts GNU C). MSVC needs emitter changes (not
planned, not ZER's audience). Level C makes the transpiler nature explicit:
defer ISA stuff to whatever C compiler the user picks.

### 8.13 The final decision

> "alright let's do C update the asm_lang_zer_safe with everything stuff etc"

**Decision: Level C. Execute the cleanup.** This document is the
comprehensive plan for the execution.

---

## 9. What We're Keeping at Level C

The Level C work preserves the truly frozen safety core. **Nothing here
requires per-ISA-extension maintenance.**

### 9.1 The 130 intrinsics (D-Alpha-1 through D-Alpha-14)

All 130 intrinsics shipped between 2026-04-23 and 2026-04-24 stay exactly
as they are:

- D-Alpha-1: 7 atomic intrinsics (xchg, nand, *_fetch × 5)
- D-Alpha-2: 10 barrier/bit-query/hint intrinsics
- D-Alpha-3: 5 interrupt control intrinsics (privileged)
- D-Alpha-4: 4 context switch intrinsics
- D-Alpha-9: 10 MSR/CR/XCR0 intrinsics
- D-Alpha-10: 10 inspection intrinsics (non-privileged)
- D-Alpha-11: 5 power management intrinsics
- D-Alpha-12: 6 privileged mode transition intrinsics
- D-Alpha-13: 20 Linux-scale x86 essentials
- D-Alpha-14: 12 final misc intrinsics

These cover ~80% of common kernel/firmware asm patterns. They're the
"primary safe path" for users. Fixed set, no per-ISA growth.

### 9.2 Z-rules Z1 through Z8, Z11, Z12 (10 of 13 wired)

Already-shipped Z-rules stay:

| Z-rule | What it does |
|---|---|
| Z1 | UAF check at asm operand boundary |
| Z2 | Move struct transfer at asm operand |
| Z3 | VRP range invalidation on asm output |
| Z4 | Provenance type clear on asm output |
| Z5 | Local-derived pointer rejection with memory clobber |
| Z6 | Asm-in-defer/async ban (forward-compat) |
| Z7 | MMIO range check on asm memory operand (via @inttoptr) |
| Z8 | Qualifier preservation on asm output |
| Z11 | Non-keep pointer param + memory clobber rejected |
| Z12 | scan_frame walker recurses into structured asm operands |

These work. They're tested. They cover the core safety dimensions
(memory/type/concurrency/MMIO/provenance/qualifier) through the asm boundary.

**Z9, Z10, Z13 are NOT shipped and won't be** (deferred indefinitely).

### 9.3 Naked-only restriction (S1)

Phase 1 verified rule: `zer_asm_allowed_in_context(in_naked)` enforces asm
only inside `naked` functions. This is the v1.0 interim guard and the
foundational structural rule.

**Stays. Permanent.** MISRA Dir 4.3 compliant.

### 9.4 Structured asm syntax (Session A)

The `asm { instructions: "..." safety: "..." }` form stays. The mandatory
`safety:` documentation string (≥30 chars) stays. This is the user-facing
entry point.

### 9.5 Typed operand bindings (Session B)

The `inputs: { "reg" = expr }`, `outputs: { "reg" = lvalue }`, `clobbers:
[...]` form stays. Operand expressions go through ZER's normal type
checking, which:
- Validates expression type is integer or pointer (existing)
- Tracks expression through escape analysis (existing)
- Tracks through provenance, qualifier preservation, etc. (existing Z-rules)

What changes at Level C: the register name in the binding key is **no
longer validated against a per-arch table**. GCC's assembler validates it
when the emitted C is compiled. If `"rax"` is used on ARM, GCC errors.

### 9.6 F7-light LR/SC pairing (hardcoded mnemonics)

The hardcoded state-machine for paired atomic operations:

```c
/* In checker.c NODE_ASM handler */
if (target_arch == TARGET_RISCV64) {
    if (mnemonic_is_lrsc_open(mnemonic)) {
        block_state.lrsc_open = true;
    } else if (mnemonic_is_lrsc_close(mnemonic)) {
        if (!block_state.lrsc_open) {
            checker_error(c, line, "sc.w without preceding lr.w");
        }
        block_state.lrsc_open = false;
    }
}
/* Similar for ARM LDXR/STXR, x86 MONITOR/MWAIT */
```

Pairs handled (frozen since 1990s-2000s):
- RISC-V: `lr.w` / `sc.w`, `lr.d` / `sc.d`
- ARM: `ldxr` / `stxr`, `ldaxr` / `stlxr`
- x86: `monitor` / `mwait`, `umonitor` / `umwait`

These mnemonics haven't changed in decades. No maintenance burden.

### 9.7 Hardcoded UB classics (NEW at Level C — replaces F7-full Step 2 table dispatch)

A small frozen list in checker.c (~30 lines):

```c
/* In checker.c NODE_ASM handler */
static const struct { 
    const char *mnemonic; 
    int operand_idx; 
    int constraint;
    const char *citation;
} ub_classics[] = {
    /* Bit-search — UB on zero (Intel SDM Vol 2 BSR/BSF) */
    { "bsr",     0, REQUIRES_NONZERO,            "Intel SDM Vol 2 BSR" },
    { "bsf",     0, REQUIRES_NONZERO,            "Intel SDM Vol 2 BSF" },
    
    /* Division — UB on zero divisor (Intel SDM Vol 2 DIV/IDIV) */
    { "div",     0, REQUIRES_NONZERO,            "Intel SDM Vol 2 DIV" },
    { "idiv",    0, REQUIRES_NONZERO | COMPOUND_INTMIN_NEG1, 
                                                  "Intel SDM Vol 2 IDIV" },
    
    /* Aligned vector loads — SIGSEGV on misalignment */
    { "movaps",  0, REQUIRES_ALIGN_16,           "Intel SDM Vol 2 MOVAPS" },
    { "movapd",  0, REQUIRES_ALIGN_16,           "Intel SDM Vol 2 MOVAPD" },
    { "movdqa",  0, REQUIRES_ALIGN_16,           "Intel SDM Vol 2 MOVDQA" },
    { "vmovaps", 0, REQUIRES_ALIGN_32,           "Intel SDM Vol 2 VMOVAPS" },
    { "vmovapd", 0, REQUIRES_ALIGN_32,           "Intel SDM Vol 2 VMOVAPD" },
    { "vmovdqa", 0, REQUIRES_ALIGN_32,           "Intel SDM Vol 2 VMOVDQA" },
    
    /* End of list — frozen at well-known UB classics */
};
```

Each entry dispatches to existing safety infrastructure:
- `REQUIRES_NONZERO` → existing VRP (System #12)
- `REQUIRES_ALIGN_N` → existing alignment infrastructure
- `COMPOUND_INTMIN_NEG1` → existing signed-overflow handling

**~12 entries. Frozen. Haven't changed semantics since 1980s-1990s.**

No new safety logic. Just a small dispatch table that uses existing systems.

### 9.6 Operand type validation via existing ZER type system

Operand expressions in `inputs:`/`outputs:` are regular ZER expressions.
They go through:
- Type inference (existing)
- Integer/pointer/distinct type checking (existing)
- Escape analysis (existing)
- Provenance tracking (existing)
- Qualifier preservation (existing)

No asm-specific code. Same systems that validate `x + y` validate the
expressions in asm operands.

---

## 10. What We're Deleting at Level C

This section enumerates the actual file/code deletions. Total: ~7,000 lines.

### 10.1 Instruction tables (per-instruction database)

```
src/safety/asm_instruction_table_x86_64.c     # 75 lines
src/safety/asm_instruction_table_aarch64.c    # 59 lines
src/safety/asm_instruction_table_riscv64.c    # 52 lines
src/safety/asm_instruction_table.h             # 188 lines
```

**Total: 374 lines.** The per-instruction safety classification database.

### 10.2 Register tables

```
src/safety/asm_register_tables_x86_64.c        # 121 lines
src/safety/asm_register_tables_x86_64_avx512f.c # 177 lines
src/safety/asm_register_tables_aarch64.c        # 74 lines
src/safety/asm_register_tables_riscv64.c        # 147 lines
src/safety/asm_register_lookup.c                # 81 lines
src/safety/asm_register_tables.h                # 116 lines
```

**Total: 716 lines.** Per-arch register name validation. GCC handles this.

### 10.3 Categories framework

```
src/safety/asm_categories.c                     # 181 lines
src/safety/asm_categories.h                     # 71 lines
```

**Total: 252 lines.** The 8-category skeleton with no enforcement.

### 10.4 Probe scripts

```
scripts/gen_instruction_table.sh                # 373 lines
scripts/gen_register_tables.sh                  # 124 lines
scripts/candidates_x86_64.txt                   # 255 lines
scripts/candidates_aarch64.txt                  # 63 lines
scripts/candidates_riscv64.txt                  # 150 lines
```

**Total: 965 lines.** GCC probe scripts and candidate lists.

### 10.5 Vendored architecture data

```
arch_data/x86_64.zerdata                        # 526 lines
arch_data/aarch64.zerdata                       # 363 lines
arch_data/riscv64.zerdata                       # 323 lines
arch_data/SCHEMA.md                              # 320 lines
```

**Total: 1,532 lines.** Per-instruction classification data.

### 10.6 Checker.c refactoring (code deletion + replacement)

| Code section | Lines deleted | Lines added (replacement) |
|---|---|---|
| F7-full Step 2 table-driven dispatch | ~80 | ~30 (hardcoded UB classics list) |
| Session G Phase 1+2 plumbing (ordering enums, fields) | ~50 | 0 |
| Register validation calls | ~30 | 0 |
| CPU feature enum + flag mapping | ~30 | ~5 (string passthrough to GCC) |
| Instruction table includes/lookups | ~40 | 0 |

**Net: ~230 lines deleted, ~35 lines added = ~195 net deletion.**

### 10.7 Makefile entries

```
# Remove from CORE_SRCS and LIB_SRCS:
src/safety/asm_register_tables_x86_64.c
src/safety/asm_register_tables_x86_64_avx512f.c
src/safety/asm_register_tables_aarch64.c
src/safety/asm_register_tables_riscv64.c
src/safety/asm_register_lookup.c
src/safety/asm_categories.c
src/safety/asm_instruction_table_x86_64.c
src/safety/asm_instruction_table_aarch64.c
src/safety/asm_instruction_table_riscv64.c

# Remove targets:
gen-asm-tables:
    bash scripts/gen_register_tables.sh ...
    bash scripts/gen_instruction_table.sh ...
```

Plus any test invocations referencing the deleted infrastructure.

### 10.8 Obsolete planning docs

In `docs/asm_plan.md`, the following sections become historical-only or
deletable:

| Section | Status |
|---|---|
| "Session G Phase 5 implementation plan" | DELETE — was planning for deferred work |
| "Re-audit round 2 through 6" | DELETE — superseded by Level C |
| "8-category framework details" | KEEP as historical (don't grow) |
| "F4-F7 trajectory tables" | KEEP as historical |
| "Stage 4 status (2026-05-02)" | KEEP as historical — frozen snapshot |

Approximately 3,000 lines of planning content can be deleted (planning
artifacts for work that's now deferred).

### 10.9 Tests that reference deleted infrastructure

| Test file | What to do |
|---|---|
| `tests/zer_fail/asm_aarch64_x86_reg.zer` | Convert: still fails, just via GCC error instead of ZER |
| `tests/zer_fail/asm_riscv64_x86_reg.zer` | Same |
| `tests/zer/asm_avx512_register.zer` | Still works, validation by GCC -mavx512f flag |
| `tests/zer_fail/asm_avx512_no_flag.zer` | Still fails (GCC errors without -mavx512f) |
| `tests/zer/asm_simd_register.zer` | Still works |
| F7-light tests (LR/SC pairing) | Unchanged — F7-light stays |
| F7-full Step 2 tests (BSR/IDIV/MOVAPS) | Unchanged — hardcoded list does same |
| `tests/test_cross_arch.sh` | Adjust to not depend on probe scripts |

### 10.10 Total deletion summary

| Category | Lines |
|---|---|
| Instruction tables + schema | 374 |
| Register tables + lookup + schema | 716 |
| Categories framework | 252 |
| Probe scripts + candidates | 965 |
| Vendored arch data | 1,532 |
| Checker.c refactoring (net) | 195 |
| Obsolete planning docs in asm_plan.md | ~3,000 |
| **Total** | **~7,034 lines** |

After deletion: ZER's asm safety infrastructure is **~600 lines total**:
~500 lines of Z-rules (generic, frozen) + ~50 lines of F7-light (hardcoded
mnemonics) + ~30 lines of hardcoded UB classics (frozen) + Session A/B
parser/AST integration.

---

## 11. The Hardcoded UB Classics List

This is THE concrete addition Level C makes. ~12 frozen entries in
checker.c that catch well-known instruction UB at compile time.

### 11.1 Why hardcoded, not table

Previous design (Level A): ~100-entry implicit-precondition table with
lookup function, schema, citations.

Level C realization: even ~100 entries is overkill. The actual list of
"well-known UB-prone instruction classics" is much smaller. Hardcoding
in checker.c is simpler, faster, and there's no growth pressure
(these instructions are decades-old classics).

### 11.2 The list (concrete, frozen)

**Bit-search (UB on zero operand):**
- `bsr` — Bit Scan Reverse, x86 (1985)
- `bsf` — Bit Scan Forward, x86 (1985)

**Integer division (UB on zero divisor, plus signed overflow):**
- `div` — Unsigned divide, x86 (1978)
- `idiv` — Signed divide, x86 (1978, INT_MIN/-1 overflow)

**Aligned vector loads (SIGSEGV on misalignment):**
- `movaps` — Move Aligned Packed Single, x86 SSE (1999)
- `movapd` — Move Aligned Packed Double, x86 SSE2 (2000)
- `movdqa` — Move Double Quadword Aligned, x86 SSE2 (2000)
- `vmovaps`, `vmovapd`, `vmovdqa` — AVX variants (2011)

**Total: ~10-12 mnemonics.** Frozen since their introduction. No new
similar instructions added in 25+ years.

### 11.3 What's NOT in the list

The list is deliberately small. NOT included:

| Category | Why not |
|---|---|
| LR/SC pairing | Handled by F7-light separately (existing) |
| MONITOR/MWAIT pairing | Handled by F7-light separately (existing) |
| AMX TILECONFIG ordering | Niche, opt-in via comments |
| Privileged instructions (RDMSR/WRMSR/etc.) | User in `naked` + kernel module context already |
| Cache control (CLFLUSH/CLWB) | User-mode safe, no implicit constraint |
| Acquire/release pairing | Niche persistent-memory case, deferred |
| Shift count > width | C compiler handles |
| Most everything else | Either handled by existing systems or GCC catches it |

The list is the **minimal viable set of well-known UB classics** that
provide compile-time protection beyond what GCC catches.

### 11.4 Concrete code (replaces F7-full Step 2 table dispatch)

```c
/* In checker.c, NODE_ASM handler, ~30 lines */

typedef enum {
    UB_REQUIRES_NONZERO     = 1 << 0,
    UB_REQUIRES_ALIGN_16    = 1 << 1,
    UB_REQUIRES_ALIGN_32    = 1 << 2,
    UB_REQUIRES_ALIGN_64    = 1 << 3,
    UB_COMPOUND_INTMIN_NEG1 = 1 << 4,
} UbConstraint;

static const struct {
    const char *mnemonic;
    int operand_idx;
    uint32_t constraint;
    const char *citation;
} ub_classics[] = {
    { "bsr",     0, UB_REQUIRES_NONZERO,                            "Intel SDM Vol 2 BSR" },
    { "bsf",     0, UB_REQUIRES_NONZERO,                            "Intel SDM Vol 2 BSF" },
    { "div",     0, UB_REQUIRES_NONZERO,                            "Intel SDM Vol 2 DIV" },
    { "idiv",    0, UB_REQUIRES_NONZERO | UB_COMPOUND_INTMIN_NEG1,  "Intel SDM Vol 2 IDIV" },
    { "movaps",  0, UB_REQUIRES_ALIGN_16,                           "Intel SDM Vol 2 MOVAPS" },
    { "movapd",  0, UB_REQUIRES_ALIGN_16,                           "Intel SDM Vol 2 MOVAPD" },
    { "movdqa",  0, UB_REQUIRES_ALIGN_16,                           "Intel SDM Vol 2 MOVDQA" },
    { "vmovaps", 0, UB_REQUIRES_ALIGN_32,                           "Intel SDM Vol 2 VMOVAPS (256-bit)" },
    { "vmovapd", 0, UB_REQUIRES_ALIGN_32,                           "Intel SDM Vol 2 VMOVAPD (256-bit)" },
    { "vmovdqa", 0, UB_REQUIRES_ALIGN_32,                           "Intel SDM Vol 2 VMOVDQA (256-bit)" },
};

static void check_ub_classics(Checker *c, AsmNode *asm_node) {
    const char *mnemonic = first_word(asm_node->instructions);
    int mn_len = first_word_len(asm_node->instructions);
    
    for (size_t i = 0; i < ARRAY_SIZE(ub_classics); i++) {
        const char *e_mn = ub_classics[i].mnemonic;
        size_t e_len = strlen(e_mn);
        if (mn_len != (int)e_len) continue;
        if (memcmp(mnemonic, e_mn, e_len) != 0) continue;
        
        /* Found — apply constraints */
        uint32_t c_flags = ub_classics[i].constraint;
        int op_idx = ub_classics[i].operand_idx;
        AsmOperand *op = get_operand(asm_node, op_idx);
        if (!op) return;
        
        if (c_flags & UB_REQUIRES_NONZERO) {
            VarRange range;
            if (find_var_range(c, op->expr, &range)) {
                if (range.min <= 0 && range.max >= 0 && !range.known_nonzero) {
                    checker_error(c, asm_node->line,
                        "asm %s requires operand %d nonzero (%s):\n"
                        "    expression range [%lld, %lld] includes zero",
                        e_mn, op_idx, ub_classics[i].citation,
                        range.min, range.max);
                }
            }
        }
        
        if (c_flags & UB_REQUIRES_ALIGN_16) {
            int actual = expr_alignment(c, op->expr);
            if (actual < 16) {
                checker_error(c, asm_node->line,
                    "asm %s requires operand %d 16-byte aligned (%s):\n"
                    "    expression has alignment %d",
                    e_mn, op_idx, ub_classics[i].citation, actual);
            }
        }
        
        /* Similar for ALIGN_32, ALIGN_64, COMPOUND_INTMIN_NEG1 */
        return;
    }
}
```

**~50 lines including the list. Frozen.**

### 11.5 What this catches

- `naked void f(u32 x) { asm { instructions: "bsr %0, %1" outputs: { "rcx" = result } inputs: { "rax" = x } } }`
  — if `x` not provably nonzero → compile error
- `naked void f(*u8 buf) { asm { instructions: "movaps %0, %1" ... inputs: { "rax" = buf } } }`
  — if `buf` not provably 16-byte aligned → compile error
- `naked void f(i32 x) { asm { instructions: "idiv %0" inputs: { "rax" = x } } }`
  — if `x` not provably nonzero → compile error
- Compound INT_MIN/-1 backstopped at runtime trap (existing infrastructure)

**Same coverage as F7-full Step 2 table-driven dispatch, smaller code.**

---

## 12. Integration with Existing Safety Systems

Level C reuses existing ZER safety infrastructure. No new safety algorithms.

### 12.1 VRP integration (System #12)

The hardcoded UB classics list uses VRP for `REQUIRES_NONZERO`:
- `find_var_range(c, expr, &range)` → returns expression's known range
- Check `range.known_nonzero || range.min > 0 || range.max < 0`
- Error if range includes zero

Same VRP that catches `arr[0]` with `arr` size > 0 catches BSR-on-zero.

### 12.2 Alignment integration

`REQUIRES_ALIGN_N` uses existing alignment infrastructure:
- `expr_alignment(c, expr)` → returns provably known alignment
- Same logic that handles `_Alignas`, `@aligned_alloc`, MMIO alignment

### 12.3 Escape analysis (System #11)

Operand expressions go through escape analysis via Z-rules Z3-Z5, Z11.
Pointer operands tracked the same as regular pointer expressions.

### 12.4 Provenance tracking (Systems #3-#5)

Operand expressions go through provenance via Z-rules Z4-Z5.
Same as regular ZER code.

### 12.5 Handle/move tracking (Systems #7, #10)

Handle and move-struct operands go through Z-rules Z1, Z2.
Same as regular ZER code.

### 12.6 MMIO range checking (System #19)

Memory operand addresses go through existing MMIO infrastructure via Z7.
Same as regular `@inttoptr` validation.

### 12.7 Qualifier preservation (System #20)

Volatile/const tracking through asm operands via Z-rule Z8.
Same as regular qualifier checking.

### 12.8 Summary: zero new safety logic

Level C's work is:
- Delete files
- Refactor F7-full Step 2 dispatch to use hardcoded list (~30 lines)
- Update Makefile

**No new safety algorithms.** Every check uses existing systems.

---

## 13. Safety Coverage Matrix — Level A vs B vs C

Side-by-side comparison.

### 13.1 Memory safety through asm operands

| Property | Level A | Level B | Level C |
|---|---|---|---|
| OOB through operand | ✓ via VRP | ✓ via VRP | ✓ via VRP |
| UAF through operand | ✓ (Z1) | ✓ (Z1) | ✓ (Z1) |
| Use-after-move | ✓ (Z2) | ✓ (Z2) | ✓ (Z2) |
| Escape | ✓ (Z3-Z5) | ✓ (Z3-Z5) | ✓ (Z3-Z5) |
| Provenance | ✓ (Z4-Z5) | ✓ (Z4-Z5) | ✓ (Z4-Z5) |
| Qualifier preservation | ✓ (Z8) | ✓ (Z8) | ✓ (Z8) |
| Keep param violations | ✓ (Z11) | ✓ (Z11) | ✓ (Z11) |
| MMIO range | ✓ via @inttoptr | ✓ via @inttoptr | ✓ via @inttoptr |

**Identical across all levels.** Z-rules unchanged.

### 13.2 Instruction-specific UB

| Property | Level A | Level B | Level C |
|---|---|---|---|
| BSR/BSF on zero | ✓ implicit table | ✓ implicit table | ✓ hardcoded list |
| IDIV on zero | ✓ implicit table | ✓ implicit table | ✓ hardcoded list |
| MOVAPS misaligned | ✓ implicit table | ✓ implicit table | ✓ hardcoded list |
| LR/SC pairing | ✓ F7-light | ✓ F7-light | ✓ F7-light |
| Privileged misuse | ✓ implicit table | ✓ implicit table | Defer to GCC |

Level C drops some niche-but-not-zero coverage (privileged context check)
in exchange for simplicity. The "loss": GCC errors instead of ZER errors.

### 13.3 ISA-specific validation

| Property | Level A | Level B | Level C |
|---|---|---|---|
| Register name valid for arch | ✓ ZER tables | Defer to GCC | Defer to GCC |
| Register width match | ✓ ZER tables | Defer to GCC | Defer to GCC |
| Register class match | ✓ ZER tables | ✓ via annotations | Defer to GCC |
| CPU feature gating | ✓ ZER enum | ✓ ZER enum | Defer to GCC -m flags |
| Instruction valid for arch | ✓ ZER tables | Defer to GCC | Defer to GCC |

**Level C defers ALL ISA-specific to GCC.** Errors still happen at compile
time, just from GCC instead of ZER.

### 13.4 Out-of-scope (all levels)

| Property | All levels |
|---|---|
| Wrong specification (user error) | ✗ |
| Algorithm correctness | ✗ |
| Microarchitectural (Spectre etc.) | ✗ |
| Runtime CPU state | ✗ |

### 13.5 Effort and maintenance

| Property | Level A | Level B | Level C |
|---|---|---|---|
| Effort to ship | ~3-4 months | ~3-4 months | **~1-2 days** |
| Lines deleted | ~5,600 | ~6,700 | **~7,000** |
| Existing code preserved | ~95% | ~85% | ~80% (intrinsics + Z-rules unchanged) |
| Maintenance per ISA extension | Low (probe rerun) | Lower | **Zero** |
| CPU feature additions/year | ~2-3 | ~2-3 | Zero |
| Architecture coverage | 3 archs | 3 archs (registers via GCC) | **All GCC archs (15+)** |
| Sole-developer friendliness | OK | Better | **Best** |

### 13.6 Summary

For ZER's specific situation (sole developer, multi-architecture goal,
transpiler design, GCC audience), **Level C is the clear winner**:
- Same safety coverage as A/B
- Less code
- Zero maintenance
- More architectures supported

---

## 14. Production-Compiler Precedents

### 14.1 Rust's `asm!()` macro

What Rust does:
- Structured operand bindings: `out("reg") result`, `in("reg") input`
- Type checking on bound expressions
- Register CLASS validation (`reg` = any GPR, `reg_byte` = byte reg)
- NO per-arch register NAME validation
- CPU feature flags via `#[cfg(target_feature = "...")]`
- NO per-instruction UB detection
- `unsafe` block boundary
- `// SAFETY:` comment convention

**Level C match:** ZER drops register name validation (same as Rust). ZER
drops per-instruction enum-based feature gating (delegates to GCC -m flags,
similar effect). ZER keeps ~12 hardcoded UB classics (slight safety bonus
over Rust). Naked-only restriction is stricter than Rust's `unsafe`.

### 14.2 Zig's `asm` keyword

What Zig does:
- `asm volatile (template : outputs : inputs : clobbers)` (GCC-style)
- Type checking on bound expressions
- NO per-arch register name validation
- NO per-instruction UB detection

**Level C match:** Essentially the same. Zig defers to LLVM's assembler.
ZER defers to GCC.

### 14.3 seL4 (proven kernel)

What seL4 does:
- Hand-written asm with hand-proven invariants
- Isabelle/HOL formal model of ISA semantics (not in compiler)
- No compile-time asm validation in their build chain

**Level C match:** seL4 is the maximum-safety reference but at proof time,
not compile time. Compile-time asm = "trust hand-proven invariants."
ZER's Level C trust pattern is similar but lighter (compiler enforces what
it can, defers ISA stuff, user takes responsibility for the rest).

### 14.4 GCC's inline asm (the foundation)

What GCC's assembler does (Level C delegates to this):
- Register name validation per target
- Instruction validity per target
- CPU feature gating via `-m` flags
- Operand encoding
- Assembly output
- Cross-toolchain coverage (15+ architectures)

**Level C reality:** This is the validation layer ZER didn't need to
reimplement. GCC does it for every architecture. ZER's transpiler design
makes this delegation natural.

### 14.5 Convergent design

All three production compilers (Rust, Zig, seL4) and the underlying
toolchain (GCC) converge on:
- Validate operand boundary (types, classes)
- Defer instruction validation to assembler
- Provide escape hatch for raw asm
- Optional formal proofs for safety-critical (Vale, seL4)

**Level C brings ZER fully into this convergent design.** Better than A
(which tried to validate per-instruction). Aligned with B (defers register
names) but goes further by also deferring CPU features.

---

## 15. Effort Breakdown — 6 Commits

Level C execution: 6 commits, ~1-2 days.

### Commit 1: Refactor F7-full Step 2 to hardcoded UB classics list (~2-3 hours)

**Goal:** Eliminate dependency on instruction tables for safety checks.

Steps:
1. In `checker.c`, add the hardcoded `ub_classics[]` array (~30 lines)
2. Add `check_ub_classics()` function that dispatches to existing VRP / alignment infrastructure
3. Replace existing F7-full Step 2 dispatch code (which reads instruction tables) with calls to `check_ub_classics()`
4. Verify all F7-full Step 2 tests still pass (BSR-on-zero, IDIV-on-zero, MOVAPS-misaligned, etc.)

**Files touched:** `checker.c` only.
**Lines:** +50 added, ~80 deleted = -30 net.
**Risk:** LOW. Same logic, smaller surface.

### Commit 2: Delete instruction tables and probe script (~1 hour)

**Goal:** Remove the instruction-level database.

Steps:
1. Delete `src/safety/asm_instruction_table_x86_64.c`
2. Delete `src/safety/asm_instruction_table_aarch64.c`
3. Delete `src/safety/asm_instruction_table_riscv64.c`
4. Delete `src/safety/asm_instruction_table.h`
5. Delete `scripts/gen_instruction_table.sh`
6. Delete `arch_data/` directory entirely (4 files: x86_64.zerdata, aarch64.zerdata, riscv64.zerdata, SCHEMA.md)
7. Update Makefile: remove from CORE_SRCS, LIB_SRCS, remove `gen-asm-tables` target
8. Run `make docker-check` — all tests still pass (Commit 1 made checker independent of these)

**Files touched:** Delete 7+ files, modify Makefile.
**Lines:** -1,906 deleted (374 + 1,532).
**Risk:** LOW. Commit 1 already made checker independent.

### Commit 3: Delete register tables and lookup (~2 hours)

**Goal:** Defer register name validation to GCC.

Steps:
1. In `checker.c`, find and remove register name validation calls
2. Add note: "register name validation deferred to GCC; ZER validates operand types and class via existing systems"
3. Delete `src/safety/asm_register_tables_x86_64.c`
4. Delete `src/safety/asm_register_tables_x86_64_avx512f.c`
5. Delete `src/safety/asm_register_tables_aarch64.c`
6. Delete `src/safety/asm_register_tables_riscv64.c`
7. Delete `src/safety/asm_register_lookup.c`
8. Delete `src/safety/asm_register_tables.h`
9. Delete `scripts/gen_register_tables.sh`
10. Delete `scripts/candidates_*.txt` (3 files)
11. Update Makefile
12. Update tests that previously checked register-name errors — they now expect GCC errors at the assembly stage

**Files touched:** Delete 10+ files, modify Makefile, update some tests.
**Lines:** -1,716 deleted (716 + 124 + 468 + 408 misc).
**Risk:** MEDIUM. Test updates needed. Some test assertions change from "ZER error: ..." to "GCC error: ...".

### Commit 4: Delete asm_categories framework and Session G plumbing (~1 hour)

**Goal:** Remove dead-weight infrastructure with no enforcement.

Steps:
1. Delete `src/safety/asm_categories.c`
2. Delete `src/safety/asm_categories.h`
3. In `checker.c`, remove any references to category framework
4. Remove `ZerBarrierKind` enum and `ZerOrderingRole` enum (Session G plumbing)
5. Remove ordering-related fields from any remaining structs
6. Update Makefile
7. Run tests — verify nothing broke (no enforcement was active)

**Files touched:** Delete 2 files, modify `checker.c`, modify Makefile.
**Lines:** -302 deleted (252 + 50).
**Risk:** LOW. No active enforcement to break.

### Commit 5: Remove CPU feature enum, switch to GCC -m flag passthrough (~2 hours)

**Goal:** Defer CPU feature gating to GCC.

Steps:
1. In `checker.c`, find the CPU feature enum (`ZerCpuFeature`) and flag parsing
2. Replace with a simpler `--target-features=foo,bar,baz` flag parser that just collects the names
3. In emitter or zerc_main, pass each as `-mfoo`, `-mbar`, etc. to GCC
4. Remove the per-feature enum and bitmap
5. Remove operand `cpu_feature:` annotation processing (not in Level C scope)
6. Update tests:
   - Tests that used `--target-features=avx512f` to allow `xmm16` register → adjust to verify GCC accepts with the flag and rejects without
7. Documentation update for the simplified flag

**Files touched:** `checker.c`, `zerc_main.c`, possibly some tests.
**Lines:** -30 deleted, +10 added (string passthrough) = -20 net.
**Risk:** LOW-MEDIUM. Test assertions change.

### Commit 6: Clean up obsolete planning docs and update README/CLAUDE.md (~1-2 hours)

**Goal:** Reflect Level C reality in documentation.

Steps:
1. In `docs/asm_plan.md`, delete obsolete planning sections:
   - "Session G Phase 5 implementation plan" subsections
   - "Re-audit round 2-6"
   - Detailed F4-F7 trajectory planning (keep status snapshot)
   - 8-category framework details (keep brief reference)
2. Keep historical context (Sub-Extension Architecture validation, decision log)
3. Update `CLAUDE.md` Stage 4 section to reflect Level C completion
4. Update `docs/asm_lang_zer_safe.md` if any tweaks needed
5. Update `docs/limitations.md` to reflect Level C scope
6. Update `BUGS-FIXED.md` with the cleanup record

**Files touched:** Docs only.
**Lines:** ~3,000 lines deleted from asm_plan.md.
**Risk:** LOW. Documentation only.

### Total

| Commit | Effort | Lines net | Risk |
|---|---|---|---|
| 1. Refactor to hardcoded list | 2-3 hrs | -30 | LOW |
| 2. Delete instruction tables | 1 hr | -1,906 | LOW |
| 3. Delete register tables | 2 hrs | -1,716 | MEDIUM |
| 4. Delete asm_categories + Session G | 1 hr | -302 | LOW |
| 5. CPU feature passthrough | 2 hrs | -20 | LOW-MED |
| 6. Doc cleanup | 1-2 hrs | -3,000 | LOW |
| **Total** | **9-12 hrs (1-2 days)** | **~-7,000** | **LOW** |

After completion: **TRULY zero maintenance** going forward.

---

## 16. Implementation Strategy

### 16.1 Sequencing principle

Each commit must leave the codebase in a working state. Tests pass after
every commit. The sequence is:

1. **First, make checker.c independent of the things we're deleting**
   (Commit 1 — refactor F7-full Step 2 to hardcoded list)
2. **Then delete the now-unused infrastructure** (Commits 2, 3, 4)
3. **Then simplify what remains** (Commit 5 — CPU feature passthrough)
4. **Finally clean up docs** (Commit 6)

This ordering ensures no broken intermediate state.

### 16.2 Per-commit checklist

For each commit:
- [ ] Make changes
- [ ] `make docker-build` succeeds
- [ ] `make docker-check` passes (all tests)
- [ ] `bash tools/walker_audit.sh` passes
- [ ] `bash tools/agreement_audit.sh` shows zero disagreements
- [ ] Commit with descriptive message
- [ ] No push until all 6 commits ready

### 16.3 Atomic by design

If any commit fails (build broken, test regression), revert and investigate
before proceeding. Each commit is independently revertable.

### 16.4 Validation milestones

After Commit 1: F7-full Step 2 tests still pass via hardcoded list
After Commit 2: Build still works without instruction tables
After Commit 3: Build still works without register tables; expected test assertion changes
After Commit 4: Build still works without categories framework
After Commit 5: Build still works with CPU feature passthrough
After Commit 6: Final docs accurate, all tests pass

### 16.5 Push as one batch

After all 6 commits succeed locally:
1. Final `make docker-check` to ensure full suite passes
2. `git push origin main` (push all 6 commits)
3. Verify GitHub CI passes
4. Tag if appropriate

---

## 17. Testing Strategy

### 17.1 Existing test coverage that must keep working

All existing tests must pass after cleanup. Specifically:

**F7-full Step 2 tests** (now driven by hardcoded UB classics list):
- `tests/zer_fail/asm_bsr_zero.zer` — BSR with unprovable nonzero
- `tests/zer_fail/asm_idiv_zero.zer` — IDIV with unprovable nonzero
- `tests/zer_fail/asm_movaps_misalign.zer` — MOVAPS with unprovable alignment
- Positive equivalents that compile clean

**F7-light tests** (unchanged):
- `tests/zer_fail/asm_lrsc_unbalanced.zer`
- `tests/zer_fail/asm_lrsc_orphan_sc.zer`
- Positive equivalents

**Z-rule tests** (unchanged):
- `tests/zer_fail/asm_uaf.zer`
- `tests/zer_fail/asm_move_transfer.zer`
- etc.

### 17.2 Test assertion updates (Commit 3 specifically)

Tests that previously expected ZER errors for register-name violations
now expect GCC errors at the assembly stage. Update:

```bash
# Before (Level A):
$ zerc test.zer
test.zer:5: asm: register 'rax' not valid on aarch64 target

# After (Level C):
$ zerc test.zer  # → emits C
$ aarch64-linux-gnu-gcc test.c
<inline asm>:1: Error: unknown register name 'rax'
```

Test files updated:
- `tests/zer_fail/asm_aarch64_x86_reg.zer` — adjust expected output
- `tests/zer_fail/asm_riscv64_x86_reg.zer` — adjust expected output
- `tests/test_cross_arch.sh` — adjust assertion patterns

### 17.3 Cross-arch test changes (Commit 5)

Tests that use CPU features now rely on GCC's `-m` flag enforcement:

```bash
# Before (Level A):
zerc test.zer --target-features=avx512f  # checker validates

# After (Level C):
zerc test.zer --target-features=avx512f  # passes through to GCC as -mavx512f
# GCC's assembler validates xmm16 usage matches -mavx512f
```

### 17.4 Build verification

- Confirm `make docker-build` works without the deleted files
- Confirm zerc binary still functions
- Confirm all 130 intrinsics still emit correctly
- Confirm Z-rules still fire

### 17.5 Regression suite

Full test suite: ~5,000+ tests including:
- `tests/test_zer.sh` (positive + negative ZER tests)
- `rust_tests/run_tests.sh` (786 Rust equivalents)
- `zig_tests/run_tests.sh` (36 Zig equivalents)
- `test_modules/run_tests.sh` (28 multi-module)
- C unit tests (~95)
- Semantic fuzzer (200/run)
- Cross-arch tests

All must pass after Commit 6.

---

## 18. Risks and Mitigations

### 18.1 Risk: Test assertion changes break CI

**Scenario:** Tests checking "ZER error: register 'rax' invalid on aarch64"
now see "GCC error: unknown register".

**Mitigation:** Update test assertions in Commit 3 to expect GCC errors.
Test framework adjustment, not safety regression.

### 18.2 Risk: F7-light LR/SC pairing breaks due to dependency removal

**Scenario:** F7-light hardcoded mnemonics depend on something we're deleting.

**Mitigation:** Verify F7-light has NO dependencies on instruction tables
(should be self-contained mnemonic-string matching). Verify in Commit 1
that F7-light tests still pass after refactor.

### 18.3 Risk: User loses CPU feature compile-time check

**Scenario:** User types `--target-features=avx512`. ZER passes to GCC as
`-mavx512`. GCC errors "unknown option `-mavx512`" (should have been
`-mavx512f`).

**Mitigation:** This is a typo at runtime; user fixes it. GCC's error is
clear. No safety regression — user can't compile with wrong flags either way.
Alternatively, can keep a small list of valid GCC `-m` flags for hint
generation (~30 lines, frozen). Defer that decision.

### 18.4 Risk: Some legitimate asm pattern breaks

**Scenario:** Existing valid asm test stops compiling for some reason.

**Mitigation:** Each commit tested with full suite. Atomic revertability.
Investigate and fix or revert before proceeding.

### 18.5 Risk: Sub-extension support degrades

**Scenario:** AVX-512 user can't use `xmm16` anymore.

**Mitigation:** They can. They pass `--target-features=avx512f` →
`-mavx512f` → GCC's assembler accepts `xmm16`. Same as before, just GCC
enforces instead of ZER.

### 18.6 Risk: Loss of probe-script automation

**Scenario:** Future ISA extension ships, register tables need updating.

**Mitigation:** At Level C, ZER doesn't track register tables at all.
GCC tracks them. When new ISA extensions ship, GCC adds support; ZER
inherits automatically. This is a feature of Level C, not a risk.

### 18.7 Risk: User-facing error messages change

**Scenario:** Some errors become GCC errors instead of ZER errors. Less
integrated with ZER's error formatting.

**Mitigation:** Document this clearly. Users understand that asm errors
come from GCC. The error message is still actionable. This is also Rust's
model — Rust's `asm!()` errors come partially from LLVM.

---

## 19. Open Questions Decided

### 19.1 Q: Level A, B, or C? → DECIDED: Level C

Per the analysis in section 7. Level C dominates for ZER's specific situation
(sole dev, multi-arch, transpiler design, GCC audience).

### 19.2 Q: Are GCC error messages clear enough? → YES

GCC's assembler error messages are well-established and actionable. The
slight UX delta vs ZER-formatted messages is acceptable for the
maintenance savings.

### 19.3 Q: Will users miss compile-time CPU feature gating in ZER? → NO

Users pass `-mavx512f` flag. GCC validates. Same outcome.

### 19.4 Q: What about Keil/IAR/MSVC users? → NOT ZER'S AUDIENCE

Documented explicitly. Future work can add `--emit-target=` modes if real
demand emerges. Not blocking Level C.

### 19.5 Q: What if a future ISA has well-known UB beyond the ~12 classics? → ADD ENTRY

The hardcoded UB classics list is small but extensible (just add an entry
in checker.c). Maintenance: ~5 lines per addition. Frequency: maybe once
per decade (most ISAs don't ship genuinely new UB classes).

### 19.6 Q: Should Session G Phase 5 ever happen? → NO

Deferred indefinitely. Niche (persistent memory + concurrency). Users
can use `requires:`-style annotations if Level D is ever pursued.

### 19.7 Q: Are we sure register tables aren't needed? → YES

Validated extensively. Rust doesn't have them. Zig doesn't have them.
GCC's assembler validates register names. The only "loss" is error message
formatting, not safety.

---

## 20. Out of Scope (Explicitly NOT Doing)

### 20.1 NOT doing: Full operand-kind IR rewrite

Sketched in earlier sessions as Option 1. ~8-12 months, no user-visible
benefit. Rejected.

### 20.2 NOT doing: Multi-backend support (LLVM, MSVC)

Not ZER's audience. ZER → GNU C (works with GCC and Clang). Future
`--emit-target=` modes possible but not planned.

### 20.3 NOT doing: Direct asm-byte emission

GCC handles assembly. No reinvention.

### 20.4 NOT doing: User-extensible UB classics list

The list is compiler-internal. Users who need specific instruction
preconditions can use `naked` function with `// SAFETY:` comment.

### 20.5 NOT doing: Vale-tier formal proofs

Algorithm correctness is opt-in, separate dimension. Deferred to v2.x+.
Not part of Level C.

### 20.6 NOT doing: Per-instruction operand annotations (Option 2 / Level A)

The annotation-driven approach was an intermediate design (Level A). Level
C simplifies further by deferring to GCC. Annotations are not added.

### 20.7 NOT doing: Session G Phase 5 / Z9-Z13 forward-compat

Deferred indefinitely. Niche, complex, hard to get right (Phase 3 lesson).

### 20.8 NOT doing: Probe-script ecosystem (Capstone/XED integration)

Deleted at Level C. Not coming back.

---

## 21. The "Smart Language vs Smart Compiler" Principle

### 21.1 The principle (one sentence)

**Compilers should be generic algorithms over annotated languages, not
databases of facts about specific entities.**

### 21.2 Level C as full application of the principle

At Level C, ZER's asm safety architecture is:

```
LANGUAGE (smart, frozen vocabulary):
  - naked function attribute
  - structured asm { } syntax
  - typed operand bindings
  - 130 intrinsics (fixed set)
  
COMPILER (generic, frozen algorithms):
  - Z-rules (existing safety systems applied to operands)
  - F7-light state machine (hardcoded LR/SC, ~50 lines)
  - Hardcoded UB classics list (~12 entries, frozen)
  
DELEGATION (defer to other tools):
  - Register name validation → GCC
  - Instruction validity → GCC
  - CPU feature gating → GCC -m flags
  - Sub-extension support → GCC
```

**No per-item databases. No probe scripts. No data files. No category
frameworks. No CPU feature enums. No instruction tables.**

The compiler stays generic. The language is frozen vocabulary. ISA
specifics defer to whoever owns that domain (GCC).

### 21.3 The general rule (reiterated)

If you're tempted to add per-X knowledge to the compiler:

```
1. Identify the property you're encoding
2. Either:
   a. Smart language — design a frozen annotation expressing it
   b. Smart delegation — defer to a more authoritative tool
   c. Hardcoded frozen list — small fixed set of well-known cases
3. Don't grow a per-X database
```

Level C applies (b) for ISA-specific things and (c) for ~12 well-known
UB classics. (a) — language annotations — was Level A's approach,
ultimately not needed because (b) covers the same cases.

---

## 22. Fresh-Session Onboarding Checklist

### 22.1 Read these documents in order

1. **This document end-to-end** (~30 min)
2. `docs/asm_plan.md` warning block + Sub-Extension Architecture section
   (~10 min) — historical context
3. `CLAUDE.md` Stage 4 section — current asm status

### 22.2 Verify current state

```bash
# Confirm 130 intrinsics implemented
grep -E "@cpu_|@atomic_|@barrier|@bswap|@popcount|@ctz|@clz" src/builtins.c | wc -l

# Confirm Z-rules wired (Z1-Z8, Z11, Z12 — 10 of 13)
grep -E "^/\* Z[0-9]+:" checker.c zercheck_ir.c | wc -l

# Confirm tests pass
make docker-check
```

### 22.3 Understand the architectural principle

Read section 21 of this document. The "smart language, generic compiler"
principle is the foundation.

### 22.4 Understand the delegation pattern

At Level C, ZER's asm safety is divided into:
- **What ZER validates:** memory/type/concurrency safety, operand types,
  well-known UB classics, LR/SC pairing
- **What GCC validates:** register names, instruction validity, CPU
  features, assembly syntax, ISA-specific stuff

ZER doesn't try to be smart about ISA-specific things. GCC is. ZER
trusts GCC for that layer.

### 22.5 Execute the cleanup (if not yet done)

Sequence per section 16:
1. Commit 1: Refactor F7-full Step 2 to hardcoded UB classics list
2. Commit 2: Delete instruction tables
3. Commit 3: Delete register tables and lookup
4. Commit 4: Delete asm_categories and Session G plumbing
5. Commit 5: Remove CPU feature enum, switch to GCC -m passthrough
6. Commit 6: Clean up obsolete planning docs

Total: ~1-2 days.

### 22.6 Decision checkpoints (these are LOCKED IN)

Don't revisit these unless explicit user direction:

- Naked-only restriction (S1) is permanent
- GCC is the C compiler dependency
- Per-instruction database not coming back
- Register tables not coming back
- CPU feature enum not coming back
- Probe scripts not coming back
- Smart-language principle applies
- ~12 hardcoded UB classics is the implicit table

### 22.7 Anti-patterns to avoid

- DON'T add per-instruction safety knowledge to ZER
- DON'T add register name validation to ZER
- DON'T add CPU feature enum to ZER
- DON'T extend the hardcoded UB classics list beyond well-known classics
- DON'T add Z14, Z15, etc. (annotation framework was Level A's idea, not used)
- DON'T resurrect Session G Phase 5
- DON'T claim "100% asm safety" — claim what's true (see section 2)

---

## Appendix A: Concrete File-by-File Deletion List

For implementation reference, here's every file affected by Level C.

### A.1 Files to DELETE entirely

```
src/safety/asm_instruction_table_x86_64.c       (75 lines)
src/safety/asm_instruction_table_aarch64.c      (59 lines)
src/safety/asm_instruction_table_riscv64.c      (52 lines)
src/safety/asm_instruction_table.h               (188 lines)
src/safety/asm_register_tables_x86_64.c          (121 lines)
src/safety/asm_register_tables_x86_64_avx512f.c  (177 lines)
src/safety/asm_register_tables_aarch64.c          (74 lines)
src/safety/asm_register_tables_riscv64.c         (147 lines)
src/safety/asm_register_lookup.c                  (81 lines)
src/safety/asm_register_tables.h                 (116 lines)
src/safety/asm_categories.c                      (181 lines)
src/safety/asm_categories.h                       (71 lines)
scripts/gen_instruction_table.sh                 (373 lines)
scripts/gen_register_tables.sh                   (124 lines)
scripts/candidates_x86_64.txt                    (255 lines)
scripts/candidates_aarch64.txt                    (63 lines)
scripts/candidates_riscv64.txt                   (150 lines)
arch_data/x86_64.zerdata                         (526 lines)
arch_data/aarch64.zerdata                        (363 lines)
arch_data/riscv64.zerdata                        (323 lines)
arch_data/SCHEMA.md                              (320 lines)
```

Plus delete `arch_data/` directory entirely after files removed.

**Total: 21 files, ~3,840 lines deleted.**

### A.2 Files to MODIFY

```
checker.c — refactor F7-full Step 2 dispatch, remove register validation
            calls, remove CPU feature enum
zerc_main.c — simplify --target-features= to passthrough
Makefile — remove CORE_SRCS/LIB_SRCS entries for deleted files,
           remove gen-asm-tables target
docs/asm_plan.md — delete obsolete planning sections (~3,000 lines)
CLAUDE.md — update Stage 4 to reflect Level C completion
docs/limitations.md — update asm scope
docs/asm_lang_zer_safe.md — this file, finalize Level C reality
BUGS-FIXED.md — append cleanup record
tests/zer_fail/asm_aarch64_x86_reg.zer — update expected output
tests/zer_fail/asm_riscv64_x86_reg.zer — update expected output
tests/test_cross_arch.sh — adjust assertion patterns
```

### A.3 Files to KEEP unchanged

```
src/safety/handle_state.c, .h                    (Z-rules infrastructure)
src/safety/range_checks.c, .h                    (Z-rules infrastructure)
src/safety/type_kind.c, .h                       (Z-rules infrastructure)
src/safety/coerce_rules.c, .h                    (Z-rules infrastructure)
src/safety/context_bans.c, .h                    (Z-rules infrastructure)
src/safety/escape_rules.c, .h                    (Z-rules infrastructure)
src/safety/provenance_rules.c, .h                (Z-rules infrastructure)
src/safety/mmio_rules.c, .h                      (Z-rules infrastructure)
... (all other Z-rule extracted predicates)
```

These provide the safety primitives Z-rules dispatch to. Unchanged.

```
src/builtins.c                                    (130 intrinsics)
parser.c                                         (Session A/B asm syntax)
ast.c, ast.h                                     (AsmNode, operand bindings)
emitter.c                                        (GCC inline asm emission)
zercheck_ir.c                                    (Z-rules implementation)
```

These provide the asm framework. Unchanged (Level C is just deletion).

---

## Appendix B: The Hardcoded UB Classics — Concrete Contents

Final concrete list. Frozen reference for implementation.

### B.1 Bit-search (UB on zero operand)

| Mnemonic | Arch | Constraint | Citation |
|---|---|---|---|
| `bsr` | x86 | NONZERO on operand 0 | Intel SDM Vol 2 BSR |
| `bsf` | x86 | NONZERO on operand 0 | Intel SDM Vol 2 BSF |

Note: `lzcnt`, `tzcnt`, `popcnt` are well-defined on zero, no constraint.

### B.2 Integer division (UB on zero divisor, signed overflow)

| Mnemonic | Arch | Constraint | Citation |
|---|---|---|---|
| `div` | x86 | NONZERO on operand 0 | Intel SDM Vol 2 DIV |
| `idiv` | x86 | NONZERO on operand 0; INT_MIN/-1 backstop | Intel SDM Vol 2 IDIV |

### B.3 Aligned vector loads (SIGSEGV on misaligned)

| Mnemonic | Arch | Constraint | Citation |
|---|---|---|---|
| `movaps` | x86 SSE | ALIGN_16 on memory operand | Intel SDM Vol 2 MOVAPS |
| `movapd` | x86 SSE2 | ALIGN_16 on memory operand | Intel SDM Vol 2 MOVAPD |
| `movdqa` | x86 SSE2 | ALIGN_16 on memory operand | Intel SDM Vol 2 MOVDQA |
| `vmovaps` | x86 AVX (256-bit) | ALIGN_32 on memory operand | Intel SDM Vol 2 VMOVAPS |
| `vmovapd` | x86 AVX (256-bit) | ALIGN_32 on memory operand | Intel SDM Vol 2 VMOVAPD |
| `vmovdqa` | x86 AVX (256-bit) | ALIGN_32 on memory operand | Intel SDM Vol 2 VMOVDQA |

Note: 512-bit AVX-512 equivalents (`vmovaps zmm0...`) could be added as
ALIGN_64 if real users hit issues. Defer until needed.

### B.4 Total

**10 entries.** Frozen. Could grow to ~12-15 if AVX-512 alignment variants
or ARM/RISC-V well-known UB instructions are added based on user demand.
No automatic growth — manual addition per discovered need.

### B.5 What's NOT in the list and why

**LR/SC pairing** — handled by F7-light separately, not in this table.
**Privileged instructions (RDMSR/WRMSR etc.)** — user in naked function
takes responsibility; GCC handles privilege-level errors.
**Cache management (CLFLUSH/CLWB)** — user-mode safe; no compile-time
constraint needed.
**Atomic primitives (LOCK prefix, etc.)** — well-defined; GCC handles.
**Shift count > width** — C language handles (UB in C, ZER `_zer_shl`
runtime check applies).
**Most x86/ARM/RISC-V instructions** — well-defined, GCC handles, ZER
type system handles operand types.

---

## Appendix C: What ZER's Public Safety Claim Becomes

### C.1 Pre-pivot claim (asm_plan original)

> "ZER's asm safety: 100% language-safe via 13 Z-rules + 8 universal
> precondition categories + System #30 atomic ordering. Per-instruction
> preconditions enforced for ~120 instructions across x86_64, aarch64,
> riscv64. Strict mode catches UB at compile time."

**Problems:**
- "100% language-safe" oversold
- "Per-instruction preconditions" commits to perpetual maintenance
- Specifies 3 archs (limits architecture support)

### C.2 Level A claim (intermediate)

> "ZER's asm safety: operand-type checking + Z-rules + 130 intrinsics +
> operand-level precondition annotations + ~100 well-known UB classics
> auto-protected + per-arch register validation + CPU feature gating
> for x86_64, aarch64, riscv64. Annotations enforced via existing safety
> infrastructure."

**Still has problems:**
- Commits to per-arch maintenance (register tables)
- Commits to CPU feature enum
- Still 3 archs specifically

### C.3 Level C claim (FINAL)

> "ZER's asm safety: operand-type checking via existing ZER type system +
> Z-rules (memory/type/concurrency/MMIO safety through asm operands) +
> 130 intrinsics for common kernel patterns + hardcoded protection for
> well-known UB classics (BSR-on-zero, IDIV-on-zero, MOVAPS-misaligned,
> LR/SC pairing) + naked-fn isolation (MISRA Dir 4.3). Register
> validation, instruction validity, and CPU feature gating delegated to
> the user's C compiler. Works on every architecture GCC supports."

**Why this is better:**
- Honest about what ZER actually does
- Honest about what ZER defers
- "Every architecture GCC supports" is stronger than "3 archs"
- No commitment to per-arch maintenance
- Matches Rust/Zig design with small bonus (UB classics)

### C.4 Comparison with Rust/Zig public claims

**Rust `asm!()` documentation says:**
> "Type-checked operand bindings, register class validation, CPU feature
> via #[cfg]. Unsafe block boundary. Inline asm contents are not validated;
> use // SAFETY: comments."

**Zig `asm` documentation says:**
> "GCC-style inline assembly with typed operand bindings. Operand types
> verified. Assembly content compiled by LLVM."

**ZER Level C claim:**
> "Same operand-type checking as Rust/Zig + 130 intrinsics + ~12 hardcoded
> UB classics + Z-rules extending ZER safety through asm operands + naked-fn
> isolation. Works on every GCC-supported architecture."

**ZER is slightly stronger than Rust/Zig in:**
- Hardcoded UB classics (compile-time BSR-on-zero etc.)
- Z-rules extending safety through asm operands (UAF, escape, MMIO etc.)
- Naked-only restriction (MISRA Dir 4.3, stricter than `unsafe`)

**ZER is equivalent to Rust/Zig in:**
- Operand type checking
- Defer-to-assembler for register/instruction validation

**This is an honest, defensible position.**

---

## Appendix D: Mapping from asm_plan to Level C

Cross-reference: what asm_plan.md item becomes what in Level C.

### D.1 asm_plan items that STAY (already shipped, working)

| asm_plan item | Status in Level C |
|---|---|
| 130 intrinsics | STAY |
| Z1-Z8, Z11, Z12 | STAY |
| Session A asm syntax | STAY |
| Session B typed bindings | STAY |
| Naked-only (S1) | STAY |
| Sub-extension architecture decisions | STAY (locked in) |
| F7-light LR/SC | STAY |

### D.2 asm_plan items that GET DELETED

| asm_plan item | Status in Level C |
|---|---|
| F2 x86_64 register table | DELETED — defer to GCC |
| F5 aarch64 register table | DELETED — defer to GCC |
| F6 riscv64 register table | DELETED — defer to GCC |
| F4 instruction table x86_64 | DELETED |
| F4 instruction table aarch64 | DELETED |
| F4 instruction table riscv64 | DELETED |
| F1a 8-category framework | DELETED |
| F2 build-time-gen pipeline (registers) | DELETED |
| Probe scripts | DELETED |
| CPU feature enum + flag mapping | DELETED |
| C8 ordering metadata | DELETED |
| Session G Phase 1+2 plumbing | DELETED |
| F7-full Step 2 table-driven dispatch | REPLACED with hardcoded list (~30 lines) |

### D.3 asm_plan items that were DEFERRED (now permanently)

| asm_plan item | Status |
|---|---|
| Session G Phase 5 (CFG OrderingState) | DEFERRED indefinitely |
| Z9 / Z10 / Z13 forward-compat | DEFERRED indefinitely |
| Naked-attribute migration | DEFERRED indefinitely |
| @verified_spec Vale-tier | DEFERRED to v2.x+ |
| Per-instruction precondition database | NOT GROWING |
| Universal precondition categories (8) | DELETED (was speculative) |
| System #30 (Atomic Ordering) | DELETED (was speculative) |

### D.4 Concept mapping

| asm_plan concept | Level C equivalent |
|---|---|
| "Make compiler smart about instructions" | "Make compiler delegate to GCC" |
| "Per-arch register tables" | "GCC validates registers" |
| "8 universal precondition categories" | "Hardcoded ~12 UB classics" |
| "Universal precondition framework" | "Existing safety systems (VRP, alignment, etc.)" |
| "Sub-extension classification" | "GCC `-m` flag passthrough" |
| "Build-time gen + vendored" | "No data, no gen" |
| "Strict mode default on" | "Naked-only default on" |
| "100% language-safe" | "Rust-level safety + bonus UB classics" |

---

## Appendix E: References

### E.1 ZER documents

- `docs/ASM_ZER-LANG.md` — original 2026-04-01 asm design (foundation)
- `docs/asm_plan.md` — D-Alpha-7.5 Phase 2 plan (historical, superseded)
- `docs/asm_preconditions_research.md` — research artifact (historical)
- `CLAUDE.md` — Stage 4 asm safety status section (update for Level C)
- `BUGS-FIXED.md` — recent asm-related bug fixes
- `docs/refactor_ir.md` — zercheck_ir helper-layer refactor (separate concern)
- `docs/compiler-internals.md` — implementation patterns
- `docs/limitations.md` — known limitations

### E.2 External references

- Intel Software Developer Manual Vol 2 — instruction-specific UB
- ARM Architecture Reference Manual — ARM instruction semantics
- RISC-V ISA Manual (Privileged + Unprivileged) — RISC-V semantics
- Rust Inline Assembly Reference — Rust's asm story
- Rust RFC 2873 — Rust asm! design rationale
- Zig Inline Assembly — Zig's asm syntax
- GCC Inline Assembly — GCC operand constraints
- GCC Machine-Specific Options — `-m` flag list per architecture

### E.3 Production-compiler precedents

- Rust `core::arch` (intrinsics) + `asm!` (raw asm)
- Zig `@asm` builtin
- seL4 proof-driven approach (different scope)
- GCC inline asm (the foundation Level C builds on)

### E.4 Key decisions (chronological)

- 2026-04-23 D-Alpha-7.5 Phase 1 ships (S1 naked-only)
- 2026-04-25 Session A structured asm syntax
- 2026-04-25 Session B typed operand bindings
- 2026-04-26 F1a category framework (now DELETED at Level C)
- 2026-04-26 F2 register tables (now DELETED at Level C)
- 2026-04-29 Sub-extension architecture validation 3-arch
- 2026-05-02 F4-F7 instruction tables (now DELETED at Level C)
- 2026-05-02 Session G Phase 1+2 plumbing (now DELETED at Level C)
- 2026-05-02 Session G Phase 3 abandoned
- 2026-05-11 Phase A/B split decision in refactor_ir
- **2026-05-12 LEVEL C DECISION — this document**

---

## End of Document

This is a planning document. Implementation follows section 16 (6 commits,
~1-2 days).

When executing:
1. Each commit must leave the codebase in a working state
2. Run `make docker-check` after each commit
3. Don't push until all 6 commits succeed locally
4. Test assertion updates are expected in Commits 3 and 5

The decisions in this document are LOCKED IN. The locked-in items:
- Level C is the chosen direction
- Per-instruction database is not coming back
- Register tables are not coming back
- CPU feature enum is not coming back
- Probe scripts are not coming back
- ~12 hardcoded UB classics is the limit
- GCC is the trusted assembler
- Naked-only restriction is permanent

**The result of Level C: ZER's asm safety is honest, simple, and zero-
maintenance forever. ZER fits every architecture GCC supports. Sole
developer is free of ISA-evolution treadmill. Safety is preserved at
the same level as production compilers (Rust, Zig) with a small bonus
(hardcoded UB classics).**

This is the right architecture for ZER.

---

## Level D Pivot Note (added 2026-05-31)

**This document was updated with Section 1.6 capturing the Level D pivot:
user-extensible intrinsics with explicit contracts via `intrinsic_def`
mechanism + `@core::*` blessed namespace + 4-level conditional safety
claim.**

**Read Section 1.6 for the complete pivot context.** Highlights:

- The new lever is "externalize authorship of Y_intrinsic" (different from
  Level C's "constrain Y")
- ZER team's maintenance burden converges from "~1-2 intrinsics/year" to
  "~per-decade safety class additions"
- The blessed catalog (~130 current) becomes FROZEN; new intrinsics flow
  through user libraries via `@<lib>::*` namespace
- Soundness claim becomes honestly conditional: pure ZER + blessed = sound;
  user-defined = sound conditional on author's contract correctness
- The seam (wrong contract at def site) is the same Definition B failure
  mode at the intrinsic_def boundary — properly named, mitigated by
  greppable trusted-once auditing
- Level C cleanup ships FIRST (Section 16). Level D adds intrinsic_def
  on the clean foundation.
- Estimated implementation: ~750-1500 lines, ~4-8 weeks for one developer

**Level D is LOCKED IN as of 2026-05-31.** Future sessions: reference
Section 1.6 instead of re-deriving the pivot.

---

## Document History

| Date | Change |
|---|---|
| 2026-05-05 (drafted) | Initial Level C planning |
| 2026-05-10 | Audit pass |
| 2026-05-11 | Phase A/B split |
| 2026-05-12 (decision) | Level C chosen over Level A/B |
| 2026-05-12 (evening) | 2-layer model crystallization (Section 1.5) |
| **2026-05-31** | **Level D pivot — user-extensible intrinsics added (Section 1.6)** |
