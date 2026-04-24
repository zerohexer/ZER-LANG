# ZER-Asm Verification Plan — Full Context Dump (2026-04-23)

**Purpose:** This document captures the complete context from the Vale-tier verified asm design discussion so any fresh session can pick up exactly where we left off. Read this top-to-bottom if you are starting fresh on the asm verification work.

**Key revisions (2026-04-23 late-day):**
- Scope reduced from 4 archs → 3 archs (x86-64, ARM64, RISC-V). Cortex-M deferred to v1.1.
- `asm` keyword renamed to `unsafe asm` — the `unsafe` marker is now **required** (Rust-style explicit escape hatch). Bare `asm(...)` is rejected with compile error. Phase 1 verified rule (`zer_asm_allowed_in_context`) unchanged — structural rule applies to new naming.
- Sail cannot generate asm code (web search confirmed). Removed "Sail codegen" as a shortcut.
- Islaris covers ARM64 + RISC-V only, single-threaded. x86 verification requires custom framework build-out.
- **IMPLEMENTATION-FIRST DECISION (2026-04-23 evening):** Intrinsic IMPLEMENTATION is decoupled from Phase 2-7 prereq. Phase D split into D-Alpha (implementation, no formal proofs) and D-Beta (verification layer added later). This matches how seL4/CompCert/Vale were actually built — code first, proofs iteratively. Users can ship pure-ZER kernels ~1-2 years earlier. See "Implementation-First Plan (Phase D-Alpha)" section below.
- **LINUX-SCALE EXPANSION (2026-04-23 late evening):** Target scope upgraded from 96 → **130 intrinsics** covering what Linux kernel arch/ uses. Explicitly out-of-scope: hypervisor (VMX/SVM/ARM-virt), enclaves (SGX/TDX/TrustZone), model-specific performance counters, Cortex-M (v1.1). These are handled via hardened `unsafe asm` escape hatch (see "Hardened `unsafe asm`" section).
- **HARDENED `unsafe asm` (planned D-Alpha-7.5):** Upgrade from raw GCC inline asm pass-through to Rust-tier typed operands + clobber validation + mandatory safety docs + audit emission. Makes "vendor-specific via unsafe asm" actually safe, unlocking all modern CPU features (TDX, MTE, PAC, CET, future extensions) without needing dedicated intrinsics per feature.
- **TIERED 100%-SAFE PLAN (2026-04-23 final):** Three-tier shipping for reaching 100% safe asm. **Tier A (v1.0):** strict mode with 18 compile-time rules (SPARK Ada tier) — 95% of asm bug classes structurally impossible. **Tier B (v1.0.1):** selective Vale-tier formal verification of 20 critical intrinsics — 99%. **Tier C (v1.1+):** full Vale-tier + `@verified_spec` attribute for any `unsafe asm` block — **100% safe asm** for every block. Hardware behavior (CPU execution of the verified instructions) is the silicon vendor's domain — not ZER's scope, not any language's scope.

**Related docs:**
- `docs/ASM_ZER-LANG.md` — earlier (2026-04-01) asm research (context switch, boot, atomics design). That was the foundation; this is the formal verification plan built on top.
- `docs/formal_verification_plan.md` — overall 8-phase verification roadmap. ZER-Asm is Phase 8 in that plan.
- `docs/phase1_catalog.md` — Phase 1 predicate catalog (85 predicates, 100% complete 2026-04-22).
- `docs/proof-internals.md` — VST/Coq proof patterns.

---

## Executive Summary

**Goal:** Achieve 100% formally verified asm coverage at Linux-kernel scale across x86-64, ARM64, and RISC-V for ZER's target domain (OS kernels, RTOSes, application-class embedded firmware). Vendor-specific code (SGX, TDX, TrustZone, etc.) uses hardened `unsafe asm`.

**Budget:** ~5,000 hours total, committed by the user.

**Approach:** Provide **130 intrinsics** for Linux-scale kernel features; hardened `unsafe asm` as typed escape hatch for vendor-specific code. Use GCC as the assembler (no new toolchain). Runtime code generation (JIT, live patching) is blocked by ZER's existing type system, not by asm rules.

**Revised effort with 3-arch scope + 7 shortcuts + hardened unsafe asm:** ~3,100 hours for asm verification + 900 hrs prerequisite (Phases 2-7) = **4,000 hours**. Leaves **~1,000 hours surplus** for crypto/certification/Cortex-M-later.

**Architectures (priority order):**
1. x86-64 — servers, desktops, laptops (Intel/AMD)
2. ARM64 / AArch64 — Apple Silicon, mobile, modern embedded application-class
3. RISC-V — open ISA, fastest-growing, best formal tooling (Sail-native)

**ARMv7 Cortex-M NOT in initial scope.** Can be added as v1.1 (+~300 hrs). Cortex-M users in v1.0 continue using C with FreeRTOS/Zephyr.

**Prerequisite:** Phases 2-7 of the formal verification plan must complete first (~900 hours). See `docs/formal_verification_plan.md`.

**Current status:** Not started. Phase 1 is 100% complete (85/85 predicates). Phase 2 is 7% complete (4/60 decisions). Resume Phase 2 before touching asm work.

---

## The Central Insight (result of this session's discussion)

The original framing was: "verify any asm a user writes." That's open-ended and requires Vale-tier formal verification per instruction — ~5K hours per arch.

**The correct framing is:**

> ZER source contains zero user-written asm. All processor-level operations go through ~96 verified intrinsics per architecture. This is a finite, bounded verification surface.

**The 96 intrinsics cover 100% of ahead-of-time-compiled systems programming.**

**Runtime code generation (JIT compilers, live patching, self-modifying code) is blocked independently by ZER's type system:** you cannot cast a `*u8` data pointer to a function pointer in ZER. This is a property of the language type system, not a special asm rule.

Therefore:
- **Asm coverage: 100%** (for any ahead-of-time-compiled program in ZER's target domain)
- **Runtime codegen: 0%** (blocked by type system; users who need JIT link an external C module via `cinclude`)
- **Target domain: OS kernels, RTOSes, embedded firmware, drivers, crypto libraries, hypervisors, databases-without-JIT, game engines, networking stacks**

---

## Why This Approach (journey of the discussion)

The conversation explored 4 options before landing here:

### Option 1: Fork Vale verbatim
- Bundle F* + Z3 + OCaml into ZER
- Build size: 100 MB → ~2 GB
- Maintenance: sync with Microsoft Research or freeze (bitrot)
- Coverage: only ~10% of ZER's needs (Vale is crypto-focused)
- **Rejected**

### Option 2: Re-implement Vale in Coq
- Use ZER's existing Coq stack
- 1-2 engineers, 12-24 months
- Reinvents the wheel
- **Rejected**

### Option 3: ZER-Asm (Vale-inspired but scoped to OS needs)
- ~15-20 verified intrinsics for OS primitives
- 300 hrs per arch
- Covers 90% of OS use cases
- **Original recommendation but expanded to 96 intrinsics in final plan**

### Option 4: Intrinsics-preferred + `unsafe asm` escape hatch (FINAL CHOICE)
- 96 verified intrinsics per arch cover 100% of ahead-of-time target-domain code
- `unsafe asm { ... }` kept as explicit escape hatch (Rust-style marking, Phase 1 verified rule: only in `naked` functions)
- Users prefer `@intrinsic()` calls; `unsafe asm` for edge cases not covered by intrinsics
- Runtime codegen blocked by type system (no special rules needed)
- GCC is the assembler — no new toolchain
- **This is the plan**

---

## Pivotal Design Decisions

### Decision 1: Require explicit `unsafe` marker for all inline assembly
**Reason:** Open-ended verification is infeasible. Fixed intrinsic surface is tractable. But permanent escape hatch is valuable for edge cases (new vendor extensions, niche hardware, rare use cases). The `unsafe` marker enforces explicit intent.
**Implication:** `unsafe asm(...)` is the only accepted form. Bare `asm(...)` gives compile error with helpful message. Still restricted to `naked` functions per Phase 1 verified rule. Users prefer `@intrinsic()` calls; `unsafe asm` for edge cases.
**Trust model:** Matches Rust's `unsafe { asm!() }` — marked, auditable, intentional. `grep -rn "unsafe asm"` finds every escape hatch in a codebase.

### Decision 2: GCC as backend, no new assembler
**Reason:** Writing a verified assembler is a separate 10-person-year project. GCC's GAS is 40 years old and production-quality.
**Implication:** ZER emits GCC extended inline asm (`__asm__ __volatile__(...)`). GCC handles encoding.
**Trust base adds:** GCC's assembler (same as all C projects).

### Decision 3: Sail + Islaris for ISA models
**Reason:** Cambridge/ARM/RISC-V International maintain Sail formal ISA models. Islaris provides Iris-based Hoare logic over Sail. Inheriting 5+ years of formal methods work saves ~2-3 person-years.
**Implication:** ZER verifies intrinsics against Sail-derived Coq models. Upstream contributions required for Sail bugs.

### Decision 4: Runtime codegen is NOT an asm problem
**Reason:** JIT/live-patch doesn't "use asm" in the language-keyword sense. They write bytes and cast data pointers to function pointers. That cast is blocked by ZER's existing type system.
**Implication:** No special asm rules needed for this category. Users who need JIT link external C via `cinclude`.

### Decision 5: cinclude allowed but marked UNSAFE-EXTERN
**Reason:** Honest escape hatch for users outside ZER's target market (JIT runtimes, research). Same pattern Linux uses with Vale (.S files linked into kernel).
**Implication:** Every `cinclude`-defined function call emits `[UNSAFE-EXTERN]` warning. Auditable via grep.

### Decision 6: CI warning if kernel-target code uses cinclude
**Reason:** Force missing intrinsics to be upstreamed, not worked around.
**Implication:** `target=kernel` + cinclude = CI warning. Non-kernel targets unaffected.

---

## The 96-Intrinsic Surface (complete)

Per architecture. Target market: OS/RTOS/firmware/kernel/driver.

| # | Category | Count | Examples |
|---|---|---|---|
| 1 | Atomics | 15 | `@atomic_cas`, `@atomic_fetch_add`, `@atomic_xchg`, `@atomic_load`, `@atomic_store`, `@atomic_fetch_sub`, `@atomic_fetch_or`, `@atomic_fetch_and`, `@atomic_fetch_xor`, `@atomic_fetch_nand`, `@atomic_min`, `@atomic_max`, `@atomic_umin`, `@atomic_umax`, `@atomic_test_and_set` |
| 2 | Memory barriers | 5 | `@barrier_full` (seq_cst), `@barrier_acquire`, `@barrier_release`, `@barrier_load`, `@barrier_store` |
| 3 | MSR/CSR access | 10 | `@cpu_read_msr(u32) -> u64`, `@cpu_write_msr(u32, u64)`, `@cpu_read_csr`, `@cpu_write_csr`, `@cpu_read_cr0/1/2/3/4`, `@cpu_write_cr0/1/2/3/4`, `@cpu_read_xcr0`, `@cpu_write_xcr0` (x86) / `@cpu_read_sysreg`, `@cpu_write_sysreg` (ARM) / `@cpu_read_csr`, `@cpu_write_csr` (RISC-V) |
| 4 | Cache maintenance | 8 | `@cache_flush_range`, `@cache_invalidate_range`, `@cache_clean_range`, `@cache_flush_line`, `@cache_prefetch_read`, `@cache_prefetch_write`, `@cache_inhibit_begin`, `@cache_inhibit_end` |
| 5 | TLB management | 5 | `@tlb_flush_all`, `@tlb_flush_asid`, `@tlb_flush_addr`, `@tlb_flush_range`, `@tlb_flush_global` |
| 6 | MMU control | 10 | `@mmu_set_pt`, `@mmu_get_pt`, `@mmu_enable`, `@mmu_disable`, `@mmu_set_asid`, `@mmu_invalidate_page`, `@mmu_probe`, `@mmu_modify_attrs`, `@mmu_set_ttbr0`, `@mmu_set_ttbr1` |
| 7 | Context save/restore | 4 | `@cpu_save_context`, `@cpu_restore_context`, `@cpu_save_fpu`, `@cpu_restore_fpu` |
| 8 | Interrupt control | 8 | `@cpu_disable_int`, `@cpu_enable_int`, `@cpu_wait_int`, `@cpu_ack_int`, `@cpu_eoi`, `@cpu_mask_int`, `@cpu_unmask_int`, `@cpu_get_int_state` |
| 9 | Privileged mode | 6 | `@cpu_enter_user`, `@cpu_exit_user`, `@cpu_syscall_entry`, `@cpu_syscall_return`, `@cpu_set_priv_stack`, `@cpu_get_priv_level` |
| 10 | Inspection | 10 | `@cpu_id`, `@cpu_read_cycles`, `@cpu_read_sp`, `@cpu_read_pc`, `@cpu_read_flags`, `@cpu_vendor_id`, `@cpu_feature_bits`, `@cpu_model_id`, `@cpu_core_id`, `@cpu_current_el` (ARM) / `@cpu_current_mode` (generic) |
| 11 | Power management | 5 | `@cpu_halt`, `@cpu_wfe`, `@cpu_sev`, `@cpu_reset`, `@cpu_idle_hint` |
| 12 | Misc hardware ops | 10 | Debug registers, performance counters, hardware breakpoints, DMA barriers, etc. |

**Total: 96 intrinsics per architecture.**

Each intrinsic:
1. Has a formal Coq specification (arch-agnostic where possible)
2. Has a verified per-arch implementation (matching spec)
3. Is exposed as safe ZER syntax (users never see asm)
4. Is target-gated (e.g., `@cpu_read_msr` only on x86)

---

## GCC Auto-Porting vs Per-Arch Work (CRITICAL distinction)

Common misconception: "we have to write everything 4 times for 4 architectures."

**Reality: GCC auto-ports the majority. Only 56 of 96 intrinsics need per-arch asm.**

### Three tiers of porting effort

**Tier 1 — Pure ZER code: 100% auto-ported by GCC. Zero per-arch work.**

Regular ZER code (no intrinsics) compiles to any GCC-supported arch automatically. x86-64, ARM64, ARMv7, RISC-V, PowerPC, MIPS, SPARC — all free.

**Tier 2 — Intrinsics via GCC builtins: GCC auto-ports. One implementation works on all archs.**

Approximately 40 of 96 intrinsics map to GCC builtins that handle arch selection internally:

| Intrinsic | GCC builtin used | Archs auto-handled |
|---|---|---|
| `@atomic_cas` | `__atomic_compare_exchange_n` | All |
| `@atomic_fetch_add` | `__atomic_fetch_add` | All |
| `@atomic_load` | `__atomic_load_n` | All |
| `@atomic_store` | `__atomic_store_n` | All |
| `@atomic_xchg` | `__atomic_exchange_n` | All |
| (all 15 atomics) | `__atomic_*` | All |
| `@barrier_full` | `__atomic_thread_fence(__ATOMIC_SEQ_CST)` | All |
| `@barrier_acquire` | `__atomic_thread_fence(__ATOMIC_ACQUIRE)` | All |
| `@barrier_release` | `__atomic_thread_fence(__ATOMIC_RELEASE)` | All |
| (all 5 barriers) | `__atomic_thread_fence` | All |
| `@cache_prefetch_read` | `__builtin_prefetch(addr, 0, 3)` | All |
| `@cache_prefetch_write` | `__builtin_prefetch(addr, 1, 3)` | All |
| `@cpu_trap` | `__builtin_trap` | All |
| `@cpu_unreachable` | `__builtin_unreachable` | All |
| Bit ops (ctz, popcount, clz) | `__builtin_*` | All |
| Byte swap | `__builtin_bswap{16,32,64}` | All |

**Count: ~40 intrinsics. Write once, compile anywhere. GCC figures out the asm.**

**Tier 3 — Intrinsics with arch-specific asm: Unavoidable per-arch work.**

Approximately 56 of 96 intrinsics need different asm per arch because the underlying hardware operation is fundamentally different.

Example — `@cpu_read_msr` (read a control register):

```c
// x86-64: Model-Specific Register
__asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(index));

// ARM64: System Register
__asm__ volatile ("mrs %0, " REG : "=r"(val));

// ARMv7: Coprocessor register
__asm__ volatile ("mrc p15, 0, %0, c1, c0, 0" : "=r"(val));

// RISC-V: Control and Status Register
__asm__ volatile ("csrr %0, " CSR : "=r"(val));
```

| Intrinsic category | Arch-specific? | Why |
|---|---|---|
| MSR/CSR access (10) | YES | Different register spaces per arch |
| Cache maintenance (8) | YES | x86 clflush vs ARM dc civac vs RV cbo.inval |
| TLB management (5) | YES | invlpg vs tlbi vs sfence.vma |
| MMU control (10) | YES | CR3 vs TTBR vs SATP; page table formats differ |
| Context save/restore (4) | YES | Different register sets (16 vs 31 vs 32 GPRs) |
| Interrupt control (8) | YES | cli/sti vs cpsid/cpsie vs csrw mstatus |
| Privileged mode (6) | YES | Ring model vs EL model vs M/S/U modes |
| Inspection (10) | YES | cpuid vs midr_el1 vs misa |

**Count: ~56 intrinsics per arch. Need separate asm strings. GCC still encodes them.**

### GCC remains the assembler for all tiers

Even Tier 3 uses GCC's built-in assembler (GAS) for encoding:

```
ZER compiler emits inline asm string for target=arm64
        ↓
GCC parses the ARM64 mnemonic (e.g., "mrs")
        ↓
GCC's assembler emits ARM64 machine code bytes
        ↓
Binary
```

**We never write a custom assembler. We never parse asm. GCC does all encoding for all 4 archs.**

This is the same pattern every OS uses — Linux has `arch/x86`, `arch/arm64`, etc. directories with per-arch asm strings, all assembled by GCC. ZER does the same, just with much less per-arch code (56 intrinsics vs Linux's thousands of asm lines).

### Per-arch work breakdown (what's manual vs automatic)

| Work item | Per-arch? | Effort |
|---|---|---|
| Compile ZER → machine code | **No** (GCC auto) | 0 hrs |
| Pure ZER code (no intrinsics) on all archs | **No** (GCC auto) | 0 hrs |
| 40 GCC-builtin intrinsics | **No** (GCC auto) | 0 hrs per additional arch |
| Asm encoding (all 4 archs) | **No** (GCC auto) | 0 hrs |
| 56 arch-specific intrinsics (asm string) | **Yes** (manual) | ~5-30 lines per intrinsic × 56 × 4 archs |
| Coq spec per intrinsic (arch-agnostic) | No (write once) | ~1 hr each, shared |
| Coq proof per arch | **Yes** (manual) | ~5-15 hrs per intrinsic per arch |

**Bottom line:** of 96 intrinsics, only 56 need per-arch implementation work. Of the total verification work, ~80% is Coq proofs (not writing asm).

---

## Verification Surface (how many intrinsics need proofs)

**3-arch scope: x86-64, ARM64, RISC-V.**

| Unit | Count | Notes |
|---|---|---|
| Unique intrinsics (the API surface) | **96** | Same set on every arch (target-gated where needed) |
| Arch-agnostic Coq specs | **96** | Write once. Defines what each intrinsic does semantically. |
| GCC-builtin intrinsics (auto-port, 1 proof each) | **28** | Atomics (15) + barriers (5) + misc builtins (8) |
| Cross-arch intrinsics (semantic portable, 3 impls each) | **42** | Context switch, MMU, TLB, interrupts, cache, privileged |
| Mono-arch intrinsics (valid on 1 arch only, 1 proof each) | **26** | MSR (x86), CSR (RISC-V), sysreg (ARM), cpuid, midr, etc. |
| **Proofs for GCC-builtin intrinsics** | **28** | Shared across all archs |
| **Per-arch proofs for cross-arch intrinsics** | **42 × 3 = 126** | Proven per arch |
| **Proofs for mono-arch intrinsics** | **26** | 1 proof each |
| **Total verification work units** | **96 specs + 180 proofs** | Within ~1,400 hrs Phase D budget |

### Detailed intrinsic breakdown (3 archs)

| Category | API count | Implementation | Proofs (3 archs) |
|---|---|---|---|
| Atomics | 15 | GCC `__atomic_*` builtins | 15 shared |
| Barriers | 5 | GCC `__atomic_thread_fence` | 5 shared |
| Cache (icache clear + prefetch) | 2 | GCC builtins | 2 shared |
| Cache (dcache ops) | 6 | Per-arch asm | 18 |
| TLB management | 5 | Per-arch asm | 15 |
| MMU control | 10 | Per-arch asm | 30 |
| Context save/restore | 4 | Per-arch asm | 12 |
| Interrupt control | 8 | Per-arch asm | 24 |
| Privileged transitions (generic) | 4 | Per-arch asm | 12 |
| Privileged (SMC/HVC/ECALL) | 2 | Mono-arch (ARM/ARM/RV) | 2 |
| Cycle counter | 1 | GCC `__builtin_readcyclecounter` | 1 shared |
| MSR/CSR access | 10 | Mono-arch (split ~3-4 per arch) | 10 |
| Inspection (cpuid, midr, misa) | 10 | Mostly mono-arch | 10 |
| Power management | 5 | Per-arch asm | 15 |
| Misc GCC builtins (trap, bit manip) | 5 | GCC builtins | 5 shared |
| Misc mono-arch (debug, perf) | 4 | Mono-arch | 4 |
| **Total** | **96** | — | **180** |

### Priority tiers for verification

If shipping incrementally / with limited budget:

| Tier | Intrinsics | Proofs (3 archs) | Rationale |
|---|---|---|---|
| MVP (must-verify for any OS work) | Atomics, barriers, context switch, interrupts, MMU, privileged, cache, TLB | ~120 proofs | Without these, can't build a kernel |
| Standard (full target coverage) | + MSR/CSR, inspection, power mgmt | ~160 proofs | Complete kernel/RTOS/firmware |
| Extended (optional) | + misc (debug regs, perf counters, DMA barriers) | ~180 proofs | Polish for advanced features |

**MVP across 3 archs: ~120 proofs (~1,000 hrs).** Sufficient for building a real kernel.

**Full coverage across 3 archs: ~180 proofs (~1,400 hrs).** Complete claim.

### What "verified to be safe" means per intrinsic

For each intrinsic, verification proves:
1. **Functional correctness** — the asm implements the Coq spec (e.g., `@atomic_cas` actually does atomic compare-and-swap)
2. **Memory safety** — no out-of-bounds access through the intrinsic
3. **Register preservation** — callee-saved registers preserved per ABI
4. **Type safety at boundary** — inputs/outputs match ZER types
5. **No undefined behavior** — no violated invariants on flags, alignment, etc.

Not verified (part of trust base):
- Hardware silicon correctness
- GCC's asm encoding correctness (empirical — 40 years of testing)
- Sail ISA model accuracy (validated against hardware in Phase B)

### Shipping order optimizes verification effort

x86-64 first (700 hrs) → covers 70% of users immediately. ARM64 second (500 hrs) → +25% coverage. RISC-V third (400 hrs) → +4%. ARMv7 Cortex-M fourth (300 hrs) → embedded niche.

Each arch is independently usable. Don't need to wait for all 4 to ship.

---

## The 7 Shortcuts (all stacked in final plan)

### Shortcut 1: Require explicit `unsafe asm` marker (bare `asm` rejected)
- `unsafe asm(...)` is the ONLY accepted form; bare `asm(...)` gives compile error
- Still restricted to `naked` functions (Phase 1 verified)
- Users prefer `@intrinsic()` calls for safety; `unsafe asm` is the explicit escape hatch
- Same pattern as Rust's `unsafe { asm!(...) }` — no implicit-unsafe allowed
- Grep-auditable: `grep -rn "unsafe asm"` finds every escape hatch
- No DSL parser needed (simple keyword prefix)
- Saves 150-200 hrs vs building full Vale-style DSL

### Shortcut 2: Fixed intrinsic set (96 per arch)
- Bounded verification surface
- Enumerable, reviewable
- Reference: Linux/seL4/Zephyr asm inventory

### Shortcut 3: Category-based proofs
- Within a category (e.g., atomics), prove pattern once
- Apply to all ops in category with per-op schema
- Saves ~40% per-op vs per-instruction proofs

### Shortcut 4: Arch-agnostic specs
- Write Coq spec once (e.g., `atomic_cas_spec`)
- Prove per-arch implementation matches spec
- Saves ~25% (spec 1×, proof 4×)

### Shortcut 5: Port existing verified work
- seL4: ARM/x86 context switch, MMU, interrupts
- CompCert: memory model, ABI
- Vale: x86-64 crypto atomics
- Islaris: Iris + Sail integration
- HACL*: crypto math
- Saves ~30-40% of Phase C + Phase D work (~400-600 hrs for 4 archs)

### Shortcut 6: Ban dangerous categories entirely
Banned (not banned for safety theater — banned because they're outside target domain):
- Self-modifying code
- Arbitrary computed jumps
- Hot-patching
- Runtime code generation
- Custom calling conventions
- Branch-prediction hints
- Non-temporal stores (provided as intrinsic if needed)
- Speculative execution tricks
- Inline profiling (use separate profiler)
- Direct DMA programming (use `@dma_*` intrinsics)

Cuts ~15-20% of per-arch verification by eliminating subsystems.

### Shortcut 7: Tiered verification depth
- Tier 1 (full Hoare logic): context switch, MMU, interrupts (~30% of intrinsics)
- Tier 2 (structural + tests): atomics, barriers, cache ops (~50%)
- Tier 3 (inspection): cpuid, rdtsc, pure reads (~20%)
- Weighted cost: ~52% of full-formal cost
- Saves ~40-50% of per-arch verification

---

## Per-Architecture Breakdown

### Priority order (do in this sequence)
1. **x86-64** — largest market, most existing formal work (Vale, CompCert)
2. **ARM64 / AArch64** — Apple Silicon, mobile, modern embedded, cleanest ISA
3. **RISC-V** — growing, cleanest formal semantics, open ISA (Sail-native)

**NOT in initial scope:** ARMv7 Cortex-M. Can be added as v1.1 upgrade (~300 hrs).

### Intrinsic implementation cost per arch (with all shortcuts applied, 3-arch scope)

| Arch | Hours | Rationale |
|---|---|---|
| x86-64 | ~650 hrs | Hardest: most complex ISA, largest privileged surface (ring 0/1/2/3, CR regs, MSRs, xsave) |
| ARM64 | ~450 hrs | Cleaner ISA, seL4 proofs port-able, Islaris covers EL0/EL1/EL2/EL3 |
| RISC-V | ~350 hrs | Smallest ISA, cleanest Sail model, Islaris native |
| Shared infra | ~250 hrs | GCC builtin proofs + arch-agnostic specs written once |
| **Total Phase D (3-arch)** | **~1,700 hrs** | (includes per-arch and shared work) |

Reduced to **~1,400 hrs** after category-based proofs + tiered verification shortcuts.

**Total proofs across all 3 archs: ~180** (see verification surface table).

### Adding ARMv7 Cortex-M later (v1.1)

If embedded demand materializes post-v1.0:
- Estimate: ~300 hrs additional
- Simpler than ARM64 (no MMU, just MPU; fewer privilege levels; Thumb-2 only)
- Can reuse arch-agnostic specs
- Adds ~40 more proofs (Cortex-M-specific intrinsics)

Use the 1,250-hr surplus to fund this when ready.

---

## Full Budget Breakdown (5,000 hrs, 3-arch scope)

| Phase | Hours | % | Status |
|---|---|---|---|
| Phase 0: Prerequisites (Phases 2-7 of formal verification) | 900 | 18% | 4/60 decisions done; ~896 hrs remaining |
| Phase A: Core infrastructure (Sail adoption, Iris Hoare logic framework, automation tactics) | 500 | 10% | Not started |
| Phase B: ISA coverage (subset of Sail models for 3 archs — only instructions used by intrinsics) | 400 | 8% | Not started |
| Phase C: Advanced semantics (privileged ops, interrupts, memory consistency, constant-time tracking) | 350 | 7% | Not started |
| Phase D: Verified stdlib (96 intrinsics across 3 archs, category-based proofs, ~180 proofs total) | 1,400 | 28% | Not started |
| Phase E: Polish + certification (SIMD extras, debugger integration, DO-178C/ISO 26262 artifacts, docs) | 200 | 4% | Not started |
| **Committed total** | **3,750** | **75%** | |
| Surplus | **1,250** | **25%** | See allocation options below |
| **Grand total** | **5,000** | **100%** | |

### Recommended allocation of 1,250-hr surplus
- **Cortex-M as fourth arch (v1.1):** +300 hrs
- **Crypto constant-time verification (Vale-tier):** +400 hrs
- **DO-178C / ISO 26262 certification artifacts:** +300 hrs
- **Risk buffer:** +250 hrs
- **Total:** 1,250 hrs

This allocation delivers **4 archs + verified crypto + certification artifacts** instead of just "4 archs" in the original plan.

---

## Calendar Timeline (REVISED 2026-04-23 evening — implementation-first)

Two tracks run in parallel:

**Track 1: Intrinsic implementation (D-Alpha) — ships user value fast**

| Year | Cumulative hrs on Track 1 | Milestones |
|---|---|---|
| Year 0.5 | 100 | D-Alpha-1 atomics + D-Alpha-2 barriers shipped. Lock-free data structures viable. |
| Year 1 | 300 | D-Alpha-3 interrupts + D-Alpha-4 context switch. ISRs + scheduler viable. |
| Year 1.5 | 700 | D-Alpha-5 MMU + D-Alpha-6 cache/TLB. Virtual memory viable. |
| Year 2 | 800 | D-Alpha-7 complete. **Pure-ZER kernels on x86-64 shipping.** |
| Year 2.5 | 1,300 | D-Gamma ARM64 port. Apple Silicon + mobile viable. |
| Year 3 | 1,700 | D-Gamma RISC-V port. 3-arch kernel dev shipping. |

**Track 2: Formal verification (Phases 2-7 + D-Beta) — strengthens safety claim**

| Year | Cumulative hrs on Track 2 | Milestones |
|---|---|---|
| Year 1 | 500 | Phase 2-4 done. Decision extraction + walker + state APIs verified. |
| Year 2 | 900 | Phases 5-7 done. Operational subsets deep. Type system fully verified. |
| Year 2.5 | 1,300 | D-Beta x86-64 intrinsics verified. Formal proofs layered on working code. |
| Year 3 | 1,800 | D-Beta ARM64 + RISC-V verified. All intrinsics formally proven. |
| Year 4 | 2,000 | Phase E polish + cert artifacts. **Fully verified + shipping.** |

**Total across both tracks: ~3,700 hrs. Still within 5K budget with ~1,300 hr surplus** for Cortex-M / crypto Vale-tier / DO-178C.

**Key shift from original plan:** Working intrinsics in Year 2 instead of Year 4. Users benefit 2 years earlier. Formal verification layer added on top of working code without blocking shipping.

---

## Escape Hatch: `unsafe asm` (decided 2026-04-23, strict mode)

ZER keeps inline asm as a permanent escape hatch but **requires** explicit `unsafe` marking. Bare `asm(...)` is rejected at compile time. This matches Rust's pattern (`unsafe { asm!(...) }`) — no implicit unsafe allowed — and acknowledges that intrinsics cannot cover 100% of every edge case (new vendor extensions, experimental hardware, undocumented quirks).

### The three trust tiers

```zer
// Tier 1: Intrinsic (preferred — verified, safe)
fn save_my_state(*State s) {
    @cpu_save_context(s);   // verified intrinsic
}

// Tier 2: unsafe asm (escape hatch — explicit, Phase 1 structurally verified)
/// @safety: reads custom vendor register CPUX_QUIRK_0x42
/// Only on MyVendor CPUs; will fault elsewhere.
naked fn read_vendor_quirk() -> u64 {
    unsafe asm {
        "mrs x0, s3_4_c15_c0_0"
        "ret"
    }
}

// Tier 3: cinclude (external — unverified, UNSAFE-EXTERN warning)
cinclude "legacy_bootstrap.S"
extern fn ancient_boot_quirk() -> void;
```

### Phase 1 verified rule still applies

`zer_asm_allowed_in_context(in_naked)` (Phase 1 predicate, VST-verified in `src/safety/context_bans.c`) still enforces:
- `unsafe asm` only allowed in `naked` functions
- `naked` function body only `unsafe asm` + `return`

**No Phase 1 code changes needed.** The predicate checks structural context, not the keyword spelling. Rename from `asm` → `unsafe asm` is a parser-level change; verified rule is unchanged.

### Migration policy

When an intrinsic is added for an operation, bare `unsafe asm` code using that operation gets a deprecation warning:

```
warning: `unsafe asm` uses rdtsc; prefer @cpu_read_cycles() intrinsic
  --> src/scheduler.zer:42:5
```

This signals the preferred path without breaking existing code.

### Why NOT remove `asm` entirely

| Rationale | Detail |
|---|---|
| Intrinsics lag CPU features | New vendor extensions ship before intrinsics. Users need escape hatch. |
| Ergonomics for rare one-offs | Inline `unsafe asm { "int3" }` beats making a .S file for debug breakpoint |
| Backward compatibility | Existing ZER code from 2026-04-01 design uses `asm` |
| Honest safety claim | "Intrinsics cover 100% of target domain" is stronger than "no asm exists" |
| Matches Rust/Zig/C++ pattern | All systems languages have this. Removing puts ZER behind, not ahead. |

The target-domain coverage claim is unchanged: intrinsics cover 100% of kernels/RTOS/firmware. `unsafe asm` handles edge cases outside target domain, just like Rust's `unsafe` blocks handle things outside safe Rust.

---

## The 10 Language Rules (replaces per-asm verification)

These rules are enforced by the compiler. They are the "100% coverage" mechanism.

| # | Rule | Enforcement |
|---|---|---|
| 1 | `unsafe` keyword is required before `asm`; bare `asm(...)` is a compile error | Parser rejects bare `asm` with message "use 'unsafe asm(...)' as explicit escape hatch marker" |
| 2 | `naked` + `unsafe asm` only allowed together; naked body = `unsafe asm(...)` + return only | Phase 1 verified (`zer_asm_allowed_in_context`) |
| 3 | All low-level operations preferred through `@intrinsic()` calls | Future: deprecation warning when `unsafe asm` has an intrinsic equivalent |
| 4 | `cinclude "foo.S"` allowed; every call site emits `[UNSAFE-EXTERN]` warning | Emitter + compile-time warning |
| 5 | Intrinsics are target-gated (e.g., `@cpu_msr_read` only on x86) | Checker rejects cross-target use |
| 6 | Privileged intrinsics require `privileged fn` context | Scope-based rule |
| 7 | Atomic intrinsics require pointer-to-shared-struct args | Type checker |
| 8 | Context switch intrinsics require verified `*State` struct | Type checker |
| 9 | Memory barriers require explicit ordering (`acquire`/`release`/`seq_cst`) | Parser enum |
| 10 | MMU intrinsics require verified `*PageTable` struct; no raw addresses | Type checker |

---

## Trust Base (what remains trusted even at 5K hrs complete)

| Trusted | What could go wrong | Mitigation |
|---|---|---|
| Coq kernel | Rare bugs | Cross-check critical theorems with Lean/Isabelle |
| Sail ISA model | Manual errors from CPU spec | Hardware validation in Phase B (run test vectors) |
| GCC inline asm assembler (GAS) | Encoding bugs | 40-year-old, billions of users, regression tests |
| Hardware ISA spec | Intel/ARM/RISC-V manuals may have errata | Submit errata when found; Sail model tracks spec versions |

Trust base: Coq kernel + Sail + GCC. Same as seL4 and CompCert. What happens when the CPU executes verified software is the silicon vendor's concern — not a language-level claim.

---

## Existing Verified Systems — Comparison

| System | Scope | Trust base | Effort |
|---|---|---|---|
| CompCert | C → asm compiler (user mode only) | Coq + hardware | ~15 person-years |
| seL4 | Microkernel (C), trusts compiler | Isabelle + GCC + hardware | ~25 person-years |
| Vale | x86-64 crypto asm, user mode only | F* + Z3 + hardware | ~10 person-years |
| **ZER (this plan)** | **Full compiler + verified asm on 4 archs, user + kernel mode, target domain 100%** | **Coq + Sail + GCC + hardware** | **~2.5 person-years (with 7 shortcuts)** |

ZER's scope is broader than any individual precedent. The effort is tractable because:
1. Reusing Sail (saves ~2 person-years of ISA work)
2. Reusing Islaris (saves ~1 person-year of Iris+Sail integration)
3. LLM-assisted development (5-10x velocity for C work)
4. Scoped to safety-only, not full semantic preservation (unlike CompCert)

---

## What ZER Does NOT Cover (explicit scope)

| Domain | Why out of scope | Alternative |
|---|---|---|
| JIT runtimes (V8, JVM, LuaJIT, CLR) | Requires runtime code generation; blocked by type system | Use C++/Rust with JIT crates |
| Hot-patching frameworks (kpatch) | Modifies running code | Reboot with new kernel |
| Exploit development / red team | Needs arbitrary asm | Custom assembler projects |
| Extreme HFT (nanosecond-optimized) | Hand-tuned beyond compiler quality | C++ with hand-written asm |
| Research on novel ISAs (no formal semantics) | No Coq model to verify against | Wait for Sail model |
| Self-decrypting binaries / obfuscation | Self-modifying by design | Not a legitimate use case |

**Critical note:** Runtime code generation (JIT, live-patch, self-modify) is NOT an asm problem. It's a general type-safety problem. ZER's existing type system already prevents casting `*u8` to function pointer. No special asm rules needed for this.

---

## The Final Claim ZER Can Make

After 3,750 hrs complete (Phase 0 prereq + Phase A-E, 3-arch scope):

> **"ZER covers 100% of its target market (memory-safe OS kernels, RTOSes, embedded firmware, drivers, crypto libraries, hypervisors, networking stacks, and any ahead-of-time-compiled systems code) through:
>
> - ~96 formally verified intrinsics per architecture (x86-64, ARM64, RISC-V)
> - User-written asm REQUIRES explicit `unsafe asm` marker and is restricted to `naked` functions (Phase 1 verified); bare `asm(...)` is rejected at compile time. Users prefer intrinsics for safety.
> - GCC as the trusted assembler (standard trust base used by Linux, BSD, etc.)
> - Type-system-level prevention of runtime code generation
>
> Domains that fundamentally require runtime code generation (JIT compilers, live patching, self-modifying code) are outside ZER's scope. Users who need them can link external C code via `cinclude` with explicit `UNSAFE-EXTERN` warnings at call sites.
>
> No other memory-safe systems language ships this degree of verified asm coverage."**

**Escape hatch trust tiers:**
1. `@intrinsic()` — verified, safe (preferred path)
2. `unsafe asm` — explicitly marked, Phase 1 structural verification (escape for edge cases)
3. `cinclude "foo.S"` — external, unverified (`UNSAFE-EXTERN` warning at every call)

---

## Runtime Code Generation — The Non-Gap Clarification

**This was a source of confusion in the discussion. Clarified:**

JIT compilers and live patching do NOT need assembly language features. They need:
1. Allocating executable memory (`mmap` with `PROT_EXEC`)
2. Writing bytes into that memory (ordinary `memcpy`)
3. Casting data pointer to function pointer (type-unsafe cast)
4. Calling the function pointer

Step 3 is the unsafe part. It's a data-vs-code type confusion, not an asm feature.

**ZER blocks step 3 via existing type system:**
```zer
*u8 buffer = ...;
fn() -> i32 jit_func = (fn() -> i32)buffer;       // COMPILE ERROR
jit_func = @cast(fn() -> i32, buffer);             // COMPILE ERROR
jit_func = @ptrcast(fn() -> i32, buffer);          // COMPILE ERROR
jit_func = @inttoptr(fn() -> i32, addr);           // COMPILE ERROR (mmio-only)
```

No path from `*u8` to function pointer in ZER. Period.

**Therefore:**
- Asm coverage = 100% (via intrinsics)
- JIT/live-patch prevention = 100% (via type system, independently)
- These are orthogonal concerns

Projects that NEED runtime codegen (JIT compilers, live patchers):
- Use `cinclude` for that specific subsystem (same pattern as Linux + Vale)
- Rest of the codebase stays 100% verified ZER
- Trust boundary is explicit at `UNSAFE-EXTERN` warning

---

## Prior Art to Port / Reuse

| Source | Provides | Arch | Port cost |
|---|---|---|---|
| Sail ISA models (Cambridge) | Full ISA formalizations | x86-64, ARMv8-A, ARMv7, RV32/RV64, CHERI | Low (Sail → Coq extraction exists) |
| Islaris | Iris Hoare logic over Sail | ARM64, RV | Low (already Iris) |
| seL4 proofs (Isabelle/HOL) | Context switch, MMU, interrupts for ARM64 + x86-64 | ARM64, x86-64 | Medium (Isabelle → Coq port) |
| CompCert | Memory model, ABI, register allocation | x86, ARM, PowerPC, RISC-V | Low (already Coq) |
| Vale | x86-64 crypto + some system ops | x86-64 | Medium (F* → Coq port) |
| HACL* | Verified crypto math primitives | All | Medium (F* → Coq port) |
| RISC-V formal | ISA semantics, privileged mode | RV | Low (Coq-native) |
| Kami (MIT) | Hardware-level RISC-V verification | RV | Optional, for hardware trust |

Estimated savings from prior-art reuse: **400-600 hrs** over the 4-arch project.

---

## Risks and Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Sail Coq backend bugs | +200-400 hrs | Pin Sail commit; contribute upstream fixes; CI gate |
| ISA manual errors (rare but real) | Spec wrong → proofs wrong | Validate against real hardware in Phase B |
| Iris learning curve | Slow early work | Front-load Iris investment in Phase 7 (prereq) |
| Proof maintenance as ISA evolves | Ongoing cost | Version the ISA model; treat like any dep |
| Scope creep (SIMD-500 intrinsics, FP, exotic archs) | Blows budget | Phase E explicitly caps; reject non-planned |
| Losing 1 engineer mid-project | Stalls progress | Document thoroughly; pair-review critical proofs |
| Coq breaking change | Weeks of churn | Pin Coq version; upgrade deliberately |
| Upstream Sail moves faster than ZER can adopt | Fork drift | Maintain ZER-specific fork; sync periodically |

---

## Linux-Scale Expansion (2026-04-23 late evening)

**User goal restated:** ship at Linux-kernel scale (~130 intrinsics covering what Linux arch/ uses). Vendor-specific features (SGX, TDX, TrustZone, MTE, PAC, future extensions) handled via hardened `unsafe asm` — not dedicated intrinsics.

### From 96 → 130 intrinsics

Earlier sections of this doc describe the "96 intrinsics" plan. That was the MVP scope. The Linux-scale plan expands to **130** by adding:

| Group | Added | What it unlocks |
|---|---|---|
| Critical multi-core (8) | pause, cpu_id, wfe, sev, breakpoint, rdrand, rdseed, barrier_dma | Spinlocks, per-CPU data, ARM event signaling, hardware RNG |
| Nice-to-have (6) | time_ns, cache_zero_line, get_pc, wait_on_addr, supports_feature, flush_pipeline | Monotonic time, cache ops, CPU feature detection |
| Linux-scale x86 (20) | fsbase/gsbase R/W (4), port I/O (6), xsave/xrstor (2), dr R/W (2), syscall entry/return (2), sbi_call, smc_call, hvc_call, clflushopt/clwb (2), nt_store | Thread-local storage, legacy devices, AVX-512 FPU, hardware breakpoints, syscall optimization, ARM/RISC-V platform firmware calls |
| **Total additions** | **34** | Linux-scale kernel parity |

New total surface: **96 + 34 = 130 intrinsics.**

### Out-of-scope (explicit, handled via hardened `unsafe asm`)

These are intentionally NOT dedicated intrinsics:

| Category | Why excluded | How users access |
|---|---|---|
| Hypervisor (VMX/SVM/ARM-virt, ~15-20 ops) | Distinct subsystem; each instruction has complex state dependencies | Hardened `unsafe asm` or separate D-Beta-hypervisor batch |
| Intel SGX / TDX / AMD SEV (~10-15 each) | Enclave-specific, rare, rapidly evolving | Hardened `unsafe asm` (typed operands + clobber tracking) |
| ARM TrustZone SMC details | Vendor-specific ABIs | `@cpu_smc_call` intrinsic + hardened `unsafe asm` for unusual ABIs |
| Debug registers (DR0-DR7, DBGBCR) beyond basic R/W | Per-model configuration | Hardened `unsafe asm` |
| Performance counters (PMC/PMU) | Model-specific identifiers | OS perf subsystem via MSR reads |
| 128-bit atomic CAS (cmpxchg16b) | Rarely needed | Two 64-bit CAS + retry loop |
| FPU control/status registers | Niche for numeric kernels | Hardened `unsafe asm` |
| Cortex-M specific (MPU, NVIC) | Post-v1.0 per scope plan | Add in v1.1 |
| SIMD / vector operations | Compiler-generated via GCC vector types | Use `gcc_extension_ok` equivalent, separate from intrinsics |

Rationale: dedicated intrinsics for every vendor feature = long tail that never converges. Hardened `unsafe asm` handles this elegantly with Rust-tier safety (typed operands, explicit clobbers, mandatory docs).

### Hardened `unsafe asm` (the escape hatch)

**Current state (from earlier section):** `unsafe asm` is a raw pass-through to GCC inline asm. No operand typing, no clobber validation, restricted to `naked` functions, mandatory `unsafe` keyword per Phase 1 rule.

**Target state (D-Alpha-7.5 — NEW BATCH):** upgrade to Rust-tier typed escape hatch.

**Seven hardening features (H1-H7):**

| # | Feature | Example | Value |
|---|---|---|---|
| H1 | Structured operand syntax (inputs/outputs/clobbers blocks) | See below | Compiler knows what's read/written |
| H2 | Typed operands (ZER type at each register binding) | `"rax" = u64` | Boundary type safety |
| H3 | Required `safety:` string literal | `safety: "TDX call per Intel §3.2.1"` | Forces documentation |
| H4 | Explicit clobber list with validation | `clobbers: ["rbx", "memory"]` | Register names validated per arch |
| H5 | Allowed inside `unsafe fn` (not just naked) | `unsafe fn foo() { unsafe asm { ... } }` | Opens non-naked contexts |
| H6 | `@asm_register_valid(arch, name)` checker predicate | Phase 1 verifiable | "rbx is valid x86-64 register" |
| H7 | Audit emission: compiler writes `build/asm_audit.log` | `grep "unsafe asm" src/` + audit log | Full auditability |

**Example — Intel TDX guest call (currently impossible to write safely):**

Current (raw, unsafe):
```zer
naked fn tdg_vp_info() {
    unsafe asm("movq $1, %rax; tdcall");
    /* How does caller get RAX return? Can't safely. */
}
```

After H1-H7 (Rust-tier safe):
```zer
fn tdg_vp_info(u64 subfunc) -> u64 {
    u64 result;
    unsafe asm {
        instructions: "tdcall"
        inputs: { "rax" = 1, "rcx" = subfunc }
        outputs: { "rax" = result }
        clobbers: ["rdx", "r8", "r9", "r10", "memory"]
        safety: "Intel TDX vp_info per TDX Module Spec v1.0 §7.2.2"
    };
    return result;
}
```

Compiler checks:
- `"rax"` is a valid x86-64 register (per arch register table)
- `subfunc` is u64 (matches rcx binding width)
- `result` is mutable lvalue (valid output target)
- Memory clobber present → compiler won't reorder loads/stores around
- Safety comment present → audit log entry emitted

This unlocks **all modern CPU features** (TDX, SGX, MTE, PAC, CET, CHERI, future extensions) with real safety without needing a dedicated intrinsic per feature.

### Three tiers of "safe asm"

| Tier | Coverage of asm bug classes | How achieved | Achievable? |
|---|---|---|---|
| **Rust-tier** (H1-H7 baseline) | ~60% (typed operands + explicit clobbers + audit log) | D-Alpha-7.5 Phase 1 | YES (+120 hrs) |
| **Structural 100%** (strict mode 18 rules) | 100% of structural bug classes (~95% of typical instances) | D-Alpha-7.5 Phase 2 (below) | YES (+240 hrs beyond H1-H7) |
| **100% safe** (structural + formal verification) | **100%** — every covered block proven correct | Full Vale-tier (D-Beta) | YES (+1,500-2,500 hrs) |

**"100% safe asm"** means: every intrinsic is formally verified, every `unsafe asm` block with `@verified_spec` is proven correct. That's the ceiling ZER reaches — same tier as seL4 / CompCert / Vale. CPU hardware execution is outside the language's scope (silicon vendor's domain).

### Strict mode: 18 rules that close the structural gap

After H1-H7 baseline, optional `--strict-asm` flag adds 18 rules. **Within structural bug classes, strict mode is 100% effective — every instance is a compile error.** Across ALL asm bug types, this covers ~95% of typical bug instances; the remaining ~5% are semantic bugs (wrong algorithm, off-by-one, wrong condition code) that Vale-tier addresses. No runtime overhead; all compile-time.

**Two dimensions of the claim (important distinction):**

| Dimension | Claim |
|---|---|
| Within the structural bug classes (missing clobbers, wrong arch, register typos, ABI violations, etc.) | **100% catch rate** — rules are deterministic compile errors |
| Across ALL possible asm bugs (structural + semantic) | **~95% caught** — the 5% are semantic bugs requiring Vale-tier |

This is NOT "95% safe with 5% random failures." It's "100% safe for the bug classes covered, 0% safe for semantic bugs, and the mix happens to be ~95/5 in typical code."

**Structural rules (5):**

| # | Rule | Rejects |
|---|---|---|
| S1 | `unsafe asm` only in `unsafe fn` or `naked fn` | Regular functions with asm |
| S2 | Max 16 instructions per `unsafe asm` block | Bloated unauditable blocks |
| S3 | No labels, jumps, calls, ret inside asm | Control flow escape |
| S4 | Mandatory `safety:` comment, min 30 chars | Undocumented escape hatches |
| S5 | Mandatory `@arch_guard(...)` or `target:` attribute | Wrong-arch asm |

**Operand rules (5):**

| # | Rule | Rejects |
|---|---|---|
| O1 | Every input typed (ZER type → register binding width match) | Bitcasting via raw asm |
| O2 | Every output typed (ZER lvalue binding) | Writing to garbage register |
| O3 | Register names validated against per-arch whitelist | Typos like `%rax0` |
| O4 | Memory operand must reference declared ZER memory | Writing to arbitrary addresses |
| O5 | `volatile` memory access requires explicit attribute | Missed compiler-reorder issues |

**Instruction rules (4):**

| # | Rule | Rejects |
|---|---|---|
| I1 | Instruction whitelist per context (interrupts batch only allows cli/sti/hlt, etc.) | Random privileged ops in wrong context |
| I2 | Privileged instructions require `privileged fn` context | User code running kernel instructions |
| I3 | SIMD/FPU requires `@fpu_required` attribute | Unexpected FPU state corruption |
| I4 | Constant-time crypto requires `@constant_time` attribute + checker validation | Timing side channels in crypto |

**Side-effect rules (4):**

| # | Rule | Rejects |
|---|---|---|
| E1 | Clobber list must include ALL modified registers (checker re-parses instruction set) | "Forgot to clobber rcx" bugs |
| E2 | Memory clobber required if ANY memory op | Compiler reorder bugs |
| E3 | Flag clobber ("cc") required if ANY flag-modifying instruction | Missed flag preservation |
| E4 | Callee-saved registers preserved (can't clobber without explicit push/pop) | ABI violations |

**Total: 18 rules.** All mechanically checkable at compile time.

### Example: strict mode catches real Linux kernel bug class

```zer
fn compute_crc(u64 data) -> u64 {
    u64 result;
    unsafe asm {
        instructions: "crc32q %1, %0"   /* modifies rcx internally */
        inputs: { "1" = data }
        outputs: { "0" = result }
        clobbers: []                     /* BUG: forgot "rcx" */
        safety: "CRC32 instruction per Intel SDM Vol 2A"
    };
    return result;
}
```

Without strict mode: compiles. At runtime, caller sees garbage in rcx.
With rule E1: compiler re-parses `crc32q`, knows it clobbers rcx → compile error with helpful diagnostic.

### What strict mode CAN'T catch (the last 4.5% — semantic correctness)

| Can't catch | Why | Workaround |
|---|---|---|
| Wrong algorithm (crypto formula error) | Semantic | D-Beta formal verification |
| Off-by-one in loop-like asm | Semantic | Tests + D-Beta proofs |
| Wrong condition code (`jz` vs `jnz`) | Semantic | Tests + careful review + D-Beta |
| Undefined behavior within instruction (e.g., BSR on zero) | Per-instruction semantic | Preconditions in `@instruction_preconditions` attribute |

For these semantic bugs, use Tier B (selective Vale-tier) or Tier C (full verification).

### Tiered plan: from 95% → 99.5% → 100%

Three-tier shipping plan to reach 100% safe asm incrementally:

| Tier | Version | Coverage | Approach | Budget impact |
|---|---|---|---|---|
| **A** | **v1.0** | ~95% | H1-H7 baseline + strict mode (18 rules, opt-in via `--strict-asm`) | +240 hrs above H1-H7 (total ~360 hrs) |
| **B** | **v1.0.1** (3-6 mo after v1.0) | ~99% | +Selective Vale-tier on 20 critical intrinsics (context switch, MMU, atomics) using Iris Hoare logic over Sail | +400 hrs (reallocated from v1.1 Cortex-M fund) |
| **C** | **v1.1+** (12+ mo after v1.0) | **100%** | +Full Vale-tier for covered subset. Every unsafe asm block with `@verified_spec` proven correct | +800-1,000 hrs (uses D-Beta budget) |

### Example: `@verified_spec` for Tier C (v1.1+ feature)

```zer
@verified_spec {
    requires: len > 0 && len <= 4096 && len % 8 == 0
    ensures: forall i in 0..len: buf[i] == 0
}
unsafe asm {
    instructions:
        "mov $0, %%rax\n\t"
        "rep stosq"
    inputs: { "rdi" = buf, "rcx" = len / 8 }
    clobbers: ["rdi", "rcx", "rax", "memory"]
    safety: "Zero-fill aligned buffer via REP STOSQ"
}
```

Compiler (Tier C mode) dispatches the `@verified_spec` to Coq/Iris:
- Prove: given preconditions, the asm produces state matching postconditions
- Reject compilation if proof fails
- Allow compilation if proof succeeds

This reaches **100% safe** for covered blocks.

### Updated claims per tier

**After Tier A (v1.0):**

> "ZER's `unsafe asm` enforces 18 compile-time rules (MISRA Directive 4.3 + SPARK Ada Machine_Code level). 95% of asm bugs are structurally impossible. Vendor-specific code (TDX, SGX, MTE, PAC, CET) handled via same hardened escape hatch. Semantic correctness relies on tests."

**After Tier B (v1.0.1):**

> "ZER's critical asm intrinsics (context switch, MMU, atomics, barriers) are formally verified in Iris/Coq over Sail ISA models. Non-critical `unsafe asm` enforces 18 structural rules. 99% of asm bugs are mechanically prevented. Remaining 1% is semantic correctness in uncommon code paths."

**After Tier C (v1.1+):**

> "ZER achieves **100% safe asm** for OS/kernel/firmware primitives. Every covered intrinsic has a mechanized proof of correctness. Every `unsafe asm` block with `@verified_spec` has a proof that it satisfies the declared pre/postconditions. External unverified code is explicitly marked `UNSAFE-EXTERN` via `cinclude`."

### Out of scope (not ZER's concern)

| Out of scope | Whose concern instead |
|---|---|
| CPU hardware behavior (instruction execution semantics beyond the ISA spec) | Silicon vendor (Intel/AMD/ARM/RISC-V designs) |
| `cinclude` external .S files | User — explicit `UNSAFE-EXTERN` marker |
| Vendor extensions without formal semantics (new ISA features) | User via `unsafe asm` + `@verified_spec` once Sail model exists |

ZER's safety claim is about what ZER emits. What the CPU does when executing ZER's proven-correct instructions is the silicon vendor's problem, the same way it is for every compiler and every language.

### Revised batch roadmap (14 batches for 130 intrinsics)

Progress: 44 of 130 shipped (34%).

| Batch | Count | Scope | Status |
|---|---|---|---|
| D-Alpha-1 | 15 | Atomics | **DONE** (2e152f3) |
| D-Alpha-2 | 10 | Barriers + bit queries | **DONE** (a14d3c0) |
| D-Alpha-3 | 5 | Interrupt control | **DONE** (3e6e2f2) |
| D-Alpha-4 | 4 | Context switch | **DONE** (e2f45af) |
| D-Alpha-5 | 10 | MMU control | **DONE** (94f958b) |
| D-Alpha-6 | 11 | TLB + cache (incl. cache_zero_line) | Pending |
| D-Alpha-7 | 8 | Critical multi-core (pause, cpu_id, wfe, sev, breakpoint, rdrand, rdseed, barrier_dma) | Pending |
| **D-Alpha-7.5 Phase 1** | **—** | **Hardened `unsafe asm` H1-H7 (Rust-tier baseline)** | **Pending — NEW** |
| **D-Alpha-7.5 Phase 2** | **—** | **Strict mode 18 rules (S1-E4) → 95% structural safety, opt-in via `--strict-asm`** | **Pending — NEW** |
| D-Alpha-8 | 6 | Nice-to-have (time_ns, get_pc, wait_on_addr, supports_feature, flush_pipeline, cpu_yield) | Pending |
| D-Alpha-9 | 10 | MSR/CSR access (includes per-arch R/W pair) | Pending |
| D-Alpha-10 | 10 | Inspection + cycle counter | Pending |
| D-Alpha-11 | 5 | Power management | Pending |
| D-Alpha-12 | 6 | Privileged transitions (syscall entry/return, mode switch) | Pending |
| D-Alpha-13 | 20 | Linux-scale x86 essentials (fsbase/gsbase, port I/O, xsave, dr regs, clflushopt/clwb, nt_store, sbi_call, smc_call, hvc_call) | Pending |
| D-Alpha-14 | 10 | Misc (remaining helpers, final polish) | Pending |
| **Total** | **130** | | **44 / 130 (34%)** |

**v1.1 additions (post-v1.0):**
- D-Alpha-15: 6 security intrinsics (PAC, MTE, CET) → 136
- D-Alpha-16: 10 Cortex-M (MPU, NVIC) → 146
- D-Alpha-Hyp: ~20 hypervisor (VMX/SVM/ARM-virt) → ~166

### Effort impact on 5K budget (tiered plan)

| Phase | v1.0 (Tier A) | v1.0.1 (+Tier B) | v1.1+ (+Tier C) |
|---|---|---|---|
| Phase 0: Prereq (Phases 2-7) | 900 | 900 | 900 |
| Phase A: Core infra | 500 | 500 | 500 |
| Phase B: ISA coverage | 400 | 400 | 400 |
| Phase C: Advanced semantics | 400 | 400 | 400 |
| Phase D-Alpha: 130 intrinsics | 1,700 | 1,700 | 1,700 |
| D-Alpha-7.5 Phase 1 (Rust-tier) | 120 | 120 | 120 |
| **D-Alpha-7.5 Phase 2 (strict mode, 18 rules)** | **+240** | +240 | +240 |
| Selective Vale-tier (20 critical intrinsics) | — | **+400** | +400 |
| Full Vale-tier (expand coverage) | — | — | **+800-1,000** |
| Phase D-Beta: Formal proofs (remaining) | 1,500 | 1,500 | 1,500 |
| Phase E: Polish + cert | 200 | 200 | 200 |
| **Committed** | **4,960** | 5,360 | 6,160-6,360 |
| Surplus/overflow (from 5K) | **40** | **-360** | **-1,160** |

**Tier A (v1.0) fits in 5K with 40 hr surplus** — tight but achievable. Strict mode included in v1.0.

**Tier B exceeds 5K by 360 hrs** — requires reallocation from Cortex-M fund OR extended budget. If Cortex-M stays at v1.1 scope, Tier B fits within original 5K.

**Tier C exceeds 5K by ~1,100 hrs** — genuinely needs second budget cycle. Deferred to v1.1+ explicitly.

**Practical approach:** ship Tier A in v1.0 using the 5K budget. Tier B requested when funding allows or v1.0 finishes ahead of schedule. Tier C is long-term research.

### Version-specific claims (tiered)

**v1.0 (Tier A — 95% structural safety):**

> "ZER ships 130 intrinsics per arch (x86-64, ARM64, RISC-V) covering Linux kernel arch/ needs. `unsafe asm` enforces 18 compile-time rules (MISRA Directive 4.3 + SPARK Ada Machine_Code level) — 95% of asm bugs are structurally impossible. Vendor-specific code (SGX, TDX, TrustZone, MTE, PAC, CHERI) handled via same hardened escape hatch."

**v1.0.1 (Tier B — 99% verified critical paths):**

> "ZER's critical asm intrinsics (context switch, MMU, atomics, barriers) are formally verified in Iris/Coq over Sail ISA models. Non-critical `unsafe asm` enforces 18 structural rules. 99% of asm bugs are mechanically prevented."

**v1.1+ (Tier C — 100% safe asm):**

> "ZER achieves **100% safe asm**. Every covered intrinsic has a mechanized proof of correctness. Every `unsafe asm` block with `@verified_spec` is verified at compile time. External code uses `cinclude` with explicit `UNSAFE-EXTERN` markers."

Strongest claim of any memory-safe systems language targeting Linux-scale work.

---

## Implementation-First Plan (Phase D-Alpha) — REVISED 2026-04-23 evening

**Previous recommendation (verify-first) was overridden.** Implementation of intrinsics is decoupled from Phase 2-7 prereq. Working intrinsics ship first; formal proofs added later (Phase D-Beta).

### Why implementation-first

1. **Matches real verified systems.** seL4 kernel existed → then verified. CompCert implemented → proofs refined. Vale primitives first → verification layer added.
2. **Ships value ~2 years earlier.** Users can write pure-ZER kernels on x86-64 after ~500-800 hrs instead of waiting for ~1,900 hrs (prereq + Phase A-D).
3. **No observable behavior change across Phase 2-7.** Phase 2-7 refactors internals but doesn't change type system behavior. Intrinsics built now don't need rework.
4. **Iteration on API design.** Real kernel code catches intrinsic design mistakes faster than proofs do.

### Phase D splits into three sub-phases

| Sub-phase | What | Hours (x86-64) | Formal proofs? |
|---|---|---|---|
| **D-Alpha** | Implement 96 intrinsics with tests | 500-800 | No — tests validate behavior |
| **D-Beta** | Add Coq specs + VST proofs on top | 400-600 | Yes — Phase 7 infra required |
| **D-Gamma** | Port to ARM64 + RISC-V | 600-800 | Yes — inherits D-Beta proof structure |

Total still ~2,000-2,400 hrs — but value ships progressively as each batch lands.

### Phase D-Alpha — 7 Batches for x86-64

Each batch is independently shippable. Users benefit after each lands.

| Batch | Intrinsics | Count | Hours | Unlocks |
|---|---|---|---|---|
| D-Alpha-1 | Atomics (GCC builtins) | 15 | 50-80 | Lock-free data structures in pure ZER |
| D-Alpha-2 | Barriers + misc GCC builtins | 10 | 30-50 | Memory ordering, prefetch, bit manip |
| D-Alpha-3 | Interrupt control | 8 | 80-120 | Can write ISRs in pure ZER |
| D-Alpha-4 | Context switch | 4 | 100-150 | Can write a scheduler |
| D-Alpha-5 | MMU control | 10 | 150-200 | Can write virtual memory manager |
| D-Alpha-6 | TLB + cache maintenance | 11 | 80-120 | Complete memory subsystem |
| D-Alpha-7 | MSR + inspection + power mgmt | 38 | 80-100 | Full kernel capability |
| **TOTAL** | | **96** | **570-820** | **Pure-ZER kernels on x86-64** |

### Per-batch deliverables (what each batch includes)

For every intrinsic in a batch:

1. **Parser support** — `@cpu_save_context`, `@atomic_cas`, etc. recognized as builtin calls
2. **Checker rules** — type validation, context validation (uses existing Phase 1 rules like `zer_asm_allowed_in_context`)
3. **Emitter output** — emits correct GCC inline asm or GCC builtin per `--target`
4. **Integration tests** — real ZER code that uses the intrinsic, compiles, runs, produces expected behavior
5. **Regression tests** — added to `tests/zer/` for positive cases, `tests/zer_fail/` for negative cases
6. **Documentation** — `docs/reference.md` entry with syntax + example

NOT included in D-Alpha (deferred to D-Beta):
- Coq formal specification
- VST verification proof
- Iris Hoare logic integration
- Sail model integration

### Testing strategy without formal proofs

Each intrinsic gets multi-layer validation:

| Layer | What | Effort |
|---|---|---|
| **Unit tests** | Parser/checker/emitter test fixtures | ~10 lines per intrinsic |
| **Integration tests** | Real ZER code exercising the intrinsic | ~20 lines per intrinsic |
| **Differential testing** | Compare against hand-written C equivalent | Where applicable |
| **Hardware validation** | Run on real CPU, compare output | CI — QEMU at minimum |
| **Negative tests** | Misuse cases rejected at compile time | 5-10 per batch |

This gives ~95% confidence in correctness without formal proofs. The remaining ~5% is captured by D-Beta proofs later.

### D-Alpha-1 detail: Atomics (first batch)

**15 intrinsics, ~50-80 hours, highest-priority first batch.**

All map to GCC `__atomic_*` builtins — GCC handles per-arch encoding automatically. ZER compiler just needs to recognize the syntax and emit the builtin.

| Intrinsic | GCC builtin emitted | ZER type signature |
|---|---|---|
| `@atomic_load(*T, ordering)` | `__atomic_load_n(ptr, ordering)` | `fn(*shared T, Ordering) -> T` |
| `@atomic_store(*T, T, ordering)` | `__atomic_store_n(ptr, val, ordering)` | `fn(*shared T, T, Ordering) -> void` |
| `@atomic_cas(*T, T, T, ord, ord)` | `__atomic_compare_exchange_n(...)` | `fn(*shared T, T, T, Ordering, Ordering) -> bool` |
| `@atomic_xchg(*T, T, ordering)` | `__atomic_exchange_n(ptr, val, ord)` | `fn(*shared T, T, Ordering) -> T` |
| `@atomic_fetch_add(*T, T, ord)` | `__atomic_fetch_add` | `fn(*shared T, T, Ordering) -> T` |
| `@atomic_fetch_sub(*T, T, ord)` | `__atomic_fetch_sub` | Same |
| `@atomic_fetch_or(*T, T, ord)` | `__atomic_fetch_or` | Same |
| `@atomic_fetch_and(*T, T, ord)` | `__atomic_fetch_and` | Same |
| `@atomic_fetch_xor(*T, T, ord)` | `__atomic_fetch_xor` | Same |
| `@atomic_fetch_nand(*T, T, ord)` | `__atomic_fetch_nand` | Same |
| `@atomic_add_fetch(*T, T, ord)` | `__atomic_add_fetch` | Same |
| `@atomic_sub_fetch(*T, T, ord)` | `__atomic_sub_fetch` | Same |
| `@atomic_or_fetch(*T, T, ord)` | `__atomic_or_fetch` | Same |
| `@atomic_and_fetch(*T, T, ord)` | `__atomic_and_fetch` | Same |
| `@atomic_xor_fetch(*T, T, ord)` | `__atomic_xor_fetch` | Same |

**Ordering enum (5 values):**
```zer
enum Ordering {
    .relaxed,        // → __ATOMIC_RELAXED
    .acquire,        // → __ATOMIC_ACQUIRE
    .release,        // → __ATOMIC_RELEASE
    .acq_rel,        // → __ATOMIC_ACQ_REL
    .seq_cst,        // → __ATOMIC_SEQ_CST
}
```

**Restrictions enforced by checker:**
- First arg must be `*shared T` pointer (non-shared pointers rejected — existing BUG fix pattern)
- `T` must be integer type of width 1/2/4/8 bytes
- Ordering argument must be a compile-time constant (Ordering enum value)

**Example user code after D-Alpha-1:**
```zer
shared struct Counter {
    u32 value;
}

Counter global_counter;

fn safe_increment() -> u32 {
    // Returns old value. Seq-cst for simplicity.
    return @atomic_fetch_add(&global_counter.value, 1, .seq_cst);
}

fn compare_and_swap(u32 expected, u32 new) -> bool {
    return @atomic_cas(&global_counter.value, expected, new, .seq_cst, .seq_cst);
}
```

**Testing for this batch:**
- `tests/zer/atomic_fetch_add.zer` — increment counter, verify exit code
- `tests/zer/atomic_cas_retry.zer` — CAS retry loop
- `tests/zer/atomic_multi_thread.zer` — spawn + atomic_fetch_add, verify total
- `tests/zer_fail/atomic_nonshared.zer` — non-shared pointer rejected
- `tests/zer_fail/atomic_wrong_width.zer` — 3-byte type rejected
- `tests/zer_fail/atomic_nonconst_ordering.zer` — runtime ordering rejected

### Phase D-Alpha — Batch Dependencies

```
D-Alpha-1 (Atomics) ─┬─→ D-Alpha-3 (Interrupts) ──→ D-Alpha-4 (Context Switch)
                     │                                        │
                     │                                        ↓
                     └─→ D-Alpha-2 (Barriers) ←──────────── D-Alpha-5 (MMU)
                                                             │
                                                             ↓
                                                         D-Alpha-6 (TLB + Cache)
                                                             │
                                                             ↓
                                                         D-Alpha-7 (MSR + rest)
```

Batches 1 and 2 can be done in parallel or either order. After that, linear dependency.

### Integration with existing ZER compiler

Each batch touches these files:

| File | Changes |
|---|---|
| `parser.c` | Add `@intrinsic_name` recognition (~5 lines per intrinsic) |
| `checker.c` | Type/context validation (~10-30 lines per intrinsic) |
| `emitter.c` | GCC asm / builtin emission (~10-30 lines per intrinsic) |
| `ir_lower.c` | Lower to IR instruction (if using IR path) |
| `ast.h` | New NODE_INTRINSIC_* kind or generic NODE_INTRINSIC with tag |
| `tests/zer/*.zer` | Positive tests (~20 lines each) |
| `tests/zer_fail/*.zer` | Negative tests (~10 lines each) |
| `docs/reference.md` | User-facing documentation |
| `BUGS-FIXED.md` | Session entry per batch |

Estimated LoC per batch: ~1,500-3,000 depending on batch complexity.

### Phase D-Beta — verification layer (LATER)

After Phase 7 completes (~12-18 months), add formal proofs to the intrinsics:

1. Write Coq specs (arch-agnostic) — ~7,700 lines total
2. Write VST proofs per arch — ~180 proofs total
3. Integrate Sail model references where applicable
4. Link into `make check-vst` CI gate

**No intrinsic code changes needed.** Only adds `src/safety/intrinsics/*.c` + `proofs/vst/verif_intrinsics/*.v`.

### Phase D-Gamma — multi-arch port (LATER)

After D-Alpha x86-64 complete:

1. Port 56 arch-specific intrinsics to ARM64 (~500 hrs)
2. Port 56 arch-specific intrinsics to RISC-V (~400 hrs)
3. The 40 GCC-builtin intrinsics work automatically (no port needed)

---

## Immediate Next Step (IMPORTANT — REVISED 2026-04-23 late evening)

**Current progress:** 44 of 130 intrinsics shipped (34%).

**Recommended next action:** D-Alpha-7 (Critical multi-core, 8 intrinsics) BEFORE D-Alpha-6 (TLB+cache).

Rationale: `@cpu_pause`, `@cpu_id`, `@cpu_wfe`, `@cpu_sev`, `@cpu_breakpoint`, `@cpu_rdrand`, `@cpu_rdseed`, `@barrier_dma` are foundational for multi-core kernels. Any real kernel needs spinlock support (`pause` + `wait_on_addr`) before it needs cache maintenance. Swap batch order.

Suggested sequence:
1. **D-Alpha-7** — critical multi-core (8 intrinsics, ~50 hrs)
2. **D-Alpha-7.5** — hardened `unsafe asm` (H1-H7 infrastructure, ~120 hrs) ← STRATEGIC
3. **D-Alpha-6** — TLB + cache (11 intrinsics, ~80 hrs)
4. **D-Alpha-8** — nice-to-have (6 intrinsics, ~40 hrs)
5. **D-Alpha-9 through D-Alpha-14** — MSR/CSR, inspection, power, privileged, Linux-scale, misc

**Phase 2 work continues independently** when user has appetite. The two tracks don't gate each other.

### Why D-Alpha-7.5 (hardened `unsafe asm`) is strategic

Once H1-H7 land, users can write ANY vendor-specific code safely:
- Intel TDX/SGX (enclaves)
- AMD SEV (memory encryption)
- ARM TrustZone SMC (secure monitor)
- ARM MTE (memory tagging)
- ARM PAC (pointer authentication)
- RISC-V CHERI (capability machine)
- Future extensions (whatever ships next)

Without H1-H7, every new CPU feature requires a dedicated intrinsic in the compiler. With H1-H7, users write typed `unsafe asm` once. Same safety as Rust, same flexibility.

### What was the old plan (kept for historical context)

Previous recommendation was "finish Phase 2-7 first (~900 hrs) before starting Phase A (~500 hrs infrastructure) before Phase D (~1,400 hrs implementation + proofs)."

Problem: Users had to wait ~2 years to ship any pure-ZER kernel code. No real-world feedback on intrinsic API design. Proof-first locked in constraints before validation.

Revised plan ships working intrinsics in ~6-12 months. Formal proofs added in D-Beta after Phase 7 completes. Total effort similar, value ships years earlier.

---

## Fresh Session Onboarding Checklist

If you are a fresh session picking up this work, do these in order:

1. Read this document top-to-bottom (you're doing that)
2. Read `docs/ASM_ZER-LANG.md` for earlier (2026-04-01) asm research context
3. Read `docs/formal_verification_plan.md` for the 8-phase overall roadmap
4. Read `docs/phase1_catalog.md` for Phase 1 completion state
5. Read `docs/proof-internals.md` for VST/Coq proof patterns
6. Check current status: `grep "Phase [0-9]:" CLAUDE.md` for phase progress
7. **For intrinsic implementation (D-Alpha):** proceed independently of Phase 2-7. See "Implementation-First Plan" section above.
8. **For intrinsic verification (D-Beta):** requires Phase 7 infrastructure. Wait.
9. **For Phase 2-7 prereq work:** see `docs/formal_verification_plan.md`. Two tracks can progress in parallel.

**Do NOT:**
- Gate D-Alpha (intrinsic implementation) on Phase 2-7 completion — decoupled per 2026-04-23 decision
- Try to write formal Coq proofs for intrinsics in D-Alpha — deferred to D-Beta
- Clone Vale verbatim (rejected — toolchain bloat, scope mismatch)
- Remove the existing `asm` keyword (kept as `unsafe asm` escape hatch — Rust-style marking)
- Attempt to verify the emitter end-to-end (CompCert-scale, out of scope)
- Verify CPU hardware (impossible without silicon work)

---

## Key Terminology (for fresh sessions)

| Term | Meaning |
|---|---|
| Intrinsic | A compiler-recognized `@name()` call that emits specific GCC inline asm |
| Sail | Cambridge's formal ISA description language; generates Coq models |
| Islaris | Iris-based Hoare logic framework over Sail ISA models |
| Vale | Microsoft Research verified asm DSL (F*/Z3-based); focused on crypto |
| seL4 | Verified microkernel; the gold standard for kernel verification |
| CompCert | Verified C compiler; semantic preservation proof |
| Target domain | OS/RTOS/kernel/firmware/driver — what ZER-Asm covers 100% |
| Runtime codegen | JIT/live-patch — blocked by type system, not asm rules |
| cinclude | ZER's escape hatch to link external C/asm (warns `UNSAFE-EXTERN`) |
| Phase 1-7 | ZER's formal verification roadmap (prereq for asm work) |
| Phase A-E | This document's asm verification plan (post-Phase-7) |

---

## Frequently Asked Questions (from fresh sessions)

**Q: Why not embed Vale directly?**
A: Vale requires F*/Z3/OCaml (~2 GB toolchain). Covers only ~10% of ZER's needs (crypto). Can't handle context switch, MMU, interrupts. Maintenance burden too high. See "Option 1 rejected" above.

**Q: Can we write the asm in ZER itself?**
A: Yes, via `unsafe asm(...)` inside a `naked` function (Phase 1 verified). Users prefer `@intrinsic()` calls. Bare `asm(...)` is rejected at compile time — the `unsafe` keyword is required to make the escape hatch explicit (matches Rust's `unsafe { asm!() }` pattern).

**Q: What if a user needs something outside the 96 intrinsics?**
A: Three escape routes in order of preference: (1) file issue for new intrinsic to be added + verified; (2) write `unsafe asm(...)` inline (in a `naked` function); (3) use `cinclude "foo.S"` with explicit `UNSAFE-EXTERN` warning; (4) fork ZER and add their own verified intrinsic.

**Q: Can we write a JIT compiler in pure ZER?**
A: No. ZER's type system prevents casting `*u8` → function pointer. This blocks JIT by design. Projects that need JIT (V8, JVM, etc.) are outside ZER's target market.

**Q: Does this cover 100% of asm?**
A: YES for ahead-of-time-compiled code. 96 intrinsics × 4 archs covers every real-world OS/RTOS/firmware goal.

**Q: What about SIMD / vector extensions?**
A: Base plan has no SIMD intrinsics. Surplus budget (400 hrs) can add ~100 SIMD intrinsics per arch if needed. SIMD is performance-critical for crypto/DSP but not for most OS code.

**Q: How does this interact with Phases 1-7?**
A: Phase 1-7 verifies the ZER type system, safety rules, decision logic. Phases A-E verify asm primitives. Together they prove: "ZER programs compiled to asm are safe from UAF/bounds/etc. AND the asm primitives they use are correct."

**Q: Is the 5K hours realistic?**
A: With the 7 shortcuts: yes. Base effort ~3,350 hrs + 900 hrs prereq = 4,250 hrs. 750 hrs surplus for risk. Without shortcuts, it would be ~7,500+ hrs.

**Q: What if Sail doesn't cover our needs?**
A: Contribute upstream fixes. Sail is maintained by Cambridge + ARM + RISC-V International. Rarely bugs; when found, fix and submit. Budget 200-400 hrs as risk contingency.

**Q: Can we ship incrementally?**
A: Yes. Each phase is independently shippable. After Phase A: infrastructure exists. After Phase B+C x86-64: x86-64 kernel dev possible. Each arch ships independently.

**Q: What's the first concrete step if starting today?**
A: Resume Phase 2 Batch 2 (handle state transitions, 6 decisions, ~10 hrs). This is Phase 0 prerequisite work, not asm work. All asm work is gated on Phases 2-7 being complete.

**Q: Do we need to manually port ZER to each architecture?**
A: No. GCC auto-ports everything for free. Pure ZER code and 40 of 96 intrinsics (atomics, barriers, builtins) use GCC builtins that handle arch selection automatically. Only 56 intrinsics need per-arch asm strings, because the underlying hardware operations are fundamentally different (e.g., MSR access, MMU setup). GCC still encodes all asm — we never write an assembler.

**Q: How many intrinsics need verification total?**
A: 96 unique intrinsics (the API surface). For 3-arch scope (x86-64, ARM64, RISC-V):
- 96 arch-agnostic Coq specs (written once)
- 28 proofs for GCC-builtin intrinsics (shared across archs)
- 42 cross-arch intrinsics × 3 archs = 126 per-arch proofs
- 26 mono-arch intrinsics × 1 proof each = 26 proofs
- **Total: 96 specs + 180 proofs = 276 verification work units**

For MVP (kernel-capable): ~120 proofs across 3 archs (~1,000 hrs).
For full coverage: ~180 proofs (~1,400 hrs). Budget: ~1,400 hrs for Phase D.

**Q: What auto-ports vs what's manual per arch?**
A:
- **Automatic (GCC handles):** Pure ZER code, 40 GCC-builtin intrinsics, all asm encoding, all 4 archs of compilation
- **Manual per arch (~20% of per-arch effort):** 56 arch-specific asm strings (5-30 lines each)
- **Manual per arch (~80% of per-arch effort):** Coq proofs that the asm matches the spec

---

## Context from the Discussion (2026-04-23)

Summary of the conversation that led to this plan:

1. User asked if ZER could achieve Vale-tier verified asm ("100% safe").
2. Discussed Vale (Microsoft Research). Determined fork is bad (toolchain bloat, scope mismatch).
3. Proposed Option 3 (ZER-Asm intrinsics, scoped to OS needs).
4. User committed to 5,000 hours to do it "fully 100%."
5. Expanded to 4 architectures (x86-64, ARM64, ARMv7, RISC-V).
6. User asked for shortcuts — introduced 7 shortcuts + NASA Power-of-10-style bans.
7. User clarified wanted GCC backend (no new assembler) — confirmed, already the plan.
8. User asked if intrinsics alone cover 100% — honest answer 95-99%, with 1-5% runtime codegen gap.
9. User reframed: "achieve same goals with restricted means, like NASA Power of 10." Brilliant insight.
10. User clarified: "without Power of 10 bans, just intrinsics, 100% real-world goals?" Recognized the gap is runtime codegen.
11. Clarified: runtime codegen (JIT/live-patch) doesn't use asm keyword — it writes bytes and casts data-to-function-pointer. ZER's type system already blocks this.
12. Final correct claim: intrinsics = 100% asm coverage; JIT/live-patch blocked independently by type system; no asm gap.
13. User confirmed: JIT is "ancient stuff" (C#/Java), Rust doesn't have it, ZER doesn't need it.
14. User requested this document: dump all context for fresh sessions.

---

## Summary Table (the TL;DR)

| Question | Answer |
|---|---|
| Do we write a new assembler? | No. GCC is the assembler. Always was. |
| Do we ban user asm? | No — kept as explicit escape hatch. `unsafe asm(...)` REQUIRED (bare `asm` rejected). Restricted to `naked` fns. Users prefer intrinsics. |
| How do users write low-level code? | Via **130 verified intrinsics** + hardened `unsafe asm` for vendor-specific. |
| Does this cover Linux-scale? | **Yes.** 130 intrinsics cover Linux kernel arch/ needs. Vendor-specific (SGX, TDX, TrustZone, MTE, PAC, CHERI) via hardened `unsafe asm`. |
| How safe is `unsafe asm`? | **Tiered: v1.0 = 95% safe (strict mode 18 rules). v1.0.1 = 99% (selective Vale-tier). v1.1+ = 100% safe (full formal verification).** |
| Can we reach 100% safe? | **YES** (Tier C, post-v1.0). CPU hardware behavior is the silicon vendor's domain, outside any compiler's scope. |
| Which archs initially? | **x86-64, ARM64, RISC-V** (3 archs for v1.0). |
| Do we manually port ZER to each arch? | No. GCC auto-ports pure ZER code + ~40 GCC-builtin intrinsics. Rest need per-arch asm strings. |
| How many intrinsics total? | **130** for v1.0 (was 96 MVP plan). |
| How many specs + proofs? | **130 specs + ~240 proofs** (scaled from 96 baseline). |
| What's hardened `unsafe asm`? | Typed operands (inputs/outputs/clobbers blocks), explicit register validation, mandatory safety docs, audit emission. Rust-tier safety without per-feature intrinsic bloat. |
| Can we ship one arch at a time? | Yes. x86-64 first, then ARM64, then RISC-V. |
| What about Cortex-M? | **Not in v1.0.** Added as v1.1 (~300 hrs, +10 intrinsics → 140 total). |
| What about hypervisor/SGX/TDX? | v1.1+ scope OR handled via hardened `unsafe asm`. Not dedicated intrinsics. |
| What about JIT compilers? | Blocked by type system (not asm rules). `cinclude` for users who need JIT. |
| What's the budget? | **~4,720 hrs committed (out of 5K). 280 hr surplus.** |
| What's the surplus for? | Risk buffer + future crypto Vale-tier + cert artifacts. Cortex-M deferred to explicit v1.1. |
| What's the calendar? | 5 years half-time, 2.5 years full-time. |
| What's the first step? | **Next batch: D-Alpha-7 (8 critical multi-core intrinsics).** After that: D-Alpha-7.5 (hardened unsafe asm, strategic). |
| Current progress? | **44 of 130 intrinsics shipped (34%).** D-Alpha-1 through D-Alpha-5 done. |
| When does asm implementation start? | **STARTED 2026-04-23.** 5 batches shipped in one day. Implementation-first working. |
| When does asm verification (D-Beta) start? | After Phase 7 infrastructure ready (~12-18 months). Formal proofs added on top of working intrinsics. |
| What's the final claim? | "100% verified asm for ZER's target domain across 3+ archs. JIT/live-patch outside scope." |

---

*End of document. Last updated 2026-04-23.*
