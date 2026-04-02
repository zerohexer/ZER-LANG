# ZER(C) Language — VS Code Extension

Memory-safe C. Same syntax, same mental model — but the compiler prevents buffer overflows, use-after-free, null dereferences, and silent memory corruption.

**Zero setup.** Compiler, LSP, and GCC are bundled. Install the extension, open a `.zer` file, start coding.

## Features

- **Syntax highlighting** — keywords, types, intrinsics, operators
- **LSP diagnostics** — real-time errors and warnings as you type
- **Hover info** — type information on hover
- **Completions** — keywords, intrinsics, types, builtins
- **Bundled compiler** — `zerc` available in VS Code terminal
- **Bundled GCC** — `zerc --run` compiles and executes directly, no toolchain setup

## Quick Start

1. Install the extension
2. Create a file `hello.zer`:

```zer
i32 puts(const *u8 s);

u32 main() {
    puts("Hello, ZER!");
    return 0;
}
```

3. Open VS Code terminal and run:

```
zerc hello.zer --run
```

## What ZER Prevents

| Bug Class | How |
|---|---|
| Buffer overflow | Bounds check on every array/slice access |
| Use-after-free | Handle generation counter (Pool/Slab) |
| Null dereference | `*T` is non-null, `?T` requires unwrap |
| Integer overflow | Wraps (defined), never UB |
| Division by zero | Compile error if divisor not proven nonzero |
| Dangling pointer | Scope escape analysis at compile time |
| Silent truncation | Must use `@truncate` or `@saturate` explicitly |

## Syntax Differences from C

```zer
// Braces ALWAYS required
if (x > 5) { return 1; }

// No ++ or --
i += 1;

// Array size between type and name
u8[256] buffer;

// Pointer: star before type
*Task ptr = &task;

// Optional unwrap
u32 val = get_value() orelse return;

// Enum dot syntax
State s = State.idle;
switch (s) {
    .idle => { start(); }
    .running => { poll(); }
}

// Safe allocation — no malloc/free
Pool(Task, 8) tasks;           // fixed pool
Slab(Task) dynamic_tasks;      // growable pool
Handle(Task) h = tasks.alloc() orelse return;
tasks.get(h).id = 42;          // generation-checked access
tasks.free(h);                 // safe free, get() traps after
```

## Configuration

| Setting | Default | Description |
|---|---|---|
| `zer.lspPath` | (bundled) | Path to `zer-lsp` executable. Leave empty to use bundled. |
| `zer.lspArgs` | `[]` | Additional arguments for `zer-lsp` |

## Links

- [GitHub](https://github.com/zerohexer/ZER-LANG)
- [Language Specification](https://github.com/zerohexer/ZER-LANG/blob/main/ZER-LANG.md)
- License: MPL-2.0
