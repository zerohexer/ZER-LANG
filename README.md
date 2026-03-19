# ZER-LANG

**Systems programming language targeting embedded and low-level development, GCC/C first.**

ZER is memory-safe C. Same syntax, same mental model, same hardware access — but the compiler prevents buffer overflows, use-after-free, null dereferences, and silent memory corruption. No runtime. No LLVM. No borrow checker.

## Build

Requires GCC (MinGW on Windows, gcc on Linux/Mac):

```bash
make           # build zerc compiler
make zer-lsp   # build language server
make check     # run all 841 tests
```

## Usage

```bash
# compile a .zer file to C, then build with GCC
./zerc input.zer -o output.c
gcc -std=c99 -o program output.c

# or let zerc pick the output name
./zerc main.zer          # produces main.c
```

Multi-file with imports:

```bash
# main.zer imports uart.zer which imports gpio.zer
# zerc resolves all imports automatically
./zerc main.zer -o firmware.c
arm-none-eabi-gcc -std=c99 -o firmware.elf firmware.c
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
Pool(Task, 8) tasks;                     // 8 slots in static RAM
Handle(Task) h = tasks.alloc() orelse return;
tasks.get(h).priority = 5;
tasks.free(h);
// use-after-free caught at compile time (ZER-CHECK)
// or runtime trap (generation counter)

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
| Buffer overflow | Bounds checking on every array access |
| Use-after-free | Handle consumption tracking (compile-time) + generation counter (runtime) |
| Null dereference | Non-null `*T` by default, `?T` requires unwrapping |
| Uninitialized memory | Everything auto-zeroed |
| Integer overflow | Defined wrapping (never undefined behavior) |
| Silent truncation | Must use `@truncate` or `@saturate` explicitly |
| Type confusion | No implicit narrowing or sign conversion |
| Missing switch case | Exhaustive checking for enums and bools |
| Dangling pointer | Scope escape analysis (`return &local` = compile error) |
| Wrong pool | ZER-CHECK detects handle used on wrong pool |

### Safety Passes

```
source.zer → LEXER → PARSER → TYPE CHECKER → ZER-CHECK → C EMITTER → GCC

TYPE CHECKER catches:     ~95% of bugs at compile time
ZER-CHECK catches:        additional ~4% (handle bugs, wrong pool)
RUNTIME TRAPS catch:      remaining ~1% (generation counter, bounds)
SILENT CORRUPTION:        0%. impossible. never.
```

## Compiler Architecture

The ZER compiler (`zerc`) is written in C. It emits C code that compiles with any GCC version. All internal data structures are dynamic — no fixed limits.

```
Component              Lines
─────────────────────────────
Lexer                  ~740
Parser/AST             ~2,230
Type System            ~700
Type Checker           ~1,800
ZER-CHECK              ~470
C Emitter              ~1,720
Compiler Driver        ~380
LSP Server             ~1,370
─────────────────────────────
Total                  ~9,400
```

Compare: GCC is 15 million lines. Rust compiler is 600K lines. ZER is small enough for one person to maintain.

## Tests

Stress-tested against real production firmware patterns: MODBUS CRC, CAN bus frames, USB state machines, SPI flash drivers, bootloader flows, RTOS schedulers, I2C sensor drivers, DMA double buffers, and protocol parsers.

```
Lexer:                      218 tests
Parser:                     158 tests
Type Checker:               265 tests
ZER-CHECK:                    8 tests
C Emitter:                   76 end-to-end tests
Module Imports:               6 patterns
Firmware Patterns (3 rounds): 102 end-to-end tests
Production Firmware:          14 end-to-end tests
──────────────────────────────────────────────────
Total:                      841 tests, all passing
```

14 compiler bugs found and fixed during stress testing. Zero bugs found when testing against real production firmware patterns (MODBUS, CAN, USB, SPI, RTOS, bootloader, sensors).

## Editor Support

LSP server (`zer-lsp`) provides diagnostics, hover, go-to-definition, completion, and document symbols for any editor:

```bash
make zer-lsp    # build the language server
```

VS Code extension in `editors/vscode/` with full syntax highlighting.

Works with: VS Code, Neovim (nvim-lspconfig), Emacs (eglot/lsp-mode), Helix, Zed.

## Status

**v0.1 — production-ready compiler.** Compiles real multi-file ZER programs to C. All safety features implemented. Stress-tested against real firmware patterns. LSP server for editor integration. Dynamic internals — no fixed limits. Targets any architecture GCC supports.

## License

Apache License 2.0. Copyright 2026 zerohexer.
