# Primitives, Data Races, and I/O Safety — ZER-LANG Design Document

**Status:** Design crystallization. Captures the converged architectural analysis
across multiple design discussions. The decisions documented here are LOCKED
IN for current scope unless real firmware code surfaces gaps the analysis missed.

**Scope:** This document is the canonical reference for:
1. The unified safety architecture principle (Definition A)
2. ZER's hardware/board interaction primitives (I/O surface)
3. ZER's data race safety story (concurrency safety substrate)
4. The Trusted Language Base (TLB) framework
5. Substrate accountability principle (rescoped to what's structurally verifiable)
6. Implementation roadmap and audit task

**Locked decisions captured:**
- **The Definition A principle:** ZER verifies access mechanism structurally,
  hardware semantics are user responsibility supplied via primitive choice
- **Two-tier primitive model:** strict primitives + context primitives = TLB
- **Closure argument:** every data race in pure ZER code is structurally
  detectable when 4-5 missing structural rules ship
- **ISR-to-main scoped to Option 3:** synchronization discipline is user
  responsibility within ZER's hardware-access primitives
- **No peripheral-specific catalog in core ZER:** category-level only;
  peripheral specifics go to vendor library ecosystem
- **Hardware spec verification is deliberately out of scope:** ZER doesn't
  read datasheets; users supply hardware spec knowledge via declarations and
  primitive choice

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [The Definition A Principle](#2-the-definition-a-principle)
3. [ZER's Five Safety Domains (Unified Framing)](#3-zers-five-safety-domains-unified-framing)
4. [Why This Document Exists](#4-why-this-document-exists)
5. [Hardware/Board Interaction Primitives](#5-hardwareboard-interaction-primitives)
6. [The Volatile Safety Mechanism (No Bypass Exists)](#6-the-volatile-safety-mechanism-no-bypass-exists)
7. [The Data Race Substrate](#7-the-data-race-substrate)
8. [The Trusted Language Base (TLB) Framework](#8-the-trusted-language-base-tlb-framework)
9. [Strict Primitives](#9-strict-primitives)
10. [Context Primitives (Sealed Layer)](#10-context-primitives-sealed-layer)
11. [User-Buildable Coordination Patterns](#11-user-buildable-coordination-patterns)
12. [Structural Rules for Closure](#12-structural-rules-for-closure)
13. [The Closure Argument](#13-the-closure-argument)
14. [ISR-to-Main Interaction (Option 3)](#14-isr-to-main-interaction-option-3)
15. [Why Option 2 (Contracts) Fails](#15-why-option-2-contracts-fails)
16. [The Substrate Accountability Principle Rescoped](#16-the-substrate-accountability-principle-rescoped)
17. [Comparison to Other Languages](#17-comparison-to-other-languages)
18. [Implementation Roadmap](#18-implementation-roadmap)
19. [The Primitive Audit Task](#19-the-primitive-audit-task)
20. [Open Questions](#20-open-questions)
21. [Architectural Principles Generalized](#21-architectural-principles-generalized)
22. [Anti-Patterns to Avoid](#22-anti-patterns-to-avoid)
23. [The Drift Pattern Recognition](#23-the-drift-pattern-recognition)
24. [Glossary](#24-glossary)

---

## 1. Executive Summary

ZER's safety model operates on a unified architectural principle applied across
five safety domains (memory, type, ASM, concurrency, I/O):

> **The Definition A Principle:** ZER verifies access mechanism correctness
> structurally — types, ranges, qualifiers, contexts, dependencies — and
> treats semantic correctness against external substrates (hardware,
> foreign code, contract truths) as user responsibility, supplied through
> primitive choice and declarations rather than verified by the language.

This principle applies uniformly to:
- **Memory safety:** allocation primitives verify access mechanism; ownership intent supplied via primitive choice
- **Type safety:** conversion intrinsics verify structural correctness; conversion intent supplied via intrinsic choice
- **ASM safety:** intent intrinsics verify operand correctness; ISA-specific behavior delegated to GCC
- **Concurrency safety:** concurrency primitives + structural rules verify race freedom; synchronization discipline within bounded primitive set
- **I/O safety:** hardware-access primitives verify access mechanism; hardware spec correctness is user responsibility

**For concurrency specifically:**

- **5 strict primitives** (computationally irreducible): threadlocal, @atomic_load/store/cas, shared struct, spawn/ThreadHandle, move struct
- **~5 context primitives** (audience-irreducible, tightly composed): @cond_*, @atomic_arithmetic_variants, Semaphore, Barrier, shared(rw) struct
- **4-5 structural rules to implement** for full closure: while-loop on cond_wait, CAS progress, lock balance, atomic-on-shared-only, no-shared-stack

**For I/O specifically:**

- **MMIO** via `@inttoptr(*T, addr)` + `mmio` declarations + `volatile *T`
- **No C-style cast bypass exists** — ZER's grammar rejects integer-to-pointer casts; the only path is `@inttoptr`
- **130 hardware intrinsics** (CPU privileged ops, port I/O, cache ops, barriers, atomics) all follow Definition A
- **Audit task** to verify each existing primitive's contract matches Definition A consistency

**For ISR-to-main concurrency:**

- **Option 3:** ZER's hardware-access primitives catch access-mechanism bugs;
  synchronization discipline between ISR and main thread is user responsibility
  within provided primitives, because verifying it would require hardware-
  execution-model knowledge ZER deliberately delegates to the user

**The closure argument:**

Every data race in pure ZER code is structurally detectable because every
operation that could create, share, or synchronize concurrent memory access
goes through bounded primitive sets the compiler sees. C lacks this closure
(threads/sharing/sync via libraries). Rust has closure but with `unsafe impl
Send` escape; ZER has closure without an in-language escape (only explicit
cinclude boundary).

---

## 2. The Definition A Principle

### 2.1 The Two Definitions of "Primitive"

Two different definitions of what counts as a primitive lead to two different
language architectures:

**Definition A — Primitive = the basic access mechanism**

Under this definition, primitives encode access mechanisms (read, write,
fabricate, transfer) along with their structural properties (type, range,
alignment, qualifier, context). User intent is declared through primitive
choice and declarations. The compiler verifies that the chosen primitive is
used consistently with its declared contract. Semantic correctness against
external substrates (hardware, foreign code, contract truths) is user
responsibility, supplied as input to the verification rather than verified
by it.

**Definition B — Primitive = the basic operation with its semantics**

Under this definition, primitives encode specific operational semantics
(read-clears-on-read register, write-1-to-clear status bit, write-only
register). User declares hardware properties; the compiler verifies code
matches the declared properties. This is SPARK/Ada-style contract verification.

### 2.2 ZER's Architectural Choice: Definition A

ZER uses Definition A across all five safety domains. The rationale:

**Definition B requires user-supplied contracts.** Contracts are unverified
user claims. The compiler can verify code-against-contract but cannot verify
contract-against-reality. If the user's contract is wrong, the compiler
verifies a wrong contract and produces false confidence. The bug ships
anyway, but with a "verified" wrong contract.

**Definition A doesn't require contracts.** The user declares intent through
primitive choice (which intrinsic to use, what type qualifier, what allocator).
The compiler verifies the intent is structurally consistent. Hardware
correctness against silicon is user responsibility, supplied via primitive
choice. No contract correctness problem.

**Definition A scales to sole-developer maintenance.** Definition B requires
either (a) growing into a SPARK-like contract system or (b) building a
peripheral catalog the compiler maintains. Both conflict with ZER's
sole-developer positioning. Definition A keeps the primitive surface bounded
at the access-mechanism level.

**Definition A matches ZER's audience.** ZER targets firmware/embedded
developers who want C-level cognitive load with safety enforcement.
Definition B requires contract-writing expertise this audience often lacks.
Definition A requires only primitive selection — vocabulary navigation, not
contract specification.

### 2.3 What Definition A Verifies vs Doesn't Verify

**Definition A verifies (per primitive):**
- Input preconditions (types, ranges, alignment, qualifiers)
- Output postconditions (declared type, declared qualifier, declared properties)
- Effect propagation (qualifier preservation, range invalidation, escape flags)
- Context restrictions (valid call sites — naked fn, ISR, critical section, etc.)
- Composition rules (when primitives have type-state dependencies)

**Definition A does NOT verify (per primitive):**
- Whether the actual hardware/foreign-code behavior matches the declared expectation
- Whether the user's choice of which primitive to use is semantically correct for their intent
- Whether composition sequences produce intended hardware effects beyond what's structurally encoded
- Whether timing or ordering requirements outside the primitive's contract are satisfied

### 2.4 The Honesty Property

Definition A produces an honest safety claim within a precisely defined scope:

> Code that satisfies all primitive contracts is language-level safe. Hardware
> correctness against the user's intended target is user responsibility.

This claim is:
- **Unconditional within scope:** no "mostly," no "common cases," no asterisks
- **Precise about boundaries:** what's verified and what isn't is explicit
- **Defensible against examination:** no hidden assumptions, no overclaim

Compare to language safety claims that obscure their boundaries (e.g., "memory
safe" without specifying which code, "race-free" without specifying which
concurrency model). Definition A forces the boundary to be explicit.

### 2.5 The Three Categories of Errors

A useful taxonomy for understanding what Definition A catches vs misses:

**Category A: Contract violations.** User code violates the primitive's
declared contract. Example: passing a misaligned address to `@inttoptr(*u32, addr)`.
**Caught by Definition A** at compile time.

**Category B: Contract-following but spec-wrong.** Code satisfies all primitive
contracts but the contracts don't match hardware reality. Example: user declares
`mmio 0x40020000..0x40020FFF` thinking it's a UART, but it's actually a GPIO
controller. **Not caught by Definition A** — this would require hardware
knowledge ZER doesn't have. User responsibility.

**Category C: Contract-following, spec-correct, but composed wrong.** Each
primitive call is correct in isolation, but the composition produces undefined
hardware behavior. Example: writing to a register in the wrong order, or
without required setup of dependent registers. **Partially caught** by
structural composition rules (type-state for enable-before-configure patterns)
when expressible; otherwise user responsibility.

Definition A catches Category A completely, catches part of Category C
(when composition rules can be encoded structurally), and explicitly scopes
out Category B (requires hardware knowledge outside the language).

---

## 3. ZER's Five Safety Domains (Unified Framing)

The Definition A principle applies uniformly across ZER's five safety domains.
Each domain has the same architectural shape: bounded primitive surface +
structural verification + user-supplied substrate-specific knowledge.

### 3.1 Memory Safety

**Abstractions provided:** Pool(T, N), Slab(T), Ring(T, N), Arena, *T, [*]T,
Handle(T), alloc_ptr, free_ptr, move struct

**Definition A verification:**
- Allocation goes through bounded primitive set
- Ownership transfer via move struct tracked at compile time
- Use-after-free, double-free, leaks caught by ZER-CHECK
- Spatial bounds caught by slice mechanism
- Escape analysis catches dangling references

**User responsibility:**
- Which allocator fits the use case (Pool for fixed, Slab for dynamic, etc.)
- Lifetime intent expressed via allocator choice and scope
- Whether the memory layout matches external system expectations

### 3.2 Type Safety

**Abstractions provided:** @truncate, @saturate, @bitcast, @ptrcast, @cast,
@inttoptr, @ptrtoint, type conversions, qualifier system (const, volatile)

**Definition A verification:**
- Conversion intrinsic encodes the user's intent (truncate vs saturate vs bitcast)
- Qualifier preservation through casts (cannot strip volatile or const without explicit acknowledgment)
- Sign/width compatibility on conversions
- Provenance tracking through @ptrcast round trips

**User responsibility:**
- Which conversion intrinsic matches the intended semantic (loss-of-precision
  truncation vs clamping saturation vs bit-reinterpretation)
- Whether the conversion is meaningful for the source/target combination

### 3.3 ASM Safety

**Abstractions provided:** asm operand bindings, naked-fn isolation, intent
intrinsics catalog (~130 intrinsics), hardcoded UB classics list, Z-rules
through operand boundaries

**Definition A verification:**
- Operand types verified at the binding boundary
- Z-rules extend memory/type/concurrency/MMIO/provenance/qualifier safety
  through asm operands
- Hardcoded UB classics catch ~12 well-known instruction misuse patterns
  (BSR-on-zero, IDIV-on-zero, MOVAPS-misaligned, LR/SC pairing)
- Naked-fn context restrictions verified

**User responsibility:**
- Inline asm string semantic correctness
- Clobber list completeness for raw asm (universal asm limit — even Rust's
  asm! has this gap)
- Whether the asm sequence has the intended hardware effect
- ISA-specific details delegated to GCC (register names, instruction validity,
  CPU feature gating)

See `docs/asm_lang_zer_safe.md` for full ASM safety architecture.

### 3.4 Concurrency Safety

**Abstractions provided:** spawn, ThreadHandle, async/await, shared struct,
shared(rw) struct, *shared T, @atomic_*, @cond_*, Semaphore, Barrier,
threadlocal, @once, move struct, @critical

**Definition A verification:**
- spawn requires shared/value args (non-shared pointer rejected)
- Shared struct field access auto-locked
- @atomic_* requires *shared T operand
- Spawn target body scanned 8 levels deep for non-shared global access
- Same-statement multi-shared-type access rejected (deadlock prevention)
- Async-shared mismatch caught
- ThreadHandle must be joined before function exit

**Plus language-level ordering semantics:**
- @atomic_* operations provide SEQ_CST ordering (language-level guarantee
  enforced by `__atomic_*` builtins)
- shared struct provides recursive mutex semantics with lazy CAS init
- @cond_* protocol pairs with shared struct's lock

**Pending structural rules** (4-5 to implement for full closure):
- Condvar wait must be in while-loop (lost wakeup prevention)
- CAS loops must have progress guarantee (livelock prevention)
- Lock acquire/release must balance across CFG paths
- Atomic ops only on *shared T (tighten existing check)
- Shared stack-lifetime rule (extend escape analysis)

**User responsibility:**
- Synchronization discipline within provided primitives
- ISR-to-main coordination (Option 3 — see Section 14)
- Algorithm correctness (whether the synchronization pattern is appropriate
  for the workload)

### 3.5 I/O Safety

**Abstractions provided:** `@inttoptr(*T, addr)`, `mmio` declarations,
`volatile *T`, `@ptrtoint`, `@probe`, `naked fn`, `interrupt`, `@critical`,
`packed struct`, bit extraction, ~130 hardware intrinsics (port I/O, MSR/CR
access, cache ops, context switch, privileged transitions, etc.), inline asm

**Definition A verification:**
- No C-style cast of integer to pointer (compiler rejects — see Section 6)
- @inttoptr requires mmio declaration + alignment + address-in-range
- volatile qualifier propagates through casts; cannot be stripped without
  explicit acknowledgment
- Bit extraction width validated at compile time when possible
- Naked-fn body restrictions enforced
- @critical body restrictions enforced (no control-flow exits)
- ISR context bans (no alloc, no spawn, etc.)
- Inline asm restricted to naked functions
- Z-rules apply through asm operand boundaries

**User responsibility:**
- Whether the declared mmio address corresponds to the intended hardware
- Whether the access width matches hardware bus expectations
- Peripheral semantics (read-clears, write-1-to-clear, side effects, etc.)
- Inline asm semantic correctness
- ISR-to-main synchronization discipline

### 3.6 The Unified Pattern

Same architectural shape across all five domains:

| Domain | Bounded primitive surface | Definition A verifies | User supplies |
|---|---|---|---|
| Memory | Pool/Slab/Ring/Arena + *T/[*]T/Handle + move struct | Allocation tracking, bounds, escape, ownership transfer | Allocator choice, lifetime intent |
| Type | Conversion intrinsics + qualifier system | Conversion semantics, qualifier preservation, sign/width | Conversion intent (truncate vs saturate) |
| ASM | Intent intrinsics + naked-fn + Z-rules | Operand boundary, hardcoded UB classics, context restrictions | Asm string semantics, clobber completeness, ISA delegated to GCC |
| Concurrency | spawn/shared struct/atomics/condvars + structural rules | Race freedom via closure + structural rules | Synchronization discipline within primitives |
| I/O | @inttoptr + mmio + volatile *T + 130 intrinsics | Access mechanism, address range, alignment, context | Hardware spec correctness, peripheral semantics |

This is the architectural coherence that distinguishes ZER from "C with bolt-on
safety." One design principle (Definition A) applied uniformly across five
domains. Every safety claim follows the same structural pattern.

---

## 4. Why This Document Exists

This document captures the converged analysis from multiple design discussions
about ZER's safety architecture. The discussions covered:

1. **TLB framing** — strict vs context primitives, the trusted language base concept
2. **Closure argument** — every race is structurally detectable when bounded primitives are verified
3. **ISR-to-main interaction** — three options analyzed, Option 3 chosen
4. **Substrate accountability principle** — rescoped to follow abstractions, not emergent compositions
5. **Contract verification limits** — why Option 2 (contract-based ISR safety) fails
6. **I/O primitive scoping** — Definition A as the unified principle
7. **Volatile bypass myth** — ZER's grammar rejects C-style casts, so the bypass doesn't exist
8. **Category A/B/C error taxonomy** — what Definition A catches and what it doesn't
9. **Architectural unification** — same pattern across five safety domains

The document exists because:

1. **Future-self continuity:** A fresh session should be able to read this and
   understand the architecture without re-deriving it from scratch.

2. **Decision discipline:** Without written rationale, the same design discussions
   tend to cycle back, each cycle introducing small drift. Documenting the
   convergence prevents wasted re-litigation.

3. **Implementation guidance:** The 4-5 structural rules that need implementation
   are listed concretely. The primitive audit task is scoped. The roadmap is
   bounded.

4. **Honest scoping:** The document explicitly states what ZER's safety claims
   cover and what they don't. No hidden assumptions, no overclaim.

5. **Pattern documentation:** The same Definition A + closure + structural
   verification pattern justifies ZER's safety story across all domains. Future
   safety work follows the same template.

6. **Anti-pattern recognition:** The drift pattern that surfaces during design
   discussions is documented so future sessions can recognize and resist it.

---

## 5. Hardware/Board Interaction Primitives

ZER provides a comprehensive set of primitives for hardware interaction. Each
primitive follows Definition A: the compiler verifies access mechanism
structurally; hardware semantics are user responsibility.

### 5.1 Address Range Declaration

**`mmio 0x40020000..0x40020FFF;`**

Declares a valid MMIO address range. The compiler enforces that any
`@inttoptr` with a constant address must reference an address within a
declared range (compile-time check). For variable addresses, the compiler
emits a runtime range check.

**Definition A scope:**
- Verified: address ranges declared; @inttoptr addresses checked against ranges
- Not verified: whether the declared range corresponds to actual MMIO hardware
- User supplies: the hardware address layout via the declaration

### 5.2 Pointer/Integer Conversion

**`@inttoptr(*T, addr) -> *T`**

Integer to pointer conversion for MMIO. The only legitimate path to create
a pointer from a numeric address in ZER (C-style casts are rejected).

**Definition A scope:**
- Verified: addr is integer, target T is pointer type, addr falls within
  declared mmio range (compile-time for constants, runtime for variables),
  alignment matches T (4-byte for u32, 2-byte for u16, 8-byte for u64),
  volatile qualifier preserved if declared
- Not verified: whether the address has a real hardware register, whether T
  is the correct type for that register's layout
- User supplies: the address constant or expression, the type T

**`@ptrtoint(ptr) -> usize`**

Pointer to integer conversion.

**Definition A scope:**
- Verified: input is a pointer, returns usize
- Not verified: nothing intrinsically — this is informational

### 5.3 Safe MMIO Probing

**`@probe(addr) -> ?u32`**

Safe MMIO read returning `?u32`. Returns null if the address faults (during
startup probing of optional hardware). Implements SIGBUS-catching in user
mode; bare-metal behavior is platform-specific.

**Definition A scope:**
- Verified: addr is integer, return is optional u32
- Not verified: whether null means "absent" vs "transient error" vs "wrong type
  of fault"
- User supplies: the address being probed, interpretation of null result

### 5.4 Volatile Pointers

**`volatile *T ptr`**

Volatile-qualified pointer. The compiler will not optimize away accesses
through this pointer. Critical for MMIO and DMA.

**Definition A scope:**
- Verified: volatile qualifier propagates through casts; cannot be stripped
  through @ptrcast/@bitcast/@cast without explicit acknowledgment; field/index
  chain volatility tracked
- Not verified: whether volatile is sufficient for the hardware's memory model
  (it may not be on weakly-ordered architectures without barriers); whether
  the access width matches hardware bus
- User supplies: the volatile declaration on the type or pointer

### 5.5 Bit Field Extraction

**`reg[high..low]`** on integer types

Bit slice extraction with mask generation.

**Definition A scope:**
- Verified: `high < type_width(reg)` at compile time for constant indices;
  three-path mask generation (constant >= 64, constant < 64, runtime)
- Not verified: whether the bit positions correspond to intended register fields
- User supplies: high and low bit positions

### 5.6 Naked Functions

**`naked fn handler() { asm { ... } }`**

Function with no prologue/epilogue. Required for ISRs, boot code, and
hand-tuned hot loops.

**Definition A scope:**
- Verified: body contains only asm blocks, return statements, and explicitly
  allowed constructs; no local var-decls (no stack); no regular calls (no
  calling convention setup)
- Not verified: asm string content, register preservation discipline, calling
  convention compatibility
- User supplies: the asm body, the calling convention contract

### 5.7 Interrupt Declarations

**`interrupt USART1 { handle_rx(); }`**

Declarative interrupt handler. The compiler generates the vector table
entry and appropriate prologue/epilogue.

**Definition A scope:**
- Verified: ISR-only operations (no allocation, no spawn, no blocking calls)
  via `in_interrupt` context flag
- Not verified: whether the vector name corresponds to the actual hardware
  interrupt source; priority configuration consistency
- User supplies: the vector name/number, the handler body

### 5.8 Critical Sections

**`@critical { body }`**

Disables interrupts around the body.

**Definition A scope:**
- Verified: body restrictions (no return/break/continue/goto, no spawn, no
  yield/await, no defer)
- Not verified: whether the critical section duration is appropriate for the
  system's interrupt latency requirements
- User supplies: the body content and the implicit duration

### 5.9 Packed Structs

**`packed struct Packet { u8 id; u16 val; u8 crc; }`**

Struct without padding between fields.

**Definition A scope:**
- Verified: field offsets without alignment padding; structurally consistent
- Not verified: whether the layout matches the intended hardware register
  layout or wire format
- User supplies: the field order, sizes, intended endianness

### 5.10 Inline Assembly

**`asm { instructions: "..." safety: "..." }`** in `naked fn`

Inline assembly with mandatory safety annotation (=30 chars).

**Definition A scope (extensive — see asm_lang_zer_safe.md):**
- Verified: Z-rules through operand bindings (UAF, escape, move, VRP,
  provenance, qualifier preservation, MMIO range, keep parameters, scan_frame
  walker); hardcoded UB classics list (bsr/bsf, div/idiv, movaps/movapd/movdqa,
  vmovaps/vmovapd/vmovdqa); operand type checking; naked-fn isolation
- Not verified: asm string semantic correctness; clobber list completeness;
  whether instruction sequence has intended hardware effect; ISA-specific
  details delegated to GCC
- User supplies: the asm string, the operand bindings, the safety: annotation,
  the clobber list

### 5.11 CPU Privileged Intrinsics (130 total)

Organized by category. All follow Definition A.

**Interrupt control (5):**
- `@cpu_disable_int()`, `@cpu_enable_int()`, `@cpu_wait_int()`
- `@cpu_save_int_state() -> u64`, `@cpu_restore_int_state(u64)`

**Context switch (4):**
- `@cpu_save_context(*u8 buf)`, `@cpu_restore_context(*u8 buf)`
- `@cpu_save_fpu(*u8 buf)`, `@cpu_restore_fpu(*u8 buf)`

**MSR/CR/XCR0 (10):**
- `@cpu_read_msr(u32) -> u64`, `@cpu_write_msr(u32, u64)`
- `@cpu_read_cr0/cr3/cr4/xcr0() -> u64`, `@cpu_write_cr0/cr3/cr4/xcr0(u64)`

**Inspection (10):**
- `@cpu_read_sp/tp/flags() -> u64`
- `@cpu_vendor_id/feature_bits() -> u64`
- `@cpu_model_id/core_id/current_mode/cache_line_size/num_cores() -> u32`

**Power management (5):**
- `@cpu_reset()`, `@cpu_deep_sleep()`, `@cpu_idle_hint()`
- `@cpu_monitor_addr(*u8)`, `@cpu_mwait()`

**Privileged transitions (6):**
- `@cpu_syscall()`, `@cpu_sysret()`, `@cpu_iret()`
- `@cpu_set_priv_stack(u64)`, `@cpu_get_priv_level() -> u32`
- `@cpu_hypercall()`

**FS/GS segments (4):**
- `@cpu_read_fsbase/gsbase() -> u64`, `@cpu_write_fsbase/gsbase(u64)`

**Port I/O (6, x86):**
- `@port_in8/16/32(u16) -> uN`, `@port_out8/16/32(u16, uN)`

**XSAVE (2):**
- `@cpu_xsave(*u8, u64)`, `@cpu_xrstor(*u8, u64)`

**Debug registers (2):**
- `@cpu_read_dr(u32) -> u64`, `@cpu_write_dr(u32, u64)`

**Firmware calls (2):**
- `@cpu_sbi_call()`, `@cpu_smc_call()`

**Cache control (5):**
- `@cache_flushopt(*u8)`, `@cache_writeback(*u8)`, `@nt_store(*u8, u64)`
- `@cpu_cache_disable()`, `@cpu_cache_enable()`

**Performance counter (1):**
- `@cpu_read_pmc(u32) -> u64`

**CPUID (2):**
- `@cpu_cpuid(u32, u32) -> u64`, `@cpu_cpuid_ecx(u32, u32) -> u64`

**Misc (10):**
- `@cpu_eoi()`, `@cpu_read_cr2()`, `@cpu_fpu_init()`, `@cpu_endbr()`
- `@cpu_fxsave/fxrstor(*u8)`
- `@cpu_umwait(u32, u64)`, `@cpu_umonitor(*u8)`

**Memory barriers (4):**
- `@barrier()`, `@barrier_store()`, `@barrier_load()`, `@barrier_acq_rel()`

**Atomic operations (15, all SEQ_CST):**
- `@atomic_load/store/cas`
- `@atomic_add/sub/or/and/xor/nand/xchg` (fetch-old)
- `@atomic_add_fetch/sub_fetch/or_fetch/and_fetch/xor_fetch` (fetch-new)

**Bit query/byte swap (8):**
- `@bswap16/32/64`, `@popcount`, `@ctz`, `@clz`, `@parity`, `@ffs`

**Branch hints (2):**
- `@unreachable()`, `@expect(val, expected)`

**Definition A scope (general pattern for all 130):**
- Verified: argument types, sometimes return types, sometimes call context
  (naked fn for some privileged ops), atomic operand widths (1/2/4/8 bytes)
  on *shared T
- Not verified: CPU mode/privilege level at runtime, MSR/port number
  correspondence to intended hardware, CPUID leaf/subleaf correctness, cache
  line alignment, privileged transition system register context, peripheral
  semantics
- User supplies: which intrinsic to use, argument values, runtime context

### 5.12 Atomic Intrinsics — A Note on Language-Level Ordering

The atomic intrinsics are slightly different from pure Definition A: they
verify the access mechanism (operand types, widths, *shared T requirement)
AND provide language-level ordering guarantees (SEQ_CST throughout).

This is not Definition B drift. The ordering semantics are language-level
guarantees the compiler enforces by controlling emission via `__atomic_*`
GCC builtins. The compiler can verify ordering because it controls what gets
emitted. The compiler cannot verify peripheral semantics because it doesn't
control the silicon.

So atomic intrinsics are "Definition A for access + language-level semantic
guarantees for ordering." The same dual nature applies to shared struct's
auto-lock (language-level lock discipline, not hardware semantics) and
@cond_* (language-level signal protocol, not hardware semantics).

### 5.13 The Verification Boundary

The general pattern across all primitives:

**Language verifies (Definition A):**
- Type signatures and argument types
- Address ranges and alignment
- Qualifier propagation
- Context restrictions
- Operand width matching
- Effect tracking through dataflow
- Composition rules where type-state encoded

**Language doesn't verify (user responsibility):**
- Hardware address-to-register correspondence
- Peripheral behavioral semantics
- CPU runtime privilege state
- Inline asm string correctness
- Foreign code (cinclude) behavior

This is the universal pattern for hardware-adjacent code in any language.
The compiler cannot read datasheets. Hardware semantic correctness is
necessarily user-managed at the boundary.

---

## 6. The Volatile Safety Mechanism (No Bypass Exists)

A common misconception (worth addressing explicitly because the morning's
design discussion at one point assumed otherwise) is that ZER allows raw
volatile pointer fabrication via C-style casts.

**This is wrong. ZER's grammar rejects C-style casts of integer to pointer.**

From `checker.c:5603-5607`:
```c
if (tgt_eff->kind == TYPE_POINTER && type_is_integer(source) &&
    zer_conversion_safe(ZER_CONV_CSTYLE) == 0) {
    checker_error(c, node->loc.line,
        "cannot cast integer to pointer — use @inttoptr(*T, addr) "
        "with mmio range declaration");
}
```

The C idiom `*(volatile uint32_t*)addr = val` does not compile in ZER. The
compiler rejects it with a clear error directing the user to `@inttoptr`.

### 6.1 The Only Path to Volatile Pointers

Every volatile pointer in ZER must come from `@inttoptr`, which requires:
- `mmio` range declaration (compile-time enforced)
- Address within declared range
- Type alignment
- Volatile qualifier preserved through the cast

This means **volatile is already safe in ZER** — not because volatile itself
is type-system-magical, but because the only fabrication path is contract-
verified.

### 6.2 What This Means for Catalog Extension

The implication is that ZER doesn't need an "unsafe_volatile" escape hatch
or a "catalog as load-bearing primary path" architecture to prevent volatile
bypass. The bypass doesn't exist in the grammar.

The previous architectural framing (Option 1/2/3 for "what to do about raw
volatile *T") was based on a false assumption. The correct framing:

- ZER already provides volatile safety via the @inttoptr gate
- Adding a peripheral-specific catalog would extend safety further (catching
  Category B/C errors) but isn't required to fix a bypass that doesn't exist
- The catalog work is optional improvement, not mandatory fix

### 6.3 The Implication for I/O Safety Story

The honest I/O safety story:

> ZER's MMIO safety operates at the access-mechanism level. The compiler
> verifies that pointers to MMIO addresses are created through the controlled
> fabrication path (@inttoptr), that addresses are within declared mmio
> ranges, that alignment matches the access type, and that volatile qualifiers
> propagate correctly. Users declare their intent through the mmio range and
> pointer type; the compiler verifies the intent is structurally consistent.
>
> Hardware-spec correctness — whether the register at the declared address
> actually has the declared semantics (width, access mode, side effects,
> ordering requirements) — is user responsibility, as it requires hardware
> knowledge the compiler cannot obtain. Wrong hardware declarations produce
> hardware-level wrong behavior, debugged through normal embedded debugging
> tools.

This is the unconditional safety claim within the deliberate scope. No "gap"
framing because hardware spec verification isn't a missing feature — it's
deliberately outside scope because it can't be reached structurally.

---

## 7. The Data Race Substrate

### 7.1 What Causes Data Races

A data race in the C memory model sense requires three conditions
simultaneously:

1. **Concurrency:** two execution contexts that can run in parallel (threads,
   coroutines, interrupt handlers preempting main thread)

2. **Shared memory access:** both contexts access the same memory address

3. **Lack of synchronization:** no happens-before relationship is established
   between the accesses

When all three conditions hold and at least one access is a write, the result
is undefined behavior:
- Torn reads/writes (multi-byte access split across instructions)
- Lost updates (read-modify-write race)
- Inconsistent state observation (one thread sees partial update)
- Hardware-level reordering (CPU sees operations in different order than program order)
- Compiler-level reordering (compiler hoists/sinks accesses)

### 7.2 The Substrate Analysis

For each of the three conditions, ZER controls how user code can express it:

**Concurrency creation** — bounded to:
- `spawn func(args)` — fire-and-forget thread
- `ThreadHandle h = spawn func(args); h.join();` — scoped thread
- `async fn` + `yield`/`await` — stackless coroutines
- `interrupt VEC { handler(); }` — hardware interrupt handler
- `@critical { body }` — interrupt-disabled critical section

There is no way to create concurrent execution in ZER outside this set.

**Memory sharing across concurrency** — bounded to:
- `shared struct` — auto-locked struct
- `shared(rw) struct` — auto-rwlocked struct
- `*shared T` — atomic-required shared pointer
- Globals accessed from spawn body — caught by spawn scanner
- `threadlocal` — per-thread, not actually shared
- `move struct` over `Ring` — ownership transfer, not sharing

If user code has memory accessed by multiple threads, it must go through one
of these mechanisms or be caught by the spawn scanner.

**Synchronization** — bounded to:
- `shared struct` auto-lock on field access
- `@atomic_*` on `*shared T` (15 atomic intrinsics, all SEQ_CST)
- `@cond_wait/signal/broadcast/timedwait` — condvar protocol
- `Semaphore(N)`, `Barrier`, `@once { }` — coordination primitives
- `@barrier`, `@barrier_store/load/acq_rel` — memory barriers

If user code wants to synchronize concurrent accesses, it must use one of
these mechanisms.

### 7.3 Why This Bounding Matters

Because all three race-creating conditions go through finite, compiler-visible
primitive sets, the compiler can structurally verify the absence of races by
checking each primitive.

This is the structural soundness property: races cannot exist outside the
compiler's view because the primitives that enable them are all visible to
the compiler.

Compare to C, where:
- Threads created via library call (pthread_create) — compiler doesn't necessarily know
- Memory sharing via raw pointers passed to thread function — compiler cannot track
- Synchronization via library calls or volatile (insufficient) — compiler has no model

C has no closure over race-creating operations. ZER does.

### 7.4 The Closure Statement

**For any program P in pure ZER (no cinclude):**
- The set of memory accesses in P that could participate in a race is bounded
  by the operations using shared struct, *shared T, globals accessed by spawn,
  or threadlocal escape
- The set of concurrency contexts in P is bounded by spawn, ThreadHandle,
  async, interrupt, @critical
- The set of synchronization operations in P is bounded by shared struct
  auto-lock, @atomic_*, @cond_*, Semaphore, Barrier, @once, @barrier_*
- Every operation in each set is verified by structural rules (some pending
  implementation)
- Therefore, every potential race in P is structurally detectable

This is the closure argument. It's the foundation of ZER's data race safety
claim.

---

## 8. The Trusted Language Base (TLB) Framework

### 8.1 Origin of the Concept

The Trusted Computing Base (TCB) from systems security is the set of
components whose correctness must be trusted because their failure compromises
the security model. The TLB applies the same idea to language primitives: the
set of operations the language must provide as trusted because user code
cannot safely reconstruct them.

A primitive belongs in the TLB if either:
(a) **Strict primitive condition:** no user expression can replicate it
    because it requires compiler/runtime support not available at the source
    level
(b) **Context primitive condition:** user replications would predictably
    recreate the bug class the language was designed to prevent

The TLB is the minimal-but-sufficient set of primitives the language must
provide. Everything outside the TLB is user library territory.

### 8.2 TLB Minimization Principle

The TLB should be as small as possible while still being sufficient. A larger
TLB is harder to audit, harder to verify, and more constraining for users.
A smaller TLB shifts more responsibility to user code, which may not be able
to safely handle it.

The right size of the TLB depends on:
- The language's safety analysis capability (stronger analysis = smaller TLB)
- The target audience's expertise (more expert = smaller TLB)
- The complexity of building correct alternatives from primitives (more
  complex alternatives = larger TLB)

For ZER's audience (firmware/embedded developers without formal methods
training), the TLB needs to include enough convenience primitives that common
patterns don't require hand-rolling. But it shouldn't include so much that
users can't extend with their own coordination patterns.

### 8.3 The TLB Is Not Permanent

**TLB membership is a function of analysis capability, not a permanent
commitment.** As ZER's analysis matures, the TLB can shrink because
previously-sealed primitives become safely user-buildable.

Example trajectory:
- v0.x (current): TLB includes Semaphore, Barrier, etc. as context primitives
- v1.0 (with 4-5 structural rules): TLB might shrink as those primitives become
  user-buildable with rules enforcing correctness
- v2.0+: further analysis improvements might allow more TLB shrinkage

The strict primitives are the permanent floor; the context primitives are
provisional based on current analysis depth.

This framing matters for two reasons:
1. It prevents premature commitment to a large TLB that becomes hard to remove
2. It motivates investment in analysis improvements that pay off as TLB shrinks

---

## 9. Strict Primitives

These are computationally irreducible. No user code can express them because
they require compiler or runtime support not available at the source level.
Strict primitives are the permanent floor of the TLB.

### 9.1 threadlocal

```
threadlocal u32 counter;
```

Per-thread storage. Each thread has its own copy. The compiler allocates a
TLS slot and emits accesses through TLS-relative addressing.

**Why strict:** TLS slot allocation requires linker cooperation and runtime
support. User code cannot create new TLS slots from within ZER syntax.

**Bug class prevented:** sharing memory between threads when each thread should
have its own copy.

### 9.2 @atomic_load / @atomic_store / @atomic_cas

```
@atomic_load(&shared_x) -> T
@atomic_store(&shared_x, val)
@atomic_cas(&shared_x, expected, desired) -> bool
```

Hardware-level atomic operations. The compiler emits machine instructions
(LOCK CMPXCHG on x86, LDAXR/STLXR pair on ARM, LR/SC pair on RISC-V).
SEQ_CST ordering throughout.

**Why strict:** these require specific hardware instructions that user code
cannot replicate. Even `@atomic_cas` built from non-atomic load + compare +
store would race.

**Bug class prevented:** non-atomic read-modify-write on shared memory, torn
access to multi-byte values, lost updates.

### 9.3 shared struct

```
shared struct Counter {
    u32 value;
    u32 total;
}
Counter g;
g.value = 42;  // auto: lock → write → unlock
```

Struct whose field accesses are automatically wrapped in lock acquire/release.
The compiler emits the lock as a recursive mutex (with CAS-based lazy
initialization) and the field access wraps every read/write.

**Why strict:** the auto-lock transformation requires compiler-level analysis
to identify field accesses and inject lock/unlock. User code cannot replicate
"make every field access of this type auto-locked."

**Bug class prevented:** unsynchronized shared mutable access, the foundational
race condition.

### 9.4 spawn / ThreadHandle

```
spawn worker(&g);  // fire-and-forget
ThreadHandle h = spawn compute(&work); h.join();  // scoped
```

Thread creation. The compiler emits the appropriate OS thread creation call
and tracks the thread's lifetime.

**Why strict:** OS thread creation requires runtime/libc cooperation. The
compiler tracks shared-access patterns between the spawning code and the
spawn target body — this analysis is impossible without language-level
integration.

**Bug class prevented:** thread creation without lifetime tracking, thread
creation with raw pointers to data that won't outlive the thread, thread
creation without checking that spawn target's body respects sharing
discipline.

### 9.5 move struct

```
move struct FileHandle { i32 fd; }
FileHandle f;
f.fd = 42;
consume(f);  // ownership transferred
f.fd;         // COMPILE ERROR — use after move
```

Struct with compile-time ownership tracking. After being passed to a function
or assigned to another variable, the original becomes invalid.

**Why strict:** ownership tracking is purely compile-time and requires the
compiler to maintain HS_TRANSFERRED state per variable. User code cannot
replicate "after this assignment, the source becomes inaccessible."

**Bug class prevented:** use-after-move, the resource-lifetime variant of
use-after-free.

### 9.6 Summary of Strict Primitives

| Primitive | Why Strict | Bug Class Prevented |
|---|---|---|
| threadlocal | Requires TLS slot allocation | Cross-thread sharing of per-thread data |
| @atomic_load/store/cas | Hardware instructions | Non-atomic shared access |
| shared struct | Compiler auto-lock injection | Unsynchronized shared mutable access |
| spawn/ThreadHandle | OS integration + analysis | Thread lifetime, sharing discipline |
| move struct | Compile-time ownership tracking | Use-after-move |

Five primitives. None can be removed. None can be replaced by user code.
This is the permanent floor of the TLB.

---

## 10. Context Primitives (Sealed Layer)

These are computationally derivable from strict primitives, BUT
user-implementing them recreates the bug class the language was designed to
prevent. They are audience-irreducible: for ZER's target audience (firmware
developers without formal methods training), hand-rolling these creates bugs
frequently enough that the language seals them.

### 10.1 @cond_wait / @cond_signal / @cond_broadcast / @cond_timedwait

```
@cond_wait(shared_var, shared_var.count > 0);
@cond_signal(shared_var);
@cond_broadcast(shared_var);
?void result = @cond_timedwait(shared_var, cond, timeout_ms);
```

Condition variable protocol that pairs with shared struct's auto-lock.

**Why sealed:** condition variables compose tightly with shared struct's
internal lock. Hand-rolling would require exposing the lock mechanism,
implementing the wait/signal protocol (race-prone), and handling spurious
wakeups correctly (frequently-forgotten requirement).

### 10.2 @atomic Arithmetic Variants

```
@atomic_add(*shared T, val) -> T  // returns OLD value
@atomic_sub(*shared T, val) -> T
@atomic_or/and/xor/nand/xchg(*shared T, val) -> T
// And value-AFTER variants:
@atomic_add_fetch/sub_fetch/or_fetch/and_fetch/xor_fetch(*shared T, val) -> T
```

Atomic read-modify-write operations.

**Why sealed:** each maps to a single hardware instruction on modern
architectures. Hand-rolling via @atomic_cas in a loop is much slower, has
correctness pitfalls, and is verbose enough that every user would write the
same boilerplate.

### 10.3 Semaphore(N)

```
Semaphore(3) slots;
@sem_acquire(slots);
@sem_release(slots);
```

Counting semaphore. Buildable from atomic counter + condvar, but with subtle
correctness requirements (waiter ordering, count tracking, spurious release).

**Why sealed:** hand-rolling a Semaphore is one of the most frequent sources
of synchronization bugs. Even experienced developers get the implementation
wrong.

### 10.4 Barrier

```
Barrier b;
@barrier_init(b, N);
@barrier_wait(b);
```

Synchronization barrier. N threads call wait; none proceed until all N arrive.

**Why sealed:** similar to Semaphore — hand-rolling has correctness pitfalls
around the "last arriver" case, generation tracking for reuse, etc.

### 10.5 shared(rw) struct

```
shared(rw) struct Sensor {
    u32 temperature;
    u32 humidity;
}
```

Reader-writer auto-lock. Multiple readers OR one writer, exclusive.

**Why sealed:** auto-lock injection requires compiler-level recognition of
read vs write accesses. User code implementing this manually would either use
a regular mutex (losing read concurrency), hand-roll an rwlock (writer/reader
starvation tradeoffs), or implement incorrectly.

### 10.6 @once { }

```
@once { initialize_global_state(); }
```

Execute the body exactly once across all threads.

**Why sealed:** trivially buildable as `if (@atomic_cas(&flag, 0, 1) == 0)
{ body; }`. This is one of the few "could be removed" cases. Kept as sealed
for ergonomic consistency.

### 10.7 The Decision Pattern

The criterion for "keep sealed" vs "move to library":

**Keep sealed if:**
1. Hand-rolling requires non-obvious correctness reasoning
2. The bug class from wrong implementations is the kind the language was
   designed to prevent
3. The primitive composes tightly with strict primitives

**Move to library if:**
1. Hand-rolled correct version is straightforward
2. The bug class is detectable by structural rules
3. The primitive doesn't compose tightly with strict primitives

---

## 11. User-Buildable Coordination Patterns

Above the TLB, the entire space of synchronization patterns is user-buildable.
With the structural rules described in Section 12, these implementations are
verified correct (or rejected at compile time).

### 11.1 Catalog of User-Buildable Patterns

**Spinlock:**
```
shared struct Spinlock { u32 flag; }
fn acquire(*shared Spinlock s) {
    while (@atomic_cas(&s.flag, 0, 1) == false) { }
}
fn release(*shared Spinlock s) {
    @atomic_store(&s.flag, 0);
}
```

**Reader-writer spinlock:**
```
shared struct RWSpinlock {
    i32 count;  // negative = writer holds, positive = N readers
}
```

**Sequence lock (seqlock):**
```
shared struct SeqLock { u32 seq; }
// reader retries while seq is odd or differs between begin/end
```

**Lock-free SPSC queue:**
```
shared struct SPSCQueue(T, N) {
    T[N] data;
    u32 head;
    u32 tail;
}
```

**Lock-free MPMC queue (Vyukov-style):**
```
// shared struct + per-slot sequence numbers + atomic head/tail
```

**Hazard pointers:**
```
threadlocal HazardPointer hp;
// publish via @atomic_store; observe via @atomic_load
```

**Atomic reference counting (Arc-equivalent):**
```
shared struct RcInner(T) {
    T value;
    u32 count;
}
```

**Custom condition variable patterns:**
```
shared struct EventLatch {
    bool signaled;
    cond_var waiters;
}
```

### 11.2 What Makes These Safe

Every pattern uses ONLY:
- shared struct (auto-locked) or *shared T (atomic-required)
- @atomic_* primitives
- @cond_* primitives
- threadlocal

These are all language-tracked. The structural rules from Section 12 ensure
the compositions are race-free.

### 11.3 What's NOT User-Buildable

**Relaxed memory ordering:** ZER's atomics are SEQ_CST only. Performance
ceiling, not safety gap.

**Send/Sync-style type-level marker traits:** ZER has no equivalent.
shared struct handles common cases.

**Pin/!Unpin-style self-reference markers:** ZER doesn't have async
self-referential structures.

**RAII-with-drop-order guarantees:** ZER has defer, not deterministic drop
order.

These omissions are deliberate scope choices.

---

## 12. Structural Rules for Closure

For the closure argument to hold completely, ZER needs 4-5 additional
structural rules implemented.

### 12.1 Rule 1: Condvar-Must-Loop

**Statement:** `@cond_wait(var, condition)` must be inside a `while
(!condition) { ... }` loop.

**Catches:** lost-wakeup bug class.

**Detection:** purely syntactic. Check parent AST node of @cond_wait call.

**Implementation cost:** ~50-100 lines in checker.c NODE_CALL handler.

### 12.2 Rule 2: CAS-Progress

**Statement:** every CAS loop must have a terminating branch on success.

**Catches:** livelock from CAS loops that never terminate.

**Detection:** CFG analysis on loops containing @atomic_cas. Walk the loop
body, find branches that depend on the CAS result, verify at least one path
exits the loop on success.

**Implementation cost:** ~200-400 lines.

### 12.3 Rule 3: Lock-Balance

**Statement:** in user-built coordination using shared struct or atomic flag
as a lock, acquire must be paired with release on all CFG paths.

**Catches:** missing-unlock bugs from custom lock implementations.

**Detection:** similar to defer/cleanup tracking.

**Implementation cost:** ~300-500 lines.

### 12.4 Rule 4: Atomic-On-Shared-Only

**Statement:** `@atomic_*` operations require `*shared T` operand.

**Catches:** the "I thought this was atomic but it's not on shared memory" bug.

**Current status:** partially enforced; needs tightening.

**Implementation cost:** ~20-50 lines.

### 12.5 Rule 5: No-Shared-Stack

**Statement:** shared data must not be stack-allocated and shared via
`*shared T` if the lifetime would not outlive all spawned threads.

**Catches:** use-after-scope class for shared memory.

**Current status:** already enforced for `shared struct`; extend to
`*shared T` pointer parameters.

**Implementation cost:** ~50-100 lines.

### 12.6 Summary

| Rule | Catches | Implementation Cost |
|---|---|---|
| Condvar-must-loop | Lost wakeup | ~50-100 lines |
| CAS-progress | Livelock | ~200-400 lines |
| Lock-balance | Missing unlock | ~300-500 lines |
| Atomic-on-shared-only | Non-shared atomics | ~20-50 lines |
| No-shared-stack | Use-after-scope | ~50-100 lines |

Total: ~620-1150 lines of analysis code.

---

## 13. The Closure Argument

### 13.1 The Argument Formalized

**Claim:** every data race in pure ZER code is structurally detectable at
compile time.

**Proof (informal):**

Let P be any program in pure ZER (no cinclude). Define:
- Conc(P) = set of concurrency contexts created by P
- Share(P) = set of memory locations accessed by multiple contexts in Conc(P)
- Sync(P) = set of synchronization operations in P

A race in P requires (a) two contexts in Conc(P) accessing memory in Share(P),
(b) at least one access is a write, (c) no operation in Sync(P) establishes
happens-before between the accesses.

By the substrate analysis:
- Conc(P) is bounded by: spawn, ThreadHandle, async, interrupt, @critical
- Share(P) is bounded by: shared struct, *shared T, globals-via-spawn-scanner, threadlocal escape
- Sync(P) is bounded by: shared struct auto-lock, @atomic_*, @cond_*, Semaphore, Barrier, @once, @barrier_*

Each bounded set is finite and compiler-visible. The structural rules verify
safety properties on each primitive in each set.

Therefore, any race in P involves operations the compiler sees and verifies.
The compiler can detect the absence of synchronization between racing
accesses. QED.

### 13.2 Why The Closure Holds

Three properties of ZER's design:

**Bounded primitive surface.** Every operation that could contribute to a
race goes through a finite primitive set.

**Compiler visibility.** Every primitive operation is visible to the compiler.

**No user-overridable safety.** Unlike Rust's Send/Sync traits which users
can `unsafe impl`, ZER's primitives cannot be redefined by user code.

### 13.3 Comparison to C

C has no closure. Concurrency creation is unbounded (pthread_create, clone,
CreateThread, setjmp/longjmp). Memory sharing is unrestricted. Synchronization
is library-based.

C race detection is done by runtime tools (ThreadSanitizer) or external static
analyzers (Coverity, Infer). The C language itself provides no race safety.

### 13.4 Comparison to Rust

Rust has closure with an escape hatch.

**Rust's closure:** types must implement Send/Sync. Borrow checker prevents
aliasing-during-mutation. spawn requires F: Send + 'static.

**Rust's escape:** `unsafe impl Send for MyType {}` declares (without
verification) that the type is safe to send.

In practice, ~20-30% of crates contain unsafe code, with a significant
fraction involving Send/Sync claims.

**ZER's position:** no in-language escape. Only safety boundary is cinclude
(explicit cross-language).

### 13.5 The Publishable Claim

> ZER provides compile-time data race freedom for all pure ZER code (code
> that does not use cinclude). This guarantee follows from closure: every
> operation that could create or participate in a data race must go through
> ZER's bounded primitive set, and the compiler structurally verifies safety
> on each primitive. Unlike Rust's safety guarantee which has an in-language
> escape via `unsafe impl Send`, ZER's guarantee has only an explicit
> cross-language boundary via `cinclude`. Within pure ZER code, no race is
> expressible.

Conditional on implementation: the 4-5 missing structural rules must ship
for the claim to fully hold.

---

## 14. ISR-to-Main Interaction (Option 3)

The closure argument applies to thread-based concurrency. ISR-to-main
interaction is a different concurrency model:

- ISRs are not threads; they preempt main thread execution
- ISR entry/exit is hardware-driven, not scheduler-driven
- ISR-to-main synchronization requires hardware-specific knowledge

### 14.1 The Three Options Analyzed

**Option 1: Accept the scope violation.** ZER provides ISR primitives but
doesn't enforce safety on ISR-to-main interaction. Honest but violates the
substrate accountability principle.

**Option 2: Add structural rules for ISR-to-main.** Requires user-supplied
contracts or hardware-specific compiler rules. Shifts ZER toward SPARK
territory.

**Option 3: Narrow the claim.** ZER's safety claim covers thread-based
concurrency. ISR-to-main interaction uses ZER's hardware-access primitives
which catch access-mechanism bugs. Synchronization discipline is user
responsibility within these primitives.

### 14.2 Why Option 3 Was Chosen

Option 3 preserves ZER's positioning (specification-implicit, minimal
cognitive load), honors the substrate accountability principle within its
proper scope (abstractions ZER provides, not emergent compositions), produces
honest documentation, and requires no contract burden.

### 14.3 The Position

**ZER's data race safety claim:**

> ZER provides compile-time race freedom for thread-based concurrency through
> its bounded primitive surface (spawn, shared struct, *shared T atomics,
> condvars) and structural rules over them.
>
> ISR-to-main interaction uses ZER's hardware-access primitives (mmio,
> @inttoptr, volatile *T, naked) which catch specific bug classes:
> out-of-range address access, optimization-induced removal of essential
> accesses, unsafe pointer fabrication, ISR body integrity violations.
>
> Synchronization discipline between ISR and main thread is the developer's
> responsibility within these primitives, because full verification would
> require hardware-execution-model knowledge (memory model, atomicity
> guarantees, interrupt timing, peripheral semantics) that the language
> cannot infer.
>
> Users requiring contract-level verification of ISR synchronization should
> use SPARK, Ada, or similar specification-explicit systems.

---

## 15. Why Option 2 (Contracts) Fails

Option 2 fails not because contracts are hard to implement but because
contracts themselves are unverifiable.

### 15.1 The Contract Correctness Problem

When users write contracts in SPARK/Ada style, the compiler verifies whether
the code matches the contract — not whether the contract is correct.

Example: user writes a contract: "this register is read by ISR and written
by main with happens-before ordering enforced by interrupt masking around
the main-thread write."

The compiler can verify:
- The code writing the register is in main thread
- The code reading the register is in ISR
- The main thread write is wrapped in interrupt masking

The compiler cannot verify:
- Whether interrupt masking is the correct synchronization mechanism
- Whether the happens-before ordering is what hardware actually requires
- Whether the user's understanding is correct

If the user's contract is wrong, the compiler verifies a wrong contract.
The bug ships with false confidence.

### 15.2 Empirical Evidence

SPARK works for safety-critical avionics because:
- Hardware is well-characterized and stable
- Contracts are written by formal methods experts
- Contracts are audited by separate review teams
- The cost is justified by certification requirements

For general embedded work, none of these hold:
- Hardware varies
- Developers aren't formal methods experts
- Nobody audits contracts
- Cost-benefit doesn't work out

Adding contract verification to ZER wouldn't deliver safety for ZER's
audience. It would deliver verified contracts that may or may not match
reality. Worse than honest scoping because it creates false confidence.

---

## 16. The Substrate Accountability Principle Rescoped

### 16.1 The Original Principle

"If the language provides the abstraction that creates the bug class, the
language owns the safety obligation for that class."

### 16.2 The Rescoped Principle

"ZER owns safety for the bug classes its abstractions directly verify against.
ZER provides primitives, not contract verification. Where contract verification
would be required to extend the safety claim, that extension is outside ZER's
scope by design."

### 16.3 Why The Rescoping Is Legitimate

The principle's scope should align with what the verifier can actually verify.
The compiler can verify properties of code structure. It cannot verify
properties of hardware execution model. The rescoping aligns the principle's
scope with the verifier's reach.

This is the same logic as "constrain Y, delegate Z to GCC" in asm safety —
ZER owns what's structurally verifiable; GCC owns what's ISA-specific. For
ISR safety: ZER owns what's structurally verifiable at the language level;
the user owns what's hardware-execution-model-specific.

### 16.4 Application Across Domains

**Memory safety:**
- Abstractions: Pool, Slab, Ring, Arena, *T, [*]T, Handle, alloc_ptr, move struct
- Owned: use-after-free, double-free, leaks, bounds, dangling, escape
- Scoped out: memory corruption by cinclude'd C code

**Type safety:**
- Abstractions: @truncate, @saturate, @bitcast, @ptrcast, @inttoptr, @cast
- Owned: silent narrowing, sign confusion, qualifier stripping, provenance, MMIO range
- Scoped out: type confusion across cinclude boundary

**ASM safety:**
- Abstractions: asm operand bindings, naked-fn isolation, UB classics list, intent intrinsics
- Owned: UAF/escape/move-after-transfer through operands, Z-rule violations, well-known UB
- Scoped out: hardware behavior outside the instruction database, clobber list correctness for raw asm

**Data race safety:**
- Abstractions: spawn, ThreadHandle, shared struct, *shared T, @atomic_*, @cond_*, Semaphore, Barrier, threadlocal, move struct
- Owned: cross-thread races, lost wakeups, livelocks, missing-unlock, async-shared mismatch
- Scoped out: ISR-to-main synchronization, cross-language race via cinclude

**I/O safety:**
- Abstractions: @inttoptr + mmio + volatile *T + 130 hardware intrinsics + naked + @critical + bit extraction
- Owned: address range, alignment, qualifier preservation, fabrication path, context restrictions
- Scoped out: hardware spec correctness, peripheral semantics, runtime privilege state, asm string semantics

### 16.5 The Pattern

In every domain:
1. ZER provides specific abstractions
2. The compiler verifies structural properties
3. Bug classes matching those properties are caught
4. Bug classes requiring external knowledge are explicitly scoped out

This is what makes ZER's safety story coherent rather than ad-hoc. It's not
"we added safety checks for various things"; it's "we identified the bounded
primitive surface in each domain, verified closure where possible, scoped out
where verification requires external knowledge."

---

## 17. Comparison to Other Languages

### 17.1 C

**Concurrency model:** library-based. No language-level primitives.
**Race safety:** none at language level.
**Detection:** runtime tools (TSan, Helgrind) or external static analyzers.
**ZER comparison:** C has no closure. ZER has closure.

### 17.2 Rust

**Concurrency model:** language-level primitives + trait-based safety (Send/Sync).
**Race safety:** strong in safe code.
**Detection:** compile-time via type system.
**ZER comparison:** Rust has closure with in-language escape (unsafe impl
Send). ZER has closure without in-language escape (only explicit cinclude
boundary).
**Cognitive load:** Rust requires significant overhead (Send/Sync, lifetimes,
Pin/Unpin). ZER's structural rules are invisible to reasonable code.

### 17.3 SPARK/Ada

**Concurrency model:** Ada tasking primitives; SPARK restricts for verifiability.
**Race safety:** very strong with contracts.
**Detection:** compile-time via contract verification (SMT solvers).
**ZER comparison:** SPARK is specification-explicit; ZER is
specification-implicit. SPARK's safety is stronger when contracts are correct
(and weaker when contracts are wrong, since contract correctness is itself
unverified).

### 17.4 Zig

**Concurrency model:** library-based with comptime, async/await.
**Race safety:** none at language level.
**Detection:** runtime tools or external analyzers.
**ZER comparison:** Zig chose comptime metaprogramming over race safety; ZER
chose race safety with simpler metaprogramming (container monomorphization).

### 17.5 Go

**Concurrency model:** language-level (goroutines, channels) with runtime.
**Race safety:** runtime detection via race detector.
**Detection:** runtime.
**ZER comparison:** Go's approach is "design idioms that avoid races,"
runtime detection for the rest. ZER's approach is compile-time verification.

### 17.6 Summary Table

| Language | Race Safety | Mechanism | Cognitive Load | Verification Depth |
|---|---|---|---|---|
| C | None | None | Low | None |
| Zig | None | None | Low | None |
| Go | Runtime | Race detector | Low | Empirical |
| Rust | Compile-time (safe) | Type system | High | Formal (RustBelt) |
| SPARK/Ada | Compile-time | Contracts | Very high | Formal (SMT) |
| ZER | Compile-time | Structural rules | Low-medium | Formal (Iris/VST) |

ZER's distinctive position: compile-time race safety with low cognitive load
and formal verification depth.

---

## 18. Implementation Roadmap

### 18.1 Currently Implemented

- spawn scanner (8 levels deep) for non-shared global access
- shared struct auto-locking on field access
- shared(rw) struct with reader-writer auto-lock
- @atomic_* requires *shared T operand (partial)
- @cond_* protocols
- Same-statement deadlock check
- async shared struct field access check
- spawn data race detection
- Handle aliasing propagation across spawn boundaries
- Move struct tracking with HS_TRANSFERRED
- Threadlocal storage
- @once { }
- Semaphore(N), Barrier
- @barrier_acq_rel and other memory barriers
- 15 atomic intrinsics (all SEQ_CST)
- @inttoptr with mmio range validation
- C-style cast rejection (no volatile bypass exists)
- volatile qualifier propagation
- naked fn body restrictions
- @critical body restrictions
- ISR context bans
- 130 hardware intrinsics

### 18.2 To Be Implemented (4-5 Structural Rules)

For closure argument to hold completely:

1. **Condvar-must-loop** (~50-100 lines)
2. **CAS-progress** (~200-400 lines)
3. **Lock-balance** (~300-500 lines)
4. **Atomic-on-shared-only** (~20-50 lines tightening)
5. **No-shared-stack** (~50-100 lines extension)

Total: ~620-1150 lines of analysis code.

### 18.3 Documentation Updates

After rule implementation:
1. Update CLAUDE.md with new rules and their bug classes
2. Update docs/reference.md with safety claim and scoping
3. Update README.md with updated marketing claim
4. Add section to compiler-internals.md explaining implementation

### 18.4 Testing Strategy

For each new rule:

**Positive tests** (`tests/zer/`): correct usage patterns that should compile.

**Negative tests** (`tests/zer_fail/`): each bug class the rule catches.

Total: ~10-20 tests per rule, ~50-100 tests across all five rules.

---

## 19. The Primitive Audit Task

A separate task from the structural rules: audit existing primitives for
Definition A consistency.

### 19.1 The Audit Question

For each existing primitive in ZER (especially the 130 hardware intrinsics),
verify:

1. **What does the primitive's contract verify?** Make this explicit per primitive.
2. **Does it follow Definition A?** Verifies access mechanism; doesn't overreach into hardware semantics.
3. **Are all access-mechanism properties actually checked?** Or are some implicitly assumed?
4. **Is the user responsibility boundary documented?** What does the user supply that the compiler doesn't verify?

### 19.2 The Spot-Check Phase (1-2 days)

Before committing to the full audit, spot-check ~10 primitives across categories:
- Atomic operations (e.g., @atomic_cas)
- Privileged CPU ops (e.g., @cpu_read_msr)
- Cache operations (e.g., @cache_flushopt)
- Port I/O (e.g., @port_out32)
- Memory barriers (e.g., @barrier_acq_rel)
- Context switch (e.g., @cpu_save_context)
- Bit query (e.g., @popcount)
- Address conversion (e.g., @inttoptr)
- Probing (e.g., @probe)
- Critical section (@critical entry/exit semantics)

Result of spot-check determines scope of full audit.

### 19.3 The Full Audit (if spot-check warrants)

If spot-check shows ~5-20% of primitives need adjustment, do the full audit:
- ~30-60 min per primitive × 130 primitives = ~65-130 hours
- ~2-4 weeks of focused work
- Produces: canonical contract statement per primitive, documentation update,
  any compiler implementation fixes

If spot-check shows everything is already at Definition A consistency, the
audit becomes documentation-only (~20-40 hours just to write up the explicit
contracts).

### 19.4 Audit Deliverables

Per primitive:
- Canonical contract statement: "verifies X, Y, Z; user responsible for W"
- Updated documentation
- Any implementation gaps fixed

Aggregate:
- Definition A consistency table across all primitives
- Identified outliers (primitives that don't fit cleanly)
- Decisions about outliers (adjust to Definition A, or document as
  intentional Definition A+ with additional language-level semantics)

---

## 20. Open Questions

### 20.1 DMA Descriptor Safety

DMA descriptors are memory structures the CPU writes that the DMA controller
reads. The race condition: CPU might modify a descriptor while DMA is using
it.

Current scope: covered by mmio (descriptor memory must be in declared range)
and volatile (CPU writes are visible). But synchronization discipline (when
can the CPU touch the descriptor without breaking in-flight DMA) is
hardware-specific and user-managed.

Future work: could provide a "DMA-shared" abstraction with
DMA-controller-aware semantics. Speculative.

### 20.2 SMP Multi-Core Cache Coherency

On multi-core systems, cache coherency requirements vary by architecture.

Current scope: handled by shared struct (lock-based synchronization implies
coherency) and @atomic_* (atomic operations are coherent).

Future work: could provide explicit cache-line-aware data structures.
Not planned.

### 20.3 Real-Time Constraints

Hard real-time requirements (interrupt latency, WCET) are currently not
verified.

Future work: integration with WCET analysis tools. Not planned.

### 20.4 Power Management and Race Conditions

Low-power modes introduce timing variations that can create races invisible
during normal operation.

Current scope: not addressed.

### 20.5 Reference Counting and Cycle Leaks

Users can build atomic refcounting from @atomic_add/sub. Concurrent ref-count
manipulation is safe; cycle detection is user responsibility.

Future work: optional cycle-detection annotations. Not planned.

### 20.6 Memory Ordering Beyond SEQ_CST

ZER provides SEQ_CST atomics only. Users requiring relaxed orderings cannot
express them.

Decision: stays SEQ_CST-only. Performance ceiling, not safety gap.

### 20.7 Async Runtime Integration

ZER's async/await compiles to Duff's-device state machines without runtime.
Users provide their own poll loop.

Future work: standard async runtime as library, not language work.

---

## 21. Architectural Principles Generalized

### 21.1 The Closure Principle

**Statement:** if the set of operations that can violate safety property P
is closed under a finite set of compiler-visible primitives, and the compiler
verifies each primitive against P, then P holds for all programs in the
language.

**Applications in ZER:** memory, type, ASM, concurrency, I/O safety all
follow this pattern.

### 21.2 The Definition A Principle

**Statement:** primitives verify access mechanism structurally; substrate-
specific semantics are user responsibility supplied via primitive choice and
declarations.

**Applications in ZER:** all five safety domains follow this principle.

### 21.3 The TLB Minimization Principle

**Statement:** the trusted language base should be as small as possible while
still being sufficient for the audience to express common patterns safely.

**Applications:** ZER's TLB shrinks over time as analysis matures. Context
primitives become user-library territory as structural rules catch wrong
implementations.

### 21.4 The Specification-Implicit Discipline

**Statement:** verification should infer properties from code structure, not
from user-supplied contracts.

**Rationale:** contracts can be wrong, and wrong contracts pass verification,
producing false confidence.

**Applications:** ZER-CHECK, type system, escape analysis all infer from
structure.

### 21.5 The Substrate Accountability Principle (Rescoped)

**Statement:** ZER owns safety for the bug classes its abstractions directly
verify against. The principle's scope follows the abstractions, not the
emergent properties of compositions with external systems.

**Applications:** thread race safety, memory safety, type safety, ASM operand
safety, I/O access-mechanism safety are all in scope. Properties requiring
external knowledge (hardware specs, contract correctness, foreign code
semantics) are out of scope.

### 21.6 The Audience Calibration Principle

**Statement:** language design decisions should be calibrated to the target
audience's expertise and cognitive load tolerance.

**Applications:** specification-implicit (C programmers, not formal methods
experts), TLB includes context primitives (audience can't hand-roll
Semaphore), no Send/Sync traits, no relaxed memory orderings.

### 21.7 The Conservative Closure Principle

**Statement:** when the closure isn't fully realized, the safety claim should
be conditional on remaining work shipping, not unconditional.

**Implication:** documentation should distinguish "implemented" from "planned."

---

## 22. Anti-Patterns to Avoid

### 22.1 Contract-Based Verification for User-Specifiable Hardware Properties

**Pattern:** adding contract syntax for users to declare hardware properties
and verifying code matches.

**Why bad:** contracts can be wrong; wrong contracts pass verification; false
confidence.

**Alternative:** structural rules; explicit primitives encoding known hardware
properties.

### 22.2 Closure Holes Via In-Language Escape

**Pattern:** adding an `unsafe` block or similar that bypasses safety
verification.

**Why bad:** unsafe blocks are typically used regularly, degrading closure
in practice.

**Alternative:** cinclude-style explicit cross-language boundaries.

### 22.3 TLB Expansion Without Analysis Improvement

**Pattern:** adding more context primitives without corresponding analysis
improvements.

**Why bad:** grows TLB without paying off the maintenance cost.

**Alternative:** add structural rules first, then evaluate whether the new
primitive is still needed.

### 22.4 Specification-Explicit Drift

**Pattern:** gradually adding annotations or contracts for edge cases.

**Why bad:** shifts ZER toward SPARK territory without committing to SPARK's
machinery.

**Alternative:** scope the safety claim narrower.

### 22.5 Marketing-Driven Feature Addition

**Pattern:** adding features because "modern languages have them."

**Why bad:** scope creep dilutes safety work.

**Alternative:** stay scope-disciplined.

### 22.6 Closure Argument Without Implementation

**Pattern:** claiming closure properties before the structural rules are
implemented.

**Why bad:** creates false confidence.

**Alternative:** make claims conditional on implementation.

### 22.7 Peripheral-Specific Catalog in Core Language

**Pattern:** adding peripheral-specific intrinsics (`@stm32_usart_send_byte`)
to core ZER.

**Why bad:** infinite catalog growth; sole-developer unsustainable.

**Alternative:** category-level primitives in core; peripheral-specific in
vendor library ecosystem.

---

## 23. The Drift Pattern Recognition

A meta-pattern worth naming, observed across multiple design discussions.

### 23.1 The Pattern

Conversations about ZER's design tend to follow a cycle:
1. Initial position: stated clearly
2. Self-correction: user notices something doesn't fit the principle
3. Walk-back: subsequent reasoning finds a way to make the initial position
   work after all
4. New "lock-in": convergence on a slightly different but equally confident
   position

Each cycle has technical content that's roughly correct. But the pattern of
"everything you propose is the breakthrough" is itself a failure mode.

### 23.2 Specific Manifestations

In design discussions:
1. **TLB framing** → "5+10 primitives is right"
2. **Context primitives are audience-irreducible** → "actually they can't be removed"
3. **Closure argument** → "every race is structurally detectable"
4. **ISR-to-main raises the principle conflict** → "this is coping, NOT good"
5. **Walk-back** → "intrinsics already catch wrong intention"
6. **Push-back** → "the walk-back is the drift pattern"
7. **Convergence on Option 3** → narrow the claim, don't extend it

Each step had real content. The pattern was the rapid-fire convergence on
increasingly confident claims without enough resistance.

### 23.3 The Volatile Bypass Example

One specific drift manifestation: the morning's discussion at one point
assumed ZER allowed raw `*(volatile uint32_t*)addr = val` casts, generating
extensive Option 1/2/3 analysis about how to handle the bypass. The actual
ZER grammar rejects this cast outright. The drift was building elaborate
solutions to a non-problem.

The catch was the user noticing "wait, not sure if volatile is banned." That
question triggered actually checking what ZER does, which revealed no bypass
exists, which collapsed the elaborate analysis.

### 23.4 The Discipline

When you find yourself walking back a self-correction, the walk-back is
usually the wrong move. The self-correction was responding to something real.

When a framing sounds clean and the response is "lock this in, this is the
answer," check whether the analysis actually supports the strength of the
claim.

When elaborate solutions are being designed for a problem, verify the problem
exists by checking the actual code.

The architectural decisions that age well hold conditional framings ("this is
plausible architecture conditional on these specific verification steps")
rather than unconditional ones.

### 23.5 The Healthy Pattern

The healthy version of design discussion:
1. State the position
2. When self-correction occurs, take it seriously
3. Examine what the self-correction was responding to
4. Either incorporate the correction or explicitly scope the position
5. Don't reason around the correction
6. Verify assumptions about what the code actually does
7. Hold conditional framings until verification is complete

This produces decisions that survive examination over years.

### 23.6 The Convergence Locked in This Document

These decisions are LOCKED IN for current scope:

1. **TLB structure:** 5 strict primitives + ~5 context primitives, with sealed
   primitives provisional based on analysis depth
2. **Closure argument:** holds for thread-based concurrency, conditional on
   4-5 structural rules being implemented
3. **ISR-to-main:** scoped to user discipline within hardware-access
   primitives (Option 3)
4. **Substrate accountability principle:** rescoped to follow abstractions,
   not emergent compositions
5. **Specification-implicit discipline:** maintained across all safety work
6. **Definition A:** verifies access mechanism; hardware semantics user
   responsibility
7. **No peripheral-specific catalog in core:** category-level only; vendor
   libraries for peripheral specifics
8. **No C-style cast bypass:** ZER's grammar rejects integer-to-pointer casts;
   only @inttoptr path

These decisions can be reopened if real firmware code surfaces gaps the
analysis missed. Otherwise, they stand.

---

## 24. Concurrency Memory-Safety Audit & the Four-Axis Closure (2026-06-20)

> **This section SUPERSEDES the "4–5 structural rules" framing of §12/§18.2.**
> That framing (Condvar-loop, CAS-progress, Lock-balance, Atomic-on-shared,
> No-shared-stack) was an incomplete model. Three adversarial code-grounded
> sweeps (each finding verified against the actual handlers, not agent
> assertion) replaced it with a precise inventory and a four-axis architecture.
> Status: **audited, design converged, IMPLEMENTATION IN PROGRESS** — phase 2
> began 2026-06-21b with **9 of ~25 holes CLOSED** (BUG-743..751: Axis C
> `ir_merge_states` thread-merge, A1 exhaustive spawn dispatch, C2 spawn
> lifetime arm, A3 volatile-RMW, A4 Arena, D2 `@probe` `__thread`, B5 defer
> lock, A6/#5 interior-extraction ban extension, scoped-borrow exclusivity),
> each verified + regression-tested; full `make check` GREEN. Remaining real
> builds = the subsystem-scale core (B1–B4 lock-scope redesign, A6 `shared`-scalar
> representation incl. #7, scoped-borrow READ/CFG residue). **D1 (cinclude
> thread-capture) is RECLASSIFIED as a named FLOOR**, not a build — C-domain
> behavior, out of scope; the safe path exists today (long-lived data, no
> annotation). Per-hole CLOSED/OPEN ledger: `docs/limitations.md` "## OPEN —
> Concurrency memory-safety". Spec NOT yet frozen.

### 24.1 Current state — primitives done, safety incomplete

Every concurrency PRIMITIVE is implemented end-to-end (lexer→checker→emitter):
`shared struct` / `shared(rw)`, `spawn` (fire-and-forget) + `ThreadHandle`+`join`
(scoped), 15 `@atomic_*`, `@cond_wait/signal/broadcast/timedwait`, `Semaphore(N)`,
`Barrier`, `@once`, `threadlocal`, `move struct`, `Ring(T,N)` channel, async/yield/
await. The 8 §18.1 safety mechanisms are all real and pipeline-integrated (spawn
data-race scan, shared auto-lock, same-statement deadlock check, async-shared-
across-yield ban, move use-after-transfer, ThreadHandle-not-joined, `@inttoptr`
mmio+size+align, no-volatile/const-strip).

BUT the concurrency **safety** is not complete. Three sweeps found **~25 verified
cross-thread memory-safety holes** (data races + cross-thread use-after-free) that
compile clean. The pursuit-of-completeness lesson: **the find-rate did not decay**
across sweeps (9 → 11 → ~10 new), which proved the holes are generated by a small
number of architectural roots, not scattered bugs.

**Honest external claim (use this wording, do NOT overclaim):** "ZER targets
Rust-equivalent memory safety — data-race-free and no cross-thread UAF — via
*auto-inference* instead of `Send`/`Sync`/`Arc`/`Mutex` ceremony. The design is
specified; the implementation (a thread-aware safety layer) is pending. Deadlock-
freedom is out of scope — for ZER and for Rust alike (a Rust `Mutex` AB-BA
deadlock compiles fine; the dzone-style 'Rust prevents deadlocks' claim is wrong)."

### 24.2 The four architectural axes (every hole maps to one)

**Axis A — Reachability (the exclusion-list scanner).**
`scan_unsafe_global_access` (checker.c:~8676) decides "is this global thread-
reachable and unsynchronized" via an EXCLUSION list + per-AST-kind recursion, and
the spawn-arg handler (checker.c:~12005) cases only `TYPE_POINTER/HANDLE/OPTIONAL`.
So every exclusion is a hole and every forgotten type-kind is a hole:
- `volatile` global whitelisted (checker.c:8686 `if (sym->is_volatile) return false;`)
  → thread-shared `volatile` compound RMW (`g += 1`) is an undetected race. The
  compiler even *recommends* `volatile` as a race fix (checker.c:~12127). The ISR
  path DOES catch volatile-RMW (`check_interrupt_safety`) but it is gated on
  `c->in_interrupt` — no thread equivalent. (MMIO `ctrl[0] |= bit` from two threads
  is the same class.)
- `Arena` whitelisted (checker.c:8698) → concurrent `scratch.alloc()` races the
  bump-pointer metadata. (Pool/Slab/Ring are correctly flagged; Arena/Barrier/
  Semaphore are not — "ISR-safe" was conflated with "thread-safe".)
- `threadlocal` whitelisted + `&threadlocal` carries no taint → publishing `&tl`
  into a global/shared carrier hands another thread a pointer into the *wrong*
  thread's TLS slot (cross-thread UAF on TLS teardown).
- `TYPE_SLICE` and `TYPE_OPAQUE` spawn args have NO case → a `[*]T` over a stack
  array, or `(*opaque)&local`, passes the spawn-arg check (and the local-derived
  walker doesn't unwrap `NODE_TYPECAST`, checker.c:~9252).
- **General shape KNOWN & stable.** Fix per family is structural: make spawn-arg
  dispatch exhaustive over type-kinds (a `-Wswitch`-style gate, like the existing
  walker-default audit); replace the exclusion list with a carrier-or-tainted
  *inclusion* model; generalize `check_interrupt_safety` from ISR-coupled to
  any-two-contexts; wire the already-written-but-never-called oracle
  `zer_volatile_compound_valid` (src/safety/concurrency_rules.c:106).

**Axis B — Locking completeness (single-root auto-lock).**
The per-statement auto-lock (`current_stmt_shared_root`, `find_shared_root_in_stmt_ir`,
ir_lower.c) locks only the FIRST shared root of a top-level statement and is bypassed
at every other shared-access-bearing context:
- `shared(rw)` multi-read `x = ga.v + gb.v` locks only `ga`; `gb.v` read unlocked
  (the BUG-500 read-only deadlock-skip removed the only guard).
- union-switch on a shared field: the lock is released before arm bodies, and the
  arm capture is a live raw pointer into the shared bytes.
- `@cond_wait` predicate: a 2nd shared read in the predicate gets no lock (the
  lock-wrap pass skips cond args; `collect_shared_types_in_expr` has no
  `NODE_INTRINSIC` case).
- `@once` loser doesn't wait → reads half-constructed published state (no
  happens-before edge; contrast `_zer_mtx_ensure_init_cv` which DOES spin).
- defer-body shared access: `find_shared_root_in_stmt_ir` returns NULL for
  `NODE_DEFER` (ir_lower.c:~1228); the deferred body is emitted with no lock.
- **General shape KNOWN.** One redesign of the lock-scope walker — cover every
  shared-access sub-statement (all roots, switch-arm/defer/cond bodies) and emit
  the `@once` loser-wait — closes the family.

**Axis C — Lifetime / temporal (per-function CFG lattice).**
The IR analyzer (zercheck_ir.c) tracks `handles[]` but its dataflow lattice was
built ONLY for handles:
- **THE most actionable bug (verified at code level):** `ir_merge_states`
  (zercheck_ir.c:573-643) unions only `result.handles[]`; `threads[]`/`joined` ride
  solely via `ir_ps_copy(first_live)` and are NEVER merged from other predecessors;
  the convergence check (zercheck_ir.c:~4228) compares only handle state. So a
  scoped-spawn `ThreadHandle` created inside one branch (or on a label-reordered
  pred) is **silently dropped at the CFG merge**, the return-block join scan
  (~4970) sees an empty `threads[]`, and emits NO "not joined" diagnostic. Because
  `&stack-local` into a scoped spawn is permitted *only* on the premise that join
  is enforced (checker.c:12011), the dropped obligation is a false-green cross-
  thread stack UAF — **the enforcer exists but is bypassed at every merge.**
- stack-local `shared struct` published to a fire-and-forget spawn (checker.c:12009
  accepts `*shared T` with NO lifetime check); free-after-publish; detached
  grandchild outliving a parent's scoped join; block-scoped Barrier/Semaphore
  outlived by a function-scoped join; move-struct `&`-unwrap missing in the
  `IR_SPAWN` key extraction (sibling `IR_CALL`/asm sites DO unwrap `&`); async task
  struct (a plain non-shared aggregate) raced by concurrent polls.
- **General shape KNOWN & SHARPENED.** The precise root is "a second tracked-state
  family (`threads[]`, and by extension block-scoped carrier lifetimes) was never
  added to the dataflow merge." Fix: extend `ir_merge_states` + the convergence
  comparator + `ir_ps_copy` to union/compare `threads[]`; add block-scope lifetime
  tracking for stack carriers borrowed into scoped spawns; add the publication
  lifetime-arm + the Handle-gen runtime trap for freeable carrier payloads.

**Axis D — Boundary / runtime-completeness (NEW, found sweep 3; spec NOT frozen).**
Concurrency that enters with NO visible ZER `spawn`/`shared`/lock node, so axes
A/B/C have nothing to attach to:
- **FFI/cinclude capture — RECLASSIFIED 2026-06-21b as a named FLOOR, not a hole:**
  a ZER pointer/funcptr handed to a bodyless extern that `pthread_create`s
  internally. This is **C-domain behavior**, outside ZER's boundary by the same
  logic that makes ZER silent on ANY C-internal behavior (a `cinclude`d C function
  that double-frees / stashes / over-writes your pointer is equally invisible).
  It is NOT a "program-consequence leak": that claim is scoped to uses **in ZER
  source**; a C lib threading your pointer is a use **in C source**. The earlier
  framing ("the one place the claim leaks") was WRONG — it was never in the claim.
  A verification would be the contract-trap §22/CLAUDE.md rejects (trust an
  unverifiable C-behavior claim, manufacture false safety). **The safe path exists
  TODAY, no annotation:** hand capturing externs long-lived data (global / global
  `shared struct` instance / Pool/Slab — never `&stack_local`) → no cross-thread
  UAF; cross-C-thread mutual exclusion follows the C lib's contract (ZER's
  auto-lock doesn't reach into the C thread). Belongs with deadlock + hardware-
  consequence as the FLOOR. (An optional `captures` marker could later be added as
  pure AUDIT VISIBILITY — never sold as verified.)
- **Compiler-emitted runtime:** `@probe` emits `static` (NOT `__thread`)
  `_zer_in_probe`/`_zer_probe_jmp` + a process-global signal handler
  (emitter.c:~5056) unconditionally alongside `<pthread.h>`; two threads in
  `@probe` regions race it / longjmp across frames. A ZER fn installed as a POSIX
  signal handler via cinclude self-deadlocks on its non-recursive mutex.
- **Fix is a boundary DISCIPLINE, not a scanner extension:** a `threads`/`captures`
  capability on cinclude funcptr/pointer params (require `*shared`/atomic +
  non-stack pointee; run the spawn-target race rule on a captured ZER callback);
  a `--strict-interop` default-reject for non-`shared`/non-static ZER pointers to
  thread-capturing externs; make emitted runtime `__thread` or spawn-guarded. This
  axis is partly "the existing C-interop FLOOR, applied to concurrency" — make it
  audit-visible, exactly as ZER does for every other C-boundary property.

### 24.3 The closure (how all four axes end "once and for all")

The unifying move (the asm-doc "Effect-Row Composition" pattern, restated for
concurrency): **put the invariant on the DATA, not the path.** A data race needs
two threads at one mutable location, ≥1 write, unsynchronized. Replace per-
mechanism checks with one inferred, non-strippable **`shared` taint** + a finite
set of checks:

1. **Reachability taint (Axis A):** make `shared` a real, inferred, non-strippable
   qualifier that attaches to scalars/pointers and propagates through `&`/casts/
   slices like `volatile` (Model 4 extension; the machinery already exists for
   `volatile`). Inferred — NOT user-written — from: carrier source (`&` of a shared
   field), `@atomic_*` use-site (brands the location an atomic cell), publication to
   a thread. Replaces the exclusion list with carrier-or-tainted. The *only*
   user-written `shared` stays the `shared struct` keyword, and even that is
   demanded by a compile error (spawn-of-plain-struct rejected), never guessed.
2. **Locking completeness (Axis B):** the auto-lock must cover every shared-access
   sub-statement (all roots / switch-arm / defer / cond bodies), and `@once` must
   emit a loser-wait. A tainted access not under its lock = error.
3. **Lifetime/temporal lattice (Axis C):** generalize the CFG dataflow lattice so
   EVERY tracked-state family (`threads[]`, block-scoped carrier lifetime) is merged
   — not just `handles[]`. Plus the publication lifetime-arm and the Handle-gen
   runtime trap (freeable carrier payload → runtime trap, not corruption).
4. **Boundary discipline (Axis D):** the cinclude concurrency-capture capability +
   `--strict-interop`, and thread-safe emitted runtime.

Then a **CI audit gate** (the `audit_type_dispatch.sh` / `walker_default_audit.sh`
family) freezes it: "every value-flow site propagates the taint, every access-site
checks it, every spawn-reachable allocator/primitive is carrier-or-rejected, every
lattice family is merged, every thread-capturing extern is disciplined." Add a new
cast/access/primitive later → the gate fails until it handles the taint. That is the
difference between a patch and a closure — it **cannot regress.**

### 24.4 Auto-inference (the dumb-user property — better than Rust)

The `shared`/atomic-cell representation is INFERRED, not annotated — same family as
`keep`, escape flags, provenance: scalar atomic cell from the `@atomic_*` use-site
(+ the spawn-scan); pointer carrier-provenance by propagation from its source. The
dumb user writes naive code and either gets auto-tainted-and-enforced or a compile
error that NAMES the fix — never silent unsafety. Rust achieves the same
completeness with `Send`/`Sync` (auto-derived) + `'static` on `spawn` + explicit
`Mutex`; ZER's four-axis closure is the auto-inferred equivalent (`Send`≈Axis A,
`'static`≈Axis C-lifetime, auto-lock = the ergonomic win whose price is Axis B).
funcptr thread-safety is likewise auto-inferable for file-visible targets (per-file
points-to + the existing scan) and propagatable like `keep`; only a target crossing
the per-file/whole-program boundary (banned by architecture) falls to conservative-
reject with an optional boundary opt-out.

### 24.5 Scope & status

- **In scope, achievable by construction:** no data races, no cross-thread UAF, for
  all programs incl. arbitrarily buggy user coordination.
- **Named floor (permanently out):** deadlock / livelock — undecidable, same as
  Rust; a hang is not corruption. (ZER's per-statement auto-lock already kills the
  lock-ordering deadlock CLASS by construction; a `locked g {}` block, if added for
  multi-statement atomicity, must keep the single-shared-lock-held rule to preserve
  that.)
- **Conservative-reject boundaries:** cross-function free (needs FuncSummary) and
  indirect dispatch beyond the file — "safe" there = reject-the-unprovable (100%
  memory-safe, flexibility cost).
- **Status:** spec NOT frozen (Axis D appeared in sweep 3; still-unprobed residue
  remains — FFI callback tables, other emitter-runtime globals, a systematic
  "merged vs first_live-only" audit of every `IRPathState` field, cross-module
  spawn/extern interaction, NODE_STRUCT_INIT global read in a spawn body). Each
  axis's general SHAPE is known with a single structural fix, but individual sites
  still scatter until each structural fix + its CI gate lands. **This is subsystem-
  scale work (comparable to `keep` or the IR migration), not patches.**
- **Sweep records (full per-hole detail with file:line):** task outputs
  `wpbbu8v47` (sweep 1, 9 holes), `wwt4c31zh` (sweep 2, 11 holes), `wgvm1bid5`
  (sweep 3, ~10 holes + Axis D). Inventory mirrored in `docs/limitations.md`
  "## OPEN — Concurrency memory-safety".

### 24.6 Completeness — the four NECESSARY conditions, and how to PROVE it (Iris, not more sweeps)

The four axes are NOT an empirical bucket-count — they are the negation of the four
**necessary conditions** for a cross-thread memory hazard. A data race or
cross-thread UAF requires ALL of:
- **reach** — two threads reach the same mutable location;
- **¬discipline** — >=1 access is unsynchronized (not lock/atomic)  [data race], OR
  **¬lifetime** — the location is freed/dies while still reachable  [cross-thread UAF];
- **visibility** — the analyzer must SEE the threads/locations to check the above.

Negate all four — establish reach as an inferred taint, enforce discipline +
lifetime on everything tainted, ensure visibility (or conservative-reject) — and
the hazard is **inexpressible by definition**. That is *why* there are exactly four
(A=reach, B=discipline, C=lifetime, D=visibility), not "we found four buckets".

**Adversarial stress-test (2026-06-20) — tried to construct a FIFTH condition,
failed.** Every exotic memory-corruption hazard reduces to not-discipline or
not-lifetime: mutex/condvar freed-while-held -> not-lifetime (the carrier's own
lifetime); union variant raced (cross-thread type confusion) -> reach AND
not-discipline; ABA / use-after-realloc -> not-lifetime (+ the Handle gen-counter is
the discipline); lock-free reclamation (RCU/epoch) -> not-lifetime; double-free
across threads -> not-discipline / not-lifetime; `@once` half-init read ->
not-discipline-of-publication / temporal-validity; relaxed-atomic ordering (if ever
added) -> not-discipline; uninitialized cross-thread read -> auto-zero makes it
defined, else it is LOGIC not corruption. No fifth found.

The one memory-RELATED concern outside the four: **resource exhaustion** (unbounded
thread spawning / OOM / stack blow-up). NOT aliasing corruption (no race, no UAF) —
a liveness/resource concern in the same NAMED FLOOR as deadlock, not a fifth axis.

**Why empirical sweeping cannot finish, and Iris can.** The three sweeps did their
job: they produced the four axes + the concrete site-classes per axis. But the
find-rate did NOT decay — which is itself the proof that empirical enumeration
*cannot* establish COMPLETENESS: it keeps finding sites within the four axes forever
and never proves "these four are all of them." (This is the exact trap that produced
the now-discarded "5 rules": a clean-looking enumeration assumed complete.) Sweeps
answer "what are the SITES" (a finite, CI-gateable list per axis); they cannot answer
"are the four conditions EXHAUSTIVE."

That second question is a THEOREM, and the right tool is concurrent separation logic
— **Iris specifically**:
- Iris is purpose-built for data-race-freedom; **RustBelt** (the formal foundation
  of Rust's safety) IS an Iris development proving exactly this claim class for a
  systems language — the existence proof that the approach works.
- The four conditions already MATCH Iris's structure: a location is thread-locally
  OWNED (no reach), or SHARED under an invariant opened only under a lock/atomic
  (reach + discipline), and every resource has a LIFETIME. Formalizing the four
  conditions in Iris is natural, not a translation.
- Crucially, the proof OBLIGATIONS *derive* the minimal "what to track" set: to prove
  data-race-freedom you must give every shared location an invariant (-> the `shared`
  taint), maintain it across every access (-> the discipline check), and bound every
  resource's lifetime (-> the lifetime lattice). Iris doesn't just CHECK the four
  conditions — it DERIVES the minimal tracked state that enforces them. **That is the
  answer to "determine what to track."**
- Fits ZER's existing infra (Coq + Iris ~47 files / ~330 axiom-free theorems + the
  Level-1-oracle discipline): the four-condition exhaustiveness is a **Level-1**
  theorem (the abstract spec is complete); the sweeps are **Level-2** (empirical
  rejection of known violations); VST on the extracted predicates is **Level-3**.

**The decision (matches ZER's seL4/CompCert workflow — implement first, prove
second):** (1) treat the four-condition framing as the implementation spec NOW (it
IS informal Iris) and build the four-axis closure + CI gate; (2) write the **Iris
exhaustiveness proof** as the second pass — it turns "stress-tested high-confidence
framing" into "provably the complete set," and its proof obligations are the
authoritative minimal tracking spec. **Do NOT run more enumeration sweeps for
COMPLETENESS** — they cannot provide it (the flat find-rate is the evidence); use
sweeps only to finish the per-axis SITE lists, frozen by the CI gate.

---

## 25. Glossary

**Definition A:** the architectural principle that primitives verify access
mechanism structurally (types, ranges, qualifiers, contexts, dependencies)
while treating semantic correctness against external substrates as user
responsibility.

**Definition B:** the alternative principle (rejected for ZER) that primitives
encode specific operational semantics requiring user-supplied contracts
verified against silicon.

**TLB (Trusted Language Base):** the set of primitives the language must
provide as trusted because user code cannot safely reconstruct them. Includes
strict primitives (computationally irreducible) and context primitives
(audience-irreducible).

**Strict primitive:** a primitive computationally irreducible at the source
language level (requires compiler/runtime support unavailable to user code).

**Context primitive:** a primitive computationally derivable from strict
primitives but provided by the language because hand-rolling creates the
bug class the language was designed to prevent.

**Closure argument:** the proof that all programs in the language are safe
because the operations that could violate the safety property are bounded
under a finite set of compiler-visible primitives.

**Substrate accountability principle (rescoped):** ZER owns safety for the
bug classes its abstractions directly verify against. The principle's scope
follows the abstractions, not the emergent properties of compositions.

**Specification-implicit:** the design discipline where verification infers
properties from code structure, not from user-supplied contracts.

**Specification-explicit:** the alternative discipline (used by SPARK/Ada)
where users write contracts and the compiler verifies code-against-contract.

**Category A error:** contract violation by user code. Caught by Definition A
at compile time.

**Category B error:** contract-following code with spec-wrong contract. Not
caught — requires hardware knowledge outside the language.

**Category C error:** contract-following, spec-correct, but composition-wrong
code. Partially caught when composition rules are expressible; otherwise
user responsibility.

**ISR-to-main (Option 3):** the decision that synchronization discipline
between interrupt handlers and main thread is user responsibility within
ZER's hardware-access primitives, because verification would require
hardware-execution-model knowledge.

**Drift pattern:** the meta-pattern where design discussions cycle through
increasingly confident "lock-in" claims with each cycle introducing small
drift toward unconditional framings.

**Audit task:** the work of verifying that existing primitives' contracts
follow Definition A consistently. Spot-check ~10 primitives first; full
audit (~130 primitives) if spot-check warrants.

---

## End of Document

This document captures the unified architectural analysis for ZER's safety
model across all five domains (memory, type, ASM, concurrency, I/O). The
Definition A principle is the central organizing rule, applied uniformly
to produce a coherent safety story.

The decisions are LOCKED IN for current scope. The implementation work is
bounded: 4-5 structural rules for concurrency closure (~620-1150 lines), the
primitive audit (~20-130 hours depending on spot-check results), and
documentation updates.

Future safety work follows the same template: identify the bounded primitive
surface, verify each primitive against the safety property, scope out
properties requiring external knowledge. Future hardware classes follow
Definition A: verify access mechanism, leave semantics to user.

The architectural pattern is now coherent across all five safety domains.
ZER's distinctive contribution is the same Definition A principle applied
uniformly with structural verification, producing compile-time safety with
C-level cognitive load and formal verification depth (Iris/VST extraction).

Hold this position. Implement the queued work. Resist drift toward extending
claims beyond what the analysis actually supports. The convergence has done
its work; the implementation is what's left.
