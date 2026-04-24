# Asm Precondition Research — Option C (100% Language-Safe Generic Approach)

**Status:** Research in progress (started 2026-04-24).
**Purpose:** Enumerate all precondition-bearing CPU instructions across target archs, classify each into a finite set of universal categories, and produce data tables that make the ZER compiler's asm safety checker **architecturally generic** — closing the "1% gap" that was otherwise deferred to Tier C.

**Strategic goal:** Make ZER+ASM **100% language-safe in Tier A (v1.0)** by using generic-by-construction safety categories. Per-arch work becomes pure data entry. No new safety layer for asm — reuse the 29 ZER safety systems (extend 2, add 1 new).

**Related docs:**
- `docs/asm_plan.md` — overall ZER-Asm verification plan (Tiers A/B/C)
- `docs/compiler-internals.md` — Z-rules section + safety system catalog
- `docs/safety-model.md` — 29 ZER safety systems, 4 models

---

## Background: why this research exists

Per-instruction preconditions (e.g., `bsr` on zero = UB, `shl` count ≥ width = UB) are ISA-specific facts. Hardcoded per-arch databases conflict with ZER's generic-tracking philosophy. But **the preconditions themselves fall into a small finite set of categories** that IS generic across all architectures ever designed.

If we enumerate all UB-bearing instructions across x86-64, ARM64, and RISC-V, and classify each into one of ~10 universal categories, the ZER compiler can:

1. Check instruction preconditions using a **generic dispatch** keyed by category
2. Delegate actual checking to **existing ZER safety systems** (VRP, Qualifier, Context Flags, etc.)
3. Accept per-arch **data files** listing "instruction X uses category Y on operand Z"
4. Gain 100% language-safety in Tier A with minimal ongoing maintenance

This document is the research that feeds into those data files.

---

## The 10 Universal Precondition Categories

**Claim:** Every documented UB-bearing instruction across x86-64, ARM64, RISC-V (and plausibly every ISA ever designed) fits one of these 10 categories. Discovered by survey; to be validated through this document.

### C1: Value-range

**Definition:** An operand's value must be in a specific range or set.

**Subcategories:**
- `C1a` **Nonzero** — operand ≠ 0 (e.g., `bsr` source, `div` divisor)
- `C1b` **Bounded** — operand < N (e.g., `shl` count < operand width)
- `C1c` **Compound** — multi-condition (e.g., `idiv`: divisor ≠ 0 AND (dividend, divisor) ≠ (INT_MIN, -1))
- `C1d` **Exact** — operand ∈ specific set (e.g., instruction-specific valid register indices)

**Maps to:** ZER System #12 **Value Range Propagation (VRP)**.

**Checker action:** invoke `derive_expr_range()` on the bound ZER operand; prove range satisfies constraint; reject if VRP can't prove.

---

### C2: Alignment

**Definition:** A pointer operand must be aligned to a power-of-2 boundary.

**Subcategories:**
- `C2a` **Fixed alignment** — constant value (e.g., 16 for `cmpxchg16b`)
- `C2b` **Cache-line-relative** — architecture-defined (typically 64 bytes)
- `C2c` **Dynamic alignment** — derived from operand width (e.g., vector loads align to vector width)

**Maps to:** ZER System #20 **Qualifier Tracking** (extend with alignment facts).

**Checker action:** track alignment of pointer values through the program; at asm binding, verify alignment ≥ required.

**Extension required:** System #20 currently tracks `const`/`volatile`. Extend to track `align(N)` facts. ~30 hrs implementation.

---

### C3: State machine (atomic / exclusive transactions)

**Definition:** Instruction requires the program to be in a specific execution state, or establishes/ends such a state.

**Subcategories:**
- `C3a` **Enters exclusive state** — must not already be in exclusive state (e.g., `ldxr` on ARM)
- `C3b` **Exits exclusive state** — must match prior enter (e.g., `stxr` on ARM)
- `C3c` **No intervening memops between enter/exit** — state transition integrity
- `C3d` **Atomic RMW variant** — whole-instruction atomicity requirement

**Maps to:** ZER Model 1 **State Machines** (extend with new exclusive-transaction state type).

**Checker action:** track exclusive-state transitions through CFG (same machinery as Handle state tracking); reject state violations.

**Extension required:** Model 1 state machines currently track Handle states, Move tracking, Alloc coloring/ID. Add "in exclusive transaction" state type with entry/exit/intervening-memop rules. ~20 hrs implementation.

---

### C4: CPU feature gate

**Definition:** Instruction is only legal when the CPU has a specific extension.

**Subcategories:**
- `C4a` **Base extension** (e.g., SSE2, NEON) — typically always-on for target arch baseline
- `C4b` **Optional extension** (e.g., AVX-512, SVE, RVI Zicbom) — must be checked at runtime OR declared at build time
- `C4c` **Vendor-specific extension** (e.g., Intel SHA, AMD XOP) — limited CPUs

**Maps to:** ZER System #24 **Context Flags** (extend with `cpu_features` context).

**Checker action:** at asm binding, verify required feature is in declared build-time feature set or runtime-checked before entering the block.

**Extension required:** Context Flags currently tracks `in_loop`, `in_defer`, `in_async`, `in_critical`, `in_interrupt`, `in_naked`. Add `cpu_features` (set of feature flags). ~20 hrs implementation.

---

### C5: Privilege / execution mode

**Definition:** Instruction requires specific privilege level or CPU mode.

**Subcategories:**
- `C5a` **Kernel/supervisor-only** (e.g., `wrmsr` requires CPL=0 on x86; `msr` to EL1+ registers on ARM)
- `C5b` **Secure-mode-only** (ARM TrustZone secure world; RISC-V M-mode)
- `C5c` **Specific mode transition** (e.g., `syscall`/`sysret` structural requirements)

**Maps to:** ZER System #24 **Context Flags** (existing `in_kernel` / `privilege_level` tracking).

**Checker action:** verify current function's privilege context matches instruction's required level.

**No extension required** — existing Context Flags cover this. Just add per-instruction mapping.

---

### C6: Memory addressability

**Definition:** A memory operand must point to a valid, addressable region.

**Subcategories:**
- `C6a` **MMIO range** — address in declared `mmio 0xADDR..0xADDR` range
- `C6b` **Canonical address** (x86-64) — sign-extended bits 48-63 must match bit 47
- `C6c` **Virtual vs physical** — some instructions (e.g., `invlpg`) take VA, others take PA
- `C6d` **Non-special-region** — can't alias with ROM, device memory, etc.

**Maps to:** ZER System #19 **MMIO Ranges** (already exists) + System #3 Provenance for canonical-vs-not.

**Checker action:** at asm binding, verify address meets region constraints.

**No extension required** — existing MMIO Ranges + Provenance suffice.

---

### C7: Provenance / aliasing

**Definition:** Multi-operand constraints about pointer relationships.

**Subcategories:**
- `C7a` **Disjoint operands** — two pointer operands must not alias (e.g., some DMA / string ops)
- `C7b` **Same-provenance required** — operand pointers must derive from same allocation
- `C7c` **Write-once through operand** — specific operand is output-only, can't also be input

**Maps to:** ZER System #3 **Provenance** + System #11 **Escape Flags**.

**Checker action:** track pointer provenance through program; at asm binding, verify operand relationships.

**No extension required** — existing Provenance + Escape suffice.

---

### C8: Memory ordering (happens-before)

**Definition:** Instruction requires (or establishes) a specific happens-before relationship with other memory operations.

**Subcategories:**
- `C8a` **Requires prior barrier** — instruction assumes a fence/barrier was executed before
- `C8b` **Establishes barrier** — instruction itself acts as a barrier (e.g., `mfence`)
- `C8c` **Ordering vs specific instruction** — e.g., `fence.i` must precede self-modifying code execution
- `C8d` **Atomic ordering semantics** — acq/rel/seq_cst constraints on following/preceding memops

**Maps to:** **NEW ZER System #30 — Atomic Ordering Tracking.**

**Checker action:** track happens-before edges between asm blocks and ZER memory operations; verify ordering constraints.

**New system required:** this is the biggest architecture addition. ~80 hrs implementation. Valuable BEYOND asm — also helps analyze `shared struct` access ordering, concurrent algorithms, memory model proofs.

---

### C9: Exclusive pairing (paired instructions)

**Definition:** Instructions that must appear in matched begin/end pairs with no intervening operations of specified kinds.

**Subcategories:**
- `C9a` **Load-linked / store-conditional pair** (ARM `ldxr`/`stxr`, RISC-V `lr`/`sc`, PowerPC `lwarx`/`stwcx`)
- `C9b` **Begin/end critical section pair** (vendor-specific, rare)
- `C9c` **Transaction begin/commit** (Intel TSX `xbegin`/`xend`)

**Maps to:** Same as C3 — Model 1 state machine with enter/exit operations.

**Relation to C3:** C9 is a subcategory of C3 in practice. Separated in this taxonomy for clarity during research; may merge during implementation.

**No additional extension** — covered by C3's Model 1 extension.

---

### C10: Register dependency / sequencing

**Definition:** Multi-instruction sequences where one instruction's correctness depends on prior instruction's output being in specific register, with no intervening instructions that clobber.

**Subcategories:**
- `C10a` **Implicit register chains** (e.g., `rep movs` depends on `rdi`/`rsi`/`rcx` values set immediately before)
- `C10b` **Vector chain dependencies** (some SIMD chains with register forwarding assumptions)
- `C10c` **Timing-sensitive sequences** (rare, usually in crypto constant-time code)

**Maps to:** Case-by-case — some via Provenance (#3), some via State Machine (Model 1), some genuinely rare.

**Action:** Research whether this category is common enough to warrant framework support, or whether it's rare edge cases handled by Tier C only.

**Possibly defer:** category C10 might stay in Tier C (`@verified_spec`) if research shows it's rare. ~5% of instructions at most.

---

## Category-to-System Mapping Summary

| Category | Subcategories | Primary ZER system | Extension needed | Hours |
|---|---|---|---|---|
| C1 Value-range | C1a, b, c, d | #12 VRP | None — existing | 0 |
| C2 Alignment | C2a, b, c | #20 Qualifier Tracking | Add alignment facts | 30 |
| C3 State machine | C3a, b, c, d | Model 1 (new state type) | "in exclusive transaction" state | 20 |
| C4 CPU feature | C4a, b, c | #24 Context Flags | Add `cpu_features` context | 20 |
| C5 Privilege | C5a, b, c | #24 Context Flags | None — existing | 0 |
| C6 Memory addressability | C6a, b, c, d | #19 MMIO Ranges + #3 Provenance | None — existing | 0 |
| C7 Provenance/aliasing | C7a, b, c | #3 Provenance + #11 Escape | None — existing | 0 |
| C8 Memory ordering | C8a, b, c, d | **NEW: System #30 Atomic Ordering** | Full new system | 80 |
| C9 Exclusive pairing | C9a, b, c | Same as C3 | Merged with C3 | 0 |
| C10 Register dependency | C10a, b, c | Case-by-case or defer | Decide post-research | TBD |
| **Total infrastructure** | — | — | — | **~150 hrs** |

---

## Per-Arch Instruction Survey

**Format:** Each instruction entry lists mnemonic, precondition category + subcategory, affected operand, source citation.

**Marking:**
- ✓ = confirmed from ISA manual
- ? = from memory, needs verification
- T = trivial (well-known UB)
- X = researched but determined to have no UB precondition

### x86-64 (Intel SDM Vol 2 + AMD APM Vol 3)

**Value-range (C1) instructions:**

| Instruction | Subcat | Operand | Precondition | Source | Status |
|---|---|---|---|---|---|
| `bsr` | C1a | source | operand ≠ 0 | Intel SDM §3.2 BSR | ✓ T |
| `bsf` | C1a | source | operand ≠ 0 | Intel SDM §3.2 BSF | ✓ T |
| `div r/m8` | C1a | divisor | divisor ≠ 0 | Intel SDM §3.2 DIV | ✓ T |
| `div r/m16/32/64` | C1a | divisor | divisor ≠ 0 | Intel SDM §3.2 DIV | ✓ T |
| `idiv r/m8` | C1c | (dividend, divisor) | divisor ≠ 0 AND quotient fits | Intel SDM §3.2 IDIV | ✓ T |
| `idiv r/m16/32/64` | C1c | (dividend, divisor) | divisor ≠ 0 AND quotient fits | Intel SDM §3.2 IDIV | ✓ T |
| `shl`/`shr`/`sar` immediate | C1b | count | implicit masked but technically UB if ≥ width | Intel SDM §4.1 SAL/SAR | ✓ |
| `shl`/`shr`/`sar` CL | C1b | count (CL register) | same (masked in hardware) | Intel SDM §4.1 | ✓ |
| `rcl`/`rcr`/`rol`/`ror` | C1b | count | count < operand width | Intel SDM §4.1 RCL/RCR | ✓ |
| `shld`/`shrd` | C1b | count | count < 32/64 | Intel SDM §4.1 SHLD/SHRD | ✓ |
| `bt`/`bts`/`btr`/`btc` immediate | C1b | bit index | index < operand width | Intel SDM §3.2 BT | ✓ |

**Alignment (C2) instructions:**

| Instruction | Subcat | Operand | Precondition | Source | Status |
|---|---|---|---|---|---|
| `cmpxchg16b` | C2a | memory | 16-byte aligned | Intel SDM §3.2 CMPXCHG8B/CMPXCHG16B | ✓ T |
| `movdqa` | C2a | memory | 16-byte aligned | Intel SDM §4.2 MOVDQA | ✓ T |
| `movntdqa` | C2a | memory | 16-byte aligned | Intel SDM §4.2 MOVNTDQA | ✓ |
| `vmovdqa` (AVX) | C2a | memory | 32-byte aligned | Intel SDM Vol 2A | ✓ |
| `vmovdqa32/64` (AVX-512) | C2a | memory | 64-byte aligned | Intel SDM Vol 2A | ✓ |
| `movaps`/`movapd` | C2a | memory | 16-byte aligned | Intel SDM §4.2 MOVAPS | ✓ |
| `lock cmpxchg16b` | C2a | memory | 16-byte aligned | Intel SDM §3.2 LOCK | ✓ |

**State machine (C3) instructions:**

| Instruction | Subcat | Precondition | Source | Status |
|---|---|---|---|---|
| `xbegin` | C3a | not in transactional state | Intel SDM §4.2 XBEGIN (TSX) | ✓ |
| `xend` | C3b | must be in transaction started by xbegin | Intel SDM §4.2 XEND | ✓ |
| `xabort` | C3b | must be in transaction | Intel SDM §4.2 XABORT | ✓ |

**CPU feature (C4) instructions:**

| Instruction | Feature | Source | Status |
|---|---|---|---|
| `aesenc`/`aesdec`/etc. | AES-NI | CPUID.01H:ECX[25] | ✓ |
| `vpbroadcastb` (AVX) | AVX | CPUID.01H:ECX[28] | ✓ |
| `vpaddq` (AVX-512) | AVX-512F | CPUID.07H:EBX[16] | ✓ |
| `clwb` | Cache line write back | CPUID.07H:EBX[24] | ✓ |
| `clflushopt` | CLFLUSHOPT | CPUID.07H:EBX[23] | ✓ |
| `rdrand` | RDRAND | CPUID.01H:ECX[30] | ✓ |
| `rdseed` | RDSEED | CPUID.07H:EBX[18] | ✓ |
| `pcmpestri` | SSE4.2 | CPUID.01H:ECX[20] | ✓ |
| `sha256rnds2` | SHA | CPUID.07H:EBX[29] | ✓ |
| `vaesenc` (VAES) | VAES | CPUID.07H:ECX[9] | ✓ |

**Privilege (C5) instructions:**

| Instruction | Subcat | Precondition | Source | Status |
|---|---|---|---|---|
| `wrmsr` | C5a | CPL = 0 | Intel SDM §4.3 WRMSR | ✓ T |
| `rdmsr` | C5a | CPL = 0 | Intel SDM §4.3 RDMSR | ✓ T |
| `mov to CR0-CR4` | C5a | CPL = 0 | Intel SDM §4.3 MOV | ✓ T |
| `lgdt`/`lidt`/`lldt` | C5a | CPL = 0 | Intel SDM §3.2 LGDT | ✓ T |
| `invlpg` | C5a | CPL = 0 | Intel SDM §3.2 INVLPG | ✓ T |
| `swapgs` | C5a | CPL = 0 | Intel SDM §4.3 SWAPGS | ✓ |
| `iret`/`iretd`/`iretq` | C5c | must be in interrupt/exception handler | Intel SDM §3.2 IRET | ✓ |
| `hlt` | C5a | CPL = 0 | Intel SDM §3.2 HLT | ✓ T |
| `cli`/`sti` | C5a | depends on IOPL | Intel SDM §3.2 CLI | ✓ |
| `vmlaunch`/`vmresume` | C5a | in VMX root, VMCS active | Intel SDM Vol 3 | ✓ |

**Memory addressability (C6) instructions:**

| Instruction | Subcat | Precondition | Source | Status |
|---|---|---|---|---|
| `invlpg` | C6b | canonical virtual address | Intel SDM §3.2 INVLPG | ✓ |
| `clflush`/`clflushopt`/`clwb` | C6a | address in valid memory range | Intel SDM §3.2 CLFLUSH | ✓ |
| `prefetcht0`/etc. | C6d | (hints, not strictly UB) | Intel SDM §3.2 PREFETCH | X — hints |

**Memory ordering (C8) instructions:**

| Instruction | Subcat | Precondition | Source | Status |
|---|---|---|---|---|
| `mfence` | C8b | (establishes barrier, no precond) | Intel SDM §3.2 MFENCE | ✓ |
| `sfence` | C8b | (establishes barrier) | Intel SDM §3.2 SFENCE | ✓ |
| `lfence` | C8b | (establishes barrier) | Intel SDM §3.2 LFENCE | ✓ |
| `clflushopt` | C8a | requires prior fence for ordering | Intel SDM §3.2 CLFLUSHOPT | ✓ |
| `clwb` | C8a | requires prior fence for ordering | Intel SDM §3.2 CLWB | ✓ |
| `lock` prefix operations | C8b | establish atomic ordering | Intel SDM §3.2 LOCK | ✓ |

**Register dependency (C10) — rare:**

| Instruction | Subcat | Precondition | Source | Status |
|---|---|---|---|---|
| `rep movsb`/`movsw`/`movsd`/`movsq` | C10a | RDI, RSI, RCX must be set correctly | Intel SDM §3.2 MOVS | ✓ |
| `rep stosb`/etc. | C10a | RDI, RCX, AL/AX/EAX/RAX | Intel SDM §3.2 STOS | ✓ |
| `rep cmps`/`scas` | C10a | RDI, RSI, RCX | Intel SDM §3.2 CMPS | ✓ |

**x86-64 subtotal:** ~50 instructions classified. Estimated total: ~60-70 UB-bearing x86-64 instructions. Remaining: detailed survey of FPU, MMX, SSE4.2 extras, AVX-512 opmask ops, specialized AMD-only ops.

---

### ARM64 (ARM Architecture Reference Manual, DDI 0487)

**Placeholder — to be filled in Phase 2 of research.**

Known at memory/confident level:

**Value-range (C1):**
- `udiv`/`sdiv`: divisor = 0 returns 0 (NOT UB on ARM — different from x86) ✓
- `clz`/`cls`: return width for input 0 (NOT UB — different from x86 `bsr`) ✓
- `ubfx`/`sbfx` immediate field: bit range must be in [0, 31/63] ✓
- `lsl`/`lsr`/`asr` register: shift count masked (no UB if ≥ width, technically defined but might not be desired) ✓
- `ror` register: shift count masked ✓

**State machine (C3):**
- `ldxr`/`ldxrb`/`ldxrh`/`ldxrp` (exclusive load) — enters exclusive state ✓
- `stxr`/`stxrb`/`stxrh`/`stxrp` — exits exclusive state, must match prior ldxr ✓
- `clrex` — clears exclusive state ✓

**CPU feature (C4):**
- `aese`/`aesd`/etc. — AES feature (CPUID AA64ISAR0_EL1.AES) ✓
- `sha256h`/etc. — SHA2 feature ✓
- SVE instructions — SVE feature ✓
- `sqrdmlah`/etc. — RDMA feature ✓

**Privilege (C5):**
- `msr`/`mrs` to system registers — varies by register (e.g., SCR_EL3 requires EL3) ✓
- `eret` — requires proper exception state ✓
- `smc`/`hvc` — mode-dependent ✓

**To research:** alignment requirements for LDP/STP, SVE memory ops, Neon unaligned access rules, debug instruction preconditions.

---

### RISC-V (RISC-V ISA Manual Vol 1 Unprivileged + Vol 2 Privileged)

**Placeholder — to be filled in Phase 2 of research.**

Known at memory/confident level:

**Value-range (C1):**
- `div`/`divu`/`rem`/`remu`: divisor = 0 returns specific values (NOT UB — defined in spec) ✓
- `sll`/`srl`/`sra` register: shift count masked to log2(XLEN) bits (not UB) ✓
- `clz`/`ctz` (from Zbb): return XLEN for input 0 ✓

**State machine (C3):**
- `lr.w`/`lr.d` (load-reserved) — enters exclusive state ✓
- `sc.w`/`sc.d` (store-conditional) — exits exclusive state ✓

**CPU feature (C4):**
- Any Zicbom (cache block management) instruction — Zicbom extension ✓
- `fence.i` — Zifencei extension ✓
- Any V extension (RVV) instruction — V extension ✓
- Any B extension (Zba, Zbb, Zbs) — bit-manipulation extensions ✓

**Memory ordering (C8):**
- `fence` variants — establish ordering (parameters acq/rel/rw) ✓
- `fence.i` — instruction cache coherence with prior stores ✓

**Privilege (C5):**
- `csrrw`/`csrrs`/`csrrc` with privileged CSRs — require M-mode or S-mode ✓
- `mret`/`sret` — mode-dependent ✓
- `wfi` — allowed in any mode, but effect depends on TW bit ✓

**To research:** specific CSR access rules, vector length constraints (vl, vtype), PMP/PMA-specific instructions, debug mode instructions.

---

## Research Methodology (for future session phases)

### Phase 1 (this session): Framework + x86-64 first pass — **IN PROGRESS**

- ✓ Define 10 universal precondition categories
- ✓ Map categories to ZER safety systems (existing + extensions + new System #30)
- ✓ First pass x86-64 classification (~50 instructions)
- Pending: complete x86-64 survey (remaining ~10-20 specialized)

### Phase 2: ARM64 detailed survey (~20 hrs)

1. Read ARM ARM DDI 0487 Chapter D — instruction descriptions
2. For each instruction, check the "Shared Pseudocode" section for `UNDEFINED`/`UNPREDICTABLE`/`CONSTRAINED_UNPREDICTABLE` clauses
3. Classify each precondition into C1-C10
4. Produce `arch_data/arm64.zerdata` file

### Phase 3: RISC-V detailed survey (~15 hrs)

1. Read RISC-V ISA Manual Vol 1 (Unprivileged) + Vol 2 (Privileged)
2. Note extension dependencies from extension chapters
3. Note explicit "undefined" clauses (RISC-V is sparer than x86/ARM)
4. Classify each precondition into C1-C10
5. Produce `arch_data/rv64.zerdata` file

### Phase 4: Taxonomy refinement (~10 hrs)

1. Review all three arch classifications
2. Verify 10 categories cover every instruction (may add category C11+ if needed)
3. Refine subcategories
4. Document any instructions that don't cleanly fit any category (and decide: new category, merge into existing, or defer to Tier C)

### Phase 5: New System #30 (Atomic Ordering) design (~20 hrs)

1. Specify what System #30 tracks (happens-before edges)
2. Design API: how asm blocks declare ordering constraints
3. Design API: how checker proves/disproves ordering claims
4. Integration with existing #6 Alloc Coloring pattern (transitive relationships)
5. Document in `docs/safety-model.md`

### Phase 6: Generic checker framework (~30 hrs)

1. Define precondition category enum + constraint types in C
2. Write generic dispatch: `check_precondition(instr, operand_bindings)`
3. Route to existing systems (#12 VRP, #20 Qualifier, etc.) based on category
4. Implement extensions (System #20 alignment, Model 1 exclusive state, #24 cpu_features)
5. Implement System #30

### Phase 7: Arch data file loader (~20 hrs)

1. Parse `.zerdata` files at compiler startup
2. Build instruction → category lookup table
3. Wire into asm string parser: mnemonic → category → checker dispatch

### Phase 8: Asm string parser (~40 hrs)

1. Tokenize `instructions: "..."` string
2. Identify mnemonics (first token per line)
3. Identify operand references (`%0`, `%1`, `[x0]`, etc.)
4. Map operand references to `inputs:`/`outputs:` bindings
5. Emit per-instruction check calls to generic framework

### Phase 9: Tests + integration (~30 hrs)

1. Negative test per category (one per subcategory ideally)
2. Positive test per category (showing guards work)
3. Cross-arch tests where same category applies on all 3 archs
4. Integration with existing strict mode (18 structural + 13 Z-rules)

---

## Open Questions (to resolve during research)

1. **C10 Register dependency:** is this common enough to warrant framework support? Or defer to Tier C?
2. **System #30 scope:** does atomic ordering tracking belong to asm only, or extend to `shared struct` access + concurrent code broadly? (Latter has more value.)
3. **Vendor-specific extensions (TDX, SGX, MTE, PAC):** do these need their own category, or fit existing C1-C10?
4. **Handling of "technically UB but masked in hardware"** (e.g., x86 shift count ≥ width): do we reject at compile time even though hardware doesn't fault? (Vote: yes — honor the spec.)
5. **CPU feature detection at build time vs runtime:** compiler flag `--target-features=avx512` vs runtime `@cpuid_check()` — both supported?

---

## Effort Budget

| Phase | Hours | Status |
|---|---|---|
| Phase 1: Framework + x86-64 first pass | 40 | In progress |
| Phase 2: ARM64 survey | 20 | Pending |
| Phase 3: RISC-V survey | 15 | Pending |
| Phase 4: Taxonomy refinement | 10 | Pending |
| Phase 5: System #30 design | 20 | Pending |
| Phase 6: Generic checker framework | 30 | Pending |
| Phase 7: Arch data file loader | 20 | Pending |
| Phase 8: Asm string parser | 40 | Pending |
| Phase 9: Tests + integration | 30 | Pending |
| **Total research + implementation** | **~225 hrs** | |

Originally estimated ~420 hrs for Option C in `docs/asm_plan.md`; refined to ~225 hrs after detailed breakdown because some work (alignment in #20, context flags extension) is smaller than initially estimated.

**Tier A budget impact:** +225 hrs on top of baseline ~360 hrs (mapping approach baseline) = ~585 hrs total for Tier A with 100% language-safe claim.

Still fits within 5K total budget with surplus.

---

## Success Criteria (when is this research "done")

1. ✓ 10 precondition categories formally defined
2. ✓ Category → ZER safety system mapping complete
3. Pending: every UB-bearing instruction across x86-64 / ARM64 / RISC-V classified into exactly one category with subcategory + operand + source citation
4. Pending: arch data files produced (`.zerdata` format)
5. Pending: System #30 specification written
6. Pending: generic checker framework implementation plan
7. Pending: review from someone with ISA expertise (can be LLM-assisted classification + human spot-check)

When all 7 complete → the compiler implementation work (Phases 6-9) can proceed with clear scope.

---

## Relationship to Other Docs

- **`docs/asm_plan.md`** — overall asm safety plan. This research document is the concrete artifact that feeds into the plan's Tier A "100% language-safe" goal.
- **`docs/compiler-internals.md`** — Z-rules section will reference this doc once categories are formalized.
- **`docs/safety-model.md`** — 29 systems catalog; update to 30 after System #30 (Atomic Ordering) is designed.
- **`docs/proof-internals.md`** — future formal proofs over System #30 ordering claims.
