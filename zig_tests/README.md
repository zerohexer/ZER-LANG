# ZER Zig-Equivalent Test Suite

22 tests (17 positive, 5 negative), 0 failures. Updated 2026-04-11.
Runner: `run_tests.sh` — same as rust_tests runner.

## How to Use This File

**Adding tests:** Drop `.zer` files in this directory. Runner picks them up automatically.
**Negative tests:** Add `// EXPECTED: compile error` as first or second line.
**Naming:** `zt_<category>_<description>.zer` for Zig-translated.

## Coverage by Feature

| Feature | Positive | Negative | Total | Key Test Prefixes |
|---|---|---|---|---|
| Container (monomorphization) | 6 | 2 | 8 | zt_container_ |
| Designated initializers | 4 | 2 | 6 | zt_desig_ |
| Comptime array/struct | 4 | 1 | 5 | zt_comptime_ |
| do-while | 3 | 0 | 3 | zt_do_while_ |

## Zig -> ZER Pattern Translation

| Zig Pattern | ZER Equivalent |
|---|---|
| `fn generic(comptime T: type)` | `container Name(T) { fields }` |
| `.{ .x = 1, .y = 2 }` | `{ .x = 1, .y = 2 }` |
| `comptime { var arr = ... }` | `comptime u32 FUNC() { u32[N] arr; ... }` |
| `@TypeOf` | No equivalent (explicit types) |
| `while (true) : (i += 1)` | `for (u32 i = 0; ...; i += 1)` |
