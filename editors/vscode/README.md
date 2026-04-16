# ZER(C) Language — VS Code Extension

Memory-safe C. Same syntax, same mental model — but the compiler prevents buffer overflows, use-after-free, null dereferences, and silent memory corruption. Compiles to C, then GCC handles backends.

**Zero setup.** Compiler, LSP, and portable GCC are bundled. Install the extension, open a `.zer` file, start coding.

## Features

- **Syntax highlighting** — keywords, types, intrinsics, operators, string literals
- **LSP diagnostics** — real-time errors and warnings as you type
- **Hover info** — type information on hover
- **Go-to-definition** — jump to function/struct/enum declarations
- **Completions** — keywords, intrinsics (`@ptrcast`, `@size`, etc.), types, builtins
- **Bundled compiler** — `zerc` available from VS Code terminal
- **Bundled GCC** — `zerc --run` compiles and executes directly, no toolchain setup
- **Cross-platform** — Windows (bundled w64devkit GCC), Linux, macOS

## Quick Start

1. Install the extension
2. Create `hello.zer`:

```zer
i32 puts(const *u8 s);

u32 main() {
    puts("Hello, ZER!");
    return 0;
}
```

3. Open VS Code terminal: `zerc hello.zer --run`

## What ZER Prevents — at Compile Time

| Bug Class | Prevention | Cost |
|---|---|---|
| Buffer overflow | Bounds check on every array/slice access (proven-safe = zero overhead) | ~0-4% |
| Use-after-free | Handle generation counter + compile-time zercheck | 0% (compile) |
| Null dereference | `*T` non-null by default, `?T` requires unwrap | 0% |
| Integer overflow | Wraps (defined behavior), never UB | 0% |
| Division by zero | Compile error if divisor not proven nonzero | 0% |
| Dangling pointer | Scope escape analysis + zercheck | 0% (compile) |
| Silent truncation | Must use `@truncate` or `@saturate` explicitly | 0% |
| Data race | `shared struct` auto-locking, `spawn` arg validation | minimal |
| Deadlock | Compile-time lock ordering analysis | 0% (compile) |
| Handle leak | zercheck: alloc without free = compile error | 0% (compile) |
| Wrong pointer cast | 4-layer provenance tracking | 0% (compile) |
| Stack overflow | `--stack-limit N` call chain analysis | 0% (compile) |

## Syntax — C Developers Feel at Home

```zer
// Types — explicit sizes, no ambiguity
u8 byte = 42;
u32 count = 0;
f64 pi = 3.14159;
bool done = false;

// Pointers — non-null by default
*Task ptr = &my_task;        // guaranteed non-null
?*Task maybe = null;         // optional — might be null

// Arrays — size between type and name
u8[256] buffer;
[*]u8 slice = "hello";      // slice with .ptr and .len

// Structs — no 'struct' keyword in usage
struct Motor { u32 speed; bool active; }
Motor m;
m.speed = 100;

// Enums — dot syntax, exhaustive switch
enum State { idle, running, done }
switch (state) {
    .idle => { start(); }
    .running => { poll(); }
    .done => { cleanup(); }
}
```

## Safe Memory — No malloc, No Free, No GC

```zer
// Pool — fixed-size, compile-time, works on bare metal
Pool(Task, 8) tasks;
Handle(Task) h = tasks.alloc() orelse return;
tasks.get(h).priority = 5;
defer tasks.free(h);           // zercheck: leak prevented

// Slab — dynamic, grows on demand
Slab(Connection) conns;
*Connection c = conns.alloc_ptr() orelse return;
defer conns.free_ptr(c);

// Arena — bump allocator, bulk reset
u8[4096] mem;
Arena scratch = Arena.over(mem);
?*Sensor s = scratch.alloc(Sensor) orelse return;
defer scratch.reset();

// Ring — circular buffer (ISR-safe push)
Ring(u8, 256) uart_rx;
uart_rx.push(byte);
u8 b = uart_rx.pop() orelse return;
```

## Concurrency — Built Into the Language

```zer
// Shared struct — auto-locked, no annotations
shared struct Counter { u32 value; }
Counter g;
g.value += 1;              // auto: lock → write → unlock

// Spawn — fire-and-forget or scoped
spawn worker(&g);                        // auto-locked args
ThreadHandle th = spawn compute(&data);  // scoped — must join
th.join();

// Async — stackless coroutines
async void sensor_poll() {
    yield;                  // suspend, resume later
    await data_ready;       // yield until condition
}

// Condvar, semaphore, barrier — all built-in
@cond_wait(shared_var, shared_var.count > 0);
@sem_acquire(slots);
@barrier_wait(sync_point);
```

## Hardware Support — Embedded First

```zer
// MMIO registers
mmio 0x40020000..0x40020FFF;
volatile *u32 reg = @inttoptr(*u32, 0x40020014);

// Interrupt handlers
interrupt UART_RX { uart_rx.push(UART_DR); }

// Critical sections
@critical { sensitive_operation(); }

// Inline assembly
asm("cpsid i");   // ARM: disable interrupts

// Bit extraction
u32 status = reg[7..4];    // extract bits 7 down to 4
```

## Error Handling — ?T + orelse

```zer
// Optional return — must handle
?u32 parse([]u8 input) {
    if (input.len == 0) { return null; }
    return compute(input);
}

// Caller must unwrap
u32 val = parse(data) orelse return;     // propagate failure
u32 val = parse(data) orelse 0;          // default value

// If-unwrap
if (parse(data)) |v| {
    use(v);                              // v guaranteed valid
}
```

## C Interop — Use Any C Library

```zer
cinclude "<stdio.h>";
i32 printf(const *u8 fmt);

cinclude "my_lib.h";
*opaque lib_create();
void lib_destroy(*opaque handle);
u32 lib_read(*opaque handle);

u32 main() {
    *opaque h = lib_create();
    defer lib_destroy(h);     // zercheck: tracks *opaque lifecycle
    return lib_read(h);
}
```

## Compiler Flags

| Flag | Description |
|---|---|
| `zerc file.zer --run` | Compile and execute |
| `zerc file.zer --emit-c` | Emit C source (keep .c file) |
| `zerc file.zer -o out` | Compile to executable |
| `--stack-limit N` | Error when stack exceeds N bytes |
| `--no-strict-mmio` | Allow @inttoptr without mmio declarations |
| `--target-bits N` | Override pointer width (32/64) |
| `--gcc PATH` | Specify cross-compiler GCC path |

## Configuration

| Setting | Default | Description |
|---|---|---|
| `zer.lspPath` | (bundled) | Path to `zer-lsp` executable. Leave empty to use bundled. |
| `zer.lspArgs` | `[]` | Additional arguments for `zer-lsp` |

## v0.4.0 Changelog

- **MIR-inspired IR emission pipeline** — 100% IR for function bodies
- IR is now the default emission path (`--no-ir` for AST fallback)
- Cross-module function and variable name mangling
- `emit_builtin_inline` — pool/slab/ring/arena/Task builtins from local IDs
- `emit_rewritten_node` — all expression types emitted directly, zero `emit_expr`
- 25 bug fixes (BUG-513 through BUG-537)
- 196 ZER + 786 Rust-equivalent + 36 Zig + 28 module tests passing

## Links

- [GitHub](https://github.com/zerohexer/ZER-LANG)
- [Language Reference](https://github.com/zerohexer/ZER-LANG/blob/main/docs/reference.md)
- [Language Specification](https://github.com/zerohexer/ZER-LANG/blob/main/ZER-LANG.md)
- License: MPL-2.0
