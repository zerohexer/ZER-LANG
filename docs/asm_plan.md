# ZER-Asm Verification Plan — Full Context Dump (2026-04-23)

**Purpose:** This document captures the complete context from the Vale-tier verified asm design discussion so any fresh session can pick up exactly where we left off. Read this top-to-bottom if you are starting fresh on the asm verification work.

**Key revisions (2026-04-23 late-day):**
- Scope reduced from 4 archs → 3 archs (x86-64, ARM64, RISC-V). Cortex-M deferred to v1.1.
- `asm` keyword renamed to `unsafe asm` — the `unsafe` marker is now **required** (Rust-style explicit escape hatch). Bare `asm(...)` is rejected with compile error. Phase 1 verified rule (`zer_asm_allowed_in_context`) unchanged — structural rule applies to new naming.
- Sail cannot generate asm code (web search confirmed). Removed "Sail codegen" as a shortcut.
- Islaris covers ARM64 + RISC-V only, single-threaded. x86 verification requires custom framework build-out.

**Related docs:**
- `docs/ASM_ZER-LANG.md` — earlier (2026-04-01) asm research (context switch, boot, atomics design). That was the foundation; this is the formal verification plan built on top.
- `docs/formal_verification_plan.md` — overall 8-phase verification roadmap. ZER-Asm is Phase 8 in that plan.
- `docs/phase1_catalog.md` — Phase 1 predicate catalog (85 predicates, 100% complete 2026-04-22).
- `docs/proof-internals.md` — VST/Coq proof patterns.

---

## Executive Summary

**Goal:** Achieve 100% formally verified asm coverage across x86-64, ARM64, and RISC-V for ZER's target domain (OS kernels, RTOSes, application-class embedded firmware).

**Budget:** ~5,000 hours total, committed by the user.

**Approach:** Discourage user-written asm via explicit `unsafe asm` marker; users prefer verified intrinsics. Provide 96 intrinsics (API surface); ~180 proofs across 3 architectures. Use GCC as the assembler (no new toolchain). Runtime code generation (JIT, live patching) is blocked by ZER's existing type system, not by asm rules.

**Revised effort with 3-arch scope + 7 shortcuts:** ~2,850 hours for asm verification + 900 hrs prerequisite (Phases 2-7) = **3,750 hours**. Leaves **~1,250 hours surplus** for crypto/certification/Cortex-M-later.

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

## Calendar Timeline (5-year plan, half-time = 1,000 hrs/year, 3-arch scope)

| Year | Hours cumulative | Milestones |
|---|---|---|
| Year 1 | 1,000 | Phases 2-7 done (~900 hrs). Phase A started. ZER type system fully verified. |
| Year 2 | 2,000 | Phase A complete. Phase B x86-64 done. Phase D x86-64 intrinsics started. First verified OS primitives shippable. |
| Year 3 | 3,000 | Phase B complete (all 3 archs). Phase C advanced semantics done. Phase D x86-64 complete. ARM64 started. |
| Year 4 | 3,750 | Phase D ARM64 + RISC-V complete. Phase E polish + cert. **All 3 archs shipping.** |
| Year 5 | 5,000 | Use 1,250-hr surplus: Cortex-M (v1.1), crypto Vale-tier, DO-178C artifacts. **All 4 archs + certification + crypto.** |

Full-time (2,000 hrs/year) halves the calendar to 2.5 years.

**Year 4 is the "ship" milestone.** 3 archs shipping with full verified intrinsics + kernel development viable.

**Year 5 is the "polish" milestone.** Cortex-M added, crypto verified to Vale-tier, certification-ready.

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
| CPU silicon | Hardware bugs (Spectre, Meltdown, FDIV) | Documented but not verified — hardware problem |
| Hardware ISA spec | Intel/ARM/RISC-V manuals may have errata | Submit errata when found |
| Microcode | x86 microcode patches | Not formally modeled anywhere |
| Physical side channels | Power, EM, acoustic | Out of scope for any software verification |

This is the same trust boundary seL4 and CompCert use. No language can do better without nation-state-level hardware verification.

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

## Immediate Next Step (IMPORTANT)

**DO NOT start asm work yet.**

The prerequisite is Phase 2 through Phase 7 of the formal verification plan (900 hrs). Current status:
- Phase 1: 100% complete (85/85 predicates)
- Phase 2: 7% complete (4/60 decisions)
- Phases 3-7: not started

**The immediate next concrete step is Phase 2 Batch 2 — handle state transitions.** Extract 6 decisions from zercheck.c/zercheck_ir.c state machine as verified decision functions.

Reason: asm verification builds on the type system. Without Phases 2-7, verified asm attaches to an unverified surrounding language — the safety claim would be weak. Finish the foundation first.

**After Phase 7 completes** (~12-18 months of part-time work from now), begin Phase A (core infrastructure for ZER-Asm).

---

## Fresh Session Onboarding Checklist

If you are a fresh session picking up this work, do these in order:

1. Read this document top-to-bottom (you're doing that)
2. Read `docs/ASM_ZER-LANG.md` for earlier (2026-04-01) asm research context
3. Read `docs/formal_verification_plan.md` for the 8-phase overall roadmap
4. Read `docs/phase1_catalog.md` for Phase 1 completion state
5. Read `docs/proof-internals.md` for VST/Coq proof patterns
6. Check current status: `grep "Phase [0-9]:" CLAUDE.md` for phase progress
7. If Phase 2-7 not done: resume that work first (see `docs/formal_verification_plan.md`)
8. If Phase 2-7 done: begin Phase A (this document's Phase A section)

**Do NOT:**
- Start Phase A before Phases 2-7 complete (builds on sand)
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
| How do users write low-level code? | Via 96 verified intrinsics (same API surface on every arch). |
| Does this cover 100% of asm? | Yes, for ahead-of-time-compiled code in target domain. |
| Which archs initially? | **x86-64, ARM64, RISC-V** (3 archs for v1.0). |
| Do we manually port ZER to each arch? | No. GCC auto-ports pure ZER code + 28 GCC-builtin intrinsics. Rest need per-arch asm strings. |
| How many intrinsics total? | **96 unique intrinsics** (API surface). |
| How many specs + proofs? | **96 specs + 180 proofs** (28 shared GCC + 126 cross-arch × 3 + 26 mono-arch). |
| Can we ship one arch at a time? | Yes. x86-64 first (~650 hrs), then ARM64 (~450), then RISC-V (~350). |
| What about Cortex-M? | **Not in v1.0.** Added as v1.1 (~300 hrs from surplus). |
| What about JIT compilers? | Blocked by type system (not asm rules). `cinclude` for users who need JIT. |
| What's the budget? | **~3,750 hrs committed (out of 5K). 1,250 hr surplus.** |
| What's the surplus for? | Cortex-M v1.1 + crypto Vale-tier + DO-178C cert artifacts. |
| What's the calendar? | 5 years half-time, 2.5 years full-time. |
| What's the first step? | Phase 2 Batch 2 (handle state transitions). NOT asm work yet. |
| When does asm work start? | After Phases 2-7 complete (~12-18 months from now). |
| What's the final claim? | "100% verified asm for ZER's target domain across 3+ archs. JIT/live-patch outside scope." |

---

*End of document. Last updated 2026-04-23.*
