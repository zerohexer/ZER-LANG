# ZER-LANG

**Systems programming language targeting embedded and low-level development, GCC/C first.**

ZER is memory-safe C. Same syntax, same mental model, same hardware access — but the compiler prevents buffer overflows, use-after-free, null dereferences, and silent memory corruption.

## Build

Requires GCC (MinGW on Windows, gcc on Linux/Mac):

```bash
make           # build zerc compiler
make zer-lsp   # build language server
make check     # run all 3,200+ tests
make release   # build release binaries in release/
```

## Usage

```bash
./zerc input.zer -o output.c       # emit C
./zerc input.zer --run             # emit C → GCC → run
./zerc input.zer --lib -o lib.c    # emit without runtime (for C interop)
gcc -std=c99 -o program output.c   # compile emitted C
```

Multi-file with imports:

```bash
# main.zer imports uart.zer which imports gpio.zer
# zerc resolves all imports automatically
./zerc main.zer -o firmware.c
arm-none-eabi-gcc -std=c99 -o firmware.elf firmware.c
```

Emit bare C without ZER preamble (`--lib` strips runtime helpers like bounds checks and trap handlers — compile-time safety is still fully enforced):

```bash
./zerc module.zer --lib -o module.c
```

Include C headers directly with `cinclude`:

```zer
cinclude "stm32f4xx_hal.h";   // C header — passed through to GCC
import uart;                   // ZER module — full safety pipeline

// Call C functions with ZER type safety at the boundary
void HAL_GPIO_WritePin(*opaque port, u16 pin, u32 state);
```

Link with any C library — the output is plain C:

```bash
# ZER + STM32 HAL + FreeRTOS — just link the .c files
arm-none-eabi-gcc -std=c99 -o firmware.elf \
    firmware.c \
    Drivers/STM32F4xx_HAL_Driver/Src/*.c \
    -I Drivers/STM32F4xx_HAL_Driver/Inc \
    -T STM32F4.ld
```

## Language Overview

ZER uses C syntax. A C developer can read ZER in 5 minutes.

```c
// Types — same as C, explicit sizes
u32 x = 5;
i64 big = 100000;
bool flag = true;
f32 pi = 3.14;

// Functions — return type first
u32 add(u32 a, u32 b) {
    return a + b;
}

// Structs — same as C
struct Task {
    u32 pid;
    u32 priority;
}

// Pointers — non-null by default
*Task t = &my_task;       // always valid
?*Task maybe = find();    // might be null — ? means optional
```

### What ZER Changes From C

```
u32[10] arr;              // all zeroed automatically
arr[i] = 5;              // bounds-checked — traps on overflow
u32 x;                   // x = 0, always — no uninitialized memory
*Task t = get_task();    // can never be null — compiler guarantees
u32 y = x + 1;          // wraps on overflow — defined, never UB
i16 s = big;             // COMPILE ERROR — must @truncate or @saturate
```

### Optional Types and Error Handling

```c
?u32 uart_read([]u8 buf) {
    if (no_data()) return null;
    return bytes_read;
}

// Caller MUST handle null:
u32 n = uart_read(buf) orelse return;   // propagate failure
u32 n = uart_read(buf) orelse 0;        // default value

if (uart_read(buf)) |n| {
    process(buf[0..n]);                  // n guaranteed valid
}
```

### Memory Builtins — No Heap Required

```c
// Fixed-count pool — compile-time bound (embedded, real-time)
Pool(Task, 8) tasks;                     // 8 slots in static RAM
Handle(Task) h = tasks.alloc() orelse return;
tasks.get(h).priority = 5;
tasks.free(h);
// use-after-free caught at compile time (ZER-CHECK)
// or runtime trap (generation counter)

// Dynamic slab — grows on demand (x86_64, servers)
Slab(Connection) conns;                  // no limit, pages allocated as needed
Handle(Connection) c = conns.alloc() orelse return;
conns.get(c).fd = new_fd;
conns.free(c);                           // same safety as Pool

Ring(u8, 256) uart_rx;                   // circular buffer
uart_rx.push(byte);
if (uart_rx.pop()) |b| { process(b); }

defer mutex_unlock(&lock);              // runs at scope exit
```

### Hardware Support

```c
// Memory-mapped registers
volatile *u32 reg = @inttoptr(*u32, 0x4002_0014);
*reg = 0xFF;

// Bit extraction
u32 mode = reg[9..8];    // extract bits 9:8

// Interrupt handlers
interrupt USART1 {
    uart_rx.push(UART_DR);
}

// Packed structs for protocol parsing
packed struct Packet {
    u8 id;
    u16 value;       // unaligned — ZER handles it safely
    u8 checksum;
}
```

## Safety Guarantees

ZER guarantees **zero silent memory corruption**. Every memory bug is either a compile error or a runtime trap. Nothing corrupts silently.

| Bug Class | How ZER Prevents It |
|---|---|
| Buffer overflow | Inline bounds checking on every array/slice access (conditions, loops, all expressions) |
| Use-after-free | Handle consumption tracking (compile-time, with alias detection) + generation counter (runtime) |
| Null dereference | Non-null `*T` by default, `?T` requires unwrapping |
| Uninitialized memory | Everything auto-zeroed |
| Integer overflow | Defined wrapping (never undefined behavior) |
| Silent truncation | Must use `@truncate` or `@saturate` explicitly |
| Type confusion | No implicit narrowing or sign conversion |
| Missing switch case | Exhaustive checking for enums and bools |
| Dangling pointer | Scope escape analysis (`return &local`, `global.ptr = &local` = compile error) |
| Union type confusion | Cannot mutate union variant during mutable switch capture |
| Arena pointer escape | Arena-derived pointers cannot be stored in global/static variables |
| Wrong pool | ZER-CHECK detects handle used on wrong pool |
| Division by zero | Forced guard — compile error if divisor not proven nonzero |
| Thread data race | `shared struct` auto-locked, non-shared pointer to `spawn` = compile error |
| Deadlock | Lock ordering on shared structs — compile error (Rust has NO deadlock detection) |
| Thread not joined | Scoped spawn `ThreadHandle` not joined = compile error |
| Handle leak | Allocated but never freed = compile error |
| Interior pointer UAF | `*u32 p = &b.field; free(b); p[0]` = compile error |
| Wrong pointer cast | `@ptrcast` provenance tracking — wrong type = compile error |

### Safety Passes

```
source.zer → LEXER → PARSER → TYPE CHECKER → ZER-CHECK → C EMITTER → GCC

TYPE CHECKER catches:     ~95% of bugs at compile time
ZER-CHECK catches:        additional ~4% (handle bugs, wrong pool)
RUNTIME TRAPS catch:      remaining ~1% (generation counter, bounds)
SILENT CORRUPTION:        0%. impossible. never.
```


### Concurrency — Full Rust Parity

```c
// Auto-locked shared data — no Arc<Mutex<T>> boilerplate
shared struct Counter { u32 value; }
Counter g;
g.value += 1;                              // auto: lock → write → unlock

// Thread creation with safety enforcement
spawn worker(&g);                          // OK — shared struct, auto-locked
ThreadHandle th = spawn compute(&data);    // scoped — allows *T
th.join();                                 // MUST join (compile error if not)

// Condvar synchronization
@cond_wait(g, g.value > 0);               // wait for condition
@cond_signal(g);                           // wake waiter

// Stackless coroutines — zero heap, zero runtime
async void blink() {
    while (true) { led_on(); yield; led_off(); yield; }
}

// Reader-writer locks
shared(rw) struct Config { u32 port; }     // auto rdlock/wrlock

// Thread-local, init-once, barriers, atomics
threadlocal u32 counter;
@once { init_hardware(); }
@atomic_add(&flag, 1);
```

## Tests

3,200+ tests across all dimensions, including 415 tests translated from Rust's test suite:

```
Lexer:                        218 tests
Parser:                       168 tests
Type Checker:                 584 tests
ZER-CHECK:                     54 tests
C Emitter:                    238 end-to-end tests
Firmware Patterns (4 rounds): 116 end-to-end tests
Parser Fuzz:                  491 adversarial inputs
Semantic Fuzzer:              200 tests/run (32 generators)
Module Imports:                23 patterns
ZER Integration:              120+ .zer E2E tests
Rust-Equivalent:              415 tests (from Rust's tests/ui/)
Conversion Tools:             139 tests
──────────────────────────────────────────────────
Total:                       3,200+ tests, all passing
```

461 compiler bugs found and fixed. 415 Rust-equivalent tests covering threads, UAF, bounds, division, null safety, narrowing, scope escape, defer, handles, *opaque provenance, async, atomics, condvar, shared struct, and deadlock detection.

## Editor Support

LSP server (`zer-lsp`) provides diagnostics, hover, go-to-definition, completion, and document symbols for any editor:

```bash
make zer-lsp    # build the language server
```

VS Code extension in `editors/vscode/` with full syntax highlighting.

Works with: VS Code, Neovim (nvim-lspconfig), Emacs (eglot/lsp-mode), Helix, Zed.

## Status: v0.3.0

ZER compiles to C99 and runs on any target GCC supports.

**Full concurrency model** — shared struct (auto-locked), spawn (fire-and-forget + scoped), condvar, RwLock, threadlocal, @once, @barrier, async/await (stackless coroutines), atomics, compile-time deadlock detection. Passes 415 tests translated from Rust's test suite.

**Proven on real hardware:** ARM Cortex-M3 firmware running on QEMU — bounds-checked arrays, optional types, exhaustive enums — all freestanding, no OS, no libc, 1225 bytes total. See [`examples/qemu-cortex-m3/`](examples/qemu-cortex-m3/).

**Proven against real CVEs:** Heartbleed (CVE-2014-0160) and Baron Samedit (CVE-2021-3156) reproduced side-by-side — C silently leaks memory, ZER traps at the bounds check. See [`examples/cve-demos/`](examples/cve-demos/).

**3,200+ tests, 461 bugs fixed, zero failures.**

## License

Mozilla Public License 2.0 with Runtime Exception.

**zerc (the compiler)** is MPL-2.0. If you modify a zerc source file and distribute it, you must share that file under MPL-2.0. New files you add alongside zerc can be under any license — your proprietary chip support, custom backends, and hardware-specific additions stay yours.

**Firmware compiled BY zerc** is yours. No license inheritance. The emitted C code and compiled binaries are not covered by MPL-2.0. Compile proprietary firmware freely.

"ZER" and "ZER-LANG" are trademarks of ZEROHEXER.

Copyright 2026 ZEROHEXER (zerohexer@gmail.com).
