# ASM & Low-Level Safety in ZER-LANG — Full Context Dump

> **Syntax update (2026-04-23):** `asm(...)` keyword renamed to `unsafe asm(...)`. The `unsafe` marker is **required** — bare `asm(...)` is rejected with a compile error (Rust-style explicit escape hatch marker). Phase 1 verified rule (`zer_asm_allowed_in_context`) unchanged — naked-function restriction still applies. See `docs/asm_plan.md` for full context.

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

## MISRA Dir 4.3 Enforcement: ASM Isolation Rule (decided 2026-04-01)

### The Rule

**MISRA C:2023 Directive 4.3:** "Assembly language shall be encapsulated and isolated."

ZER enforces this at compile time. No other language does this.

**asm statements are ONLY allowed inside `naked` functions.** Regular functions cannot contain asm. This is a compile error, not a warning.

```zer
// REJECTED — asm mixed with regular code:
void process_data() {
    u32 x = 5;
    asm("cpsid i" :::);      // COMPILE ERROR: asm only allowed in naked functions
    x += 1;
    asm("cpsie i" :::);      // COMPILE ERROR
}

// ACCEPTED — asm isolated in naked function:
naked void context_switch(*Context old, *Context new) {
    asm("stmia %0, {r0-r12, sp, lr}" : : "r"(old) : "memory");
    asm("ldmia %0, {r0-r12, sp, lr}" : : "r"(new) : "memory");
    asm("bx lr" :::);
}

// ACCEPTED — naked boot startup:
naked void _start() {
    asm("ldr sp, =_stack_top" :::);
    asm("bl main" :::);
}

// Safe ZER code calls the isolated asm function:
void schedule() {
    *Context old = current_task();    // full safety — bounds, null, escape
    *Context new = next_task();       // full safety
    context_switch(old, new);          // type-checked call to asm function
}
```

### What This Means

| Code pattern | Allowed? | MISRA compliant? | Safety |
|---|---|---|---|
| `@critical { }` | Yes | Yes — asm encapsulated in intrinsic | 100% safe |
| `@atomic_add(&x, 1)` | Yes | Yes — asm encapsulated in intrinsic | 100% safe |
| `@barrier()` | Yes | Yes — asm encapsulated in intrinsic | 100% safe |
| `naked void f() { asm(...); }` | Yes | Yes — dedicated asm function | Interface type-checked |
| `void f() { asm(...); x += 1; }` | **No — compile error** | N/A — rejected | N/A |
| `void f() { u32 x = 5; }` | Yes | Yes — no asm | 100% safe |

### Zero Functionality Loss

The rule restricts WHERE you write asm, not WHAT you write. Inside a `naked` function, you can write ANY instruction, ANY register operation, ANY architecture-specific code. The asm contents are not restricted.

The call INTERFACE between safe ZER code and naked asm functions IS type-checked:
- Argument types validated (pointer width, integer size)
- Argument count validated
- Return type validated
- The naked function's parameters and return type are ZER types

### naked Function Rules

1. Body must contain ONLY `asm(...)` statements — no variable declarations, no expressions, no if/for/while
2. No automatic prologue/epilogue (GCC `__attribute__((naked))`)
3. Parameters are accessible via asm operands, NOT as regular variables
4. Caller's argument types are validated by ZER's type checker
5. If a naked function needs local variables, use register operands — not stack

### Why This Is Better Than Rust

| | Rust | ZER |
|---|---|---|
| asm location | `unsafe { asm!() }` — anywhere | Only in `naked` functions — isolated |
| asm + regular code mixing | Allowed inside `unsafe` block | **Compile error** — must separate |
| MISRA compliance | Not enforced | **Enforced at language level** |
| Type checking on operands | Yes — per register class | Yes — per ZER type width |
| Call interface validation | Via function signature | Via function signature (same) |

Rust allows `unsafe { asm!("..."); let x = 5; asm!("..."); }` — asm mixed with regular code inside one function. ZER rejects this. Asm functions are asm-only. Regular functions are asm-free. Clean separation.

### Example: Complete OS Kernel File Structure

```
kernel/
  boot.zer           ← 1 naked function: _start (set SP, jump to main)
  context.zer         ← 1 naked function: context_switch (save/restore regs)
  syscall_entry.zer   ← 1 naked function: syscall_entry (read args, dispatch)
  scheduler.zer       ← pure ZER: @critical, @atomic_cas, full safety
  memory.zer          ← pure ZER: Pool/Slab/Arena, full safety
  drivers/
    uart.zer          ← pure ZER: mmio + @inttoptr + interrupt, full safety
    gpio.zer          ← pure ZER: mmio + volatile, full safety
    spi.zer           ← pure ZER: mmio + Ring, full safety
  ipc.zer             ← pure ZER: Ring + slices, full safety
  fs.zer              ← pure ZER: Arena + structs, full safety
```

3 files with `naked` functions (~60 lines of asm total). Everything else is fully checked ZER. No C files. No cinclude. Pure ZER OS kernel.

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

## Critical Implementation Context for Fresh Sessions

### Current Compiler State (2026-04-01)
- **14,000+ lines** of compiler code (lexer.c, parser.c, ast.h, types.c, checker.c, emitter.c, zercheck.c)
- **checker.c: 6,200+ lines** — the largest file. Type checking, escape analysis, range propagation, ISR safety, stack depth, provenance tracking
- **emitter.c: 3,800+ lines** — C code generation, preamble with runtime helpers
- **557 checker + 238 E2E + 50 zercheck + 139 convert = 1,700+ tests**
- **11 audit rounds this session, 15 bugs fixed, convergence reached**

### Escape Analysis Walker — MOST CRITICAL FUNCTION
`call_has_local_derived_arg` (checker.c ~line 383) has **9 cases**. This is the most-patched function. When implementing asm features, if a new expression type can carry a pointer (e.g., asm output operand), it MUST be added here.

### Emitter Preamble Pattern
All runtime helpers are emitted in `emit_file()` before user code. Pattern:
1. Type typedefs (`_zer_opt_u32`, `_zer_slice_u8`, etc.)
2. Trap function (`_zer_trap`)
3. Fault handler (`_zer_fault_handler` via `signal()`)
4. @probe (`_zer_probe` via `setjmp`/`longjmp`)
5. Shift macros (`_zer_shl`, `_zer_shr`)
6. Bounds check (`_zer_bounds_check`)
7. Pool/Slab/Ring runtime helpers
8. MMIO startup validation (`_zer_mmio_validate`)

New intrinsics (@critical, @atomic_*) should add their helpers HERE — between step 7 and 8.

### Parser Pattern for New Intrinsics
Intrinsics are parsed as `NODE_INTRINSIC` with name string. Parser detects `@` token, reads name, parses args. New intrinsics like `@critical`, `@atomic_add` follow this pattern. `@critical { body }` is special — it's a BLOCK intrinsic, not an expression. May need a new NODE_CRITICAL or extend NODE_INTRINSIC with a body field.

### Checker Pattern for New Intrinsics
NODE_INTRINSIC handler in `check_expr` (checker.c ~line 3280). Each intrinsic has its own validation block. Add new blocks for @atomic_* (validate ptr-to-integer first arg, matching type second arg). @critical would be in `check_stmt` instead (it's a statement, not expression).

### Emitter Pattern for New Intrinsics
NODE_INTRINSIC handler in `emit_expr` (emitter.c ~line 1860). Each intrinsic emits specific C code. @atomic_* emits `#if defined` blocks with dual path (native `__atomic_*` or interrupt-disable fallback). @critical emits interrupt disable/enable wrapper.

### GCC Flags
Emitted C requires `-fwrapv -fno-strict-aliasing`. `zerc --run` adds these. Tests compile with these flags.

### Docker Testing
`make docker-check` — builds + runs ALL tests in gcc:13 container. `docker build --no-cache` required after emitter.c changes (Docker layer caching can miss file changes that only modify emitted C patterns).

### Files NOT to Modify Without Reading compiler-internals.md First
- checker.c — 6,200+ lines, 100+ bug fix patterns documented
- emitter.c — 3,800+ lines, critical emission patterns (optional handling, bounds checks, etc.)
- types.c — type_width, type_equals, coercion rules
- zercheck.c — path-sensitive handle analysis (not integrated into zerc yet)

## Related ZER-LANG Docs
- `docs/compiler-internals.md` — emitter patterns, checker passes, safety analysis, ALL bug fix patterns
- `docs/safety-roadmap.md` — MMIO safety layers, auto-guard design, @probe design
- `CLAUDE.md` — language reference, implementation status, ZER syntax rules
- `BUGS-FIXED.md` — 415+ bugs with root causes
- `docs/ASM_ZER-LANG.md` — THIS FILE — asm design, safe intrinsics, MISRA enforcement
