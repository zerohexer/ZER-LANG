# Compiler Internals — Read When Working on Specific Components

## Pipeline Overview
```
source.zer → Scanner (lexer.c) → Parser (parser.c) → AST (ast.h)
           → Checker (checker.c) → ZER-CHECK (zercheck.c)
           → Emitter (emitter.c) → output.c → GCC
```

## Lexer (lexer.c/h)
- `Scanner` struct holds source pointer, current position, line number
- Keywords: `TOK_POOL`, `TOK_RING`, `TOK_ARENA`, `TOK_HANDLE`, `TOK_STRUCT`, `TOK_ENUM`, `TOK_UNION`, `TOK_SWITCH`, `TOK_ORELSE`, `TOK_DEFER`, `TOK_IMPORT`, `TOK_VOLATILE`, `TOK_CONST`, `TOK_STATIC`, `TOK_PACKED`, `TOK_INTERRUPT`, `TOK_TYPEDEF`, `TOK_DISTINCT`
- Type keywords: `TOK_U8`..`TOK_U64`, `TOK_I8`..`TOK_I64`, `TOK_USIZE`, `TOK_F32`, `TOK_F64`, `TOK_BOOL`, `TOK_VOID`, `TOK_OPAQUE`
- `TOK_IDENT` is any user identifier (struct/enum/union/variable/function names)

## Parser (parser.c) — ~1500 lines

### Key Functions
- `parse_type()` — parses type syntax into `TypeNode*` (TYNODE_*)
- `parse_expression()` → `parse_precedence()` — Pratt parser with precedence climbing
- `parse_unary()` → `parse_postfix()` → `parse_primary()` — expression parsing chain
- `parse_statement()` — statement parsing (var decl, if, for, while, switch, return, defer, etc.)
- `parse_declaration()` — top-level declarations (struct, enum, union, func, global var, import, interrupt, typedef)
- `parse_func_ptr_after_ret()` — helper for function pointer type parsing at 4 sites

### Type Name Heuristic (line ~1041-1080)
When `parse_statement` sees a token that could begin a type (`is_type_token`), it does speculative parsing:
1. Save scanner state
2. Suppress errors (`panic_mode = true`)
3. Tentatively parse as type
4. Check if next token is identifier (var decl) or `(*` (func ptr decl)
5. Restore state and route to var_decl or expression_stmt

**Known fragility:** `is_type_token` returns true for `TOK_IDENT`, so ALL identifier-starting statements go through speculative parse. Works but wasteful.

### Function Pointer Detection Pattern
At 4 sites (global, local, struct field, func param):
1. After parsing return type, check for `(`
2. Save state, advance, check for `*`
3. If `*` found → parse func ptr with `parse_func_ptr_after_ret()`
4. If not → restore state, continue normal parsing

### Parser Limits (stack arrays)
- 1024 statements per block, 1024 declarations per file
- 64 call arguments, 128 struct fields, 128 switch arms
- 256 enum variants, 32 function parameters
- 16 intrinsic arguments, 16 switch arm values

## AST (ast.h)

### Node Kinds (NODE_*)
**Expressions:** NODE_INT_LIT, NODE_FLOAT_LIT, NODE_STRING_LIT, NODE_CHAR_LIT, NODE_BOOL_LIT, NODE_NULL_LIT, NODE_IDENT, NODE_BINARY, NODE_UNARY, NODE_CALL, NODE_INDEX, NODE_FIELD, NODE_ASSIGN, NODE_ORELSE, NODE_INTRINSIC, NODE_SLICE, NODE_ADDR_OF, NODE_DEREF, NODE_CAST (unused), NODE_SIZEOF (unused)

**Statements:** NODE_VAR_DECL, NODE_BLOCK, NODE_IF, NODE_FOR, NODE_WHILE, NODE_SWITCH, NODE_RETURN, NODE_BREAK, NODE_CONTINUE, NODE_DEFER, NODE_EXPR_STMT, NODE_ASM

**Declarations:** NODE_FUNC_DECL, NODE_STRUCT_DECL, NODE_ENUM_DECL, NODE_UNION_DECL, NODE_GLOBAL_VAR, NODE_IMPORT, NODE_INTERRUPT, NODE_TYPEDEF, NODE_FILE

### Type Nodes (TYNODE_*)
TYNODE_U8..TYNODE_U64, TYNODE_I8..TYNODE_I64, TYNODE_USIZE, TYNODE_F32, TYNODE_F64, TYNODE_BOOL, TYNODE_VOID, TYNODE_OPAQUE, TYNODE_NAMED, TYNODE_POINTER, TYNODE_OPTIONAL, TYNODE_ARRAY, TYNODE_SLICE, TYNODE_CONST, TYNODE_POOL, TYNODE_RING, TYNODE_ARENA, TYNODE_HANDLE, TYNODE_FUNC_PTR

## Type System (types.c/h)

### Type Kinds (TYPE_*)
TYPE_VOID, TYPE_BOOL, TYPE_U8..TYPE_U64, TYPE_USIZE, TYPE_I8..TYPE_I64, TYPE_F32, TYPE_F64, TYPE_POINTER, TYPE_OPTIONAL, TYPE_ARRAY, TYPE_SLICE, TYPE_STRUCT, TYPE_ENUM, TYPE_UNION, TYPE_OPAQUE, TYPE_POOL, TYPE_RING, TYPE_ARENA, TYPE_HANDLE, TYPE_FUNC_PTR, TYPE_DISTINCT

### Key Functions
- `type_equals(a, b)` — structural equality (note: ignores is_const on pointers)
- `can_implicit_coerce(from, to)` — allowed implicit conversions: small→big int, T→?T, T[N]→[]T, mut→const slice/ptr
- `type_is_integer(t)` — includes TYPE_ENUM (enums are i32 internally)
- `type_width(t)` — bit width (bool=8, usize=32 hardcoded)
- `type_name(t)` — returns string for error messages. Uses TWO alternating buffers (fixed in BUG-028) so `type_name(a), type_name(b)` works in one printf.

### Coercion Rules
- Widening: u8→u16→u32→u64, i8→i16→i32→i64 (implicit, safe)
- Narrowing: COMPILE ERROR — must use @truncate or @saturate
- Signed↔unsigned: COMPILE ERROR — must use @bitcast
- T → ?T: implicit (wraps value)
- T[N] → []T: implicit (creates slice)
- []T → *T: implicit (takes .ptr, for C interop)
- *T → bool: NOT allowed (use `if (opt) |val|` for optionals)

## Checker (checker.c) — ~1800 lines

### Key Functions
- `resolve_type(TypeNode*) → Type*` — converts parser type nodes to type system types
- `check_expr(Node*) → Type*` — type-checks expression, returns resolved type, stores in typemap
- `check_stmt(Node*)` — type-checks statement
- `register_decl(Node*)` — registers top-level declarations in scope (called first pass)
- `checker_check(file)` — full check: register_decl all decls, then check all bodies
- `checker_register_file(file)` — register only (for imported modules)
- `checker_check_bodies(file)` — check bodies only (for imported modules)
- `checker_get_type(Node*) → Type*` — retrieve resolved type from typemap (used by emitter)

### Builtin Method Type Resolution
NODE_FIELD (line ~830): When accessing `.method` on Pool/Ring/Arena, returns a placeholder type (often ty_void for methods needing args).

NODE_CALL (line ~630): When calling `obj.method(args)`, resolves actual return types:
- `pool.alloc()` → `?Handle(T)`
- `pool.get(h)` → `*T`
- `pool.free(h)` → void
- `ring.push(val)` → void
- `ring.pop()` → `?T`
- `ring.push_checked(val)` → `?void`
- `arena.alloc(T)` → `?*T` (resolved via scope_lookup on type name arg)
- `arena.alloc_slice(T, n)` → `?[]T` (same pattern)
- `arena.reset()` → void (warns if not inside defer)

### Scope System
- `Scope` has parent pointer for nested scopes
- `global_scope` — module-level, persists
- `current_scope` — changes during traversal
- `scope_lookup(scope, name, len)` — walks up scope chain
- `add_symbol(scope, name, len, type, line)` — adds to current scope
- Forward declarations: if existing symbol is func with no body, update it instead of error

### Typemap
Global hash map: `Node* → Type*`. Set during checking via `typemap_set()`. Read by emitter via `checker_get_type()`.

### Multi-Module Order
1. Register imported modules first (index 1..N)
2. Register main module last (index 0)
3. Check imported module bodies (`checker_check_bodies`)
4. Check main module (`checker_check`)

## Emitter (emitter.c) — ~1800 lines

### Key Patterns
- `emit_type(Type*)` — emits C type. For TYPE_FUNC_PTR emits complete anonymous `ret (*)(params)`.
- `emit_type_and_name(Type*, name, len)` — handles arrays (`T name[N]`) and func ptrs (`ret (*name)(params)`)
- `emit_expr(Node*)` — emits C expression
- `emit_stmt(Node*)` — emits C statement
- `emit_file(Node*)` — emits complete C file with preamble

### Preamble (emitted at top of every .c file)
- `#include <stdint.h>`, `<stdbool.h>`, `<string.h>`, `<stdlib.h>`
- Optional type typedefs: `_zer_opt_u8`, `_zer_opt_u16`, ..., `_zer_opt_bool`, `_zer_opt_void`
- Slice type typedefs: `_zer_slice_u8`, `_zer_slice_u32`
- Pool runtime functions: `_zer_pool_alloc`, `_zer_pool_get`, `_zer_pool_free`
- Ring runtime function: `_zer_ring_push` (pop is inlined)
- Arena runtime: `_zer_arena` typedef, `_zer_arena_alloc(arena*, size, align)` — bump allocator with `_Alignof` alignment
- Bounds check: `_zer_bounds_check`, `_zer_trap` — checks are inline in `emit_expr(NODE_INDEX)`, NOT statement-level. Uses comma operator: `(_zer_bounds_check(idx, len, ...), arr)[idx]`. This respects `&&`/`||` short-circuit and works in if/while/for conditions.

### Builtin Method Emission (Pool/Ring/Arena)
The emitter intercepts `obj.method()` calls (line ~350):
1. Check callee is `NODE_FIELD`, extract object node and method name
2. Look up object type via `checker_get_type(obj_node)` (falls back to global_scope)
3. Match type kind → method name → emit inline C

```c
// Detection pattern (simplified):
if (obj_node->kind == NODE_IDENT) {
    Type *obj_type = checker_get_type(obj_node);
    if (obj_type && obj_type->kind == TYPE_POOL) {
        // match method name, emit C, set handled = true
    }
}
```

**Pool methods:**
- `pool.alloc()` → `({ _zer_opt_u32 r; uint32_t h = _zer_pool_alloc(..., &ok); r = {h, ok}; r; })`
- `pool.get(h)` → `(*(T*)_zer_pool_get(slots, gen, used, slot_size, h, cap))`
- `pool.free(h)` → `_zer_pool_free(gen, used, h, cap)`

**Ring methods:**
- `ring.push(val)` → `({ T tmp = val; _zer_ring_push(data, &head, &tail, &count, cap, &tmp, sizeof(tmp)); })` — tail advances on full (BUG-137)
- `ring.pop()` → `({ _zer_opt_T r = {0}; if (count > 0) { r.value = data[tail]; r.has_value = 1; tail = (tail+1) % cap; count--; } r; })`
- `ring.push_checked(val)` → same as push but wrapped: check `count < cap` first, return `_zer_opt_void`

**Arena methods:**
- `Arena.over(buf)` → `((_zer_arena){ (uint8_t*)buf, sizeof(buf), 0 })` (for arrays) or `{ buf.ptr, buf.len, 0 }` (for slices)
- `arena.alloc(T)` → `((T*)_zer_arena_alloc(&arena, sizeof(T), _Alignof(T)))` — returns NULL if full, which is `?*T` null sentinel
- `arena.alloc_slice(T, n)` → statement expression: alloc `sizeof(T)*n`, wrap in `?[]T` struct with `.value.ptr`, `.value.len`, `.has_value`
- `arena.reset()` / `arena.unsafe_reset()` → `(arena.offset = 0)`

### Optional Type C Representations

**This is the #1 source of emitter bugs. Know these cold:**

```c
// ?*Task — pointer optional (null sentinel, zero overhead)
typedef Task* _optional_ptr_task;  // NOT actually typedef'd — just a raw pointer
// NULL = none, non-NULL = some
// Check: if (ptr)     Unwrap: just use ptr

// ?u32 — value optional (struct wrapper)
typedef struct { uint32_t value; uint8_t has_value; } _zer_opt_u32;
// Check: .has_value   Unwrap: .value

// ?void — void optional (NO value field!)
typedef struct { uint8_t has_value; } _zer_opt_void;
// Check: .has_value   Unwrap: NOTHING — there is no .value
// ⚠️ Accessing .value on _zer_opt_void is a GCC error

// ?Status — optional enum (uses _zer_opt_i32 since enums are int32_t)
// Emits as _zer_opt_i32, NOT anonymous struct
// ⚠️ BUG-042: Previously fell to anonymous struct default — caused GCC type mismatch

// ?FuncPtr — optional function pointer (null sentinel, same as ?*T)
// Uses IS_NULL_SENTINEL macro: TYPE_POINTER || TYPE_FUNC_PTR
// All null-sentinel checks in emitter use this macro — never check TYPE_POINTER alone

// ?[]T — optional slice (named typedef for all types)
// _zer_opt_slice_u8, _zer_opt_slice_StructName, etc.
// Previously anonymous — BUG-069 fixed with named typedefs

// []T — slice (named typedef for ALL types now)
// _zer_slice_u8..u64, i8..i64, usize, f32, f64 in preamble
// _zer_slice_StructName emitted after struct declarations
// Previously only u8/u32 had typedefs — BUG-069
```

**Return patterns:**
```c
// return null from ?T func:    return (_zer_opt_u32){ 0, 0 };
// return null from ?void func: return (_zer_opt_void){ 0 };     // ONE field
// bare return from ?T func:    return (_zer_opt_u32){ 0, 1 };
// bare return from ?void func: return (_zer_opt_void){ 1 };     // ONE field
// return value from ?T func:   return (_zer_opt_u32){ val, 1 };
// return ptr from ?*T func:    return ptr;  // just the pointer, NULL = none
```

### Slice Emission

```c
// []u8 → _zer_slice_u8   (typedef'd)
// []u32 → _zer_slice_u32  (typedef'd)
// []T (other) → struct { T* ptr; size_t len; }  (anonymous — each emit is a NEW type)

// Slice indexing: items.ptr[i]  — NOT items[i] (items is a struct, not array)
// Slice orelse unwrap: __auto_type items = _zer_or0.value;  — NOT explicit type
//   (avoids anonymous struct type mismatch between optional's .value and declared var)
```

### Orelse Emission

```c
// u32 x = expr orelse return;  →  (var decl path, emitter.c ~line 998)
__auto_type _zer_or0 = <expr>;
if (!_zer_or0.has_value) { return 0; }     // ?T path
uint32_t x = _zer_or0.value;

// *Task t = expr orelse return;  →  (pointer optional path)
__auto_type _zer_or0 = <expr>;
if (!_zer_or0) { return 0; }               // ?*T null check
Task* t = _zer_or0;

// push_checked(x) orelse return;  →  (?void expression stmt path)
({__auto_type _zer_tmp0 = <expr>;
  if (!_zer_tmp0.has_value) { return 0; }   // ?void — NO .value access
  (void)0; })
```

### Var Decl Optional Init
When target type is `?T` and inner is not pointer:
- `NODE_NULL_LIT` init → `{ 0, 0 }`
- `NODE_CALL` or `NODE_ORELSE` init → assign directly (might already be `?T`)
- `NODE_IDENT` init → check `checker_get_type()`: if already `?T`, assign directly; if plain `T`, wrap as `{ val, 1 }`
- Other expressions → wrap as `{ val, 1 }`

### Enum Emission

Enums emit as `#define` constants, not C enums:

```c
// ZER: enum Prio { low = 1, med = 5, high = 10 }
// Emitted C:
#define _ZER_Prio_low 1
#define _ZER_Prio_med 5
#define _ZER_Prio_high 10
```

**Pipeline:** Parser stores explicit value in `EnumVariant.value` (NODE_INT_LIT or NULL). Checker resolves to `SEVariant.value` (int32_t) with auto-increment for implicit values. Emitter reads `EnumVariant.value` from AST — if present uses it, otherwise auto-increments.

**Gaps with auto-increment work like C:** `enum Code { ok = 0, warn = 100, err, fatal }` → ok=0, warn=100, err=101, fatal=102.

**Switch emission:** Enum switch emits as `if/else if` chain (not C switch), comparing against `_ZER_EnumName_variant` constants.

### Naming Conventions in Emitted C
- User types: as-is (`Task`, `State`, `Packet`)
- Enum values: `_ZER_EnumName_variant` (e.g., `_ZER_State_idle`)
- Optional typedefs: `_zer_opt_T` (e.g., `_zer_opt_u32`, `_zer_opt_StructName`, `_zer_opt_UnionName`)
- Slice typedefs: `_zer_slice_T` (all primitives + `_zer_slice_StructName`, `_zer_slice_UnionName`)
- Optional slice typedefs: `_zer_opt_slice_T` (all primitives + struct/union names)
- Temporaries: `_zer_tmp0`, `_zer_uw0` (unwrap), `_zer_or0` (orelse), `_zer_sat0` (saturate)
- Pool helpers: `_zer_pool_alloc`, `_zer_pool_get`, `_zer_pool_free`
- Ring helper: `_zer_ring_push`
- Arena: `_zer_arena` (typedef), `_zer_arena_alloc` (runtime helper)

### Function Pointer Handling
- `?FuncPtr` uses null sentinel (same as `?*T`) — `IS_NULL_SENTINEL` macro
- `emit_type_and_name` handles TYPE_FUNC_PTR, TYPE_OPTIONAL+FUNC_PTR, TYPE_DISTINCT+FUNC_PTR for name-inside-parens syntax
- `typedef u32 (*Callback)(u32);` supported in both regular and distinct typedef paths
- Checker unwraps TYPE_DISTINCT before TYPE_FUNC_PTR dispatch in NODE_CALL
- Negative enum values: parser produces NODE_UNARY(MINUS, INT_LIT) — checker and emitter both handle this pattern

### Union Switch Emission
Union switch takes a POINTER to the original: `__auto_type *_zer_swp = &(expr)`. Tag checked via `_zer_swp->_tag`. Immutable capture copies: `__auto_type v = _zer_swp->variant`. Mutable capture takes pointer: `Type *v = &_zer_swp->variant`. This ensures `|*v|` modifications persist to the original union.

### emit_file and emit_file_no_preamble — UNIFIED (RF2)
Both functions now call `emit_top_level_decl(e, decl, file_node, i)`. Adding a new NODE kind only requires updating that one function. The old pattern of two parallel switch statements (which caused BUG-086/087) is eliminated.

### GCC Extensions Used
- `__auto_type` — C equivalent of `auto` (type inference)
- `({...})` — statement expressions (GCC/Clang extension)
- `_Alignof(T)` — type alignment (C11, supported by GCC/Clang)
- These make the emitted C NOT portable to MSVC

## ZER-CHECK (zercheck.c) — ~400 lines

### What It Checks
Path-sensitive handle tracking after type checker, before emitter:
- Use after free: `pool.free(h); pool.get(h)` → error
- Double free: `pool.free(h); pool.free(h)` → error
- Wrong pool: `pool_a.alloc() → h; pool_b.get(h)` → error
- Free in loop: `for { pool.free(h); }` → error (may use-after-free next iteration)

### Handle States
`HS_UNKNOWN` → `HS_ALIVE` (after alloc) → `HS_FREED` (after free)

### Handle Aliasing (BUG-082 fix)
When `Handle(T) alias = h1` or `h2 = h1` is detected, the new variable is registered with the same state, pool_id, and alloc_line as the source. When `pool.free(h)` is called, all handles with the same pool_id + alloc_line are also marked HS_FREED (aliases of the same allocation). Independent handles from the same pool (different alloc_line) are unaffected.

### Path Merging (under-approximation — zero false positives)
- **if/else**: mark freed only if freed on BOTH branches
- **if without else**: keep original state (accept false negatives over false positives)
- **switch**: mark freed only if freed in ALL arms
- **loops**: check that handles alive before loop aren't freed inside

### Scope
- Looks up Pool types via `checker_get_type()` first, then `global_scope` fallback
- Checks `NODE_FUNC_DECL` and `NODE_INTERRUPT` bodies

## Test Files
| File | What | Count |
|---|---|---|
| `test_lexer.c` | Token scanning | 218 |
| `test_parser.c` | AST construction | 70 |
| `test_parser_edge.c` | Edge cases, func ptrs, overflow | 93 |
| `test_modules/` | Multi-file imports, typedefs, interrupts | 6 |
| `test_checker.c` | Type checking basic | 71 |
| `test_checker_full.c` | Full spec coverage + security + audit | 237 |
| `test_extra.c` | Additional checker | 18 |
| `test_gaps.c` | Gap coverage | 4 |
| `test_emit.c` | Full E2E (ZER→C→GCC→run) | 160 |
| `test_zercheck.c` | Handle tracking, aliasing, params | 24 |
| `test_fuzz.c` | Parser adversarial inputs | 491 |
| `test_firmware_patterns.c` | Round 1 firmware | 39 |
| `test_firmware_patterns2.c` | Round 2 firmware | 41 |
| `test_firmware_patterns3.c` | Round 3 firmware | 22 |
| `test_production.c` | Production firmware E2E | 14 |

### Test Helpers
- `test_emit.c`: `test_compile_and_run(zer_src, expected_exit, name)` — full E2E
- `test_emit.c`: `test_compile_only(zer_src, name)` — ZER→C→GCC, no run
- `test_checker_full.c`: `ok(src, name)` — must type-check OK
- `test_checker_full.c`: `err(src, name)` — must produce type error
- `test_zercheck.c`: `ok(src, name)` — must pass ZER-CHECK
- `test_zercheck.c`: `err(src, name)` — must fail ZER-CHECK

## Common Bug Patterns (from 127 bugs fixed, 19 audit rounds + 4 QEMU demos)
1. **Checker returns `ty_void` for unhandled builtin method** — always check NODE_CALL handler for new methods
2. **Emitter uses `global_scope` only** — use `checker_get_type()` first for local var support
3. **Optional emission mismatch** — `?void` has no `.value`, `?*T` uses null sentinel (no struct)
4. **Parser needs braces** — if/else/for/while bodies are always blocks
5. **Enum values need `_ZER_` prefix in emitted C** — `State.idle` → `_ZER_State_idle`
6. **Forward decl then definition** — checker must update existing symbol, not reject as duplicate
7. **Bounds checks must be inline, not hoisted** — hoisting breaks short-circuit `&&`/`||` and misses conditions. Use comma operator pattern in `emit_expr(NODE_INDEX)`.
8. **Scope/lifetime checks must walk field/index chains** — `global.ptr = &local` has target `NODE_FIELD`, not `NODE_IDENT`. Walk to root before checking.
9. **Union switch arms must lock the switched-on variable** — mutation during mutable capture creates type confusion. Track `union_switch_var` in Checker.
10. **Handle aliasing must propagate freed state** — `alias = h` copies the handle value. Freeing the original must mark all aliases freed (match by pool_id + alloc_line).
11. **`emit_top_level_decl` handles ALL declaration types (RF2)** — Unified dispatch function. NODE_TYPEDEF and NODE_INTERRUPT are handled here. Adding new declaration types: update this ONE function.
12. **`is_null_sentinel()` must unwrap TYPE_DISTINCT** — `?DistinctFuncPtr` must be treated as null sentinel. Use `is_null_sentinel(type)` function, not `IS_NULL_SENTINEL(kind)` macro.
13. **NODE_SLICE must use named typedefs for ALL primitives** — not just u8/u32. Anonymous structs create type mismatches with named `_zer_slice_T`.
14. **Struct field lookup must error on miss** — don't silently return ty_void (old UFCS fallback). Same for field access on non-struct types.
15. **If-unwrap and switch capture defer scope** — these paths unwrap blocks to inject captures. Must save `defer_stack.count` before, emit `emit_defers_from()` after, then restore count. Without this, defers fire at function exit instead of block exit.
16. **Use `type_unwrap_distinct(t)` helper for ALL type dispatch** — defined in `types.h`. Applies to: emit_type inner switches (optional, slice, optional-slice element), NODE_FIELD handler (struct/union/pointer dispatch), switch exhaustiveness checks, auto-zero paths (global + local), intrinsic validation, NODE_SLICE expression emission. Always unwrap: `Type *inner = type_unwrap_distinct(t);`. Never write the unwrap manually.
17. **ZER-CHECK must track Handle parameters** — `zc_check_function` scans params for TYNODE_HANDLE and registers as HS_ALIVE. Without this, use-after-free on param handles goes undetected.
18. **`[]bool` needs TYPE_BOOL in all slice type switches** — bool = uint8_t, maps to `_zer_slice_u8`. Missing from any emit_type slice switch causes anonymous struct mismatch.
19. **Emitter uses `resolve_tynode()` — typemap first, fallback to `resolve_type_for_emit` (RF3)** — `resolve_type()` in checker caches every resolved TypeNode in typemap. Emitter's `resolve_tynode(e, tn)` reads from typemap via `checker_get_type(e->checker, (Node *)tn)`. Falls back to old `resolve_type_for_emit()` for uncached TypeNodes. New type constructs only need updating in `resolve_type_inner()` — emitter gets them automatically.
20. **`eval_const_expr()` in `ast.h` for compile-time sizes** — Array/Pool/Ring sizes support expressions (`4 * 256`, `512 + 512`). Without the constant folder, non-literal sizes silently become 0.
21. **Scope escape must check implicit array-to-slice coercion in assignments** — `global_slice = local_array` bypasses `&local` check because no TOK_AMP is involved. Check TYPE_ARRAY value → TYPE_SLICE target with local/global mismatch.
22. **String literals are const — block assignment to mutable `[]u8`** — Check NODE_STRING_LIT in var-decl and assignment. Only `const []u8` targets allowed. Function args still work (slice struct is copied).
23. **Bit extraction mask has 3 paths** — constant >= 64 → `~(uint64_t)0`; constant < 64 → precomputed `(1ull << N) - 1`; runtime variables → safe ternary. Never raw `1ull << (high - low + 1)`.
26. **`checker_get_type(Checker *c, Node *node)` — takes Checker* (RF1)** — Typemap moved into Checker struct. All call sites pass the Checker: emitter uses `checker_get_type(e->checker, node)`, zercheck uses `checker_get_type(zc->checker, node)`. No more global typemap state.
27. **Source mapping via `#line` directives** — `emitter.source_file` set per module. Emits `#line N "file.zer"` before each non-block statement. NULL = disabled (for tests).
24. **Shift operators use `_zer_shl`/`_zer_shr` macros** — ZER spec: shift >= width = 0. C has UB. Macros use GCC statement expression for single-eval. Compound shifts (`<<=`) emit `x = _zer_shl(x, n)`.
25. **Bounds check side-effect detection** — NODE_CALL AND NODE_ASSIGN both trigger single-eval path (GCC statement expression). All other index expressions use comma operator (lvalue-safe, double-eval OK for pure expressions).
28. **Bare `if(optional)` / `while(optional)` must emit `.has_value`** — Non-null-sentinel optionals (`?u32`, `?bool`, `?void`) are structs in C. `if (val)` where val is a struct is a GCC error. The emitter's regular-if and while paths must check `checker_get_type(cond)` — if `TYPE_OPTIONAL` and `!is_null_sentinel(inner)`, append `.has_value`. The if-unwrap (`|val|`) path already handles this.
29. **`const` on var-decl must propagate to the Type** — Parser puts `const` into `var_decl.is_const` (NOT into TYNODE_CONST). The checker must create a const-qualified Type in NODE_VAR_DECL/NODE_GLOBAL_VAR: `type_const_slice()` / `type_const_pointer()`. Without this, `check_expr(NODE_IDENT)` returns a non-const Type, and const→mutable function arg checks don't fire. Function param types ARE wrapped in TYNODE_CONST by the parser (because `parse_type()` handles `const` prefix), so they resolve correctly through `resolve_type(TYNODE_CONST)`.
30. **Array init/assignment → `memcpy`** — C arrays aren't lvalues. `u32[4] b = a` and `b = a` produce invalid C. Emitter detects TYPE_ARRAY in var-decl init (emit `= {0}` + memcpy) and NODE_ASSIGN (pointer-hoist + memcpy). **BUG-252:** Assignment uses `({ __typeof__(target) *_p = &(target); memcpy(_p, src, sizeof(*_p)); })` to avoid double-evaluating side-effecting targets like `get_s().arr`.
31. **`emit_type(TYPE_POINTER)` emits `const` keyword** — When `pointer.is_const` is true, emit `const` before inner type. Same check as `is_volatile`.
32. **`@cstr` bounds check for fixed arrays** — When destination is TYPE_ARRAY, emit `if (slice.len + 1 > size) _zer_trap(...)` before memcpy.
33. **`is_arena_derived` only for LOCAL arenas** — When setting flag, check arena object against `global_scope`. Global arenas outlive functions, so returning their pointers is safe.
34. **`?void` return with void expression** — `return do_stuff()` in `?void` function: emit void call as statement, then `return (_zer_opt_void){ 1 };` separately. Can't put void expression in compound literal initializer.
35. **Compound shift `<<=`/`>>=` with side-effect targets** — `arr[func()] <<= 1` would double-eval func(). Detect via NODE_INDEX + NODE_CALL/NODE_ASSIGN on index, hoist target via pointer: `*({ auto *_p = &target; *_p = _zer_shl(*_p, n); })`.
36. **Enum/union exhaustiveness supports >64 variants** — Uses `uint8_t[]` byte array (stack up to 256, arena for larger) instead of `uint64_t` bitmask.
37. **Arena-derived propagates through assignment AND return walks chains** — `h.ptr = arena.alloc()` marks `h` as arena-derived. `return h.ptr` walks field/index chains to root `h` and checks the flag. Both var-decl and assignment paths detect `arena.alloc`/`arena.alloc_slice` including through orelse.
38. **Division/modulo wrapped in zero-check trap** — `/` and `%` emit `({ auto _d = divisor; if (_d == 0) _zer_trap(...); (a / _d); })`. Same for `/=` and `%=`. Single-eval of divisor via GCC statement expression.
39. **Integer literal range validation** — `is_literal_compatible` checks value fits target: u8 0-255, i8 -128..127, u16 0-65535, etc. Negative literals reject all unsigned types. Without this, GCC silently truncates.
40. **Bit extraction high index validated against type width** — Checker NODE_SLICE checks constant `high < type_width(obj)`. Prevents reading junk bits beyond the type's bit width.
41. **`[]T → *T` coercion removed** — Empty slice has `ptr=NULL`, violates `*T` non-null guarantee. Removed from `can_implicit_coerce` in types.c. Use `.ptr` explicitly for C interop.
42. **`is_local_derived` tracks pointers to locals** — `p = &x` (local) sets `p.is_local_derived`. Propagated through aliases and field/index chains. Return check rejects alongside `is_arena_derived`. Defined in types.h on Symbol struct.
43. **Base-object side effects in NODE_INDEX** — `get_slice()[0]` hoists slice into `__auto_type _zer_obj` temp when object chain contains NODE_CALL or NODE_ASSIGN. Prevents double-evaluation of side-effecting base objects.
44. **Per-module scope for multi-module type isolation** — `checker_push_module_scope(c, file_node)` pushes a scope with the module's own struct/union/enum types before `checker_check_bodies`. This overrides the global scope so each module's functions see their own types. `checker_pop_module_scope(c)` pops it after. Called from `zerc_main.c` for each imported module.
45. **Module-prefix mangling in emitter** — `EMIT_STRUCT_NAME`/`EMIT_UNION_NAME`/`EMIT_ENUM_NAME` macros emit `module_prefix + "_" + name` when prefix is set. `module_prefix` stored on Type struct (struct_type, enum_type, union_type). Set during `register_decl` from `checker.current_module`. Main module has NULL prefix (no mangling).
46. **Cross-module same-named types allowed** — `add_symbol` silently allows when `existing_mod != c->current_module` for struct/union/enum types. Each module's body check resolves to its own type via module scope. Emitter mangles names to prevent C collision.
47. **Safety flags cleared+recomputed on reassignment** — `is_local_derived` and `is_arena_derived` on Symbol are cleared+recomputed in NODE_ASSIGN (`op == TOK_EQ`). Clear both on target root first, then re-derive: `&local` → set local-derived, alias of derived ident → propagate. Fixes both false positives (reassign to safe) and false negatives (reassign to unsafe).
48. **`all_paths_return` handles infinite loops** — NODE_WHILE returns true when condition is `NODE_BOOL_LIT(true)`. NODE_FOR returns true when `cond` is NULL. Infinite loops are terminators — never exit normally.
49. **Compile-time OOB for constant array index** — In NODE_INDEX, if index is NODE_INT_LIT and object is TYPE_ARRAY, compare `idx_val >= array.size` → compile error. Variable indices still rely on runtime bounds checks. Prevents obvious buffer overflows before they reach the emitter.
50. **Switch on struct-based optionals** — Emitter detects `is_opt_switch` when switch expression is `TYPE_OPTIONAL` with non-null-sentinel inner. Arms compare `_zer_sw.has_value && _zer_sw.value == X` instead of raw struct comparison. Captures extract `.value` (immutable `__auto_type v = _zer_sw.value`) or `&.value` (mutable pointer). Null-sentinel optionals use direct comparison as before.
58. **`contains_break` walks orelse/var-decl/expr-stmt** — NODE_ORELSE checks `fallback_is_break`. NODE_VAR_DECL recurses into `init`. NODE_EXPR_STMT recurses into `expr`. Without these, `orelse break` inside while(true) is invisible to break detection.
59. **Local-derived escape via assignment to global** — After flag propagation in NODE_ASSIGN, check if value ident has `is_local_derived` and target root is global/static. Catches `global_p = local_ptr` that the direct `&local` check misses.
60. **Orelse unwrap preserves is_local_derived** — Var-decl init flag propagation walks through NODE_ORELSE (`init_root = init_root->orelse.expr`) before walking field/index chains. Preserves `is_local_derived`/`is_arena_derived` from the orelse expression.
56. **orelse &local propagates is_local_derived** — Var-decl init checks both direct `&local` AND `NODE_ORELSE` fallback `&local`. Both paths set `sym->is_local_derived`. Without this, `p = opt orelse &local_x; return p` is a dangling pointer escape.
57. **Slice from local array marks is_local_derived** — `[]T s = local_arr` where init is `NODE_IDENT(TYPE_ARRAY)` and source is local → `sym->is_local_derived = true`. Array→slice coercion creates slice pointing to stack. Return check catches it via existing `is_local_derived` path.
54. **`contains_break` guard on infinite loop terminator** — `all_paths_return` for while(true)/for(;;) checks `!contains_break(body)`. `contains_break` walks recursively but stops at nested NODE_WHILE/NODE_FOR (their breaks target the inner loop). Without this, `while(true) { break; }` falsely passes return analysis.
55. **Type query functions unwrap distinct** — `type_width`, `type_is_integer`, `type_is_signed`, `type_is_unsigned`, `type_is_float` all call `type_unwrap_distinct(a)` first. Without this, `type_width(distinct u32)` returns 0, breaking @size and intrinsics for distinct types.
52. **`@size(T)` as compile-time constant** — In TYNODE_ARRAY resolution, when `eval_const_expr` fails and the size expr is `NODE_INTRINSIC("size")`, resolve the type and compute bytes: `type_width/8` for primitives, field sum for structs, 4 for pointers. Emitter still uses `sizeof(T)` at runtime. The key insight: `eval_const_expr` lives in `ast.h` (no checker access), so @size handling must be in the checker-specific array size path.
53. **Duplicate enum variant check** — In `NODE_ENUM_DECL` registration, inner loop checks `variants[j].name == variants[i].name` for j < i. Same pattern as struct field duplicate check (pattern 56/BUG-191). Prevents GCC `#define` redefinition.
51. **Volatile propagation on `&`** — `check_expr(NODE_UNARY/TOK_AMP)` looks up operand symbol; if `is_volatile`, sets `result->pointer.is_volatile = true`. Var-decl init checks: `init.pointer.is_volatile && !type.pointer.is_volatile && !var_decl.is_volatile` → error. Parser puts `volatile` on `var_decl.is_volatile` (not TYNODE_VOLATILE) for local/global var decls.
61. **Sub-slice from local marks is_local_derived** — Var-decl init walks through NODE_SLICE to find the sliced object, then field/index chains to root. If root is local TYPE_ARRAY, marks symbol. Catches `s = arr[1..4]` that BUG-203's NODE_IDENT check missed.
62. **&union_var blocked in mutable capture arm** — In TOK_AMP handler, if operand name matches `union_switch_var`, error. Prevents creating pointer aliases that bypass the variant lock name check.
64. **Bit-set assignment** — `reg[7..0] = val` in NODE_ASSIGN: detect NODE_SLICE target on integer type, emit `reg = (reg & ~mask) | ((val << lo) & mask)`. Constant ranges use precomputed masks. Same safe mask generation as bit extraction (3 paths for width).
65. **Union switch lock walks to root** — Lock setup walks through NODE_FIELD/NODE_INDEX/NODE_UNARY(STAR) to find root ident. Mutation check does the same. Catches `s.msg.b = 20` inside `switch(s.msg)` arm.
66. **If-unwrap capture propagates safety flags** — When creating if-unwrap capture symbol, walk condition expression to root ident. If root has `is_local_derived` or `is_arena_derived`, propagate to capture symbol. Prevents dangling pointer return through captures.
67. **Static declarations registered globally** — `checker_register_file` no longer skips static vars/functions. They must be visible to the module's own function bodies during `checker_check_bodies`. Cross-module visibility handled by module scope system.
68. **Slice-to-slice propagates is_local_derived** — When checking slice init, look up source symbol FIRST. If `is_local_derived`, propagate immediately (before TYPE_ARRAY root check). Catches `s2 = s[0..2]` where `s` was already marked local-derived.
69. **Unary ~ and - narrow type cast** — For u8/u16/i8/i16 results, wrap `~` and `-` in type cast: `(uint8_t)(~a)`. Same integer promotion issue as binary arithmetic (BUG-186).
72. **Function/global name mangling** — `module_prefix` on Symbol struct. NODE_IDENT looks up global scope for prefix, emits `mod_name` instead of `name`. Function/global declarations use `EMIT_MANGLED_NAME`. Static symbols not mangled (module-private).
73. **@size alignment matches C sizeof** — Constant resolution computes field alignment (align each field to its natural boundary, pad struct to largest alignment). Packed structs use alignment 1. Fixes mismatch between checker's constant-eval size and GCC's sizeof.
74. **Volatile @cstr byte loop** — If destination ident has `is_volatile`, cast to `volatile uint8_t*` and emit `for (_i = 0; _i < len; _i++) dst[_i] = src[_i]` instead of `memcpy`. Prevents GCC from optimizing away writes to MMIO/DMA buffers.
70. **Bit-set single-eval via pointer hoist** — `({ __typeof__(obj) *_p = &(obj); *_p = (*_p & ~mask) | ((val << lo) & mask); })`. `__typeof__` doesn't evaluate. `&(obj)` is the single evaluation point. `*_p` reads/writes through cached pointer. Prevents double-eval of side-effecting targets like `regs[next_idx()]`.
71. **Compile-time slice bounds for arrays** — In NODE_SLICE, if object is TYPE_ARRAY and end (or start) is a constant, check against `array.size`. Catches `arr[0..15]` on `u8[10]` at compile time instead of runtime trap.
63. **@cstr slice destination bounds check** — For TYPE_SLICE dest, hoists slice into `__auto_type _zer_cd` temp for `.len`, uses `.ptr` for memcpy target. Emits `if (src.len + 1 > dest.len) _zer_trap(...)`. Array dest check was already in place (BUG-152).
75. **Recursive struct by value rejected** — In `register_decl(NODE_STRUCT_DECL)`, after `resolve_type(fd->type)`, check `sf->type == t` (the struct being defined). Catches `struct S { S next; }` — GCC would emit "incomplete type". Use `*S` pointer for self-referential types.
76. **Const propagation on `&`** — TOK_AMP handler propagates `sym->is_const` to `result->pointer.is_const`. Existing const-mutable checks (type_equals, call/return/assign/init) then catch mismatch. `&const_var` → const pointer prevents write-through-mutable-alias to `.rodata`.
77. **Static mangled keys in global scope** — `checker_push_module_scope` registers statics under mangled key (`module_name`) in global scope. Prevents collision between same-named statics in different modules. Emitter NODE_IDENT tries mangled lookup (`current_module + "_" + name`) when raw lookup fails.
78. **Pointer parameter escape** — NODE_ASSIGN `&local` escape check extended: if target root is a pointer parameter and assignment goes through field access, treated as potential escape (parameter may alias globals). `target_is_param_ptr` flag checked alongside `target_is_static`/`target_is_global`.
79. **@size(void) and @size(opaque) rejected** — In @size handler, resolve `type_arg`, reject TYPE_VOID and TYPE_OPAQUE (no meaningful size). `@size(*opaque)` still works (pointer has known size 4).
80. **Global symbol mangled keys** — `checker_register_file` registers imported non-static functions/globals under mangled key (`module_name`) in global scope, in addition to raw key. Emitter NODE_IDENT prefers mangled lookup (`current_module + "_" + name`) when `current_module` is set. Falls back to raw lookup for main module calls. Prevents cross-module variable/function resolution errors when modules share identifiers.
81. **Recursive struct unwraps arrays** — BUG-227 self-reference check unwraps TYPE_ARRAY chain before comparing element type to struct being defined. `struct S { S[1] next; }` is caught (GCC would reject as incomplete element type).
82. **@cstr compile-time overflow** — In @cstr checker handler, if dest is TYPE_ARRAY and src is NODE_STRING_LIT, checks `string.length + 1 > array.size` → compile error. Runtime trap still fires for variable-length slices.
83. **check_expr recursion depth guard** — `c->expr_depth` counter in Checker struct. Incremented on entry, decremented on exit. Limit 1000. Prevents stack overflow on pathological input (deeply chained orelse, nested expressions). Returns `ty_void` with error on overflow.
84. **Const builtin method check** — `obj_is_const` flag computed by walking field/index chains to root symbol and checking `is_const`. All mutating Pool/Ring/Arena methods check this flag. Non-mutating methods (`get`, `over`) skip the check.
85. **Nested array return walks chains** — NODE_RETURN array→slice escape check walks field/index chains to root ident (not just NODE_IDENT). Catches `return s.arr` where `s` is local struct with array field.
85b. **NODE_RETURN walks through NODE_ORELSE for safety flags (BUG-251)** — `return opt orelse p` where `p` is local/arena-derived must be caught. The return escape check splits: if `ret.expr` is `NODE_ORELSE`, check BOTH `.orelse.expr` AND `.orelse.fallback` for is_local_derived/is_arena_derived roots.
85c. **Global non-null `*T` requires init (BUG-253)** — Check in BOTH `register_decl` (NODE_GLOBAL_VAR) and `check_stmt` (NODE_VAR_DECL). Auto-zero creates NULL, violating `*T` guarantee. `?*T` is exempt.
85d. **TOK_AMP walks field/index chains for const/volatile (BUG-254)** — `&arr[i]`, `&s.field` now walk to root ident, propagating `is_const` and `is_volatile`. Previously only checked direct `NODE_IDENT` operand (BUG-228).
85e. **NODE_ORELSE triggers single-eval in index (BUG-255)** — `arr[get() orelse 0]` now detected as side-effecting index. Added `NODE_ORELSE` to `idx_has_side_effects` check in emitter.
85f. **@ptrcast return checks local/arena-derived idents (BUG-256)** — `return @ptrcast(*u8, p)` where `p` is local/arena-derived. Only fires when return type is pointer (avoids false positives on value `@bitcast`).
86. **@cstr const destination check** — Looks up destination ident symbol; if `is_const`, error. Separate from the compile-time overflow check (BUG-234) which validates sizes.
88. **Typemap is per-Checker struct (RF1)** — `type_map`, `type_map_size`, `type_map_count` moved from static globals into Checker struct. `typemap_init/set/get` take `Checker*`. `checker_get_type()` now takes `Checker *c, Node *node`. Emitter uses `checker_get_type(e->checker, node)`. Eliminates use-after-free risk in LSP multi-request scenarios.
89. **Unified emit_top_level_decl (RF2)** — `emit_file` and `emit_file_no_preamble` now share a single `emit_top_level_decl()` function. Previously two parallel switch statements that had to stay in sync (caused BUG-086/087). Adding a new NODE kind now requires updating only one place.
90. **Mangled name buffers use arena allocation (RF4)** — Fixed-size `char[256]` buffers for module name mangling replaced with arena-allocated buffers sized to actual need. Eliminates silent truncation on long module+symbol names.
91. **@cstr volatile detection walks field/index chains (RF7)** — `@cstr(reg_block.buf, src)` now correctly detects volatile by walking through NODE_FIELD/NODE_INDEX to root ident. Previously only handled direct NODE_IDENT destinations.
93. **resolve_type stores in typemap for emitter access (RF3)** — `resolve_type()` split into `resolve_type()` (wrapper with cache check + store) and `resolve_type_inner()` (actual resolution). Every resolved TypeNode is stored in typemap keyed by `(Node *)tn`. Emitter's `resolve_tynode()` tries `checker_get_type(e->checker, (Node *)tn)` first, falls back to `resolve_type_for_emit()` for uncached TypeNodes. This is the transitional step — `resolve_type_for_emit` becomes dead code once all TypeNode paths are cached.
92. **Null literal error messages improved (RF6)** — `u32 x = null` now says "'null' can only be assigned to optional types" instead of confusing "cannot initialize X with 'void'".
87. **Parser var-decl detection: lightweight lookahead for IDENT** — For TOK_IDENT-starting statements, the parser uses 2-3 token lookahead instead of full speculative `parse_type()`. Patterns: IDENT IDENT → var decl, IDENT `[`...`]` IDENT → array decl, IDENT `(*` → func ptr decl. Saves scanner+current+previous, scans tokens, restores. No AST allocation or error suppression. Non-IDENT type tokens (`*`, `?`, `[]`) still use speculative `parse_type()` since they unambiguously start types.
7. **Defer stack scoping** — return emits ALL defers, break/continue emit only loop-scope defers
8. **Type arg parsing** — intrinsics use `type_arg`, but method calls pass types as NODE_IDENT expression args. Primitive type keywords (`u32`) can't be passed as args (only struct/enum names work as NODE_IDENT).
