# ZER

Language support for [ZER](https://github.com/zerohexer/ZER-LANG) — a systems language
for embedded and low-level work that keeps C's syntax and compiles to portable C99.

**Zero setup.** The compiler, the language server and a portable GCC are bundled.
Install, open a `.zer` file, start writing.

## Why ZER?

- **Performance:** No garbage collector, no runtime, no hidden allocation. Compiles to
  C99 and hands the result to GCC, so every target GCC supports is a target ZER
  supports. Checks that can be proven at compile time emit no code at all.

- **Reliability:** Every wrong use of a value in ZER source is caught at the use site —
  bounds, lifetime, ownership, provenance, qualifiers, context. Each one is a compile
  error or a runtime trap; nothing corrupts silently. There is no `unsafe` keyword to
  step outside it.

- **Ease of use:** If you can read C, you can read ZER. No lifetime annotations, no
  borrow syntax — the compiler infers what it needs.

## Quick start

Create `hello.zer`:

```zer
i32 printf(const *u8 fmt, ...);

u32 main() {
    u8[4] buf;
    for (u32 i = 0; i < 4; i += 1) { buf[i] = (u8)(i * 2); }
    printf("%d\n", (i32)buf[3]);
    return 0;
}
```

Then, in a terminal:

```bash
zerc hello.zer --run        # compile and run
zerc hello.zer -o hello.c   # emit C instead
```

## What this extension gives you

Syntax highlighting, and a language server providing diagnostics as you type, hover,
go-to-definition, completion and document symbols.

**ZER: Open Language Reference** in the command palette opens the full reference.

## Settings

| Setting | Default | Description |
|---|---|---|
| `zer.lspPath` | *(bundled)* | Path to `zer-lsp`. Leave empty to use the bundled one. |
| `zer.lspArgs` | `[]` | Extra arguments for `zer-lsp`. |

If the language server does not start, `zer.lspPath` is the first thing to check — a
stale path left over from an earlier install overrides the bundled binary.

## Links

- [GitHub](https://github.com/zerohexer/ZER-LANG)
- [Language reference](https://github.com/zerohexer/ZER-LANG/blob/main/docs/reference.md)
- [Known limitations](https://github.com/zerohexer/ZER-LANG/blob/main/docs/limitations.md)

License: MPL-2.0 with Runtime Exception. Firmware you compile with ZER is yours — the
emitted C and the resulting binaries carry no license inheritance.
