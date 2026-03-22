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
- `can_implicit_coerce(from, to)` — allowed implicit conversions: small→big int, T→?T, T[N]→[]T, []T→*T
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
- Bounds check: `_zer_bounds_check`, `_zer_trap`

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
- `ring.push(val)` → `({ T tmp = val; _zer_ring_push(data, &head, &count, cap, &tmp, sizeof(tmp)); })`
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
- Optional typedefs: `_zer_opt_T` (e.g., `_zer_opt_u32`)
- Struct optional typedefs: `_zer_opt_StructName`
- Temporaries: `_zer_tmp0`, `_zer_uw0` (unwrap), `_zer_or0` (orelse), `_zer_sat0` (saturate)
- Pool helpers: `_zer_pool_alloc`, `_zer_pool_get`, `_zer_pool_free`
- Ring helper: `_zer_ring_push`
- Arena: `_zer_arena` (typedef), `_zer_arena_alloc` (runtime helper)

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
| `test_parser_edge.c` | Edge cases, func ptrs | 92 |
| `test_checker.c` | Type checking basic | 71 |
| `test_checker_full.c` | Full spec coverage | 176 |
| `test_extra.c` | Additional checker | 18 |
| `test_gaps.c` | Gap coverage | 4 |
| `test_emit.c` | Full E2E (ZER→C→GCC→run) | 122 |
| `test_zercheck.c` | Handle tracking | 17 |
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

## Common Bug Patterns (from 71 bugs fixed)
1. **Checker returns `ty_void` for unhandled builtin method** — always check NODE_CALL handler for new methods
2. **Emitter uses `global_scope` only** — use `checker_get_type()` first for local var support
3. **Optional emission mismatch** — `?void` has no `.value`, `?*T` uses null sentinel (no struct)
4. **Parser needs braces** — if/else/for/while bodies are always blocks
5. **Enum values need `_ZER_` prefix in emitted C** — `State.idle` → `_ZER_State_idle`
6. **Forward decl then definition** — checker must update existing symbol, not reject as duplicate
7. **Defer stack scoping** — return emits ALL defers, break/continue emit only loop-scope defers
8. **Type arg parsing** — intrinsics use `type_arg`, but method calls pass types as NODE_IDENT expression args. Primitive type keywords (`u32`) can't be passed as args (only struct/enum names work as NODE_IDENT).
