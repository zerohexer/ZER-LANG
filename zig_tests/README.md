# ZER Zig-Equivalent Test Suite

36 tests (31 positive, 5 negative), 0 failures. Updated 2026-04-11.
Runner: same pattern as rust_tests — positive must exit 0, negative must fail to compile.

## How to Use This File

**Adding tests:** Drop `.zer` files in this directory. Runner picks them up automatically.
**Negative tests:** Add `// EXPECTED: compile error` as first or second line.
**Naming:** `zt_<category>_<description>.zer` for hand-written, `zt_zig_<source>.zer` for Zig-translated.

## Coverage by Feature

| Feature | Positive | Negative | Total | Key Test Prefixes |
|---|---|---|---|---|
| Container (monomorphization) | 6 | 2 | 8 | zt_container_ |
| Designated initializers | 4 | 2 | 6 | zt_desig_ |
| Comptime array/struct | 4 | 1 | 5 | zt_comptime_ |
| do-while | 3 | 0 | 3 | zt_do_while_ |
| Zig-translated (generics) | 3 | 0 | 3 | zt_zig_generic_ |
| Zig-translated (struct init) | 5 | 0 | 5 | zt_zig_struct_ |
| Zig-translated (comptime) | 3 | 0 | 3 | zt_zig_comptime_ |

## Zig Source Translations

Tests with `zt_zig_` prefix are translated from real Zig test files:

| ZER Test | Zig Source | Zig Test Name |
|---|---|---|
| zt_zig_generic_list | test/behavior/generics.zig | "type constructed by comptime function call" |
| zt_zig_generic_node | test/behavior/generics.zig | "generic struct" |
| zt_zig_generic_two_types | test/behavior/generics.zig | "function with return type type" |
| zt_zig_struct_init | test/behavior/struct.zig | "struct initializer" |
| zt_zig_struct_return | test/behavior/struct.zig | "return struct byval from function" |
| zt_zig_struct_byval_assign | test/behavior/struct.zig | "struct byval assign" |
| zt_zig_struct_partial_init | test/behavior/struct.zig | "default struct initialization fields" |
| zt_zig_struct_anon_literal | test/behavior/struct.zig | "anonymous struct literal syntax" |
| zt_zig_comptime_const_decl | test/behavior/generics.zig | "const decls in struct" |
| zt_zig_comptime_array_build | test/behavior/comptime_memory.zig | comptime array patterns |
| zt_zig_comptime_struct_init | test/behavior/comptime_memory.zig | comptime struct store |

## Zig -> ZER Pattern Translation

| Zig Pattern | ZER Equivalent |
|---|---|
| `fn generic(comptime T: type)` | `container Name(T) { fields }` |
| `.{ .x = 1, .y = 2 }` | `{ .x = 1, .y = 2 }` |
| `comptime { var arr = ... }` | `comptime u32 FUNC() { u32[N] arr; ... }` |
| `@TypeOf` | No equivalent (explicit types) |
| `while (true) : (i += 1)` | `for (u32 i = 0; ...; i += 1)` |
| `struct { fn method(self) }` | Free function: `void func(*Struct s)` |
| `?*@This()` (self-ref generic) | Not supported in container (use named struct) |

### Not-translatable Zig patterns (skip these)
Closures, interfaces (`anytype`), error unions (`!T`), `@TypeOf`, `@This()` in generics,
comptime type manipulation, tagged unions with methods, async/suspend (Zig-specific model),
allocators as parameters, `@ptrCast`+`@alignCast` chains, `std.meta` introspection.
