# ZER-Asm Safety — Level C: Defer to GCC, Frozen Core

**Status:** Planning document. Decision finalized 2026-05-12. Execution pending.
**Date:** 2026-05-05 (drafted), 2026-05-10 (audit), 2026-05-11 (Phase A/B split),
2026-05-12 (Level C decision — this version)
**Supersedes:** the extension trajectory of `docs/asm_plan.md` (Session G Phase 5,
Z9/Z10/Z13, per-instruction database growth, register-table maintenance,
CPU feature gating tracking).
**Decision:** **Level C** — drop everything ISA-specific from ZER's compile-time
asm validation. Defer to GCC. Keep only the truly frozen safety layer.

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
