# ASM & Low-Level Safety in ZER-LANG — Full Context Dump

## Decision History (2026-04-01)

This document captures the full research and design discussion for adding safe low-level primitives to ZER-LANG. The goal: enable writing a pure ZER OS kernel (no C files, no cinclude) with full safety on all non-asm code.

### The Question
Can ZER-LANG build an OS kernel from scratch, like Rust built Redox OS?

### The Answer
Almost — but needs 3 features: safe atomics, critical sections, and minimal raw asm for 2 unavoidable cases (context switch, boot startup).

---

## What Rust Has for OS Development vs ZER

| Rust feature | Used for in Redox | ZER equivalent | Gap? |
|---|---|---|---|
| `asm!()` with operands | Context switch, syscall, barriers | `asm("nop")` only | GAP |
| `#[link_section]` | Memory layout, vector table | None | GAP |
| `#[no_mangle]` | Linker-visible symbols | Already works (no mangling in main module) | No gap |
| `#[naked]` | ISR entry, no prologue | `interrupt` keyword (similar) | Small gap |
| `unsafe {}` | Raw pointer deref, asm, FFI | Not needed — ZER validates differently | Design choice |
| Traits / generics | Driver interfaces | Function pointers + structs | No gap |
| Ownership / borrow checker | Memory safety | 17 safety categories | No gap |
| `#[no_std]` | Freestanding kernel | `__STDC_HOSTED__` guard | No gap |
| `#[panic_handler]` | Kernel panic | `@trap()` | No gap |
| `#[repr(C)]` | FFI struct layout | Already C layout | No gap |
| `#[cfg()]` | Conditional compilation | `comptime if` | No gap |
| Allocator trait | Custom allocators | Pool/Slab/Arena built in | No gap |

## The Key Insight: Assembly is Patterns, Not Arbitrary Code

OS kernels use asm for specific, well-defined operations:

1. **Atomics** — add/sub/cas/load/store with memory ordering
2. **Critical sections** — disable interrupts, do work, restore interrupts
3. **Barriers** — memory/store/load fences
4. **Context switch** — save registers to struct, load from another struct
5. **Syscall entry** — read number from register, dispatch
6. **Boot startup** — set stack pointer, zero BSS, jump to main

Items 1-3 can be made into SAFE intrinsics. Items 4-6 need raw asm but are tiny (5-20 lines each).

---

## GCC __atomic Builtins — NOT Universally Safe

### The Problem

GCC `__atomic_*` builtins work on platforms WITH native atomic instructions. On platforms WITHOUT them, GCC calls `libatomic` which may not exist for embedded targets.

| Platform | Native atomic? | GCC __atomic works? |
|----------|---------------|-------------------|
| x86/x86_64 | Yes (`lock` prefix) | Yes |
| ARM Cortex-M3/M4/M7 | Yes (`ldrex`/`strex`) | Yes |
| ARM Cortex-M0/M0+ | **No** | **Broken** — calls missing libatomic |
| RISC-V with A extension | Yes (`amoadd`, `lr`/`sc`) | Yes |
| RISC-V without A extension | **No** | **Broken** — calls missing libatomic |
| AVR | **No** | **Broken** — undefined reference at link |
| MIPS | Yes (LL/SC) | Yes |
| PowerPC | Yes (lwarx/stwcx) | Yes |

### The Solution: Dual-Path Emission

ZER emits BOTH paths — GCC preprocessor picks the right one at compile time:

```c
// For targets WITH native atomics:
#if defined(__ARM_FEATURE_LDREX) || defined(__riscv_atomic) || defined(__x86_64__) || defined(__i386__) || defined(__mips__) || defined(__powerpc__)
    __atomic_fetch_add(&x, 1, __ATOMIC_SEQ_CST);

// For targets WITHOUT native atomics (AVR, Cortex-M0, RISC-V no A):
#else
    // Disable interrupts, do operation, restore interrupts
    // Architecture-specific interrupt disable/enable
#endif
```

### GCC Target Detection Macros

| Macro | Meaning |
|---|---|
| `__ARM_FEATURE_LDREX` | ARM with ldrex/strex (Cortex-M3+) |
| `__ARM_ARCH_6M__` | Cortex-M0/M0+ (no ldrex) |
| `__riscv_atomic` | RISC-V with A extension |
| `__AVR__` | AVR microcontroller |
| `__x86_64__` / `__i386__` | x86 (always has atomics) |
| `__mips__` | MIPS (has LL/SC) |
| `__powerpc__` | PowerPC (has lwarx/stwcx) |

### Interrupt Disable/Enable Per Architecture

```c
// ARM (all variants):
__asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(saved));  // disable
__asm__ volatile("msr primask, %0" :: "r"(saved));             // restore

// AVR:
saved = SREG; __asm__ volatile("cli");  // disable
SREG = saved;                           // restore

// RISC-V:
__asm__ volatile("csrrci %0, mstatus, 8" : "=r"(saved));  // disable
__asm__ volatile("csrw mstatus, %0" :: "r"(saved));        // restore

// x86:
__asm__ volatile("pushf; pop %0; cli" : "=r"(saved));  // disable
__asm__ volatile("push %0; popf" :: "r"(saved));        // restore
```

---

## Planned Safe Intrinsics

### @atomic_add, @atomic_sub, @atomic_or, @atomic_and, @atomic_xor

```zer
// User writes:
u32 old = @atomic_add(&counter, 1);

// ZER emits (platform-appropriate):
// Native: __atomic_fetch_add(&counter, 1, __ATOMIC_SEQ_CST)
// Fallback: { disable_irq; old = counter; counter += 1; enable_irq; }
```

**Safety:** ZER validates:
- First arg must be a pointer to integer (`*u32`, `*i32`, etc.)
- Second arg must match pointed-to type
- Result type matches pointed-to type

### @atomic_cas (Compare And Swap)

```zer
// User writes:
bool swapped = @atomic_cas(&value, expected, desired);

// ZER emits:
// Native: __atomic_compare_exchange_n(&value, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
// Fallback: { disable_irq; if (value == expected) { value = desired; swapped = true; } else { swapped = false; } enable_irq; }
```

### @atomic_load, @atomic_store

```zer
u32 val = @atomic_load(&shared_var);
@atomic_store(&shared_var, 42);

// Native: __atomic_load_n / __atomic_store_n
// Fallback: disable_irq wrapper
```

### @critical { }

```zer
// User writes:
@critical {
    shared_state += 1;
    other_shared -= 1;
}

// ZER emits:
// { uint32_t _saved; disable_irq(&_saved);
//   shared_state += 1; other_shared -= 1;
//   restore_irq(_saved); }
```

**Safety:** ZER can verify:
- Variables accessed inside @critical that are shared with ISR → correct usage
- @critical blocks aren't nested (double-disable is usually a bug)
- @critical doesn't contain function calls that might re-enable interrupts

### @barrier() — Already Exists

```zer
@barrier();        // __atomic_thread_fence(__ATOMIC_SEQ_CST)
@barrier_store();  // __atomic_thread_fence(__ATOMIC_RELEASE)
@barrier_load();   // __atomic_thread_fence(__ATOMIC_ACQUIRE)
```

---

## Planned Attributes

### section("name")

```zer
section(".isr_vector")
u32[64] vectors;

section(".text.startup")
void reset_handler() { ... }
```

Emits `__attribute__((section("name")))`. Pure annotation — doesn't affect safety checks.

### naked

```zer
naked void _hardfault_entry() {
    asm("mrs r0, msp" ::: );
    asm("b hardfault_handler" ::: );
}
```

Emits `__attribute__((naked))`. ZER should reject any non-asm statement inside naked functions.

---

## Extended Inline ASM

For the 2 unavoidable cases (context switch, boot startup), ZER needs extended GCC asm:

```zer
asm("msr msp, %0" : : "r"(stack_top) : "memory");
```

### ZER Validation (Option C — raw GCC syntax with basic checks):

1. **Operand count** — count `%N` in template, verify matches operand count
2. **Type width** — operand variable type width matches register constraint expectation
3. **Volatile implicit** — all ZER asm is volatile (no optimization removal)
4. **Reserved register rejection** — reject sp/bp/pc if used as operand (parse clobber list)
5. **Unused operand** — operand declared but not referenced in template → warning

### What ZER Does NOT Validate (GCC handles):
- Instruction validity
- Register names
- Addressing modes
- Instruction encoding

---

## Comparison: What % of Kernel ASM is Eliminated by Safe Intrinsics

| Kernel operation | Lines of asm | Replaced by ZER intrinsic? |
|-----------------|-------------|--------------------------|
| Atomic add/sub/or/and | ~20 per arch | **Yes** — @atomic_add etc. |
| Compare-and-swap | ~15 per arch | **Yes** — @atomic_cas |
| Spinlock acquire/release | ~10 per arch | **Yes** — @atomic_cas + @barrier |
| Critical section (disable/enable IRQ) | ~5 per arch | **Yes** — @critical { } |
| Memory barriers | ~3 per arch | **Already done** — @barrier() |
| Context switch (save/restore regs) | ~30 per arch | **No** — raw asm needed |
| Syscall entry/exit | ~20 per arch | **No** — raw asm needed |
| Boot startup (set SP, zero BSS) | ~10 per arch | **No** — raw asm needed |
| Exception entry (read fault addr) | ~5 per arch | **Partial** — @probe handles some |

**~80% of kernel asm replaced by safe intrinsics. ~20% remains as raw asm.**

The 20% is concentrated in 3 files: boot.zer, context_switch.zer, syscall.zer. Everything else is pure safe ZER.

---

## Implementation Plan

### Phase 1: @critical { } (~100 lines)
- Parser: new block statement `@critical { body }`
- Checker: validate body, warn on nested @critical, verify ISR-shared vars
- Emitter: emit interrupt disable/enable per arch via `#if defined` blocks
- Tests: 5+ checker + E2E tests

### Phase 2: @atomic_add/sub/or/and/xor (~150 lines)
- Parser: extend NODE_INTRINSIC for @atomic_* with 2 args
- Checker: validate ptr-to-integer first arg, matching type second arg
- Emitter: dual-path emission (native `__atomic_*` or interrupt-disable fallback)
- Tests: 5+ checker + E2E tests

### Phase 3: @atomic_cas, @atomic_load, @atomic_store (~100 lines)
- Same pattern as Phase 2
- Tests: 5+ tests

### Phase 4: Extended asm with validation (~150 lines)
- Parser: capture full GCC asm syntax (template : outputs : inputs : clobbers)
- Checker: operand count, type width, reserved register checks
- Emitter: pass through verbatim
- Tests: 3+ tests

### Phase 5: section() and naked attributes (~80 lines)
- Parser: `section("name")` before declarations, `naked` before functions
- AST: store attribute strings on nodes
- Emitter: prepend `__attribute__((...))` to C output
- Tests: 3+ tests

### Total: ~580 lines for "pure ZER OS kernel" capability.

---

## Design Philosophy

**ZER's approach to unsafe operations:**

1. **Make safe intrinsics for known patterns** — atomics, critical sections, barriers
2. **Validate the interface** for raw asm — operand types, counts, clobbers
3. **Accept that asm contents are unchecked** — same as Rust's `unsafe { asm!() }`
4. **Minimize asm surface area** — safe intrinsics replace ~80% of kernel asm
5. **asm IS the unsafe block** — no separate `unsafe` keyword needed

**ZER does NOT need Rust's `unsafe` keyword because:**
- Raw pointer deref → validated by provenance tracking (17 safety categories)
- MMIO access → validated by mmio declarations + @probe + signal() handler
- FFI → not needed (ZER IS C, cinclude for interop)
- Only asm is truly unchecked — and there's no way to make asm safe in any language

---

## References

- [GCC __atomic Builtins](https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html)
- [Rust Inline Assembly Reference](https://doc.rust-lang.org/reference/inline-assembly.html)
- [Rust RFC 2873 - Inline ASM](https://rust-lang.github.io/rfcs/2873-inline-asm.html)
- [AVR libatomic discussion](https://lists.gnu.org/archive/html/avr-libc-dev/2016-06/msg00000.html)
- [Cortex-M0 atomic issues](https://answers.launchpad.net/gcc-arm-embedded/+question/265649)
- [Slotmap: generation counters](https://electrp.com/posts/slotmap/)
- [Stabilizing naked functions in Rust (2025)](https://blog.rust-lang.org/2025/07/03/stabilizing-naked-functions.html)
- [Redox OS (Rust kernel)](https://www.redox-os.org/)

## Related ZER-LANG Docs
- `docs/compiler-internals.md` — emitter patterns, checker passes, safety analysis
- `docs/safety-roadmap.md` — MMIO safety layers, auto-guard design
- `CLAUDE.md` — language reference, implementation status
- `BUGS-FIXED.md` — 400+ bugs with root causes
