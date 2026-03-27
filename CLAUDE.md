# CLAUDE.md

## ZER Language — Complete Quick Reference

ZER is memory-safe C. Same syntax, same mental model — but the compiler prevents buffer overflows, use-after-free, null dereferences, and silent memory corruption. Compiles to C, then GCC handles backends.

### Primitive Types
```
u8  u16  u32  u64        unsigned integers
i8  i16  i32  i64        signed integers
usize                    pointer-width unsigned (hardcoded 32-bit currently)
f32  f64                 floating point
bool                     true / false (NOT an integer — no bool↔int coercion)
void                     return type only, cannot declare void variables
```

### Compound Types
```
u8[256] buf;             fixed array — NOTE: size AFTER type, BEFORE name
[]u8 data;               slice — {ptr, len} pair, always bounded
*Task ptr;               pointer — guaranteed non-null
?*Task maybe;            optional pointer — might be null (null sentinel, zero overhead)
?u32 result;             optional value — struct { u32 value; u8 has_value; }
?bool flag;              optional bool — struct { u8 value; u8 has_value; }
?void status;            optional void — struct { u8 has_value; } (NO value field!)
*opaque raw;             type-erased pointer (C's void*)
```

### Builtin Container Types
```
Pool(Task, 8) tasks;     fixed-slot allocator — compile-time count, ALWAYS global
Slab(Task) tasks;         dynamic slab allocator — grows on demand, ALWAYS global
Ring(u8, 256) rx_buf;    circular buffer — ALWAYS global
Arena scratch;            bump allocator — (over, alloc, alloc_slice, reset)
Handle(Task) h;          index + generation counter, not a pointer
```

### CRITICAL Syntax Differences from C

**These cause the most wasted turns in fresh sessions:**

1. **Braces ALWAYS required** for if/else/while/for bodies. No braceless one-liners.
   ```
   if (x > 5) { return 1; }      // OK
   if (x > 5) return 1;           // PARSE ERROR — "expected '{'"
   ```

2. **No C-style casts.** Use intrinsics instead.
   ```
   (u32)x                         // PARSE ERROR
   @truncate(u32, x)              // OK — explicit truncation
   @saturate(i8, big)             // OK — clamp to [-128, 127]
   @bitcast(u32, signed_val)      // OK — reinterpret bits
   ```

3. **Variable declarations: type before name, no `struct` keyword in usage.**
   ```
   struct Task { u32 id; }        // declaration uses 'struct' keyword
   Task t;                        // usage: just the name, NO 'struct' prefix
   *Task ptr = &t;                // pointer to Task
   ```

4. **Array declaration: size between type and name.**
   ```
   u8[256] buf;                   // ZER: type[size] name
   // NOT: u8 buf[256];           // C style — won't work
   ```

5. **Enum values: dot syntax, not bare names.**
   ```
   enum State { idle, running, done }
   State s = State.idle;          // qualified access
   switch (s) {
       .idle => { ... }           // dot-prefixed in switch arms
       .running => { ... }
       .done => { ... }
   }
   ```

6. **Switch uses `=>` arrows, no fallthrough, no `case` keyword.**
   ```
   switch (x) {
       .a => { ... }              // enum arm
       .b, .c => { ... }          // multi-value arm
       default => { ... }         // default arm (required for int switches)
   }
   ```

7. **Optional unwrapping: `orelse` and `if |capture|`.**
   ```
   u32 val = maybe_func() orelse 0;           // default value
   u32 val = maybe_func() orelse return;       // propagate failure
   u32 val = maybe_func() orelse { cleanup(); return; }  // block fallback

   if (maybe_val) |v| { use(v); }             // if-unwrap with capture
   if (maybe_val) |*v| { v.field = 5; }       // mutable capture (pointer)
   ```

8. **No `++`/`--` operators.** Use `+= 1` / `-= 1`.

9. **No `malloc`/`free`.** Use Pool, Slab, Ring, or Arena builtins.

10. **String literals are `[]u8` (slices), not `char*`.**
    ```
    []u8 msg = "Hello";           // slice with .ptr and .len
    ```

11. **`else if` is supported.** Both forms work:
    ```
    if (a) { ... } else if (b) { ... }       // OK — parsed as nested if
    if (a) { ... } else { if (b) { ... } }   // OK — same result
    ```

12. **`orelse return` is bare — no value.** Return value comes from function's return type.
    ```
    *Task t = pool.alloc(Task) orelse return;    // OK — bare return
    *Task t = pool.alloc(Task) orelse return 1;  // PARSE ERROR
    ```

13. **`arena.alloc(T)` — T must be a struct/enum name, not a primitive keyword.**
    ```
    arena.alloc(Task)           // OK — Task is an identifier
    arena.alloc(u32)            // PARSE ERROR — u32 is a keyword
    ```

14. **For loops use `i += 1`, compound init, and C-style structure.**
    ```
    for (u32 i = 0; i < 10; i += 1) { ... }   // OK
    for (u32 i = 0; i < 10; i++) { ... }       // PARSE ERROR — no ++
    ```

### Intrinsics (@ builtins)
```
@size(T)                 sizeof — returns usize
@truncate(T, val)        keep low bits (big→small)
@saturate(T, val)        clamp to min/max of T
@bitcast(T, val)         reinterpret bits (same width required)
@cast(T, val)            distinct typedef conversion only
@inttoptr(*T, addr)      integer to pointer
@ptrtoint(ptr)           pointer to integer (usize)
@ptrcast(*T, ptr)        pointer type cast
@barrier()               full memory barrier
@barrier_store()         store barrier
@barrier_load()          load barrier
@offset(T, field)        offsetof
@container(*T, ptr, f)   container_of
@trap()                  crash intentionally
```

### Function Pointers
```
u32 (*fn)(u32, u32) = add;              // local variable
void (*callback)(u32 event);             // global variable
struct Ops { u32 (*compute)(u32); }      // struct field
u32 apply(u32 (*op)(u32, u32), x, y);   // parameter
?void (*on_event)(u32) = null;           // optional — null sentinel
typedef u32 (*BinOp)(u32, u32);          // function pointer typedef
```

### Defer
```
void f() {
    *u8 buf = alloc();
    defer free(buf);                     // runs at scope exit, in reverse order
    if (error) { return; }               // defer fires on ALL return paths
}
```

### Hardware Support
```
volatile *u32 reg = @inttoptr(*u32, 0x4002_0014);  // MMIO register
u32 bits = reg[9..8];                               // bit extraction
interrupt USART1 { handle_rx(); }                    // interrupt handler
packed struct Packet { u8 id; u16 val; u8 crc; }    // unaligned struct
```

### What ZER Does NOT Have
- No classes, inheritance, templates, generics
- No exceptions, try/catch
- No garbage collector
- No heap/malloc/free (use Pool/Slab/Ring/Arena)
- No implicit narrowing or sign conversion
- No undefined behavior (overflow wraps, shift by >=width = 0)
- No `++`/`--`, no comma operator, no `goto`
- No C-style casts
- No header files (use `import`)
- No preprocessor (#define, #ifdef)

### Safety Guarantees
| Bug Class | Prevention |
|---|---|
| Buffer overflow | Inline bounds check on every array/slice access (conditions, loops, all expressions) |
| Use-after-free | Handle generation counter + ZER-CHECK (with alias tracking) |
| Null dereference | `*T` non-null by default, `?T` requires unwrapping |
| Uninitialized memory | Everything auto-zeroed |
| Integer overflow | Wraps (defined), never UB |
| Silent truncation | Must `@truncate` or `@saturate` explicitly |
| Missing switch case | Exhaustive check for enums and bools |
| Dangling pointer | Scope escape analysis (walks field/index chains, catches struct fields + globals) |
| Union type confusion | Cannot mutate union variant during mutable switch capture |
| Arena pointer escape | Arena-derived pointers cannot be stored in global/static variables |

### Implementation Status
| Feature | Checker | Emitter (E2E) |
|---|---|---|
| All primitives, arrays, slices | Done | Done |
| Pointers, optionals, structs | Done | Done |
| Enums, unions, switch | Done | Done |
| Function pointers | Done | Done |
| Pool + Handle | Done | Done |
| Slab (dynamic growable pool) | Done | Done |
| Ring | Done | Done |
| Arena (alloc, alloc_slice, over, reset) | Done | Done |
| Modules/imports | Done | Done (multi-file) |
| Intrinsics (@size, @truncate, etc.) | Done | Done |
| Defer | Done | Done |
| ZER-CHECK (handle tracking) | Done | N/A (analysis pass) |
| ?FuncPtr (optional function pointers) | Done | Done (null sentinel) |
| Function pointer typedef | Done | Done |
| Distinct typedef (including func ptrs) | Done | Done |
| cinclude (C header inclusion) | Done | Done |
| Enum explicit values (incl. negative) | Done | Done |
| Named slice typedefs (all types) | Done | Done |
| ?[]T optional slice typedefs | Done | Done |
| Volatile emission | Done | Done |
| Array→slice coercion (call/var/return) | Done | Done |
| Mutable union capture |*v| | Done | Done (pointer to original) |

### Architecture Decision: Emit-C Permanently (decided 2026-03-25)

ZER will **NOT** have native backends, IR, or QBE. Emit-C via GCC is the permanent architecture.
- GCC handles all embedded targets, provides decades of optimization, acts as a second validation layer
- No IR layer planned — emit-C → GCC is the only compilation path
- **Never suggest IR, LLVM, QBE, or native code generation in this project**

### Self-Hosting Strategy (decided 2026-03-27)

**Develop in C, prove in ZER.** The compiler is always written in C (`.c` files). LLMs assist development. Fast iteration.

Self-hosting means zerc can emit a working copy of itself in ZER (`zerc.zer`), compile that copy, and the result is an identical compiler. The ZER mirror is the ultimate integration test — NOT the primary development codebase.

```
src/                ← active C development (LLM-assisted, fast)
  lexer.c, parser.c, checker.c, emitter.c, ...
zer-src/            ← ZER mirror (proof of self-hosting)
  lexer.zer, parser.zer, checker.zer, emitter.zer, ...
bootstrap/          ← frozen C snapshot (insurance, never touched)
  lexer.c, parser.c, ...
```

**Build chain:**
```
gcc src/*.c -o zerc              ← build from C (primary)
./zerc zer-src/main.zer -o zerc_zer.c  ← compile ZER mirror
gcc zerc_zer.c -o zerc2          ← build from emitted C
diff zerc zerc2                  ← identical = v1.0 proven
```

**Why not develop in `.zer` directly:**
- LLMs have zero ZER training data — they write C syntax and call it ZER
- C development is faster with LLM assistance today
- ZER development becomes viable when users post real ZER code online (trains future models)
- The C codebase is the productive path; the ZER mirror is the correctness proof

**v1.0 definition:** `zerc.zer` compiles itself and produces an identical compiler.

**Migration to `.zer`-primary happens when:** ZER has enough public code (GitHub repos, blog posts, Stack Overflow) that LLMs learn the syntax natively. Until then, C is the development language.

**Roadmap:**
- **v0.2 (RELEASED):** Slab(T), volatile slices, stdlib (str/fmt/io), bundled GCC, zer-convert Phase 1+2
- **v0.3:** bounds check optimization, better error messages, zer-convert Layer 2 refinement
- **v1.0:** self-hosting proof (zerc.zer compiles itself identically)

### C-to-ZER Conversion Tools (implemented v0.2)

Two tools + one library for automated C-to-ZER migration:

**`tools/zer-convert.c`** — Phase 1: C syntax → ZER syntax (token-level transform)
- Types: `int`→`i32`, `unsigned int`→`u32`, `char`→`u8`, `size_t`→`usize`
- Operators: `i++`→`i += 1`, `NULL`→`null`, `->`→`.`
- Memory: `malloc`→`zer_malloc_bytes`, `free`→`zer_free`
- Strings: `strlen`→`zer_strlen`, `strcmp`→`zer_strcmp`, `memcpy`→`zer_memcpy`
- Casts: `(Type *)expr`→`@ptrcast(*Type, expr)`, `(int)x`→`@truncate(i32, x)`
- sizeof: `sizeof(Type *)`→`@size(*Type)`, inside cast args too
- Preprocessor: `#include`→`cinclude`, `#define N 42`→`const u32 N = 42;`
- Struct: `struct Node` in usage→`Node` (keeps `struct` in declarations)
- Auto-imports `compat.zer` when unsafe patterns detected

**`tools/zer-upgrade.c`** — Phase 2: compat builtins → safe ZER (source-to-source)
- Layer 1: `zer_strlen(s)`→`s.len`, `zer_strcmp(a,b)==0`→`bytes_equal(a,b)`, `zer_memcpy`→`bytes_copy`, `zer_memset(d,0,n)`→`bytes_zero(d)`, `zer_exit`→`@trap()`
- Layer 2: Scans `@ptrcast(*T, zer_malloc_bytes(@size(T)))` patterns, matches with `zer_free()`, generates `static Slab(T)` declarations, rewrites `var.field`→`slab.get(var_h).field`
- Scope-aware: tracks function boundaries per malloc — parameters in other functions NOT wrapped with `slab.get()`
- Line-based replacement: detects malloc pattern at line start, replaces entire line. No output buffer rollback (was causing stray `)` bugs).
- Null check removal: `if (!var) return ...;` after `orelse return;` automatically skipped
- Auto-adds `import str;`, removes `import compat;` when fully upgraded
- Reports: N upgraded, M kept (for remaining compat calls)

**zer-upgrade architecture notes for fresh sessions:**
- Layer 2 malloc detection uses `find_func_bounds()` to record `func_start`/`func_end` for each alloc. `find_alloc(name, pos)` only matches if `pos` is within the function where malloc occurred.
- Cross-function support: `scan_handle_params()` finds function params of Slab types. `find_handle_param(name, pos)` enables `slab.get()` wrapping and `slab.free()` for params that receive Slab-allocated objects. This handles the pattern where malloc is in func A and free is in func B.
- Function signature rewriting: `rewrite_signatures()` post-processing pass detects function declarations (line contains `(` and ends with `{` or `)`, skipping `\r`). Rewrites `SlabType *param` → `Handle(SlabType) param_h` in params, and `SlabType *func(` → `?Handle(SlabType) func(` for return types.
- The line-based approach (detect pattern at `\n` boundary, emit replacement, skip source line) is much more reliable than mid-stream output buffer rollback. The rollback approach broke because earlier Layer 1 transforms changed output length vs source length.
- `strcmp`/`strncmp`/`memcmp` → `bytes_equal` strips trailing `== 0` and `!= 0` comparisons (strcmp returns int, bytes_equal returns bool).
- **Windows `\r\r\n` pitfall:** Phase 1 output may have `\r` chars. The `is_func_decl` backward scan must skip `\r` in addition to space/tab, otherwise no function signatures are detected.

**`lib/compat.zer`** — Scaffolding library (NOT part of ZER). Wraps C stdlib via `cinclude`. Tagged `zer_` prefix for Phase 2 detection. Removed after full upgrade.

**Pipeline:** `input.c → zer-convert → input.zer → zer-upgrade → input_safe.zer`

### Structural Refactors Completed (RF1-RF7)

These changed fundamental patterns. Fresh sessions MUST know:
- **RF1:** Typemap moved from static globals into Checker struct. `checker_get_type(Checker *c, Node *node)` — takes Checker* now. Emitter: `checker_get_type(e->checker, node)`.
- **RF2:** `emit_file` and `emit_file_no_preamble` unified into single `emit_top_level_decl()`. Add new NODE kinds in ONE place only.
- **RF3:** `resolve_type()` caches in typemap. Emitter's `resolve_tynode(e, tn)` reads typemap first, falls back to `resolve_type_for_emit()`. Add new types to `resolve_type_inner()` — emitter gets them automatically.
- **RF4:** Module name mangling uses arena-allocated buffers (no fixed 256 limit).
- **RF5:** Parser uses lightweight token scanning for IDENT-starting statements (not full speculative parse_type).
- **RF6:** `null` used with non-optional types gives targeted error message.
- **RF7:** `@cstr` volatile detection walks field/index chains to root symbol.

### Emitter Critical Patterns (causes of most bugs)

**Optional types in emitted C:**
- `?*T` → plain C pointer (NULL = none, non-NULL = some). No struct wrapper.
- `?T` (value) → `struct { T value; uint8_t has_value; }`. Check `.has_value`, extract `.value`.
- `?void` → `struct { uint8_t has_value; }`. **NO `.value` field** — accessing it is a GCC error.
- Bare `return;` from `?void` func → `return (_zer_opt_void){ 1 };`
- `return null;` from `?void` func → `return (_zer_opt_void){ 0 };`
- Bare `return;` from `?T` func → `return (opt_type){ 0, 1 };`

**Bounds checks in emitted C (BUG-078/079/119 — inline, NOT hoisted):**
- **Simple index** (ident, literal): `(_zer_bounds_check((size_t)(idx), size, ...), arr)[idx]` — comma operator, preserves lvalue.
- **Side-effecting index** (NODE_CALL): `({ size_t _zer_idxN = (size_t)(idx); _zer_bounds_check(_zer_idxN, size, ...); arr[_zer_idxN]; })` — GCC statement expression, single evaluation, rvalue only.
- Detection: `node->index_expr.index->kind == NODE_CALL` → single-eval path.
- **NEVER hoist bounds checks to statement level.** That breaks short-circuit and misses conditions.
- **NEVER double-evaluate function call indices.** That causes side effects to execute twice.

**Slice types in emitted C:**
- `[]T` → named typedef `_zer_slice_T` for ALL types (primitives in preamble, struct/union after declaration)
- Slice indexing embedded in bounds check pattern above — `.ptr` added automatically
- `?[]T` → named typedef `_zer_opt_slice_T` (all types)

**Null-sentinel types (`is_null_sentinel()` function):**
- `?*T` → plain C pointer (NULL = none).
- `?FuncPtr` → plain C function pointer (NULL = none).
- `?DistinctFuncPtr` → also null sentinel (unwraps TYPE_DISTINCT).
- **CRITICAL:** Use `is_null_sentinel(inner_type)` (function, not macro). It unwraps TYPE_DISTINCT before checking TYPE_POINTER/TYPE_FUNC_PTR. The old `IS_NULL_SENTINEL` macro is kept for backward compat but doesn't handle distinct.
- `emit_type_and_name` handles name-inside-parens for `TYPE_OPTIONAL + TYPE_DISTINCT(TYPE_FUNC_PTR)`.

**Builtin method emission pattern (emitter.c ~line 350-520):**
1. Check if callee is `NODE_FIELD` with object of type Pool/Ring/Slab/Arena
2. Get object name and method name
3. Emit inline C code or call to runtime helper
4. Set `handled = true` to skip normal call emission

**Adding new builtin methods:** Copy the Pool/Ring/Slab/Arena pattern. Need: checker NODE_CALL handler (return type), emitter interception (C codegen), and E2E test.

**Slab(T) — dynamic growable pool:**
- Same Handle(T) API as Pool: `alloc() → ?Handle(T)`, `get(h) → *T`, `free(h)`
- Emitter: `_zer_slab` struct with `slot_size`, pages grow via `calloc` on demand (64 slots/page)
- Runtime: `_zer_slab_alloc()` scans for free slot, grows if full; `_zer_slab_get()` checks generation; `_zer_slab_free()` marks unused + bumps generation
- Global var: `_zer_slab name = { .slot_size = sizeof(T) };`
- Method interception passes `&name` to runtime helpers (unlike Pool which passes individual arrays)
- Same restrictions as Pool: must be global/static, not copyable, not in struct fields, get() is non-storable

### Critical Patterns That Cause Bugs — READ BEFORE MODIFYING

These patterns caused 74 bugs across 6 audit rounds. A fresh session MUST know them:

**1. `?void` has ONE field, everything else has TWO.**
Every code path that emits optional null `{ 0, 0 }` MUST check `inner->kind == TYPE_VOID` and emit `{ 0 }` instead. There are 6+ paths: NODE_RETURN, assign null, var-decl null, expression orelse, var-decl orelse, global var. We fixed ALL of them. If you add a new path, check for `?void`.

**2. Use `is_null_sentinel(type)` for null-sentinel checks, not the macro.**
The `is_null_sentinel()` function in emitter.c unwraps TYPE_DISTINCT before checking TYPE_POINTER/TYPE_FUNC_PTR. The old `IS_NULL_SENTINEL` macro only checks the kind directly and misses distinct wrappers.

**3. `TYPE_DISTINCT` must be unwrapped before type dispatch.**
The checker and emitter have paths that check `TYPE_FUNC_PTR`, `TYPE_STRUCT`, etc. If the type is wrapped in `TYPE_DISTINCT`, these checks fail silently. Always unwrap: `if (t->kind == TYPE_DISTINCT) t = t->distinct.underlying;`

**4. Named typedefs for EVERY compound type.**
Anonymous `struct { ... }` in C creates a new type at each use. Slices, optional slices, and optional types for structs/unions/enums ALL need named typedefs (`_zer_slice_T`, `_zer_opt_T`, `_zer_opt_slice_T`). If you add a new compound type, emit its typedef after declaration AND in `emit_file_no_preamble`.

**5. Function pointer syntax — name goes INSIDE `(*)`.**
`emit_type_and_name` handles this for TYPE_FUNC_PTR, TYPE_OPTIONAL wrapping func ptr, and TYPE_DISTINCT wrapping func ptr. If you add new optional/distinct combinations, check that `emit_type_and_name` handles the name placement.

**6. `emit_file` AND `emit_file_no_preamble` share `emit_top_level_decl()` (RF2).**
Both functions now call a single `emit_top_level_decl(e, decl, file_node, i)` dispatch. Adding a new NODE kind requires updating only ONE place. The old pattern of two parallel switch statements that had to stay in sync (caused BUG-086/087) is eliminated.

**7. UFCS is dropped.** Dead code commented out in checker.c. Do not implement or rely on `t.method()` syntax. Builtin methods (Pool/Ring/Arena) work via compiler intrinsics, NOT UFCS.

**8. Scope escape checks must walk field/index chains to root.**
`global.ptr = &local` has target `NODE_FIELD`, not `NODE_IDENT`. The checker walks through NODE_FIELD/NODE_INDEX to find the root ident, then checks if it's static/global. Same pattern used for arena-derived pointer escape detection.

**9. Union switch arms must lock the switched-on variable.**
During a union switch arm with capture, `checker.union_switch_var` is set to the switched-on variable name. Any assignment to `var.variant` where `var` matches the lock is a compile error. This prevents type confusion from mutating the active variant while a capture pointer is alive. Lock is saved/restored for nesting.

**10. Arena-derived pointers tracked via `is_arena_derived` flag.**
When a variable is initialized from `arena.alloc(T)` (including through orelse), the Symbol gets `is_arena_derived = true`. Assigning this variable to a global/static target (walking field/index chain) is a compile error. Flag propagates through aliases (`q = arena_ptr` marks `q` as arena-derived too).

**11. Use `type_unwrap_distinct(t)` before any type-kind dispatch.**
`types.h` provides `type_unwrap_distinct(Type *t)` — returns `t->distinct.underlying` if TYPE_DISTINCT, otherwise `t` unchanged. Call this before ANY `switch (type->kind)` that maps to named typedefs or validates type categories. This was the #1 bug pattern (BUG-074, 088, 089, 104, 105, 110). Never write `if (t->kind == TYPE_DISTINCT) t = t->distinct.underlying;` manually — use the helper.

**12. If-unwrap and switch capture arms must manage their own defer scope.**
These code paths unwrap the block body to inject capture variables. Without saving/restoring `defer_stack.count`, defers inside the block accumulate at function scope and fire at function exit instead of block exit. Save `defer_base` before emitting block contents, call `emit_defers_from(e, defer_base)` after, restore count.

**13. `@cast` supports wrap AND unwrap — both directions.**
`@cast(Celsius, u32_val)` wraps underlying → distinct. `@cast(u32, celsius_val)` unwraps distinct → underlying. Cross-distinct (`@cast(Fahrenheit, celsius_val)`) is rejected. The old code only allowed wrapping (target must be distinct).

**14. `type_unwrap_distinct()` applies EVERYWHERE types are dispatched.**
Not just emit_type — also: checker NODE_FIELD (struct/union/pointer dispatch), switch exhaustiveness checks, auto-zero (global + local), intrinsic validation (@ptrcast, @inttoptr, @ptrtoint, @bitcast, @truncate, @saturate), NODE_SLICE expression emission, `?[]T` element type. If you add ANY new switch on `type->kind`, call `type_unwrap_distinct()` first.

**15. ZER-CHECK tracks Handle parameters, not just alloc results.**
`zc_check_function` scans params for TYNODE_HANDLE and registers them as HS_ALIVE. This catches `pool.free(param_h); pool.get(param_h)` within function bodies. Pool ID is -1 for params (unknown pool).

**16. `arena.alloc()` AND `arena.alloc_slice()` both set `is_arena_derived`.**
The detection checks `mlen == 5 "alloc" || mlen == 11 "alloc_slice"`. Both results are tracked for escape to global/static. Propagates through aliases (var-decl init + assignment).

**17. Emitter uses `resolve_tynode()` — tries typemap first, falls back to `resolve_type_for_emit()` (RF3).**
`resolve_type()` in checker.c now caches every resolved TypeNode in the typemap (split into `resolve_type` wrapper + `resolve_type_inner`). The emitter's `resolve_tynode(e, tn)` reads from typemap via `checker_get_type(e->checker, (Node *)tn)`, falling back to the old `resolve_type_for_emit()` for any uncached TypeNodes. The fallback will become dead code over time. **If adding a new type construct:** ensure `resolve_type_inner()` handles it — the typemap cache means the emitter gets it automatically.

**18. `eval_const_expr()` in `ast.h` — shared constant folder.**
Evaluates compile-time integer expressions (+, -, *, /, %, <<, >>, &, |, unary -). Used by both checker (array/pool/ring size resolution) and emitter (resolve_type_for_emit). Without this, `u8[4 * 256]` silently becomes `u8[0]`.

**19. `[]bool` maps to `_zer_slice_u8` / `_zer_opt_slice_u8`.**
TYPE_BOOL must be handled in emit_type(TYPE_SLICE), emit_type(TYPE_OPTIONAL > TYPE_SLICE), and NODE_SLICE expression emission. Bool is uint8_t in C — uses the same slice typedef as u8.

**20. String literals are `const []u8` — CANNOT assign to mutable `[]u8`.**
`[]u8 msg = "hello"` is a compile error (BUG-124). String literals live in `.rodata` — writing through a mutable slice segfaults. Use `const []u8 msg = "hello"`. Passing string literals as function arguments still works (parameter receives a slice struct copy, function can read but not write through `.rodata` pointer — this is safe because ZER doesn't allow pointer arithmetic to write past bounds).

**21. Scope escape checks cover BOTH `return` AND assignment paths.**
- `return &local` → error (BUG original)
- `return local_array` as slice → error (BUG-120)
- `global.ptr = &local` → error (BUG-080)
- `global_slice = local_array` → error (BUG-122)
- All walk field/index chains to find the root identifier.

**22. Bit extraction has 3 safe paths for mask generation.**
Constant width >= 64 → `~(uint64_t)0`. Constant width < 64 → `(1ull << width) - 1` (precomputed). Runtime width (variables) → safe ternary `(width >= 64) ? ~0ULL : ((1ull << width) - 1)`. Never emit raw `1ull << (high - low + 1)` — UB when width == 64. (BUG-125, BUG-128)

**23. Shift operators use `_zer_shl`/`_zer_shr` macros — spec: shift >= width = 0.**
ZER spec promises defined behavior for shifts. C has UB for shift >= width. The preamble defines safe macros using GCC statement expressions (single-eval for shift amount). Both `<<`/`>>` and `<<=`/`>>=` use these. Compound shift `x <<= n` emits `x = _zer_shl(x, n)`. (BUG-127)

**24. Bounds check side-effect detection: NODE_CALL AND NODE_ASSIGN.**
Index expressions with side effects (function calls, assignments) use GCC statement expression for single evaluation. Simple indices (ident, literal) use comma operator for lvalue compatibility. Detection: `kind == NODE_CALL || kind == NODE_ASSIGN`. (BUG-119, BUG-126)

**25. GCC flags: `-fwrapv -fno-strict-aliasing` are MANDATORY.**
`-fwrapv` = signed overflow wraps (ZER spec). `-fno-strict-aliasing` = prevents GCC optimizer from reordering through `@ptrcast` type-punned pointers. Both in `zerc --run` invocation and emitted C preamble comment.

**26. Side-effect index lvalue: `*({ ...; &arr[_idx]; })` pattern.**
`arr[func()] = val` needs single-eval AND lvalue. Plain statement expression `({ ...; arr[_idx]; })` is rvalue only. Fix: take address inside, dereference outside: `*({ size_t _i = func(); check(_i); &arr[_i]; })`. (BUG-132)

**27. Union switch hoists expr into temp before `&` — rvalue safe.**
`switch(get_union())` with capture needs `&(expr)` but rvalue addresses are illegal. Fix: `__auto_type _swt = expr; __typeof__(_swt) *_swp = &_swt;`. Can't use `__auto_type *` (GCC rejects) — must use `__typeof__`. (BUG-134)

**28. Bare `if(optional)` / `while(optional)` must emit `.has_value` for struct optionals.**
`if (val)` where val is `?u32` emits `if (val)` in C — but val is a struct. GCC rejects: "used struct type value where scalar is required." The emitter's regular-if and while paths must check `checker_get_type(e->checker, cond)` — if it's a non-null-sentinel optional, append `.has_value`. The if-unwrap path (`|val|`) already handles this correctly. **NOTE:** `checker_get_type` now takes `Checker *c` as first arg (RF1). Emitter uses `e->checker`. (BUG-139)

**29. `const` on var declaration must propagate to the Type, not just the Symbol.**
Parser puts `const` into `node->var_decl.is_const`, NOT into the TypeNode (TYNODE_CONST only wraps when `const` appears inside a type expression like function params). The checker must propagate: in NODE_VAR_DECL and NODE_GLOBAL_VAR, when `is_const` is true and type is slice/pointer, create a const-qualified Type via `type_const_slice()` / `type_const_pointer()`. Without this, `const []u8 msg = "hello"; mutate(msg)` passes because `check_expr(NODE_IDENT)` returns `sym->type` which has `is_const = false`. (BUG-140)

**30. Array init/assignment must use `memcpy` — C arrays aren't lvalues.**
`u32[4] b = a;` and `b = a;` are valid ZER but invalid C. Emitter detects TYPE_ARRAY in var-decl init and NODE_ASSIGN, emits `memcpy(dst, src, sizeof(dst))` instead. For init: emit `= {0}` first, then memcpy on next line. For assignment: emit `memcpy(target, value, sizeof(target))` and goto assign_done. (BUG-150)

**31. `emit_type(TYPE_POINTER)` must emit `const` when `is_const` is set.**
Without this, `const *u32` emits as plain `uint32_t*` in C — external C libraries can't see the const qualifier. The checker enforces const at call sites, but the C output should also reflect it for C interop safety. (BUG-151)

**32. `@cstr` must bounds-check when destination is a fixed-size array.**
`@cstr(buf, slice)` emits raw memcpy. If `buf` is TYPE_ARRAY, insert `if (slice.len + 1 > array_size) _zer_trap("@cstr buffer overflow", ...)` before the memcpy. Without this, a long slice silently overflows the stack buffer. (BUG-152)

**33. `is_arena_derived` only set for LOCAL arenas — global arenas are safe to return from.**
The arena return escape check (`NODE_RETURN` + `is_arena_derived`) must not block pointers from global arenas. Fix: when setting `is_arena_derived`, check if the arena object is a global symbol via `scope_lookup_local(global_scope, ...)`. (BUG-143)

**34. Arena-derived propagates through struct field assignment AND return walks chains.**
`h.ptr = arena.alloc(T) orelse return` must mark `h` as arena-derived (walk field/index chain to root). The return check must also walk `return h.ptr` through field/index chains to find root and check `is_arena_derived`. Both var-decl init (line ~1719) and assignment (line ~683) paths detect `arena.alloc`/`arena.alloc_slice` in orelse expressions. (BUG-155)

**35. Division and modulo wrap in zero-check trap.**
`a / b` → `({ auto _d = b; if (_d == 0) _zer_trap("division by zero", ...); (a / _d); })`. Same pattern for `%`, `/=`, `%=`. Uses GCC statement expression for single-eval of divisor. ZER spec: division by zero is a trap, not UB. (BUG-156)

**36. Integer literal range checking in `is_literal_compatible`.**
`u8 x = 256` must be rejected. `is_literal_compatible` validates literal value fits target type range (u8: 0-255, i8: -128..127, etc.). Negative literals (`-N`) reject unsigned targets entirely. Without this, GCC silently truncates. (BUG-153)

**37. Bit extraction index must be within type width.**
`u8 val; val[15..0]` reads junk bits. Checker NODE_SLICE validates constant `high` index < `type_width(obj)`. Runtime indices are not checked (would need emitter support). (BUG-154)

**38. `[]T → *T` implicit coercion REMOVED — null safety hole.**
An empty slice has `ptr = NULL`. Implicit coercion to `*T` (non-null) would allow NULL into a non-null type. Removed from `can_implicit_coerce`. Users must use `.ptr` explicitly. (BUG-162)

**39. `is_local_derived` tracks pointers to local variables.**
`*u32 p = &x` where `x` is local sets `p.is_local_derived = true`. Propagates through aliases (`q = p`). Return check rejects `is_local_derived` symbols. Same pattern as `is_arena_derived` but for stack pointers. (BUG-163)

**40. Base-object side effects in slice indexing must be hoisted.**
`get_slice()[0]` emits `get_slice()` twice (bounds check + access). Detect side effects in entire object chain (NODE_CALL/NODE_ASSIGN at any level), hoist slice into `__auto_type _zer_obj` temp. (BUG-164)

**41. Const checking covers ALL 4 sites: call args, return, assignment, var-decl init.**
`type_equals` ignores `is_const` by design. Const→mutable must be checked separately at each site where a value flows from one type to another. All 4 are now covered for both pointers and slices. (BUG-140, 157, 165, 166)

**42. Signed bit extraction casts to unsigned before right-shift.**
Right-shifting negative signed integers is implementation-defined in C (arithmetic vs logical). Emitter casts to unsigned equivalent (`(uint8_t)val`, `(uint16_t)val`, etc.) before the shift to guarantee logical (zero-fill) behavior. (BUG-167)

**43. `orelse` fallback in return must be checked for local pointers.**
`return opt orelse &x` — if `opt` is null, returns pointer to local `x`. NODE_RETURN must check orelse fallback for `&local` pattern, walking field/index chains. (BUG-168)

**44. Division by literal zero is a compile-time error.**
`10 / 0` and `10 % 0` are caught at checker level (NODE_BINARY, NODE_INT_LIT with value 0). Runtime zero-check trap still fires for variable divisors. (BUG-169)

**45. Slice/array `==`/`!=` comparison rejected — produces invalid C.**
Slices are structs in C; `struct == struct` is a GCC error. Arrays decay inconsistently. Checker rejects TYPE_SLICE and TYPE_ARRAY with `==`/`!=`. Users must compare elements manually. Pointer comparison (`*T == *T`) is still allowed. (BUG-170)

**46. Global variable initializers must be constant expressions.**
`u32 g = f()` is invalid C at global scope. Checker rejects NODE_CALL in NODE_GLOBAL_VAR init. Literals, constant expressions, and `Arena.over()` are allowed. (BUG-171)

**47. NODE_SLICE hoists side-effect base objects.**
`get_slice()[1..]` calls `get_slice()` twice (ptr + len). Detect side effects in object chain, hoist into `__auto_type _zer_so` temp. Emits full slice struct inside `({ ... })` statement expression. (BUG-172)

**48. Array/Pool/Ring sizes are `uint64_t` internally, emitted with `%llu`.**
`types.h` uses `uint64_t` for `array.size`, `pool.count`, `ring.count`. All format specifiers in emitter.c use `%llu` with `(unsigned long long)` cast. Matches GCC's handling of large sizes. (BUG-173)

**49. `type_equals` is const-aware for pointers and slices — recursive.**
`type_equals(TYPE_POINTER)` checks `is_const` match before recursing into inner type. Same for TYPE_SLICE. This means const laundering is blocked at ANY depth of pointer indirection (`**const T != **T`). The manual const checks at call/return/assign/init sites are kept for better error messages but are now redundant with `type_equals`. (BUG-176)

**50. `void` variables rejected — void is for return types only.**
NODE_VAR_DECL and NODE_GLOBAL_VAR reject TYPE_VOID. `?void` is still allowed (has `has_value` field). (BUG-175)

**51. Const array → mutable slice blocked at call sites.**
`const u32[4] arr; mutate(arr)` where `mutate([]u32)` — checker looks up arg symbol, rejects if `is_const` and param slice is mutable. Arrays don't have `is_const` on the Type (only on Symbol), so the check must lookup the symbol. (BUG-182)

**52. Signed division overflow: INT_MIN / -1 trapped.**
Division by -1 on the minimum signed value overflows (result can't fit). Emitter checks `divisor == -1 && dividend == TYPE_MIN` for each width (i8: -128, i16: -32768, i32: -2147483648, i64). (BUG-183)

**53. Volatile on struct fields emitted via TYNODE_VOLATILE check.**
Struct field emission checks if the field's TypeNode has a TYNODE_VOLATILE wrapper. If so, emits `volatile` before the type. The Type system doesn't carry volatile for non-pointer scalars — it's a syntactic property. (BUG-185)

**54. Narrow type arithmetic cast: `(uint8_t)(a + b)` for u8/u16/i8/i16.**
C integer promotion makes `u8 + u8` return `int`. Without cast, wrapping comparison `a + b == 0` fails for `255 + 1`. Emitter checks result type from typemap, casts for types narrower than `int`. (BUG-186)

**55. `all_paths_return()` checks non-void functions for missing returns.**
Recursive analysis: NODE_RETURN → true, NODE_BLOCK → last stmt, NODE_IF → both branches (requires else), NODE_SWITCH → all arms (exhaustive if passed checker). Called after `check_stmt` on function body for non-void return types. (BUG-190)

**56. Duplicate struct/union field/variant names rejected.**
Checker loops through previous fields during registration, errors on name collision. Prevents GCC "duplicate member" errors. (BUG-191)

**57. Return/break/continue inside defer blocks rejected.**
`defer_depth` counter tracked on Checker struct. NODE_RETURN, NODE_BREAK, NODE_CONTINUE check `defer_depth > 0` → error. Prevents control flow corruption in defer cleanup. (BUG-192)

**58. Per-module scope + module-prefix mangling for multi-module type isolation.**
Same-named types in different modules (e.g., `struct Config` in both `cfg_a.zer` and `cfg_b.zer`) now work. Three pieces:
1. **Checker**: `checker_push_module_scope()` pushes a scope with the module's own type declarations before checking bodies. Each module sees its own types.
2. **Checker**: `add_symbol` silently allows cross-module same-named types (first wins in global scope, module scope overrides during body check).
3. **Emitter**: `EMIT_STRUCT_NAME`/`EMIT_UNION_NAME`/`EMIT_ENUM_NAME` macros prepend `module_prefix` — emits `struct cfg_a_Config` vs `struct cfg_b_Config`. All 60+ name emission sites use these macros.
Diamond imports (same module imported via two paths) still deduplicate correctly. (BUG-193)

**59. `is_local_derived` and `is_arena_derived` must be cleared+recomputed on reassignment.**
These flags are "sticky" — once set during var-decl, they persist. But `p = &local; p = &global; return p` is safe. On `NODE_ASSIGN` with `op == TOK_EQ`, clear both flags on the target root symbol, then re-derive from the new value: `&local` → set `is_local_derived`, alias of local/arena-derived ident → propagate flag. Without this, false positives reject valid code AND false negatives miss unsafe reassignments (flag only set in var-decl, not assignment). (BUG-194)

**60. `while(true)` and `for(;;)` are terminators in `all_paths_return` — BUT only if no `break`.**
`all_paths_return(NODE_WHILE)` returns `true` when condition is literal `true` AND body does not contain a `break` targeting this loop (`!contains_break(body)`). Same for `NODE_FOR` with no condition. `contains_break` walks the body recursively but stops at nested loops (their breaks target the inner loop, not ours). Without the break check, `while(true) { if (c) { break; } return 1; }` falsely passes — function falls off end after break. (BUG-195, BUG-200)

**61. Compile-time OOB for constant array index.**
`u8[10] arr; arr[100] = 1;` is caught at compile time, not just at runtime. In `NODE_INDEX`, if index is `NODE_INT_LIT` and object is `TYPE_ARRAY`, compare `idx_val >= array.size` → error. Runtime bounds checks still fire for variable indices. (BUG-196)

**62. Switch on struct-based optionals emits `.has_value && .value == X`.**
`switch (?u32 val) { 5 => { ... } }` must compare `.value` (not the raw struct). Emitter detects `is_opt_switch` when `TYPE_OPTIONAL` with non-null-sentinel inner. Captures extract `.value` (immutable) or `&.value` (mutable `|*v|`). Null-sentinel optionals (`?*T`, `?FuncPtr`) still use direct comparison. (BUG-196b)

**63. Volatile propagation on address-of (`&volatile_var`).**
`&x` where `x` has `sym->is_volatile` produces a pointer with `pointer.is_volatile = true`. Assigning this to a non-volatile pointer variable is an error (volatile qualifier dropped → optimizer may eliminate writes). `volatile *u32 p = &x` is allowed because `var_decl.is_volatile` matches. (BUG-197)

**64. `@size(T)` resolved as compile-time constant in array sizes.**
`u8[@size(Task)] buffer;` now works. In the checker's TYNODE_ARRAY resolution, when `eval_const_expr` returns -1 and the size expression is `NODE_INTRINSIC` with name "size", resolve the type and compute byte size: primitives via `type_width / 8`, structs via field sum, pointers = 4. The emitter still uses `sizeof(T)` for runtime expressions. (BUG-199)

**69. `contains_break` walks NODE_ORELSE, NODE_VAR_DECL, NODE_EXPR_STMT.**
`orelse break` inside while(true) body is a hidden break. `contains_break` checks `NODE_ORELSE.fallback_is_break`, recurses into `NODE_VAR_DECL.init` and `NODE_EXPR_STMT.expr`. Without this, `while(true) { x = opt orelse break; }` falsely passes return analysis. (BUG-204)

**70. Local-derived escape via assignment to global blocked.**
`global_p = p` where `p` has `is_local_derived` and target root is global/static → error. Previous check only caught direct `&local` in assignment value, not aliased local-derived pointers. (BUG-205)

**71. Orelse unwrap preserves is_local_derived from expression.**
Var-decl init flag propagation walks through `NODE_ORELSE` to reach the expression root. `*u32 p = maybe orelse return` where `maybe` is local-derived marks `p` as local-derived. (BUG-206)

**67. orelse &local in var-decl propagates is_local_derived.**
`*u32 p = maybe orelse &local_x;` marks `p` as local-derived. The detection checks both direct `NODE_UNARY/TOK_AMP` AND `NODE_ORELSE` fallback for `&local`. Without this, orelse with local address fallback creates a dangling pointer escape. (BUG-202)

**68. Slice from local array marks is_local_derived.**
`[]u8 s = local_arr;` where `local_arr` is a local `TYPE_ARRAY` marks `s` as local-derived. The implicit array→slice coercion creates a slice pointing to stack memory. Without this, `return s` returns a dangling slice. Detection: init is `NODE_IDENT` with `TYPE_ARRAY`, target is `TYPE_SLICE`, source is local. (BUG-203)

**66. `type_width`/`type_is_integer`/etc. unwrap TYPE_DISTINCT.**
All type query functions in `types.c` now call `type_unwrap_distinct(a)` first. Without this, `type_width(distinct u32)` returns 0, breaking `@size(Distinct)` and potentially confusing intrinsic validation. (BUG-201)

**65. Duplicate enum variant names rejected.**
`enum Color { red, green, red }` is caught at checker level (BUG-198). Same pattern as struct field duplicate check (BUG-191). Prevents GCC `#define` redefinition warnings.

**72. Sub-slice from local array marks is_local_derived.**
`[]u8 s = local_arr[1..4];` walks through `NODE_SLICE` to find the object being sliced, then walks field/index chains to the root. If root is a local array, marks `sym->is_local_derived`. BUG-203 only caught `NODE_IDENT` init — this catches `NODE_SLICE` init too. (BUG-207)

**73. `&union_var` blocked inside mutable switch capture arm.**
Taking the address of the union being switched on creates a pointer alias that bypasses the variant lock (`union_switch_var` name check). Fix: in `check_expr(NODE_UNARY/TOK_AMP)`, if operand matches `union_switch_var`, error. Prevents type confusion via `*Union alias = &msg; alias.other_variant = ...`. (BUG-208)

**74. `@cstr` slice destination gets bounds check.**
`@cstr(slice_dest, src)` now checks `src.len + 1 > dest.len` before memcpy. For slice destinations, the emitter hoists the full slice into `__auto_type _zer_cd` temp (for `.len`), and uses `.ptr` for the memcpy target. Array destinations already had bounds checks (BUG-152). (BUG-209)

**75. Bit-set assignment: `reg[7..0] = 0xFF` emits mask-and-set.**
`target = (target & ~mask) | ((value << low) & mask)`. In `NODE_ASSIGN`, if target is `NODE_SLICE` on an integer type, emit bitmask operation instead of struct literal assignment. Constant bit ranges use precomputed masks. Runtime ranges use safe ternary for width >= 64. (BUG-210)

**76. Union switch lock walks field/index chains to root.**
`switch (s.msg)` now locks root `s`, not just direct idents. The lock check in field mutation (`s.msg.b = 20`) also walks to root. Prevents field-based union alias bypass. Both the switch lock setup AND the mutation check walk through NODE_FIELD/NODE_INDEX. (BUG-211)

**77. If-unwrap capture propagates is_local_derived/is_arena_derived.**
`if (opt) |p| { return p; }` where `opt` has `is_local_derived` now marks `p` as local-derived. Walks through NODE_ORELSE and field/index chains to find the condition's root ident, then propagates flags to the capture symbol. (BUG-212)

**78. Static declarations registered in global scope — visible to own module.**
`checker_register_file` no longer skips static vars/functions. They're needed by the module's own function bodies. Cross-module visibility prevention is handled by the module scope system, not by skipping registration. (BUG-213)

**79. Slice-to-slice sub-slicing propagates is_local_derived.**
`[]u8 s2 = s[0..2]` where `s` is already local-derived marks `s2` as local-derived. The check looks up the source symbol first — if it has `is_local_derived`, propagate immediately (before the TYPE_ARRAY root check). (BUG-214)

**80. Unary `~` and `-` cast for narrow types (u8/u16/i8/i16).**
C integer promotion makes `~(uint8_t)0xAA` = `0xFFFFFF55`. Emitter wraps narrow unary results: `(uint8_t)(~a)`. Same pattern as binary arithmetic casting (BUG-186). (BUG-215)

**81. Bit-set assignment uses pointer hoist for single-eval.**
`reg[7..0] = val` emits `({ __typeof__(obj) *_p = &(obj); *_p = (*_p & ~mask) | ((val << lo) & mask); })`. The `__typeof__` doesn't evaluate its argument in GCC. The `&(obj)` evaluates exactly once. `*_p` reads/writes through the cached pointer. Without this, `regs[next_idx()][3..0] = 5` calls `next_idx()` twice. (BUG-216)

**82. Compile-time slice bounds check for arrays.**
`u8[10] arr; []u8 s = arr[0..15];` — slice end 15 exceeds array size 10. In `NODE_SLICE`, if object is `TYPE_ARRAY` and end/start is a constant, check against `array.size`. Complements BUG-196 (index OOB) for slicing operations. (BUG-217)

**83. Function/global name mangling for multi-module.**
Imported module functions emit as `module_name` (`mod_a_init`). `module_prefix` stored on Symbol struct (set during `register_decl`). `NODE_IDENT` emission looks up global scope for the symbol's prefix. `EMIT_MANGLED_NAME` macro handles declaration-site mangling. Static functions/globals are NOT mangled (module-private). (BUG-218)

**84. @size struct alignment matches C sizeof.**
Constant @size resolution now computes with natural alignment: field offset = align(total, field_size), struct padded to multiple of largest field alignment. Packed structs use alignment 1 (no padding). Matches GCC's sizeof exactly. (BUG-219)

**85. keep parameter rejects local-derived pointers.**
`store(p)` where `p` has `is_local_derived` and the parameter is `keep` → error. Previous check only caught direct `&local`, not aliased local pointers. (BUG-221)

**86. Recursive `compute_type_size()` for @size constant evaluation.**
Handles nested structs, arrays, pointers, slices. Computes natural alignment (fields aligned to their size, struct padded to max alignment). Used for all @size constant paths. Fixes `@size(OuterStruct)` mismatch with GCC sizeof. (BUG-220)

**87. Static symbols in imported modules: module-scope only.**
Statics from imported modules skip `checker_register_file` (global scope) and are registered only during `checker_push_module_scope`. The module scope provides visibility during body checking. The emitter uses global scope with `module_prefix` for name resolution. (BUG-222)

**88. Volatile @cstr uses byte-by-byte copy loop.**
`@cstr(volatile_buf, slice)` emits `volatile uint8_t*` cast and a `for` loop instead of `memcpy`. `memcpy` discards the volatile qualifier — GCC may optimize away writes to hardware registers/DMA buffers. The volatile detection checks `is_volatile` on the destination symbol. (BUG-223)

**89. Void struct fields and union variants rejected.**
`struct S { void x; }` and `union U { void a; u32 b; }` produce checker errors. Void is for return types only. (BUG-224)

**90. Pool/Ring assignment rejected — hardware resources not copyable.**
`Pool p; Pool q; p = q;` is an error. Pool/Ring are unique hardware resource containers with internal state (slots, generation counters). Arena is NOT blocked (needs `Arena.over()` init pattern). (BUG-225)

**91. Float switch rejected per spec.**
`switch (f32_val) { ... }` is an error. ZER spec: "switch on float: NOT ALLOWED." Use if/else for float comparisons. (BUG-226)

**92. Recursive struct by value rejected.**
`struct S { S next; }` is caught in register_decl — field type == struct being defined → error. Use `*S` (pointer) for self-referential types. (BUG-227)

**93. `&const_var` yields const pointer — prevents const leak.**
`const u32 x = 42; *u32 p = &x;` is rejected. The TOK_AMP handler propagates `sym->is_const` to `result->pointer.is_const`. Existing const-mutable checks (BUG-140/176) then catch the mismatch. Without this, writing through the mutable pointer corrupts `.rodata`. (BUG-228)

**94. Static symbol collision fixed — mangled keys in global scope.**
`static u32 x` in mod_a and mod_b no longer collide. `checker_push_module_scope` registers statics under mangled key (`sa_x`, `sb_x`) in global scope. Emitter NODE_IDENT tries mangled lookup (`module_name`) when raw lookup fails and `current_module` is set. (BUG-229)

**97. Global symbol collision fixed — all imported symbols get mangled keys.**
Non-static functions/globals from imported modules also registered under mangled key in global scope by `checker_register_file`. Emitter NODE_IDENT **prefers** mangled lookup for current module (tries `current_module + "_" + name` FIRST). This ensures `val` inside `ga`'s body resolves to `ga_val`, not `gb_val`. Raw key still registered for backward compat (main module unqualified calls). (BUG-233)

**95. Pointer parameter escape blocked — `h.p = &local` rejected.**
`void leak(*Holder h) { u32 x = 5; h.p = &x; }` is caught. NODE_ASSIGN escape check treats pointer parameters with field access as potential escape targets (parameter may alias globals). (BUG-230)

**96. `@size(void)` and `@size(opaque)` rejected.**
Both types have no meaningful size. `@size(opaque)` previously emitted `sizeof(void)` which is a GCC extension returning 1. Now caught at checker level. `@size(*opaque)` still works (pointer has known size). (BUG-231)

**98. Mutating methods on const builtins rejected.**
Pool (alloc, free), Ring (push, push_checked, pop), Arena (alloc, alloc_slice, unsafe_reset) are rejected when the object is `const`. The checker walks field/index chains to find the root symbol and checks `is_const`. `over` and `get` are non-mutating and allowed. (BUG-236)

**99. Nested array return escape walks field chains.**
`return s.arr` where `s` is local and `arr` is TYPE_ARRAY → slice coercion now caught. NODE_RETURN walks field/index chains to root ident (same pattern as BUG-155 arena escape). (BUG-237)

**100. `@cstr` to const destination rejected.**
`@cstr(buf, "hello")` where `buf` is `const` → error. Checks destination symbol `is_const`. (BUG-238)

**101. `@cstr` compile-time overflow for constant arguments.**
`@cstr(buf, "hello world")` where `buf` is `u8[4]` is caught at compile time (string length 11 + null > buffer size 4). Runtime trap still fires for variable-length slices. (BUG-234)

**102. Non-null pointer `*T` requires initializer.**
`*u32 p;` without init is rejected for local vars — auto-zero creates NULL, violating `*T` non-null guarantee. Use `?*T` for nullable, or provide init: `*u32 p = &x;`. Globals already require init via NODE_GLOBAL_VAR. (BUG-239)

**103. Nested array assign escape to global walks chains.**
`global_s = s.arr` where `s` is local and `s.arr` is TYPE_ARRAY→TYPE_SLICE coercion — walk value's field/index chains to root, check if local vs global target. (BUG-240)

**104. `@cstr` const pointer destination rejected.**
`@cstr(p, "hi")` where `p` is `const *u8` → error. Checks `pointer.is_const` on destination type, in addition to `is_const` on symbol (BUG-238). (BUG-241)

**105. Parser: lightweight lookahead replaces speculative parse for IDENT-starting statements.**
`is_type_token` returns true for `TOK_IDENT`, which previously caused every identifier-starting statement (`foo(bar)`, `x = 5`) to trigger a full `parse_type()` + backtrack. Now IDENT-starting statements use token scanning: IDENT IDENT → var decl, IDENT `[` ... `]` IDENT → array var decl, IDENT `(*` → func ptr decl, anything else → expression. No AST allocation, no error suppression. Speculative `parse_type()` only used for unambiguous type starters (`*`, `?`, `[]`).

**106. `slice.ptr` field access returns `*T` (const-aware).**
`msg.ptr` on a `[]u8` returns `*u8`. If the slice is `const []u8`, returns `const *u8`. Required for C interop (`puts("hello".ptr)`). (BUG-242)

**107. `@size(?T)` resolved by `compute_type_size`.**
Null-sentinel `?*T` = pointer size. `?void` = 1. Value `?T` = inner_size + 1 (has_value) + alignment padding. `@size(?u32)` = 8, matching GCC `sizeof`. (BUG-243)

**108. Union switch lock walks ALL deref/field/index levels.**
`switch(**pp)` with double pointer now correctly locks `pp`. Both the lock setup AND mutation check use a unified walk loop (deref + field + index). Catches `(*pp).b = 20` inside capture arms at any pointer indirection depth. (BUG-244)

**109. Const array → mutable slice assignment blocked.**
`const u32[4] arr; []u32 s; s = arr;` rejected — writing through `s` would modify read-only data. NODE_ASSIGN checks if value is TYPE_ARRAY with `is_const` symbol and target is mutable TYPE_SLICE. (BUG-245)

**110. `@ptrcast`/`@bitcast` of `&local` caught in return.**
`return @ptrcast(*u8, &x)` where `x` is local → error. NODE_RETURN walks into NODE_INTRINSIC args to find `&local` patterns inside ptrcast/bitcast wrappers. (BUG-246)

**111. Array size overflow > 4GB rejected.**
`u8[1 << 33]` silently truncated to `arr[0]` via `(uint32_t)val` cast. Now explicitly checked: if `val > UINT32_MAX`, error. ZER targets embedded — 4GB+ arrays are nonsensical. (BUG-247)

**112. `check_expr` recursion depth guard (limit 1000).**
`c->expr_depth` incremented on entry, decremented on exit. At depth > 1000, emits "expression nesting too deep" error and returns `ty_void`. Prevents stack overflow on pathological input like 10,000 chained `orelse` expressions. (BUG-235)

**113. NODE_RETURN walks through NODE_ORELSE for safety flag checks.**
`return opt orelse p` where `p` has `is_local_derived` or `is_arena_derived` must be caught. The return escape check splits the expression: if `ret.expr` is `NODE_ORELSE`, check both `.orelse.expr` AND `.orelse.fallback` for local/arena-derived roots. Without this, `return opt orelse local_ptr` escapes unchecked. (BUG-251)

**114. Array assignment uses pointer hoist for single-eval.**
`get_s().arr = local` emits `({ __typeof__(target) *_p = &(target); memcpy(_p, src, sizeof(*_p)); })`. The old pattern `memcpy(target, src, sizeof(target))` called `target` twice — double-evaluating side effects. Same hoist pattern as BUG-216 (bit-set assignment). (BUG-252)

**115. Global non-null pointer `*T` requires initializer.**
`*u32 g_ptr;` at global scope auto-zeros to NULL, violating `*T` non-null guarantee. Check added in BOTH `register_decl` (global registration) AND `check_stmt` (local vars). `?*T` without init is still allowed (nullable by design). (BUG-253)

**116. `&arr[i]` and `&s.field` propagate const/volatile from root.**
BUG-228 only checked `NODE_IDENT` operands in TOK_AMP. Now walks field/index chains to root ident and propagates `is_const`/`is_volatile`. `&const_arr[0]` yields `const *u32`, preventing const laundering through indexing. (BUG-254)

**117. NODE_ORELSE in index triggers single-eval path.**
`arr[get() orelse 0]` duplicated the orelse expression (bounds check + access). Added `NODE_ORELSE` to `idx_has_side_effects` detection — now uses the hoisted `_zer_idx` temp path. (BUG-255)

**118. `@ptrcast`/`@bitcast` return checks local/arena-derived idents.**
BUG-246 only caught `return @ptrcast(*u8, &local)`. Now also catches `return @ptrcast(*u8, p)` where `p` has `is_local_derived` or `is_arena_derived`. Only fires when the return type is a pointer (not value bitcasts like `@bitcast(u32, x)`). (BUG-256)

**119. Optional `== null` / `!= null` emits `.has_value` for struct optionals.**
`?u32 x; if (x == null)` emitted `if (x == 0)` — but `x` is a struct in C. Now emits `(!x.has_value)` for `== null` and `(x.has_value)` for `!= null`. Null-sentinel optionals (`?*T`) still use direct pointer comparison. (BUG-257)

**120. `@ptrcast` cannot strip volatile qualifier.**
`@ptrcast(*u32, volatile_ptr)` was allowed — GCC optimizes away writes through the non-volatile result. Now checks both type-level `pointer.is_volatile` AND symbol-level `sym->is_volatile` on the source ident. (BUG-258)

**121. `return @cstr(local_buf, ...)` rejected — dangling pointer.**
`@cstr` returns `*u8` pointing to its first arg. If that arg is a local buffer, the returned pointer dangles. NODE_RETURN checks for `NODE_INTRINSIC` with name "cstr" and walks the buffer arg to root ident — rejects if local. (BUG-259)

**122. `*func() = &local` rejected — escape through dereferenced call.**
`*pool.get(h) = &x` stores a local address into memory returned by a function call (which may be global). NODE_ASSIGN walks the target through deref/field/index; if root is NODE_CALL, rejects `&local` and local-derived values. (BUG-260)

**123. Union switch lock blocks pointer aliases of same type.**
`*Msg alias` inside `switch(g_msg)` capture arm — mutation through `alias.b.y = 99` rejected if alias type matches the locked union type. Only applies to pointers (might alias external memory), not direct local variables of same type. (BUG-261)

**124. Slice start/end hoisted for single evaluation.**
`arr[get_start()..get_end()]` was calling `get_start()` 3x and `get_end()` 2x. Now hoisted into `_zer_ss`/`_zer_se` temps inside a GCC statement expression. (BUG-262)

**125. Volatile pointer to non-volatile param rejected at call sites.**
`write_reg(volatile_ptr)` where param is `*u32` strips volatile — GCC may optimize away writes. Checker checks both `pointer.is_volatile` on the Type AND `sym->is_volatile` on the arg ident. (BUG-263)

**126. If-unwrap `|*v|` on rvalue hoists into temp.**
`if (get_opt()) |*v|` emitted `&(get_opt())` — illegal C (rvalue address). Now detects `NODE_CALL` condition and hoists: `__auto_type _tmp = get_opt(); ... &_tmp`. Lvalue conditions still use direct `&` for mutation semantics. (BUG-264)

**127. Multi-dimensional arrays supported.**
`u8[10][20] grid` — parser chains TYNODE_ARRAY dimensions. `u8[10]` is the element type, `[20]` is the outer count. Emitter collects all dims: `uint8_t grid[20][10]`. Bounds checks on each dimension. Statement disambiguator scans through multiple `[N]` suffixes. (New feature)

**128. Recursive union by value rejected.**
`union U { A a; U recursive; }` caught — same pattern as struct BUG-227. Walks through arrays (`U[4]` also contains `U` by value). Use `*U` for self-referential unions. (BUG-265)

**129. Arena `alloc_slice` uses overflow-safe multiplication.**
`arena.alloc_slice(T, huge_n)` emitted `sizeof(T) * n` which overflowed `size_t` to a small value, creating a tiny buffer with a huge `.len`. Now uses `__builtin_mul_overflow` — overflow returns null (allocation fails). (BUG-266)

### Design Decisions (NOT bugs — intentional)
- **`@inttoptr(*T, 0)` allowed:** MMIO address 0x0 is valid on some platforms. `@inttoptr` is the unsafe escape hatch — users accept responsibility. Use `?*T` with null for safe optional pointers.
- **Shift widening (`u8 << 8 = 0`):** Spec-correct. Shift result = common type of operands. Integer literal adapts to left operand type. `u8 << 8` → shift by 8 on 8-bit value → 0 per "shift >= width = 0" rule. Use `@truncate(u32, 1) << 8` for widening.
- **`[]T → *T` coercion removed:** Empty slice has `ptr = NULL`, violating `*T` non-null guarantee. Use `.ptr` explicitly for C interop.

**130. If-unwrap uses `emit_type_and_name` to preserve volatile.**
`__auto_type` drops volatile. Now uses explicit type emission via `emit_type_and_name` (handles func ptr name placement correctly). (BUG-267)

**131. Union switch `|*v|` uses direct `&` for lvalue expressions.**
`switch(g_msg) { .a => |*v| { v.x = 99; } }` was modifying a copy. Now detects lvalue vs rvalue — lvalue uses `&(expr)`, rvalue (NODE_CALL) uses temp hoist. (BUG-268)

**132. Compile-time div-by-zero uses `eval_const_expr`.**
`10 / (2 - 2)` now caught at compile time. Uses `eval_const_expr` on divisor instead of just checking `NODE_INT_LIT == 0`. (BUG-269)

**133. Array return type rejected.**
`u8[10] get_buf()` is invalid C — arrays can't be returned. Checker rejects TYPE_ARRAY return types in `check_func_body`. Use struct wrapper or slice. (BUG-270)

**134. Distinct typedef union/enum in switch unwrapped.**
`switch(distinct_event)` failed when the underlying type was a union. Both checker and emitter now call `type_unwrap_distinct` before TYPE_UNION/TYPE_ENUM dispatch. (BUG-271)

**135. Volatile preserved in if-unwrap capture initial copy.**
`volatile ?u32 reg; if(reg) |v|` now emits `volatile _zer_opt_u32 _zer_uw0 = reg` — volatile qualifier carried from source symbol. (BUG-272)

**136. Volatile array assignment uses byte loop, not memcpy.**
`hw_buf = src` where `hw_buf` is volatile emits `for(_i) vd[_i] = vs[_i]` instead of `memcpy`. Same pattern as @cstr volatile (BUG-223). (BUG-273)

**137. Volatile union switch capture pointer preserves qualifier.**
`switch(volatile_msg) { .a => |*v| }` now emits `volatile struct A *v = &_zer_swp->a`. Detects volatile on switch expression root symbol. (BUG-274)

### Structural Refactors RF8-RF10 (2026-03-26)

**RF8: `eval_const_expr` uses `CONST_EVAL_FAIL` (INT64_MIN) sentinel.**
Old `-1` sentinel collided with valid negative values. `u8[10 - 5]` now evaluates to 5. `u8[5 - 10]` correctly reports "array size must be > 0" instead of "not a constant". All callers updated to check `== CONST_EVAL_FAIL`.

**RF9: Parser arrays are dynamic (stack-first, arena-overflow).**
All fixed-size parser arrays replaced with hybrid stack/arena pattern. No more artificial limits. Includes: OOM flag on Parser struct, `parser_alloc()` helper, depth limit (64), Token-before infinite-loop guards on ALL parse loops (block, file, struct, enum, union, switch). Prevents hangs on malformed input like `"enum struct union;"`.

**RF10: Function pointer detection consolidated into `is_func_ptr_start()`.**
5 duplicated `save → advance('(') → check('*') → restore` patterns replaced with single helper. Saves/restores scanner, current, and previous tokens. Eliminates the "Nth site forgot the pattern" bug class.

**157. `@ptrcast` cannot strip const qualifier.**
`@ptrcast(*u32, const_ptr)` rejected — mirrors volatile check. Source `pointer.is_const` must be matched by target. (BUG-304)

**158. Mutable capture `|*v|` on const source forced const.**
`const ?u32 val; if(val) |*v|` — capture pointer marked const. Walks condition to root symbol, checks `is_const`. (BUG-305)

**159. Array assignment uses `memmove` (overlap-safe).**
`arr = arr` self-assignment no longer UB. Changed `memcpy` to `memmove` in both NODE_ASSIGN and NODE_VAR_DECL array paths. (BUG-306)

**160. `@saturate(u64, f64)` upper bound check.**
`f64` can exceed `UINT64_MAX`. Now clamps: `> 18446744073709551615.0 ? UINT64_MAX : (uint64_t)val`. (BUG-308)

**156. Rvalue struct field assignment rejected.**
`get_s().x = 5` now caught — walks field/index chains to find base NODE_CALL, checks if return type is non-pointer (value type → rvalue). Pointer-returning calls (`pool.get(h).field`) are still valid lvalues via auto-deref. (BUG-302)

**153. `type_unwrap_distinct` recursive — handles any nesting depth.**
`distinct typedef (distinct typedef u32) P2` now unwraps fully to `u32`. Uses `while` loop. Fixes arithmetic, type queries, and intrinsic validation on nested distinct types. (BUG-295)

**154. Constant folder guards `INT_MIN / -1`.**
Signed overflow UB in the compiler itself prevented. Both `/` and `%` paths check `l == INT64_MIN && r == -1` → `CONST_EVAL_FAIL`. (BUG-296)

**155. `emit_type(TYPE_ARRAY)` includes dimensions.**
`sizeof(u32[10])` now emits `sizeof(uint32_t[10])` = 40, not `sizeof(uint32_t)` = 4. Walks array chain to base, emits all `[N]` dimensions. Multi-dim also works. (BUG-297)

**151. Volatile `|*v|` mutable capture pointer preserved.**
`if (volatile_reg) |*v|` — `_zer_uwp` pointer now declared as `volatile T *` when source is volatile. Uses `expr_is_volatile` helper. (BUG-292)

**152. Assignment to non-lvalue rejected.**
`get_val() = 5` now caught at checker level — "not an lvalue". Checks NODE_CALL, NODE_INT_LIT, NODE_STRING_LIT, NODE_NULL_LIT, NODE_BOOL_LIT as assignment targets. (BUG-294)

**149. Orelse temp preserves volatile via `__typeof__`.**
`volatile ?u32 reg; u32 val = reg orelse 0` — orelse temp now uses `__typeof__(expr)` instead of `__auto_type`. `__typeof__` preserves volatile, `__auto_type` does not. (BUG-289)

**150. Local escape via `*param = &local` blocked.**
`void leak(**u32 p) { u32 x; *p = &x; }` — target walk extended to handle `NODE_UNARY(STAR)` in addition to `NODE_FIELD`/`NODE_INDEX`. Catches all deref-through-param escape paths. (BUG-290)

**146. Arena.over slice arg single-eval.**
`Arena.over(next_buf())` called `next_buf()` twice (`.ptr` and `.len`). Now hoists slice arg into `__auto_type` temp. Array path unchanged (sizeof doesn't eval). (BUG-286)

**147. Pool/Ring as struct fields rejected.**
`struct M { Pool(u32, 4) tasks; }` → error. Pool/Ring macros can't be emitted inside C structs. Must be global/static. v0.2 will support this. (BUG-287)

**148. Bit extraction `hi < lo` rejected.**
`reg[0..7]` now caught at compile time — "high index must be >= low index". Prevents silent negative-width extraction. (BUG-288)

**144. Volatile pointer stripping on return rejected.**
`return volatile_ptr` as non-volatile `*T` return type now caught. Checks both type-level and symbol-level volatile on the return expression. (BUG-281)

**145. Volatile pointer stripping on init/assign rejected.**
`*u32 p = volatile_ptr` and `p = volatile_ptr` now caught. Checks symbol-level `is_volatile` on source ident when type-level `pointer.is_volatile` is not set. (BUG-282)

**141. Volatile array var-decl init uses byte loop.**
`volatile u8[4] hw = src` used `memcpy` — doesn't respect volatile. Now uses byte-by-byte loop when `var_decl.is_volatile` is set. Same pattern as BUG-273 (array assignment). (BUG-278)

**169. Const return type parsing — global scope lookahead.**
`const []u8 get_name() { ... }` — parser sees `const` at global scope and now peeks ahead for function declarations (same lookahead as volatile return types). Without this, `const` routes to `parse_var_decl` and rejects the `(` after the function name. Enables stdlib functions returning `const []u8`.

**166. Return orelse @ptrcast(&local) caught.**
`return opt orelse @ptrcast(*u8, &local)` — orelse root walk now inspects NODE_INTRINSIC (ptrcast/bitcast) and NODE_UNARY(&) in fallback branch. Only fires when return type is pointer (value bitcasts like `@bitcast(u32, s[1])` are safe). (BUG-317)

**167. Orelse fallback flag propagation bidirectional.**
`*u32 q = opt orelse p` where `p` is local-derived — var-decl init flag propagation now checks BOTH `orelse.expr` AND `orelse.fallback` for local/arena-derived flags. Previously only checked the expr side. (BUG-318)

**168. `@size(distinct void)` rejected.**
`distinct typedef void MyVoid; @size(MyVoid)` — unwraps distinct before checking TYPE_VOID/TYPE_OPAQUE. Also checks expression args (named types parsed as NODE_IDENT, not type_arg). (BUG-320)

**163. Orelse assignment escape to global caught.**
`g_ptr = opt orelse &local` — NODE_ASSIGN now walks into NODE_ORELSE fallback, checks both `&local` and local-derived idents. Rejects when target is global/static. (BUG-314)

**164. Distinct slice/array comparison rejected.**
`distinct typedef []u8 Buffer; a == b` — binary ==/!= now calls `type_unwrap_distinct` before checking TYPE_SLICE/TYPE_ARRAY. Prevents GCC "invalid operands to binary ==" on emitted C. (BUG-315)

**165. Bit-set index single evaluation.**
`reg[get_hi()..get_lo()] = val` — runtime (non-constant) hi/lo hoisted into `_zer_bh`/`_zer_bl` temps. Constant path unchanged (precomputed masks). Prevents side-effect functions from executing multiple times. (BUG-316)

**142. `is_null_sentinel` unwraps ALL distinct levels.**
`distinct typedef (distinct typedef *u32) Ptr2; ?Ptr2 maybe` was treated as struct optional. Now uses `while` loop to unwrap through any depth of distinct to find the base pointer/func_ptr. (BUG-279)

**143. `@size(usize)` target-dependent via `sizeof()`.**
`compute_type_size` returns `CONST_EVAL_FAIL` for `TYPE_USIZE`. Same approach as BUG-275 for pointers/slices — emitter uses `sizeof(size_t)`. (BUG-280)

**162. Volatile slices — `volatile []T` with `is_volatile` on TYPE_SLICE.**
`volatile u8[16] hw_regs; poll(hw_regs)` where `poll(volatile []u8)` — volatile propagates through array→slice coercion. Emitter uses `_zer_vslice_T` typedefs with `volatile T *ptr`. Passing volatile array to non-volatile slice param is rejected (would strip volatile). `type_equals` checks `is_volatile` match. `can_implicit_coerce` allows non-volatile→volatile widening (safe) but blocks volatile→non-volatile stripping. Parser: `volatile []u8` parses as `TYNODE_VOLATILE(TYNODE_SLICE(u8))`. Checker `TYNODE_VOLATILE` propagates to TYPE_SLICE. Var-decl `volatile []u8 s` propagates via qualifier. (BUG-310)

**161. `usize` width matches host platform — 64-bit gap closed.**
`type_width(TYPE_USIZE)` now returns `sizeof(size_t) * 8` instead of hardcoded 32. On 64-bit hosts: `u32 → usize` widening works, big literals accepted, `@truncate(u32, usize)` valid, `usize → u32` direct blocked. `is_literal_compatible` also uses `sizeof(size_t)` for range. Emitter unchanged — already emits `size_t`.

**140. `keep` qualifier carried through function pointer types.**
`void (*fn)(keep *u32) = store` — keep flags stored per-param in `TYPE_FUNC_PTR` via `param_keeps` array. Parser parses `keep` in func ptr params. `type_equals` checks keep mismatch. Call-site validation works for both direct calls and function pointer calls using the Type's `param_keeps`. (BUG-277)

**138. `@size` on pointer/slice types emits `sizeof()` — target-portable.**
`u8[@size(*u32)] buf` now emits `uint8_t buf[sizeof(uint32_t*)]` instead of hardcoded `buf[4]`. `compute_type_size` returns `CONST_EVAL_FAIL` for pointer/slice types. Array Type stores `sizeof_type` — emitter uses `emit_array_size()` helper. GCC resolves per target. (BUG-275)

**139. `_zer_` prefix reserved — prevents compiler internal shadowing.**
Variables starting with `_zer_` rejected in `add_symbol`. Prevents accidental collision with compiler-generated temporaries (`_zer_tmp0`, `_zer_ss0`, etc.). (BUG-276)

**RF11: Shared `expr_is_volatile()` / `expr_root_symbol()` helpers.**
4 independent inline volatile detection walks (array assign, if-unwrap, switch capture, @cstr) replaced with single `expr_is_volatile(e, expr)` helper. Walks any expression through field/index/deref chains to root ident, looks up symbol `is_volatile`. New emission sites just call the helper — no more per-site volatile walk duplication.

### Known Technical Debt
- **Global Compiler State:** `non_storable_nodes` is a static global. `type_map` was moved into Checker struct (RF1). Remaining global makes compiler non-thread-safe for LSP concurrent requests.
- **Static vars in imported modules:** Fixed in BUG-222/229/233. All imported symbols (static and non-static) register under mangled keys. Cross-module same-named symbols work correctly. No qualified call syntax yet (unqualified calls resolve to last import).

## Spawning Agents That Write ZER Code — MANDATORY

When spawning ANY agent that writes ZER source code (tests, examples, anything), you MUST include these rules in the agent prompt. Agents do NOT read CLAUDE.md automatically:

```
ZER SYNTAX RULES (not C — these differ):
- No ++ or --. Use += 1, -= 1
- else if supported: if (a) { } else if (b) { } else { }
- No C-style casts. Use @truncate, @saturate, @bitcast
- Braces ALWAYS required for if/else/for/while bodies
- Array decl: u8[256] buf (size between type and name)
- Pointer: *Task ptr (star before type, not after)
- Enum access: State.idle (qualified), switch arms: .idle => (dot prefix)
- Switch uses => arrows, no case keyword, no fallthrough
- orelse return is BARE — no value: x orelse return (NOT orelse return 1)
- arena.alloc(T) — T must be struct/enum name, NOT primitive keyword (u32 etc.)
- For loops: for (u32 i = 0; i < N; i += 1) { }
- String literals are []u8, not char*
- bool is NOT an integer — no bool↔int coercion
- Optional: ?*T (null sentinel), ?T (struct with .value/.has_value), ?void (has_value ONLY, no .value)
- Unwrap: if (opt) |val| { use(val); }  or  val = opt orelse default;
```

Failure to include these rules causes agents to write invalid ZER (e.g., using i++ which silently passes parse-error-tolerant test harnesses).

### Lessons From Writing ZER Code (stdlib pitfalls)

These tripped us while writing `lib/str.zer`, `lib/fmt.zer`, `lib/io.zer`. Fresh sessions WILL hit them:

1. **`orelse return false` is a PARSE ERROR.** `orelse return` is bare — no value. Use `orelse return;` for void functions. For bool returns: restructure to avoid orelse in return, or use a temp variable.

2. **`arena.alloc_slice(u8, n)` is a PARSE ERROR.** Primitive keywords (`u8`, `u32`, etc.) can't be type arguments. Use a struct wrapper: `struct Byte { u8 val; }` then `arena.alloc_slice(Byte, n)`. Or avoid alloc_slice for primitives — use fixed-size buffers instead.

3. **`const []u8` return type requires parser lookahead.** `const` and `volatile` at global scope trigger var-decl parsing. The parser now peeks ahead to detect function declarations. Works as of this session — but if adding new qualifier keywords, they need the same lookahead treatment.

4. **Functions returning sub-slices of `const` input must return `const []u8`.** `bytes_trim(const []u8 s) → []u8` fails — the sub-slice inherits const. Return type must be `const []u8`.

5. **C macros (stderr, stdout) are NOT accessible from ZER.** They're preprocessor symbols, not variables. Wrap them in a C helper function in a `.h` file: `static inline FILE *zer_get_stderr(void) { return stderr; }`. Then `cinclude` the header and declare the function in ZER.

6. **`_zer_` prefix is RESERVED.** BUG-276 rejects any identifier starting with `_zer_`. Name helpers `zer_get_stderr` not `_zer_stderr`.

7. **Multiple files concatenated must NOT redeclare the same function.** If `fmt.zer` declares `fputc` and `io.zer` also declares `fputc`, the checker errors "redefinition." Keep declarations in one file or use ZER's import system for multi-file projects.

8. **`@ptrcast(*opaque, data.ptr)` on `const []u8 data` fails.** The `.ptr` of a const slice is `const *u8`. Casting to `*opaque` strips const. Use `@ptrcast(const *opaque, data.ptr)` instead.

9. **`cinclude` emits `#include` but does NOT register C symbols.** You must declare every C function you want to call as a ZER function signature: `i32 putchar(i32 c);`. The `cinclude` just makes the header available to GCC.

10. **`bool` return via `orelse` needs restructuring.** Can't do `*opaque f = mf orelse return false;`. Instead: `?*opaque mf = io_open(...); *opaque f = mf orelse return;` for void, or use an if-unwrap pattern for bool returns.

## First Session Workflow

When starting a new session or lacking context:

1. Read `CLAUDE.md` (this file) — has FULL language reference above, rules, conventions
2. **MANDATORY — read `docs/compiler-internals.md` BEFORE modifying any compiler source file** (parser.c, checker.c, emitter.c, types.c, zercheck.c). It documents every emission pattern, optional handling, builtin method interception, scope system, type resolution flow, and common bug patterns. Skipping this and discovering patterns by reading source files wastes 20+ tool calls. The document exists specifically to prevent this.
3. Read `BUGS-FIXED.md` — 41 past bugs with root causes. Prevents re-introducing fixed bugs.
4. `ZER-LANG.md` — full language spec (only if CLAUDE.md quick reference is insufficient)
5. Read the relevant header files: `lexer.h` → `parser.h` → `ast.h` → `types.h` → `checker.h` → `emitter.h` → `zercheck.h`
4. Run `make docker-check` (preferred) or `make check` to verify everything passes before making changes
5. The compiler pipeline is: ZER source → Lexer → Parser → AST → Type Checker → ZER-CHECK → C Emitter → GCC

## Project Architecture

- **zerc** = the compiler binary (`zerc_main.c` + all lib sources)
- **zer-lsp** = LSP server (`zer_lsp.c` + all lib sources)
- Source files: `lexer.c/h`, `parser.c/h`, `ast.c/h`, `types.c/h`, `checker.c/h`, `emitter.c/h`, `zercheck.c/h`
- Test files: `test_lexer.c`, `test_parser.c`, `test_parser_edge.c`, `test_checker.c`, `test_checker_full.c`, `test_extra.c`, `test_gaps.c`, `test_emit.c`, `test_zercheck.c`, `test_firmware_patterns.c`, `test_fuzz.c`
- E2E tests in `test_emit.c`: ZER source → parse → check → emit C → GCC compile → run → verify exit code
- Cross-platform: `test_emit.c` uses `#ifdef _WIN32` macros (`TEST_EXE`, `TEST_RUN`, `GCC_COMPILE`) for `.exe` extension and path separators. Works on both Windows and Linux/Docker.
- Spec: `ZER-LANG.md` (full language spec), `zer-type-system.md` (type design), `zer-check-design.md` (ZER-CHECK design)
- Compiler flags: `--run` (compile+execute), `--lib` (no preamble/runtime, for C interop)
- GCC flags: emitted C requires `-fwrapv` (ZER defines signed overflow as wrapping). `zerc --run` adds this automatically.
- Emitted C uses GCC extensions: statement expressions `({...})`, `__auto_type`, `_Alignof`, `__attribute__((packed))`
- User-defined struct/enum/union names emit as-is (no `_zer_` prefix). Only internal names are prefixed.

## Building & Testing

**PREFERRED: Use Docker** to avoid Windows Defender false positives on compiled executables:
```
make docker-check     # build + run ALL tests in container (gcc:13 image)
make docker-build     # just build zerc in container
make docker-shell     # interactive bash inside container for debugging
```

**Fallback: Native (triggers AV on Windows corporate laptops):**
```
make zerc             # build the compiler
make check            # build + run all tests
```

**DO NOT run `make check` or execute compiled binaries outside Docker** on the dev machine — Windows flags rapid create-execute-delete of unsigned .exe files as malware.

**WARNING:** Do NOT use bind mounts (`docker run -v $(pwd):/zer`) for test runs. Compiled binaries would land on the Windows filesystem and Defender sees them again. The Dockerfile uses `COPY` — all compilation stays inside the container's filesystem. Keep it that way.

The codebase is **cross-platform** (Windows + Linux/Docker):
- `test_emit.c`: `#ifdef _WIN32` macros for `.exe` extension and path separators
- `test_modules/run_tests.sh`: detects platform via `$OSTYPE` for executable extension
- `zerc_main.c --run`: platform-appropriate exe path and run command
- `Dockerfile`: uses `gcc:13`, copies sources, runs `make check`

## Git Rules

- NEVER add Co-Authored-By or any Claude/AI attribution to commits
- All commits must be under zerohexer@gmail.com only
- No other authors, no co-authors, no AI mentions in commit messages

## Testing Methodology — MANDATORY

Every compiler component (lexer, parser, AST, type checker, ZER-CHECK, safety passes,
C emitter) MUST follow specification-based testing with full positive/negative coverage.

### The Rule

For EVERY rule in ZER-LANG.md:
1. **Positive test**: valid code that exercises the rule — must compile/pass
2. **Negative test**: code that violates the rule — must produce an error

NO EXCEPTIONS. If a spec rule exists, both tests must exist.

### Checklist Derivation

1. Read the spec section
2. Extract every rule (explicit or implied by example)
3. Write positive test proving the rule works
4. Write negative test proving the violation is caught
5. If either test fails, fix the implementation BEFORE moving on

### When Adding New Features

- Write tests FIRST (or alongside), never after
- Every new AST node kind must have parser tests
- Every new type check must have positive + negative checker tests
- Every new coercion rule needs both directions tested
- Every error message must be triggered by at least one test

### Edge Case Protocol

Before marking ANY component as "done":
1. Run the full test suite (all components)
2. Write adversarial tests — code that SHOULD break
3. Test boundary conditions (empty input, max values, deeply nested)
4. Test interactions between features (orelse + if-unwrap, defer + loop + break)
5. Verify error messages are helpful (file, line, what went wrong)

### Regression Prevention

- Never delete a passing test
- If a bug is found, write a test that reproduces it BEFORE fixing
- All tests must pass before any commit
- Run `make docker-check` (or `make check`) before every push

## Debugging Workflow — ZER Source Code Bugs

### The Rule
COMMIT before debugging. Always. No exceptions.
If you can't commit (tests failing), stash.
`git checkout -- file` reverts EVERYTHING including your fix.

### Correct Workflow

1. **Reproduce first**
   Write the minimal ZER program that triggers the bug.
   Smallest possible. One struct. One function. If you can't reproduce in 10 lines, simplify.

2. **State what you expect vs what you get**
   "current.next should return ?*Node (kind=14) but returns *Node (kind=13)"
   Not "the orelse is broken somehow."

3. **One debug line, targeted**
   Add ONE fprintf at the exact decision point. Run. Read output. Remove debug. Fix.
   Never add multiple debug prints across multiple files.

4. **Confirm root cause before fixing**
   "kind=13 is_optional=0 on pointer auto-deref path" — confirmed root cause.
   "I think the typemap might be overwritten" — hypothesis, needs confirmation.

5. **Fix one thing**
   The fix should be 1-5 lines. If growing beyond that, you're fixing the wrong thing.

6. **`make docker-check` immediately after fix**
   950+ tests must pass. If not — revert, re-examine root cause.

7. **Update BUGS-FIXED.md** with: symptom, root cause, fix, test reference.

8. **Commit before anything else**

### Always Update Docs After Changes — MANDATORY

After any bug fix or feature change that passes `make check`, update ALL relevant docs in the SAME commit. Never leave doc updates for later. Future sessions depend on these being accurate.

- `BUGS-FIXED.md` — add the bug with symptom, root cause, fix, test reference
- `docs/compiler-internals.md` — if ANY emitter pattern, checker behavior, type handling, builtin method, or preamble changed. This is the primary reference future sessions read. Stale info here causes bugs.
- `docs/reference.md` — if ANY language feature, syntax, intrinsic, builtin method, or type behavior changed. This is the user-facing language reference. Must reflect what `zerc` actually compiles, not what's spec'd but unimplemented.
- `README.md` — if test counts, features, or status changed
- `ZER-LANG.md` — if spec behavior changed
- `CLAUDE.md` — if syntax rules, implementation status table, or workflow changed

**Only update docs after `make check` passes and you have high confidence the change is correct.** Do not update docs for speculative or in-progress work.

### Red Flags — Stop and Revert

- Fix growing beyond 10 lines
- Adding platform-specific ifdefs to fix a type bug
- Restructuring unrelated code to make the fix work
- Debug prints spreading across multiple files
- Circular reasoning ("but it should work because...")
- More than 2 rounds of debug-without-fix

When you see these: `git checkout -- <file>`. Start over with a cleaner hypothesis.

### Anti-Circular Rule

If you've spent more than 3 debug cycles on the same bug without a confirmed root cause:
1. STOP. Revert all uncommitted changes.
2. State the bug in ONE sentence: "X returns Y but should return Z."
3. Add ONE fprintf at the exact decision point.
4. Run. Read. The output tells you where to look next.
5. If the output contradicts your hypothesis, your hypothesis is wrong. Form a new one.

The pattern that causes circular debugging: assuming the fix location before confirming the root cause. You end up modifying code that isn't the problem, which creates new symptoms that look like the original bug.

### The Pointer Auto-Deref Pattern (ZER-specific)

When a field access bug presents:
- **Value path** (`a.field`): struct field lookup at line ~848
- **Pointer path** (`ptr.field`): auto-deref then field lookup at line ~878
Check BOTH paths separately. They have different code and different bugs.

### Prompt Template for Debugging Sessions

When asking Claude to debug a ZER compiler bug:

```
Here is the minimal ZER program that fails: [paste]
Here is the exact error: [paste]
Here is what I expect: [one sentence]
Here is the relevant compiler code: [paste the ONE function]
Add one debug print to confirm the root cause.
Do not restructure. Do not fix yet. Just confirm.
```

## Agent-Verify Workflow — Bug Hunting & Test Writing

When looking for bugs or writing new tests, use the spawn-then-verify pattern:

### The Pattern

1. **Spawn agent(s)** to audit code or write tests (they act as fresh sessions)
2. **Verify every finding** yourself before acting — agents make mistakes
3. **Fix confirmed issues**, reject false positives

### For Bug Hunting

Spawn an agent to audit a specific file (e.g., emitter.c). Tell it to:
- Read CLAUDE.md and docs/compiler-internals.md first
- Look for specific bug patterns (optional handling, type emission, intrinsic gaps)
- Report with exact line numbers, triggering ZER code, wrong C output, expected C output
- NOT fix anything — just report

Then verify each finding by reading the actual code at the cited lines. Agents find real bugs (e.g., BUG-042 `?Enum` anonymous struct) but also report false positives.

### For Test Writing

Spawn an agent to write new E2E tests. **MUST include the ZER syntax rules block** from "Spawning Agents" section above. Then verify:
- ZER syntax is correct (switch arms have braces, no `++`)
- Expected exit codes are mathematically correct
- Tests actually compile and pass with `make check`

### Multi-Round Audit Protocol

For pre-release auditing, run multiple rounds. Each round gets more targeted as obvious bugs are fixed. The prompt quality matters — here's the pattern that converged from 12 bugs to 1 across 5 rounds:

**Round 1 prompt (broad):**
- "Read CLAUDE.md, compiler-internals.md, BUGS-FIXED.md, then read checker.c/emitter.c"
- "Look for: optional handling gaps, slice emission, intrinsic validation, missing error cases"
- Results: finds obvious gaps (bool coercion, missing handlers)

**Round 2+ prompts (targeted interactions):**
- List ALL bugs already fixed — "Previous rounds fixed: [list]. Do NOT re-report these."
- Ask for INTERACTION bugs: "Look for two features combining incorrectly"
- Give specific combinations to check: "defer + orelse + break in for loop", "volatile + @inttoptr", "?void + orelse return in var-decl path"
- Say: "If you find ZERO bugs, say CLEAN — that's a valid outcome."

**Key prompt elements that improve agent quality:**
1. Tell what was ALREADY fixed (prevents re-reporting — agents waste turns on known bugs)
2. Tell it to check INTERACTIONS, not basic rules (negative test sweep covers those)
3. Give specific interaction patterns (agents check exactly what you ask)
4. Allow "CLEAN" as valid output (prevents agents from inventing false positives)
5. Include the ZER syntax rules block (prevents agents from writing invalid test code)

**Expected convergence:** Round 1: 10+ bugs. Round 2: 5-10. Round 3: 2-3. Round 4+: 0-1. If round 3+ still finds 5+, the checker/emitter has structural issues — stop and refactor.

**After auditing:** Run a systematic negative test sweep — write one test for every `checker_error()` call site that has no test. This closes the gaps permanently so future audits find only interaction bugs.

### Why This Works

- Agents explore without consuming your main context window
- Fresh perspective catches blind spots
- Verification step catches agent mistakes (wrong syntax, false positives)
- Multi-round convergence proves stability — each round finds fewer bugs
- You get both breadth (agent exploration) and depth (your verification)
