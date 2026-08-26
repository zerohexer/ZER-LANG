# ZER

This is the main source code repository for ZER. It contains the compiler, the
language server, and the documentation.

ZER is a systems language for embedded and low-level work. It keeps C's syntax and
mental model — same types, same pointers, same hardware access — and compiles to
portable C99.

**Note: this README is for _users_ rather than _contributors_.**

## Why ZER?

- **Performance:** No garbage collector, no runtime, no hidden allocation. Compiles to
  C99 and hands the result to GCC, so every target GCC supports is a target ZER
  supports. Safety checks that can be proven at compile time emit no code at all.

- **Reliability:** Every wrong use of a value in ZER source is caught at the use site —
  bounds, lifetime, ownership, provenance, qualifiers, context. Each one is a compile
  error or a runtime trap; nothing corrupts silently. There is no `unsafe` keyword to
  step outside it.

- **Ease of use:** If you can read C, you can read ZER. No lifetime annotations, no
  borrow syntax — the compiler infers what it needs. Allocators, threads, optionals and
  bit-level hardware access are in the language rather than in a library.

## Quick Start

Requires GCC.

```bash
make                                # build the zerc compiler
./zerc hello.zer --run              # compile and run
./zerc hello.zer -o hello.c         # emit C instead
```

```zer
i32 printf(const *u8 fmt, ...);

u32 main() {
    u8[4] buf;
    for (u32 i = 0; i < 4; i += 1) { buf[i] = (u8)(i * 2); }
    printf("%d\n", (i32)buf[3]);
    return 0;
}
```

## Building from source

```bash
make            # zerc
make zer-lsp    # language server
make release    # release binaries in release/
make check      # the full test suite
```

`make docker-check` runs the same suite in a container, which is the supported path on
Windows.

## Editor support

`zer-lsp` speaks LSP — diagnostics, hover, go-to-definition, completion and document
symbols — and works with VS Code, Neovim, Emacs, Helix and Zed. The VS Code extension
lives in [`editors/vscode/`](editors/vscode/).

## Documentation

- [`docs/reference.md`](docs/reference.md) — the language reference. Every example in it
  is compiled by the test suite, so it cannot drift from the compiler.
- [`docs/limitations.md`](docs/limitations.md) — known gaps, in the open.
- [`examples/`](examples/) — including bare-metal ARM Cortex-M3 firmware for QEMU, and
  CVE reproductions built side by side in C and ZER.

## What ZER does not claim

ZER covers **program** consequences: what happens when a value is used wrongly in ZER
source. It does not verify **hardware** consequences — whether a peripheral behaves as
its datasheet says, or whether a register value is right for your board. No language
verifies that, and ZER surfaces those boundaries rather than hiding them.

Values arriving through `cinclude` from hand-written C are outside the language, and so
is deadlock: ZER rejects the lock-ordering shapes it can see, but liveness in general is
out of scope here exactly as it is elsewhere.

## License

Mozilla Public License 2.0 with Runtime Exception.

**zerc** is MPL-2.0: modify one of its source files and distribute it, and that file
stays MPL-2.0. Files you add alongside it are yours.

**Firmware compiled by zerc is yours.** The emitted C and the resulting binaries carry
no license inheritance.

"ZER" and "ZER-LANG" are trademarks of ZEROHEXER.

Copyright 2026 ZEROHEXER (zerohexer@gmail.com).
