# Compiler Internals — Read When Working on Specific Components

## Pipeline Overview
```
source.zer → Scanner (lexer.c) → Parser (parser.c) → AST (ast.h)
           → Checker (checker.c) → ZER-CHECK (zercheck.c)
           → Emitter (emitter.c) → output.c → GCC

NOTE: zercheck integrated into zerc_main.c pipeline on 2026-04-03.
Before this date, zercheck only ran in test_zercheck.c test harness.
Now: checker_check() → zercheck_run() → emit_file(). UAF and
double-free are compile errors. Leaks are warnings (zercheck can't
perfectly track handles across function calls or in struct fields).
Arena allocations excluded from handle tracking (arena.alloc() does
not need individual free — arena.reset() frees everything).
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

## Error Display

Both checker and parser show source lines with caret underlines on errors:
```
file.zer:3: error: array index 10 is out of bounds for array of size 4
    3 |     arr[10] = 1;
      |     ^^^^^^^^^^^^
```

**Implementation:**
- `Checker.source` / `Parser.source` — pointer to source text (NULL = skip display)
- `print_source_line(stderr, source, line)` in checker.c — finds line N (1-based), prints `line | text` then `| ^^^` under content
- `print_source_line_p()` in parser.c — identical copy (avoids shared header for 20 lines)
- Called from `checker_error()`, `checker_warning()`, parser `error_at()`
- `zerc_main.c` sets `parser.source = m->source` and `checker.source = m->source` — switches when checking imported module bodies
- Tests/LSP: source=NULL (from memset zero in init), display silently skipped
- Carets span from first non-whitespace to end of line (capped at 60 chars)

**When modifying error reporting:** Always call `print_source_line` after printing the error text line. The function handles NULL source gracefully. If adding a new error path that doesn't go through `checker_error`/`checker_warning`, add the call manually.

**Parser `warn()` function:** Added for `[]T` deprecation. Suppressed when `parser.source == NULL` (test harness) to avoid noise from 200+ test strings using `[]T`.

**`?T` → `T` orelse hint:** When var-decl init type is `TYPE_OPTIONAL` and inner matches target type, error message includes "add 'orelse { return; }' to unwrap" hint. Checker line ~4275.

### Windows `--run` — `-mconsole` flag

`zerc_main.c` adds `-mconsole` to GCC invocation on Windows (`#ifdef _WIN32`). Without this, msys64 mingw GCC links as GUI app expecting `WinMain` instead of `main`. Linux/macOS unaffected.

### Default compile mode — temp .c, native-looking output

`zerc main.zer` now compiles directly to `main.exe` (or `main` on Linux). The `.c` intermediate is a temp file, deleted after GCC finishes. User sees `.zer → exe` — looks like a native compiler.

**Modes:**
- `zerc main.zer` → compile to exe (temp .c, deleted)
- `zerc main.zer --run` → compile + run (temp .c, deleted)
- `zerc main.zer --emit-c` → emit C to `main.c` (kept)
- `zerc main.zer -o out.c` → emit C to `out.c` (kept)
- `zerc main.zer -o out` → compile to `out` exe (temp .c, deleted)

**Implementation:** `use_temp_c` bool in `zerc_main.c`. When true, `remove(output_path)` called after GCC succeeds or fails. The `emit_c` flag set by `--emit-c` or when `-o` path ends in `.c`.

### Goto + Labels

**Lexer:** `TOK_GOTO` keyword, `TOK_COLON` token (`:` was not tokenized before).

**Parser:** `goto label;` → `NODE_GOTO` with label name/len. Labels detected by lookahead: if `TOK_IDENT` followed by `TOK_COLON`, parse as `NODE_LABEL`. Scanner state saved/restored on failed lookahead (same pattern as func ptr detection). Labels are statement-level, no semicolon after `label:`.

**Checker:** `NODE_GOTO` banned inside defer blocks (compile error). `NODE_LABEL` is a no-op. `check_goto_labels()` runs per function body after all statements checked: collects all labels via `collect_labels()` (recursive AST walk), checks for duplicates, then `validate_gotos()` verifies every goto target exists. Max 128 labels per function (stack array).

**Emitter:** `NODE_GOTO` → `goto label;`. `NODE_LABEL` → `label:;` (empty statement after colon for C compliance). Direct pass-through to C — no transformation needed.

**Design:** Both forward and backward goto allowed. Safe because: (1) auto-zero prevents uninitialized memory from skipped declarations, (2) defer fires before every goto (emitter calls `emit_defers(e)` before `goto label;`). The only restriction is no goto inside defer blocks.

**Audit fixes (2026-04-04):**
- `collect_labels()` / `validate_gotos()` now recurse into NODE_SWITCH arms, NODE_DEFER body, NODE_CRITICAL body (was missing — labels inside switch arms were invisible).
- `NODE_GOTO` emitter now calls `emit_defers(e)` before emitting `goto` — defers fire on goto same as return/break/continue.

**Known limitation:** zercheck is linear — backward goto UAF (`free(h); goto retry;` looping back) not caught at compile time. Runtime gen check catches it. Full CFG analysis would require ~500+ lines of refactoring zercheck to work on control-flow graphs instead of linear statement lists.

### Handle Auto-Deref (`h.field` → `slab.get(h).field`)

**Checker (NODE_FIELD, ~line 2960):** When object type is `TYPE_HANDLE`, unwrap element type, find struct field. Mark as non-storable (same as `.get()` result). Uses `slab_source` provenance on Symbol to know which allocator.

**Slab source tracking:** Set at var-decl init — when Handle is assigned from `pool.alloc()` or `slab.alloc()`, the variable's `Symbol.slab_source` points to the allocator Symbol. Fallback: `find_unique_allocator()` walks scopes to find the ONE Slab/Pool for that element type. Multiple → ambiguous → compile error.

**Emitter (NODE_FIELD, ~line 1290):** When object is Handle, emits `((T*)_zer_slab_get(&slab, h))->field` or equivalent `_zer_pool_get(...)`. Slab/Pool name comes from `slab_source` or `find_unique_allocator()`.

### alloc_ptr / free_ptr — `*T` from Slab/Pool

**Checker:** `alloc_ptr()` returns `?*T` (null sentinel optional pointer). `free_ptr(*T)` takes a pointer. Both on Slab and Pool. Same ISR ban as `slab.alloc()`.

**Emitter:** `alloc_ptr()` emits alloc + get combined — allocates a slot, gets the pointer, returns NULL if allocation fails. `free_ptr()` on Slab uses `_zer_slab_free_ptr()` (linear scan to find slot index from pointer address). On Pool, computes index from pointer arithmetic.

**zercheck (Level 9):** Extended to recognize `alloc_ptr` as allocation source and `free_ptr` as free call. NODE_FIELD checks root ident for freed status — `t.field` after `free_ptr(t)` is caught as UAF at compile time. Same ALIVE/FREED/MAYBE_FREED tracking as Handle.

**Preamble:** `_zer_slab_free_ptr(_zer_slab *s, void *ptr)` — scans all slots to find matching pointer, frees it. Traps if pointer not found (invalid free).

**Design decision:** `*Task` from `alloc_ptr()` is 100% compile-time safe for pure ZER code (zercheck tracks all uses). For C interop boundary (`*opaque` round-trips), Level 2+3+5 runtime backup. Handle remains available for cases requiring gen-check on every access (paranoid mode).

**Audit fixes (2026-04-04):**
- `free_ptr()` now type-checks argument — `*Motor` to `Task` pool is a compile error. Both Pool and Slab.
- Handle auto-deref verifies allocator exists at check time. No allocator in scope → compile error (was: emitter silently output `0`).
- const Handle semantics: `const Handle(Task) h; h.id = 42` is ALLOWED. Handle is a key (like const file descriptor), const key ≠ const data. Assignment checker sets `through_pointer = true` when TYPE_HANDLE found in field chain. This also fixes if-unwrap `|t|` + Handle auto-deref (capture is const but data mutation allowed).
- Ghost handle check extended to `alloc_ptr()` (was `alloc` only).
- zercheck recognizes `Task.delete()`/`Task.delete_ptr()` as free (TYPE_STRUCT method in `zc_check_call`). `Task.new()`/`Task.new_ptr()` recognized as alloc in `zc_check_var_init`.
- `Task.new()`/`Task.new_ptr()` banned in interrupt handler (same ISR check as slab.alloc).

### Handle(T)[N] — Array of Handles

Parser: after parsing `TYNODE_HANDLE`, checks for `[N]` suffix. If found, wraps in `TYNODE_ARRAY` with element = handle type. Auto-deref works on array elements: `tasks[i].field` emits gen-checked access.

### zercheck 9a/9b/9c — Extended *opaque compile-time tracking

**9a (struct field tracking):** `free()` on untracked key (e.g., parameter's struct field `c.data`) now registers the key as FREED with `pool_id = -2`. Subsequent use at `@ptrcast` or field access triggers compile error. Also: `alloc_ptr` recognized in NODE_ASSIGN tracking (was `alloc`-only).

**9b (cross-function summary):** `zc_build_summary()` now checks `TYNODE_POINTER` params (was `TYNODE_HANDLE` only). Functions like `void destroy(*Task t) { heap.free_ptr(t); }` produce FuncSummary that marks param 0 as FREED. Call sites see `t` as FREED after `destroy(t)`. Also: summary extraction checks `TYNODE_POINTER` params for final state.

**9c (return state analysis):** `NODE_RETURN` in zercheck checks if returned expression is FREED or MAYBE_FREED. `return p` where `p` was freed → compile error "returning freed pointer."

**Emitter cstdlib skip list:** `calloc`, `realloc`, `strdup`, `strndup`, `strlen` added to `is_cstdlib` in `emit_top_level_decl`. User declarations of these names are skipped to avoid conflicting with `--track-cptrs` wrappers (`_zer_opaque` type mismatch with libc's `void*`).

**Coverage after 9a+9b+9c:** ~98% compile-time for `*opaque`. Remaining ~2% is runtime: dynamic array indices (`cache[slot]`), C library internals (can't see inside C code). Runtime check is 1ns inline header at `@ptrcast`.

### orelse block null-sentinel bug (fixed 2026-04-05)

The `orelse { block }` path in the emitter (line ~1883) emitted literal `0` as the final expression of the statement expression. For null-sentinel `?*T` (like `?*Node` from `alloc_ptr`), this assigned integer 0 instead of the pointer. The `orelse return` bare path (line ~1863) was correct.

**Pattern to watch:** There are THREE orelse emission paths:
1. `orelse return/break/continue` (bare) — line ~1835. Emits `_zer_tmp` or `_zer_tmp.value`.
2. `orelse { block }` — line ~1869. Was emitting `0`, now emits `_zer_tmp` / `_zer_tmp.value`.
3. `orelse default_value` — line ~1884. Emits ternary `tmp ? tmp : default`.

Any future changes to orelse must update ALL THREE paths consistently. Check `is_ptr_optional` AND `is_void_optional` branching in each.

**BUG-401 audit fixes (2026-04-05):**
- Paths 2 and 3 now use `__typeof__` instead of `__auto_type` (preserves volatile/const on temp).
- Paths 2 and 3 now check `is_void_optional` — `?void` emits `(void)0` instead of accessing nonexistent `.value`.
- Division guard temps (lines 624, 630, 899) changed from `__auto_type` to `__typeof__` for volatile preservation.
- All `optional.inner->kind == TYPE_VOID` checks wrapped with `type_unwrap_distinct()` (14 sites in emitter + 1 in checker). Same for `pointer.inner->kind == TYPE_OPAQUE` (6 sites in emitter).
- Optional null init changed from `{ {0} }` to `{ 0, 0 }` (6 sites) — eliminates GCC "braces around scalar initializer" warning.

### Comptime Evaluator Architecture (2026-04-11, refactored)

The comptime evaluator is a compile-time interpreter for `comptime` function bodies. Evaluates to `int64_t`.

**Entry points (3 call sites in checker.c):**
1. `check_expr` NODE_CALL handler — resolves comptime call during expression type checking
2. `eval_comptime_call_subst` — resolves nested comptime calls from within `eval_const_expr_subst`
3. `checker_check_bodies` NODE_STATIC_ASSERT handler — resolves comptime calls in static_assert

**Call chain:** caller creates `ComptimeCtx` with params → `eval_comptime_block(body, &ctx)` → walks statements → `ct_ctx_free(&ctx)`.

**`ComptimeCtx` — mutable shared evaluation context:**
```c
typedef struct {
    ComptimeParam stack[8];    // stack-first small buffer
    ComptimeParam *locals;     // points to stack or malloc'd
    int count, capacity;
} ComptimeCtx;
```
- `ct_ctx_init(ctx, params, count)` — initialize from function parameters
- `ct_ctx_set(ctx, name, len, value)` — add or update binding
- `ct_ctx_free(ctx)` — free if malloc'd
- Block scoping via `saved_count` — pop locals on block exit
- Loop bodies share ctx (mutations persist across iterations — NO copy)

**`ct_eval_assign(ctx, assign_node)` — compound assignment helper:**
Handles all compound operators (`+=`, `-=`, `*=`, `/=`, `%=`, `<<=`, `>>=`, `&=`, `|=`, `^=`, `=`). ONE function, used by both block-level assignment and for-loop step. No duplicate code.

**`eval_comptime_block` handles (all via recursive `ComptimeCtx`):**
- `NODE_VAR_DECL` — evaluate init, `ct_ctx_set`
- `NODE_EXPR_STMT(NODE_ASSIGN)` — `ct_eval_assign(ctx, ...)`
- `NODE_FOR` — init + condition + recursive body + step, max 10000 iterations
- `NODE_WHILE` — condition + recursive body, max 10000 iterations
- `NODE_SWITCH` — evaluate expr, match arms, execute matching arm body
- `NODE_RETURN` — evaluate expression, return value
- `NODE_IF` — conditional branching, recursive
- `NODE_BLOCK` — recursive walk with scope save/restore

**CRITICAL design decision: `ComptimeCtx` is passed by POINTER (mutable).**
Previous design copied locals on each `eval_comptime_block` call. This broke loop body mutations — `total += i` modified the copy, outer `total` stayed 0. The refactored design passes `ComptimeCtx*` so all mutations propagate. Block scoping uses `saved_count` to pop block-local variables without losing loop state.

**`eval_const_expr_subst` vs `eval_const_expr_scoped`:**
- `eval_const_expr_subst(expr, params, count)` — expression evaluation with parameter substitution. Used inside comptime evaluator. Takes raw `ComptimeParam*` array (read-only).
- `eval_const_expr_scoped(checker, expr)` — uses `eval_const_expr_ex` with const ident resolver callback. Used by static_assert and array size evaluation.
- For static_assert with comptime calls: `check_expr` MUST run FIRST to set `call.is_comptime_resolved`, then `eval_const_expr_scoped` reads `call.comptime_value`.

**Debugging comptime issues:**
1. Check if `is_comptime_resolved` is set on the NODE_CALL after `check_expr`
2. Check if `eval_comptime_block` returns CONST_EVAL_FAIL — trace which statement fails
3. For loops: verify ctx is passed by pointer (not copied) — mutations must persist
4. For top-level static_assert: verify `check_expr` runs before `eval_const_expr_scoped`
5. For block scoping: verify `saved_count` is restored — block-local vars must be popped

### static_assert (2026-04-11)

New keyword: `static_assert(expr, "message");` — compile-time assertion.
- Lexer: `TOK_STATIC_ASSERT` (13-char keyword with underscore)
- Parser: `NODE_STATIC_ASSERT` with `cond` (expression) and `message` (optional string)
- Checker: `check_expr(cond)` then `eval_const_expr_scoped(cond)`. False → compile error with message.
- Emitter: emits nothing (compile-time only)
- Works at both top-level and inside function bodies
- Top-level handled in `checker_check_bodies` (after comptime functions registered)

### Range-based for (2026-04-11, fixed)

`for (Type item in slice) { body }` — parser desugars to:
```c
for (usize __ri = 0; __ri < collection.len; __ri += 1) { Type item = collection[__ri]; body }
```
- `in` is a contextual keyword (only detected in for-loop `Type ident` context, not reserved)
- Collection expression CLONED for each AST position (no shared node pointers)
- Non-ident/non-field collection expressions REJECTED at parse time with clear error: "assign complex expression to a variable first"
- This ensures 100% correctness — no triple-evaluation of function calls
- Bounds-checked slice indexing (not raw pointer)

### Comptime Call In-Place Conversion (BUG-402/403/404, 2026-04-05)

**BUG-402:** `const u32 P = PLATFORM(); comptime if (P) { ... }` failed — comptime if condition resolver required `is_comptime` flag (only on comptime functions, not const vars from comptime calls). Fix: relaxed to `is_const`, also checks `call.is_comptime_resolved` on init expression.

**BUG-404:** Resolved comptime calls kept `NODE_CALL` kind. `eval_const_expr` (ast.h) only handles `NODE_INT_LIT` — so comptime results were invisible in binary expressions (`VER() > 1`), array sizes (`u8[BUF_SIZE()]`), and comptime if conditions.

**Fix:** After resolving a comptime call, convert the node in-place:
```c
node->kind = NODE_INT_LIT;
node->int_lit.value = (uint64_t)val;
```
This makes `eval_const_expr` work universally for comptime results — no special handling needed anywhere else. The emitter's `NODE_CALL` comptime handler (line 994) is now dead code (converted nodes go to `NODE_INT_LIT` instead).

**comptime if condition resolution now has 3 stages:**
1. `check_expr(cond)` — resolves comptime calls to NODE_INT_LIT
2. `eval_const_expr(cond)` — evaluates literals and binary expressions
3. Const ident fallback — looks up `is_const` symbol init value + `call.is_comptime_resolved`

**Also:** After resolution, checker sets `cond->kind = NODE_INT_LIT` so emitter's `eval_const_expr` in `emit_stmt(NODE_IF)` correctly strips dead branches.

### *opaque Test Coverage (2026-04-05)

**Finding: raw `malloc/free` with `*opaque` and `--track-cptrs`:** When `--track-cptrs` is active (default for `--run`), `*opaque` emits as `_zer_opaque` (struct with type_id), not `void*`. C's `malloc()` returns `void*` — type mismatch with `_zer_opaque`. Positive tests using raw malloc/free must use `--release` mode OR (preferred) use Slab/Pool instead. The existing negative tests work because they fail at zercheck before reaching GCC.

**Finding: Task.new() + explicit Slab for same type = ambiguous allocator.** `Slab(Task) heap;` + `Task.new()` creates two Slabs for the same struct type. Handle auto-deref can't pick which one → compile error. This is correct behavior — use one or the other, not both.

### Handle Auto-Deref Scalar Store (BUG-405, 2026-04-05)

The `is_non_storable` check blocked ALL var-decl/assignment from Handle auto-deref, including scalar field reads like `u32 v = h.value`. This was too aggressive — only `*T` pointer storage is dangerous (caches `pool.get()` result that becomes invalid after alloc/free). Scalar field values (`u32`, `bool`, etc.) are safe to store.

Fix: both check sites (NODE_ASSIGN line 1635, NODE_VAR_DECL line 4468) now only error when the result type is TYPE_POINTER, TYPE_SLICE, TYPE_STRUCT, or TYPE_UNION. Scalar types pass through.

### Distinct Typedef Wrapping Optional (BUG-409, 2026-04-05)

`distinct typedef ?u32 MaybeId` was completely broken — checker rejected `return null`, `orelse`, and `T → ?T` coercion. Emitter produced wrong C for return/orelse paths.

**Root causes (5 sites):**
1. `type_is_optional()` / `type_unwrap_optional()` — didn't unwrap distinct
2. `can_implicit_coerce()` T→?T path — didn't unwrap distinct on target
3. Emitter `is_ptr_optional` / `is_void_optional` — didn't unwrap distinct on orelse_type
4. Emitter return-null path — checked `current_func_ret->kind == TYPE_OPTIONAL` directly
5. Emitter bare-return path — same

**Fix pattern:** Every site that checks `->kind == TYPE_OPTIONAL` must either use `type_is_optional()` (which now unwraps) or call `type_unwrap_distinct()` explicitly. This is the same lesson as BUG-279/BUG-295 — distinct unwrap is needed EVERYWHERE types are dispatched.

### ?T[N] Parser Precedence Fix (BUG-413, 2026-04-05)

`?Handle(Task)[4]` was parsed as `OPTIONAL(ARRAY(HANDLE))` — optional wrapping an array. Indexing failed because you can't index an optional. User wants `ARRAY(OPTIONAL(HANDLE))` — array of optional handles.

**Fix:** In the `?T` parser path, after parsing the inner type, check if it was already wrapped in `TYNODE_ARRAY` by the inner parser (e.g., `Handle(T)[N]`). If so, swap: pull array outside optional → `ARRAY(OPTIONAL(inner_elem))`. Also handle `?NamedType[N]` by checking for `[N]` suffix after optional parsing.

**Design:** `?T[N]` = "array of N optional T values." This matches the intuition: `?u32[4]` = "4 slots, each either u32 or null." The alternative (`?(T[N])` = "optionally an entire array") is expressible with parentheses if ever needed, but extremely rare.

### Nested Comptime Function Calls (BUG-425, 2026-04-05)

`comptime u32 QUAD(u32 x) { return DOUBLE(DOUBLE(x)); }` rejected — checker's NODE_CALL handler validated comptime args via `eval_const_expr()` during body type-checking, but parameters are `NODE_IDENT` (not yet substituted). Fix: `bool in_comptime_body` flag on Checker, set during `check_func_body` for comptime functions. When true, skip the "all args must be compile-time constants" error. The real evaluation happens at the call site via `eval_comptime_block` + `eval_const_expr_subst`, which correctly substitutes params and handles nested calls.

**Pattern:** Any validation that requires compile-time constant args must check `c->in_comptime_body` — comptime function bodies contain parameter references that aren't constants until substituted at the call site.

### `!` Operator Accepts Integers (BUG-426, 2026-04-05)

`!integer` now returns bool (was: "'!' requires bool"). Common C idiom for `#ifndef` → `comptime if (!FLAG())`. Checker changed from `type_equals(operand, ty_bool)` to `!type_equals(operand, ty_bool) && !type_is_integer(operand)`. Result always TYPE_BOOL. Emitter unchanged (`(!expr)` works in C for both types).

### Dynamic Array Handle UAF Auto-Guard (2026-04-06)

`pool.free(handles[k])` with variable `k` followed by `handles[j].field` — compiler auto-inserts `if (j == k) { return <zero>; }` before the Handle auto-deref. Same compile-time decision pattern as bounds auto-guard.

**Checker:** `DynFreed` struct tracks `{array_name, freed_idx_node, all_freed}`. Set in pool.free/slab.free NODE_CALL handler when argument is `arr[variable]`. `all_freed` set when `c->in_loop` is true (loop free pattern). Checked in NODE_FIELD Handle auto-deref — when object is `arr[j]` and `arr` has DynFreed entry: `all_freed` → compile error, otherwise → auto-guard with UAF sentinel `array_size == UINT64_MAX`.

**Emitter:** `emit_auto_guards(NODE_FIELD)` checks for UAF sentinel. When found, looks up `dyn_freed` to find the freed index node, emits `if ((use_idx) == (freed_idx)) { return <zero>; }`.

**Coverage:** loop-free-all → compile error. Dynamic free + dynamic use → auto-guard. No dynamic free → no guard. Works for both Pool and Slab.

### Module-Qualified Variable Access (BUG-432, 2026-04-06)

`config.VERSION` failed — NODE_CALL had pre-`check_expr` interception for module-qualified calls (BUG-416), but NODE_FIELD did not. `check_expr(NODE_IDENT)` errored "undefined identifier" for the module name before NODE_FIELD could intercept. Fix: added same pre-`check_expr` interception in NODE_FIELD — when object is NODE_IDENT not found in scope, try `module__field` mangled lookup. Rewrite to NODE_IDENT with raw field name. **Pattern:** Both NODE_CALL and NODE_FIELD must intercept module-qualified access BEFORE `check_expr` on the object.

### *opaque Compile-Time Tracking Improvements (2026-04-06)

**Signature heuristic:** Bodyless `void func(*opaque)` auto-detected as free in `is_free_call` and `zc_apply_summary`. Covers cinclude C interop without annotations. Excludes functions named "free" (already handled by explicit check).

**UAF-at-call-site:** After a handle is FREED, passing it as argument to any non-free function → compile error. Skips free/delete calls and functions whose FuncSummary shows they free the param.

**Cross-module summaries:** `zercheck_run` now scans imported module ASTs (`zc->import_asts`) for FuncSummary building. Multi-pass (4 iterations) for wrapper chain propagation.

**@ptrcast alias tracking:** `*RealData r = @ptrcast(*RealData, handle)` creates alias link via `zc_check_var_init`. Walker through NODE_INTRINSIC extracts source ident. When `free_ptr(r)` fires, FREED propagates to `handle` via `alloc_line` match.

**Qualified call summary lookup:** `zc_apply_summary` handles NODE_FIELD callee (`module.func()`) by extracting field name as function name.

**CRITICAL: import_asts must use topological order.** BFS discovery order caused `mid_close` summary to be built BEFORE `base_del` — the dependency's summary didn't exist when needed. Fix: use `topo_order` (same array used for emission) when populating `zc->import_asts` in `zerc_main.c`. Dependencies first = summaries chain correctly through any depth. This was a 3-line fix that unblocked the entire multi-layer *opaque tracking.

**Wrapper allocator recognition:** `?*opaque r = wrapper_create()` now registers `r` as ALIVE. Any function call returning `?*opaque`, `?*T`, `*opaque`, or `*T` is treated as an allocation. This covers arbitrary wrapper depths — the variable is tracked regardless of how many layers the allocation goes through.

**Full cross-module *opaque tracking now works:**
```
// resource.zer — wrapper module
void resource_destroy(*opaque h) { *RealData r = @ptrcast(*RealData, h); slab.free_ptr(r); }

// main.zer
resource.resource_destroy(r);   // zercheck: r = FREED (summary + alias)
resource.resource_destroy(r);   // COMPILE ERROR: double free
resource.resource_read(r);      // COMPILE ERROR: use after free
```

### @ptrcast _zer_check_alive .ptr (BUG-431, 2026-04-05)

With `--track-cptrs`, `_zer_check_alive((void*)ctx, ...)` tried to cast `_zer_opaque` struct to `void*`. Fix: use `ctx.ptr` instead. **Pattern:** When `track_cptrs` is active, `*opaque` is `_zer_opaque` struct — any site emitting a `(void*)` cast on an opaque variable must use `.ptr` to extract the raw pointer.

### Const Ident in Comptime Call Args (BUG-430, 2026-04-05)

`const u32 perms = FLAG_READ() | FLAG_WRITE(); HAS_FLAG(perms, ...)` failed — `eval_const_expr` can't resolve `NODE_IDENT` (no scope access). Fix: `eval_const_expr_scoped(Checker *c, Node *n)` uses `eval_const_expr_ex` with `resolve_const_ident` callback. The callback walks scope chain for const symbols, recursively evaluates init values. Also: `sym->func_node = node` now set for local var-decls (was only globals/functions).

**Pattern:** Any site that evaluates user expressions at compile time and needs to resolve const variables should use `eval_const_expr_scoped` (or call `eval_const_expr_ex` with a custom resolver). Currently used for comptime call arg evaluation.

### Union Array Variant Emission (BUG-429, 2026-04-05)

`union Data { u32[4] quad; }` emitted `uint32_t[4] quad;` — invalid C. Union variant emission used `emit_type()` + manual name, which doesn't place array dimensions after the name. Fix: use `emit_type_and_name()` (same as struct fields). **Pattern:** Any site emitting a type+name pair must use `emit_type_and_name()`, never `emit_type()` + manual name — arrays and function pointers require special name placement.

### @atomic_or Name Length (BUG-427, 2026-04-05)

Atomic intrinsic prefix check was `nlen >= 10` but `"atomic_or"` is 9 chars. Fixed to `>= 9`. All other atomics (add=10, sub=10, and=10, xor=10, load=11, store=12, cas=10) were fine.

### @atomic_cas Literal Expected Value (BUG-428, 2026-04-05)

`@atomic_cas(&state, 0, 1)` emitted `&(0)` — taking address of rvalue literal. `__atomic_compare_exchange_n` needs `&expected` as lvalue. Fix: hoist expected into `__typeof__` temp inside GCC statement expression. Pattern: `({ __typeof__(*ptr) _zer_cas_exp = expected; __atomic_compare_exchange_n(ptr, &_zer_cas_exp, desired, 0, SEQ_CST, SEQ_CST); })`.

### Comptime Call in Pool/Ring Size (BUG-423, 2026-04-05)

`Pool(Item, POOL_SIZE())` failed — `eval_const_expr` ran before `check_expr` resolved the comptime call. Fix: call `check_expr` before `eval_const_expr` in TYNODE_POOL and TYNODE_RING. **General rule:** any site calling `eval_const_expr` on user expressions must call `check_expr` first.

### String Literal to Const Slice Field (BUG-424, 2026-04-05)

`e.msg = "hello"` where `msg` is `const [*]u8` was blocked. Assignment string literal check didn't check `slice.is_const`. Fix: added const + distinct unwrap check.

### Comptime Negative Return Values (BUG-415, 2026-04-05)

`comptime i32 NEG() { return -1; }` broke — in-place NODE_INT_LIT conversion stored `-1` as `uint64_t` (0xFFFFFFFFFFFFFFFF), failing `is_literal_compatible`.

**Fix:** `eval_const_expr_d` in ast.h extended to handle `NODE_CALL` with `is_comptime_resolved` — reads `comptime_value` directly. Comptime calls are NOT converted to NODE_INT_LIT (previous approach converted positive results to NODE_INT_LIT but couldn't handle negatives in uint64_t, creating a fragile two-path split). Now ALL resolved comptime calls stay as NODE_CALL with `is_comptime_resolved + comptime_value`. Single path, works for both positive and negative.

### Cross-Module Handle Auto-Deref + Qualified Calls (BUG-416, 2026-04-05)

**Handle auto-deref in imported module functions:** `e.id = id` inside entity.zer function emitted `/* ERROR: no allocator */ 0 = id`. Root cause: `find_unique_allocator()` returned NULL (ambiguous) because imported module globals are registered TWICE in global scope — raw name (`cross_world`) AND mangled name (`cross_entity__cross_world`, from BUG-233). Both have the same `Type*` pointer, but `find_unique_allocator` counted them as two different allocators. Fix: in `find_unique_allocator()`, skip duplicate matches where `found->type == t` (same Type pointer = same allocator). The name-based fallback from the previous session was removed — pointer identity works correctly, the bug was in the ambiguity check.

**Module-qualified function calls:** `config.MAX_SIZE()` now works. Implementation: in NODE_CALL, before builtin method dispatch, detect callee `NODE_FIELD(NODE_IDENT, field)` where the ident isn't a variable/type. Look up `module__func` in global scope via mangled name. If found, rewrite callee to `NODE_IDENT(raw_func_name)` and goto normal call resolution. This reuses all existing call handling (comptime, arg checking, type validation).

**Root cause update (2026-04-05 later session):** The original session diagnosed pointer-identity failure and added a name-based struct matching fallback. Root cause investigation with debug fprintf showed pointer identity WORKS correctly (`slab.elem == handle.elem`). The REAL bug: `find_unique_allocator()` found TWO matches — raw name (`cross_world`) and mangled name (`cross_entity__cross_world`) from BUG-233 dual registration — and returned NULL for "ambiguous." Fix: `if (found && found->type == t) continue;` (same Type* = same allocator, skip). Name-based fallback removed.

### popen Segfault on 64-bit Linux (BUG-417, 2026-04-05)

`zerc` crashed with SIGSEGV at `fgets()` during GCC auto-detection probe. Root cause: `popen`/`pclose` are POSIX extensions, not declared in strict C99 `<stdio.h>`. Without declaration, compiler assumes `popen` returns `int` (32-bit), truncating 64-bit `FILE*` → segfault. Did NOT manifest on Windows or Docker `gcc:13` (GNU extension defaults). Fix: `#define _POSIX_C_SOURCE 200809L` before `<stdio.h>` (guarded `#ifndef _WIN32`).

**Lesson:** Always check GCC warnings for `implicit declaration of function 'popen'`. On 64-bit, implicit `int` return = truncated pointer = guaranteed crash. This is a classic C99 portability bug.

### Scalar Field Extract False Local-Derived (BUG-421, 2026-04-05)

`Token tok = get_tok(&state); u32 val = tok.val; return val;` falsely rejected as "cannot return pointer to local." Root cause: BUG-360/383 marks struct results of calls with `&local` args as `is_local_derived`. Alias propagation at var-decl walks field chain to root and propagates unconditionally — `u32 val = tok.val` inherits from `tok` even though `u32` can't carry a pointer.

**Fix:** In alias propagation (~line 4742), only propagate `is_local_derived`/`is_arena_derived` when target type can carry a pointer: TYPE_POINTER, TYPE_SLICE, TYPE_STRUCT, TYPE_UNION, TYPE_OPAQUE. Scalar types (integers, floats, bools, enums, handles) skip propagation.

### Auto-Guard emit_zero_value for Struct/Union Return (BUG-422, 2026-04-05)

Auto-guard `if (idx >= size) { return 0; }` in a function returning struct/union emitted bare `return 0` — GCC error "incompatible types." `emit_zero_value` only handled void, optional, pointer, and scalar cases.

**Fix:** Added TYPE_STRUCT and TYPE_UNION case: `emit(e, "("); emit_type(e, t); emit(e, "){0}");` — compound literal with zero initializer.

### Funcptr Typedef Optional Return Type (BUG-420, 2026-04-05)

`typedef ?u32 (*Handler)(u32)` created `?(u32 (*)(u32))` (nullable funcptr) instead of `(?u32) (*)(u32)` (funcptr returning `?u32`). All 6 funcptr declaration sites had the same `is_opt_fp` unwrap-and-rewrap pattern.

**Fix:** Split behavior by context:
- **Typedef sites** (regular + distinct): `?` binds to the return type. `typedef ?u32 (*Handler)(u32)` = funcptr returning `?u32`. This is the only context where it makes sense — typedefs create named types, and `?Handler` separately gives you nullable.
- **All other 4 sites** (local var, global var, struct field, function param): `?` wraps the function pointer as optional/nullable. `?void (*cb)(u32)` = nullable callback. This is the common use case at declaration sites.

**Design decision:** The `?` prefix is inherently ambiguous for raw function pointer declarations. The typedef rule resolves it: at typedef you specify the *signature* (including optional return), and at usage sites you wrap the *pointer* (including nullable). Both `?RetType` and `?FuncPtr` are expressible — just through different syntax paths.

**Implementation:** `parse_funcptr_with_opt(p, type, &name, &len, is_typedef)` helper enforces the invariant in one place. All 6 funcptr declaration sites call it with `is_typedef=false` (local/global/field/param) or `is_typedef=true` (typedef/distinct typedef). The helper unwraps+re-wraps `?` for non-typedef sites, passes through for typedef sites.

### Else-If Chain #line Directive (BUG-418, 2026-04-05)

`if (a) { } else if (b) { }` emitted `else #line N "file"` on the same line — GCC error "stray '#' in program." Root cause: `emit_stmt` emits `#line` before each non-block statement. When else_body is NODE_IF, the `#line` follows `else ` without a newline.

**Fix:** Both regular-if and if-unwrap else paths: when `else_body->kind == NODE_IF && e->source_file`, emit `"else\n"` instead of `"else "`. Same class of bug as BUG-396 (orelse defer #line).

**Pattern:** Any site that emits text followed by `emit_stmt()` on the same line risks `#line` collision when source mapping is active. The text before `emit_stmt` must end with `\n` if the child statement might emit `#line`.

### Array→Slice Coercion Missing in Assignment (BUG-419, 2026-04-05)

`[*]u8 s; s = arr;` emitted `s = arr` — GCC error "incompatible types" because `arr` decays to `uint8_t*` but `s` is `_zer_slice_u8`. Array→slice coercion (`emit_array_as_slice`) was implemented for var-decl init (line 2603), call args (line 1419), and return (line 2920) — but NOT for NODE_ASSIGN.

**Fix:** In NODE_ASSIGN emission, after the optional-wrap and null-assign paths, check if `target is TYPE_SLICE && value is TYPE_ARRAY`. If so, call `emit_array_as_slice()`.

**Pattern:** Any new coercion path must be checked in ALL FOUR value-flow sites: (1) var-decl init, (2) assignment, (3) call args, (4) return. Missing any one creates a silent GCC error on valid ZER code.

### Volatile Struct Array Fields (BUG-414, 2026-04-05)

`struct Hw { volatile u8[4] regs; }` — array assignment `dev.regs = src` used `memmove` which strips volatile (GCC warning, optimizer can eliminate write). Root cause: `expr_is_volatile()` only checked root symbol `is_volatile`, not struct field qualifiers.

**Fix:** Added `SField.is_volatile` flag in types.h. Checker sets it during struct field resolution when `fd->type->kind == TYNODE_VOLATILE`. `expr_is_volatile()` now walks field chains, checking each field's `SField.is_volatile` (also checks type-level `slice.is_volatile` and `pointer.is_volatile`). Emitter uses byte loop for volatile array assignment.

### Function Pointer Array Emission (BUG-412, 2026-04-05)

`Op[3] ops` where `Op` is `typedef u32 (*Op)(u32)` emitted `uint32_t (*)(uint32_t) ops[3]` — name outside the `(*)` instead of inside `(*ops[3])`. Fix: in `emit_type_and_name` for TYPE_ARRAY, when base type (after unwrapping array chain) is TYPE_FUNC_PTR (or distinct wrapping it), use function pointer emission pattern: `ret (*name[dims])(params)`. Works with distinct typedef func ptrs too.

### Comprehensive Distinct Typedef Audit (BUG-409/410, 2026-04-05)

**The #1 bug class in ZER:** Every `->kind == TYPE_X` check on a type from `checker_get_type()` or `check_expr()` must call `type_unwrap_distinct()` first. This session found 35+ sites across checker.c, emitter.c, and types.c. The systematic audit used `grep "->kind == TYPE_X"` for each type kind and verified each site.

**Sites fixed by category:**
- **TYPE_OPTIONAL (15 sites):** `type_is_optional()`, `type_unwrap_optional()`, `can_implicit_coerce()`, emitter orelse 3 paths, emitter return null/value/bare, assign null, `== null`, bare `if(opt)`, `while(opt)`, var-decl orelse
- **TYPE_FUNC_PTR (2 sites):** `emit_type_and_name` distinct funcptr (non-optional + optional)
- **TYPE_POINTER (7 sites):** checker deref (TOK_STAR), checker NODE_FIELD dispatch, emitter NODE_FIELD `->` emission, volatile pointer var-decl (local + global, 4 sites)
- **TYPE_SLICE (12 sites):** checker NODE_INDEX, checker NODE_SLICE, emitter proven index, bounds check, sub-slice, call-site decay, Arena.over(), @cstr dest, var-decl orelse, array→slice coercion (var-decl + return)
- **TYPE_ARRAY (6 sites):** emitter array assign target, @cstr array dest, array init memcpy, checker assign value, escape check, const array→slice
- **TYPE_VOID (14+6 sites):** `optional.inner->kind == TYPE_VOID` (emitter + checker), `pointer.inner->kind == TYPE_OPAQUE` (emitter)

**How to prevent future occurrences:**
1. When adding ANY new `->kind == TYPE_X` check, call `type_unwrap_distinct()` first
2. The helpers `type_is_optional()`, `type_is_integer()`, `type_is_signed()`, `type_width()` all unwrap internally — safe to call directly
3. For `optional.inner`, `pointer.inner`, `slice.inner` — these are already-resolved inner types, usually don't need unwrapping (but check for `distinct typedef void` edge cases)
4. Use the grep audit pattern: `grep "->kind == TYPE_X" file.c` → check each site

### Nested Distinct FuncPtr Name Placement (BUG-407, 2026-04-05)

`emit_type_and_name` only checked one level of TYPE_DISTINCT before TYPE_FUNC_PTR dispatch. `distinct typedef (distinct typedef Fn) ExtraSafeFn` wrapped TWO levels — the second was missed, producing `void (*)(uint32_t) name` instead of `void (*name)(uint32_t)`. Fixed: use `type_unwrap_distinct()` at both the optional and non-optional distinct func ptr paths (lines 480, 506).

### ?void Init from Void Function (BUG-408, 2026-04-05)

`?void result = do_work()` where `do_work()` returns void — emitter put void expression in struct initializer (`_zer_opt_void result = do_work()`). GCC error: void can't be in initializer. Fixed: detect void call target + ?void type, hoist call to statement, then assign `(_zer_opt_void){ 1 }`. Same pattern as BUG-145 (NODE_RETURN void-as-statement logic).

### Return String Literal from Const Slice Function (BUG-406, 2026-04-05)

`const [*]u8 get() { return "hello"; }` was rejected — checker fired "cannot return string literal as mutable slice" without checking if the return type was `const`. Fix: added `!ret->slice.is_const` condition. Also handles `?const [*]u8` optional return path.

### Task.new() / Task.delete() — Auto-Slab Sugar

**Checker:** When NODE_FIELD on TYPE_STRUCT with method `new`/`new_ptr`/`delete`/`delete_ptr`, checker creates/finds auto-Slab in `checker.auto_slabs[]`. One auto-Slab per struct type (program-wide). `auto_slabs` is an arena-allocated dynamic array on the Checker struct.

**Auto-Slab creation:** On first `Task.new()`, creates `_zer_auto_slab_Task` as a Symbol in global scope with TYPE_SLAB. Subsequent `Task.new()` calls reuse the same auto-Slab. `Task.new_ptr()` shares the same auto-Slab.

**Emitter:** Two-pass declaration emission in `emit_file()`:
1. Pass 1: emit struct/enum/union/typedef declarations
2. Emit auto-Slab globals: `static _zer_slab _zer_auto_slab_Task = {sizeof(Task), 0, ...};`
3. Pass 2: emit functions, global vars, everything else

The two-pass ensures `sizeof(Task)` is available (struct declared in pass 1). The `emit_file_no_preamble` (for imported modules) is NOT affected — auto-slabs are only emitted in the main file's `emit_file`.

**CRITICAL: use designated initializers for auto-slab.** The emission MUST be `{ .slot_size = sizeof(T) }`, NOT `{sizeof(T), 0, 0, ...}`. Positional init put sizeof into `pages` field (wrong) because `_zer_slab` struct field order doesn't start with `slot_size`. Normal Slab emission (line ~3422) already uses `.slot_size =` — auto-slab must match.

**Method emission:**
- `Task.new()` → `_zer_slab_alloc(&_zer_auto_slab_Task, &ok)` wrapped in optional u64
- `Task.new_ptr()` → `_zer_slab_alloc` + `_zer_slab_get` combined, returns pointer
- `Task.delete(h)` → `_zer_slab_free(&_zer_auto_slab_Task, h)`
- `Task.delete_ptr(p)` → `_zer_slab_free_ptr(&_zer_auto_slab_Task, p)`

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
Union switch takes a POINTER to the original: `__auto_type *_zer_swp = &(expr)`. Tag checked via `_zer_swp->_tag`. Immutable capture copies: `__typeof__(...) v = _zer_swp->variant`. Mutable capture takes pointer: `Type *v = &_zer_swp->variant`. This ensures `|*v|` modifications persist to the original union.

### Volatile and Qualifier Preservation in Captures (BUG-319/321/322)
**CRITICAL:** Never use `__auto_type` for capture variables or orelse temporaries — GCC drops volatile and const qualifiers. Always use `__typeof__(expr)` which preserves them. Three sites in if-unwrap fixed:
1. Mutable capture `|*v|` on struct optional: `volatile __typeof__(ptr->value) *v = &ptr->value;`
2. Null-sentinel capture `|v|`: `__typeof__(tmp) v = tmp;`
3. Value capture `|v|` on struct optional: `__typeof__(tmp.value) v = tmp.value;`
Also: var-decl orelse uses `__typeof__(expr) _zer_or = expr;` (not `__auto_type`).
Also: array copy checks BOTH target AND source for volatile — `expr_is_volatile(target) || expr_is_volatile(value)`.

### emit_file and emit_file_no_preamble — UNIFIED (RF2)
Both functions now call `emit_top_level_decl(e, decl, file_node, i)`. Adding a new NODE kind only requires updating that one function. The old pattern of two parallel switch statements (which caused BUG-086/087) is eliminated.

### GCC Extensions Used
- `__auto_type` — C equivalent of `auto` (type inference)
- `({...})` — statement expressions (GCC/Clang extension)
- `_Alignof(T)` — type alignment (C11, supported by GCC/Clang)
- These make the emitted C NOT portable to MSVC

## ZER-CHECK (zercheck.c) — ~900 lines

### What It Checks
Path-sensitive handle tracking after type checker, before emitter:
- Use after free: `pool.free(h); pool.get(h)` → error
- Double free: `pool.free(h); pool.free(h)` → error
- Wrong pool: `pool_a.alloc() → h; pool_b.get(h)` → error
- Free in loop: `for { pool.free(h); }` → error (may use-after-free next iteration)
- **Maybe-freed use**: `if (c) { pool.free(h); } pool.get(h)` → error (handle may have been freed)
- **Handle leak**: `h = pool.alloc(); /* no free */` → error at function exit
- **Handle overwrite**: `h = pool.alloc(); h = pool.alloc()` → error (first handle leaked)

### Handle States
`HS_UNKNOWN` → `HS_ALIVE` (after alloc) → `HS_FREED` (after free)
                                         → `HS_MAYBE_FREED` (freed on some paths, not all)

`HS_MAYBE_FREED` is an error state — using or freeing a MAYBE_FREED handle produces a compile error. This closes the gap where conditional frees (if-without-else, partial switch arms) were previously undetected.

### Handle Aliasing (BUG-082 fix)
When `Handle(T) alias = h1` or `h2 = h1` is detected, the new variable is registered with the same state, pool_id, and alloc_line as the source. When `pool.free(h)` is called, all handles with the same pool_id + alloc_line are also marked HS_FREED (aliases of the same allocation). Independent handles from the same pool (different alloc_line) are unaffected.

### Path Merging
- **if/else**: both freed → `FREED`, one freed → `MAYBE_FREED`
- **if without else**: then frees → `MAYBE_FREED`
- **switch**: all arms free → `FREED`, some arms free → `MAYBE_FREED`
- **loops**: unconditional free inside loop → error. Loop second pass: if state changed after first pass, run body once more; if still unstable → widen to `MAYBE_FREED`

### Leak Detection — Compile Error (2026-04-06 redesign)
At function exit, any handle that is `HS_ALIVE` or `HS_MAYBE_FREED` and was allocated inside the function (not a parameter) triggers a **compile error** (MISRA C:2012 Rule 22.1):
- `HS_ALIVE` → "handle allocated but never freed — add defer pool.free(h)"
- `HS_MAYBE_FREED` → "handle may not be freed on all paths"
- Parameter handles (pool_id == -1, alloc_line == func start) are excluded — caller is responsible.

**alloc_id grouping:** Each allocation gets a unique `alloc_id`. Aliases (orelse unwrap, struct copy, assignment) share the same `alloc_id`. At leak check, if ANY handle in the group is FREED or escaped, the allocation is covered — no error. This naturally handles `?Handle mh` / `Handle h` pairs without false positives.

**Escape detection:** Handles marked `escaped = true` when:
- Returned from function (`return h`)
- Stored in global variable
- Stored in pointer parameter field (`s.top = h` where s is `*Stack`)
- Assigned to untrackable target (variable-index array `handles[i] = h`)

**if-unwrap alloc_id propagation:** `if (mh) |t| { free(t); }` — `t` gets `mh`'s alloc_id. When `t` is freed in then_state, the freed alloc_id propagates back to mark `mh` as covered in the main state. The not-taken path (null = no allocation) is not a leak.

**Error deduplication:** One error per allocation (alloc_id), not per variable name. After reporting, alloc_id added to covered set.

**Defer free scanning (2026-04-06, recursive):** Before leak detection, zercheck scans the ENTIRE function body recursively (not just top-level) for defer blocks containing free calls. Handles freed in defer are marked HS_FREED. `defer_scan_all_frees()` walks ALL statements in a defer block (not just the first — BUG-443 fix). Recognizes pool.free, slab.free, slab.free_ptr, Task.delete, Task.delete_ptr, and bare free() calls.

**If-exit MAYBE_FREED fix (2026-04-05):** In if-without-else merging, `block_always_exits()` checks if the then-branch always exits (NODE_RETURN, NODE_BREAK, NODE_CONTINUE, NODE_GOTO, or NODE_IF with both branches exiting). If the freeing branch always exits, handles stay ALIVE on the continuation path — the pattern `if (err) { free(h); return; } use(h);` is now correctly safe.

### Overwrite Detection
If a handle target is already `HS_ALIVE` when a new `pool.alloc()` is assigned to it, the first handle is leaked. Error: "handle overwritten while alive — previous handle leaked."

### Cross-Function Analysis (Change 4)
Pre-scan builds `FuncSummary` for each function with Handle params:
- `frees_param[i]` — this function definitely frees parameter i on all paths
- `maybe_frees_param[i]` — this function conditionally frees parameter i

**Summary building:** `zc_build_summary()` runs the existing `zc_check_stmt` walker with `building_summary=true` (suppresses errors). After walking, checks each Handle param's final state: FREED → `frees_param`, MAYBE_FREED → `maybe_frees_param`.

**Summary usage:** `zc_apply_summary()` called from `zc_check_expr(NODE_CALL)` when callee is `NODE_IDENT` (not `NODE_FIELD` which is pool.method). Looks up callee's summary, applies effects to caller's PathState. Freed params → mark handle arg as FREED. Maybe-freed → MAYBE_FREED.

**Flow in `zercheck_run`:** (1) register pools, (2) pre-scan: build summaries, (3) main analysis: check functions.

### CFG-Aware Analysis (2026-04-09)
zercheck uses CFG-aware path merging instead of linear AST walk hacks:
- `PathState.terminated` flag — set on return/break/continue/goto. Used by merge logic to determine which paths reach post-if/switch.
- **if/else merge:** 4-way — both-exit (post-if unreachable), then-exit (only else state), else-exit (only then state), both-fallthrough (merge).
- **switch merge:** terminated arms excluded from post-switch state. Only non-exiting arms count for FREED/MAYBE_FREED.
- **Loop:** dynamic fixed-point iteration (ceiling 32) until states stabilize. Convergence guaranteed by finite lattice (5 states per handle, monotone transitions). No manual widen step.
- **Backward goto:** same dynamic fixed-point over label..goto region.
- `pathstate_equal(a, b)` — checks if all handle states match (convergence test).

### Unified Helpers (Option A refactor, 2026-04-09)
All state/type checks go through centralized helpers. New states or move-like types only need updating in ONE place:
- `should_track_move(Type *t)` — `is_move_struct_type(t) || contains_move_struct_field(t)`. Used at 5 sites.
- `is_handle_invalid(HandleInfo *h)` — `HS_FREED || HS_MAYBE_FREED || HS_TRANSFERRED`. Used at 5 use-check sites.
- `is_handle_consumed(HandleInfo *h)` — same states, for path merge (if/else/switch/loop). Used at 4 merge sites.
- `zc_report_invalid_use(zc, h, line, key, len)` — emits correct error message by state + pool_id. Used at 5 error sites.

### Scope
- Looks up Pool types via `checker_get_type()` first, then `global_scope` fallback
- Checks `NODE_FUNC_DECL` and `NODE_INTERRUPT` bodies

## Unified Helpers — Checker (checker.c, 2026-04-09)

### Escape Flag Propagation
- `type_can_carry_pointer(Type *t)` — TYPE_POINTER/SLICE/STRUCT/UNION/OPAQUE. Scalars return false.
- `propagate_escape_flags(Symbol *dst, Symbol *src, Type *dst_type)` — propagates `is_local_derived`, `is_arena_derived`, `is_from_arena` from src to dst, but ONLY if `type_can_carry_pointer(dst_type)`. Prevents BUG-421 (scalar false positive). Used at all 5 grouped propagation sites.

### ISR Ban + Auto-Slab
- `check_isr_ban(Checker *c, int line, const char *method)` — rejects heap allocation in interrupt handler. Replaces 4 scattered `c->in_interrupt` checks (slab.alloc, slab.alloc_ptr, Task.new, Task.new_ptr).
- `find_or_create_auto_slab(Checker *c, Type *struct_type)` — finds or creates auto-Slab for a struct type. Replaces 2× 40-line duplicate blocks in Task.new() and Task.new_ptr().

### Volatile Strip
- `check_volatile_strip(Checker *c, Node *src_expr, Type *src_type, Type *tgt_type, int line, const char *context)` — checks if a cast/intrinsic strips volatile from pointer. Walks source ident for `sym->is_volatile`. Replaces 5 scattered sites (@ptrcast BUG-258, @bitcast BUG-341, @cast BUG-343, @container, C-style cast BUG-447).

## Barrier Type (2026-04-11)

`Barrier` is a keyword type (like Arena, Pool, Ring, Slab). Emits as `_zer_barrier` (pthread_mutex_t + pthread_cond_t + counters).
- Lexer: `TOK_BARRIER`, Parser: `TYNODE_BARRIER`, Types: `TYPE_BARRIER` + `ty_barrier` singleton
- Checker: `@barrier_init`/`@barrier_wait` validate first arg is `TYPE_BARRIER` (rejects `u32`)
- Emitter: `TYPE_BARRIER` → `_zer_barrier`, zero-init uses `{0}` (compound)
- Usage: `Barrier b; @barrier_init(b, 3); @barrier_wait(b);`

## Shared Struct Locking (2026-04-11 — BUG-473 fix)

All shared structs now use **recursive pthread_mutex** instead of spinlock:
- Struct field: `pthread_mutex_t _zer_mtx` + `uint8_t _zer_mtx_inited` (lazy init)
- Lock: `_zer_mtx_ensure_init(&obj._zer_mtx, &obj._zer_mtx_inited); pthread_mutex_lock(&obj._zer_mtx);`
- Unlock: `pthread_mutex_unlock(&obj._zer_mtx);`
- `PTHREAD_MUTEX_RECURSIVE` via `_XOPEN_SOURCE 500` — allows re-entrant locking for cross-function auto-lock.
- Condvar types still use same mutex (condvar was already pthread_mutex). rwlock types unchanged.
- `shared_needs_condvar()` check removed from lock/unlock — all paths use pthread_mutex now.

## Unified emit_file_module (2026-04-11 — BUG-472 fix)

`emit_file_module(Emitter *e, Node *file_node, bool with_preamble)` — single flow for both preamble and non-preamble modules. Prevents setup steps being forgotten in one of two parallel functions.
- `emit_file()` and `emit_file_no_preamble()` are thin wrappers calling `emit_file_module`.
- Both paths do: prescan for spawn → pass 1 (struct/enum/union/typedef) → spawn wrappers → pass 2 (functions/globals).

## Unified Helpers — Emitter (emitter.c, 2026-04-09)

### Optional Emission (ALL sites migrated)
- `is_void_opt(Type *t)` — true if `?void` (unwraps distinct). `?void` has NO `.value` field. Replaced 11 manual `type_unwrap_distinct(...)->kind == TYPE_VOID` checks.
- `emit_opt_null_check(e, tmp_id, type)` — emits `!tmp` (null sentinel) or `!tmp.has_value` (struct optional). Available for incremental migration at ~20 branching sites.
- `emit_opt_unwrap(e, tmp_id, type)` — emits `tmp` (null sentinel), `tmp.value` (struct), or `(void)0` (?void).
- `emit_opt_null_literal(e, type)` — emits `(T*)0`, `{ 0 }` (?void), or `{ 0, 0 }` (?T struct). Replaced 6 manual literal emission sites.
- `emit_return_null(e)` — emits `return <zero>` for current function's return type. Handles ?void, ?T struct, ?*T, void, scalar. Replaced 6 duplicate return-null code blocks.

### Full Refactoring Summary (2026-04-09/10/11)
16 helpers added across zercheck.c/checker.c/emitter.c. 39 scattered sites unified. ~250 lines of duplicated code eliminated. All 9 gaps from `docs/refactoring_gaps.md` complete. Adding a new handle state, move-like type, optional variant, escape flag, ISR-banned method, or volatile-checked intrinsic now requires updating ONE helper function instead of finding N scattered sites.

## Bugs Fixed This Session (2026-04-09/10/11)

### BUG-468: move struct conditional transfer not caught
`if (c) { consume(t); }` then `t.field` — zercheck path merge only checked `HS_FREED || HS_MAYBE_FREED`, not `HS_TRANSFERRED`. Fix: `is_handle_consumed()` helper includes all three states. Applied to if/else merge, if-no-else merge, switch merge, loop check.

### BUG-469: struct containing move struct field not tracked
`consume_wrapper(w)` where `Wrapper` has `Key` (move struct) field — compiled. Fix: `contains_move_struct_field()` helper walks struct fields for move types. Unified into `should_track_move()`. Applied at NODE_CALL (first/second pass), NODE_IDENT, NODE_RETURN.

### BUG-470: return move struct not marked as transferred
`return t; t.kind;` — dead code not flagged. Fix: NODE_RETURN handler marks move struct ident as `HS_TRANSFERRED` using `should_track_move()`.

### BUG-471: pool.free()/slab.free() missing Handle element type check
`pool_b.free(handle_from_pool_a)` compiled — Handle is `u64` at C level, no type mismatch. Fix: checker Pool/Slab `.free()` handler validates `type_equals(handle.elem, pool.elem)`. Same pattern as existing `.free_ptr()` check. ~5 lines each.

### BUG-472: spawn wrapper missing in multi-module builds
`emit_file_no_preamble` didn't prescan for spawn. In topo order, main module emitted last via this function. Fix: added prescan + `emit_spawn_wrappers()` to `emit_file_no_preamble`. Then unified into `emit_file_module(e, file, with_preamble)` — one function, one flow.

### BUG-473: shared struct recursive mutex
Non-recursive spinlock deadlocked on re-entrant auto-lock (cross-function call on shared struct). Fix: all shared structs use `pthread_mutex_t` with `PTHREAD_MUTEX_RECURSIVE`. Lazy init via `_zer_mtx_ensure_init`. `_XOPEN_SOURCE 500` for portability.

### Barrier keyword type — eliminates pre-existing UB
`u32 barrier` with `@barrier_init` was 4-byte variable for ~120-byte struct. `memset` overflow caused silent stack corruption. Fix: `Barrier` keyword type (lexer/parser/types/checker/emitter). Checker rejects non-Barrier args to `@barrier_init`/`@barrier_wait`.

## Container Keyword — Parameterized Struct Monomorphization (2026-04-11)

`container Name(T) { fields }` defines a parameterized struct template. `Name(ConcreteType)` stamps a concrete `struct Name_ConcreteType` with T substituted. No methods, no `this` — functions take `*Container(T)`.

**Lexer:** `TOK_CONTAINER` keyword. Also accepted after `@` for the `@container` (container_of) intrinsic — parser's `@` handler matches both `TOK_IDENT` and `TOK_CONTAINER`.

**AST:** `NODE_CONTAINER_DECL` (template definition with name, type_param, fields). `TYNODE_CONTAINER` (instantiation: name + type_arg TypeNode).

**Parser:**
- Top-level: `container Name(T) { fields }` → NODE_CONTAINER_DECL. Fields parsed same as struct fields.
- Type position: After TYNODE_NAMED, if `(` followed by type token → parse as TYNODE_CONTAINER `{ name, type_arg }`.
- Statement lookahead: `IDENT(TypeToken) IDENT` pattern detected in the statement heuristic (line ~1773) — skips past `(Type)` to check if IDENT follows, confirming var-decl.

**Checker:**
- `ContainerTemplate` stored on Checker struct (name, type_param, fields). Registered in `register_decl(NODE_CONTAINER_DECL)`.
- `ContainerInstance` cache: `(tmpl_name, concrete_type) → TYPE_STRUCT`. Checked before stamping to avoid duplicates.
- `resolve_type(TYNODE_CONTAINER)`: looks up template by name, stamps by creating TYPE_STRUCT with mangled name (`Stack_u32`), substitutes T in field types. Handles T, *T, ?T, []T, T[N] field patterns. Registers stamped struct in scope for field access.

**Emitter:** `emit_container_structs(e)` iterates `checker->container_instances`, emits each stamped struct as regular C struct declaration. Called between pass 1 (regular structs) and spawn wrappers in both preamble and non-preamble paths.

**zercheck:** No special handling — stamped containers are regular TYPE_STRUCT, tracked same as any struct.

**Limitation:** T substitution currently handles direct field type, one-level pointer/optional/slice/array wrapping. Nested containers (`container Map(K) { Pair(K)[64] entries; }`) would need recursive TypeNode substitution — not implemented yet. Add when needed.

## Designated Initializers — NODE_STRUCT_INIT (2026-04-11)

`Point p = { .x = 10, .y = 20 };` — C99-style designated struct initialization.

**Parser:** `parse_primary` detects `{` followed by `.` → parses `{ .field = expr, ... }` as NODE_STRUCT_INIT. Up to 128 fields. `DesigField` struct: `{name, name_len, value}`.

**Checker:** `check_expr(NODE_STRUCT_INIT)` type-checks all value expressions. Field validation deferred to context (var-decl or assignment) where target struct type is known. Validates: field names exist on struct, value types match field types. Sets `init_type = target_type` and stores in typemap.

**Emitter:** Always emits as C99 compound literal: `(StructType){ .field = val, ... }`. Works in both var-decl init and assignment contexts. The type cast prefix is read from `checker_get_type(node)`.

**All 4 value-flow sites validated** via unified `validate_struct_init(c, sinit, target_type, line)` helper. ONE function, 4 call sites — var-decl, assignment, call arg, return. Checks field names exist on struct, value types match. Returns bool (true = valid struct). Caller sets `typemap_set(node, target_type)` on success. Audit found this was duplicated 4x (~120 lines), extracted to helper (-76 net lines).

## do-while Loop (2026-04-11)

`do { body } while (cond);` — C-style execute-at-least-once loop.
- Lexer: `TOK_DO` keyword. Parser: `NODE_DO_WHILE` reuses `while_stmt` union member (same cond+body).
- Checker: merged with `case NODE_WHILE:` everywhere (same validation). Comptime evaluator handles do-while by executing body before first condition check.
- Emitter: `do { body } while (cond);` — direct C pass-through.
- zercheck: merged with `case NODE_FOR: case NODE_WHILE:` loop handler (same fixed-point iteration).

## Comptime Array Indexing (2026-04-11)

`ComptimeParam` extended with `int64_t *array_values` and `int array_size` for array bindings.
- `ct_ctx_set_array(ctx, name, len, values, size)` — creates zero-filled array binding (max 1024 elements).
- `eval_comptime_block` NODE_VAR_DECL: detects TYNODE_ARRAY, allocates array via `calloc`, registers binding.
- `ct_eval_assign`: handles `NODE_INDEX` targets (`arr[i] = val`) — looks up array binding, sets element.
- `eval_const_expr_subst`: handles `NODE_INDEX` reads (`arr[i]`) — looks up array binding, returns element.
- Memory: array_values freed in `ct_ctx_free` and on block scope pop (`saved_count` restore).
- **CRITICAL:** All `ComptimeParam` arrays (stack and malloc'd) must be zero-initialized (`memset`) to prevent `ct_ctx_free` from freeing garbage `array_values` pointers.

## Comptime Struct Return (2026-04-11)

`comptime Point MAKE(i32 a, i32 b) { return { .x = a, .y = b }; }` — comptime functions returning structs.

**Architecture:** Does NOT change `eval_comptime_block` return type (stays `int64_t`). Instead, when scalar eval returns `CONST_EVAL_FAIL`, the checker tries struct return as a parallel path:
1. `find_comptime_struct_return(body)` — recursively finds `return { .field = expr }` (NODE_STRUCT_INIT) in function body
2. `eval_comptime_struct_return(arena, struct_init, params, count)` — evaluates each field value via `eval_const_expr_subst`, creates new NODE_STRUCT_INIT with NODE_INT_LIT constant values
3. Result stored as `node->call.comptime_struct_init` (new field on NODE_CALL)
4. Emitter: when `comptime_struct_init` is set, emits via `emit_expr` (reuses existing NODE_STRUCT_INIT compound literal emission)

**Design decision:** Avoids changing `eval_comptime_block`'s return type to a tagged union. The struct path is handled entirely at the call site in the checker. Simpler, no architectural disruption, handles all cases where return expression is a designated initializer with comptime-evaluable field values.

## Spawn Global Data Race Detection (2026-04-11)

When `spawn func()` is used, the checker scans the spawned function's body for non-shared, non-const, non-volatile, non-threadlocal global variable access.

**`scan_unsafe_global_access(c, node, &name, &len)`** — recursive AST walker. Finds NODE_IDENT matching global scope symbols that are not safe for concurrent access. Skips: const, volatile (explicit opt-in), threadlocal, shared/shared(rw) structs, Pool/Slab/Ring/Arena/Barrier. Skips `@atomic_*` intrinsic arguments (atomic ops are thread-safe).

**Transitive scanning:** When NODE_CALL is encountered, follows the callee into its function body (depth limit 8). Catches `spawn worker()` where `worker()` calls `helper()` which accesses a global.

**Error vs Warning:** `has_atomic_or_barrier(node)` scans the spawned function body for `@atomic_*` or `@barrier*` intrinsics. If found → **warning** (developer is doing manual synchronization, lock-free pattern possible). If not found → **error** (no synchronization at all, definitely unsafe).

**Escape hatches:** `volatile` global (explicit opt-in, like `#[allow(data_race)]`), `shared struct` (auto-locked), `threadlocal` (per-thread copy), `@atomic_*` (thread-safe by definition), `const` (read-only).

## --stack-limit N (2026-04-11)

`zerc --stack-limit 2048` — compile error when estimated stack usage exceeds N bytes.

**Two checks:**
1. **Per-function frame size** — catches big local arrays (`u8[4096] buf` in a function with 2048 limit). `estimate_type_size()` sums local variable sizes.
2. **Entry point total call chain** — `compute_max_depth()` DFS through call graph from `main()` and interrupt handlers. Catches deep call chains where no single function is over-limit but the total exceeds it.

**Recursive functions:** Warning only (can't compute max depth for unbounded recursion). `--stack-limit` check skipped for recursive entry points.

**Added to Checker struct:** `uint32_t stack_limit` (0 = disabled). Set from CLI via `--stack-limit N` in zerc_main.c.

## @ptrtoint(&local) Escape Detection (2026-04-12)

`return @ptrtoint(&local)` — address of stack variable escapes as integer. Two checks:

**Direct return:** `return @ptrtoint(&x)` — NODE_RETURN handler detects @ptrtoint intrinsic wrapping `&local_ident`. Walks field/index chains to root. Error if root is non-global, non-static.

**Indirect return:** `usize a = @ptrtoint(&x); return a` — NODE_VAR_DECL handler detects @ptrtoint init with `&local`, sets `sym->is_local_derived = true`. Existing return escape check (line ~7338) catches `is_local_derived` on return ident.

**Both paths needed:** Direct has no intermediate variable (no symbol to flag). Indirect has a symbol but the @ptrtoint is not in the return expression.

## Function Pointer Indirect Recursion in Call Graph (2026-04-12)

`scan_frame` NODE_CALL now tracks function pointer calls. When callee is NODE_IDENT resolving to TYPE_FUNC_PTR variable, checks if the variable's init was a known function name. If so, adds that function as a callee in the call graph. Enables recursion detection through `void (*fp)() = func_a;` patterns.

## Full Codebase Audit + Refactor (2026-04-14)

Deep audit of all 25,757 compiler lines. 15,000 lines read directly, 100% pattern-searched.

### Refactor B1: `track_dyn_freed_index()` (checker.c)
Unified Pool.free/Slab.free DynFreed tracking into single helper. Was exact 20-line copy-paste (caused BUG-471). Both `pool.free` and `slab.free` handlers now call `track_dyn_freed_index(c, node)`.

### BUG-506: 13 missing `type_unwrap_distinct` sites
**Emitter (6 sites):** var-decl optional init (3232), init_type ident/expr (3259, 3272), comptime call (1438), global var null init (4925), if-unwrap condition (3362). All checked `type->kind == TYPE_OPTIONAL` without unwrapping distinct. `distinct typedef ?u32 MaybeId; MaybeId x = null;` failed.

**Checker (7 sites):** cross-module collision (182), `*void`/`[]void` (1108/1128), `??T` nesting (1117), const/volatile propagation (1405-1424), comptime enum resolve (1553), resource assignment (2614), string return mutable slice (7566).

### Buffer over-read fix (5 sites)
`snprintf` return value used unclamped in `memcpy`. Clamp added: `if (sn_len >= sizeof(buf)) sn_len = sizeof(buf) - 1;` at lines 746, 1320, 8521, 8538, 8554.

### Refactor B2: `check_union_switch_mutation()` (checker.c)
Union switch lock check was duplicated between pointer-auto-deref union and direct union field (~50 lines each, identical). Extracted to `check_union_switch_mutation(c, field_object)`. Walks mut_root, checks name match + pointer alias + precise key, emits error. Both sites now: `if (check_union_switch_mutation(c, node->field.object)) { result = ty_void; break; }`. Net -38 lines.

### A7: Spawn string literal safety check (checker.c)
Added string-literal-to-mutable-slice check in NODE_SPAWN arg loop. Same check as regular NODE_CALL (line 3871). Without it, `spawn process("hello")` where `process([]u8 data)` would let spawned thread write to .rodata.

### A16/A17: Fixed arrays → stack-first dynamic
- `LabelInfo labels[128]` in `check_goto_labels` → stack-first with arena overflow
- `FieldDecl fields[128]` in `parse_container_decl` → stack-first with `parser_alloc` overflow (matches `parse_struct_decl` pattern)

### A18: Volatile bounds check temps
`__auto_type` strips volatile. Bounds check slice temps (emitter.c:2028, 2204) now use `__typeof__(expr)` to preserve volatile. Same fix pattern as BUG-319 captures.

### C1/C2: Zig test runner
Created `zig_tests/run_tests.sh` (36 tests: 31 positive, 5 negative). Added to Makefile `check` target. Previously existed but never automated.

### Remaining refactors documented in `docs/ZER_Refactor.md`
**ALL REFACTORS EXECUTED** except B5-B6 and B11 (deferred to v0.4 — intentional structural differences between Pool/Slab, not pure duplication).

### Completed refactors (second batch, same session):
- **B3:** Orelse emission → `emit_opt_null_check`/`emit_opt_unwrap` helpers. 4 blocks consolidated. Net -64 lines.
- **B4:** `emit_opt_wrap_value()` helper — 3 identical `{val, 1}` wrapping sites consolidated.
- **B7:** Return optional wrapping → `emit_opt_wrap_value`.
- **B8:** Union typedef emission → `EMIT_UNAME()` local macro (12 repetitions → 12 calls).
- **B10:** `handle_key_arena()` in zercheck.c — 27 `char key[128]` sites → arena-allocated. Deep expressions no longer silently untracked.
- **A15:** Spawn validation: `is_literal_compatible` + `validate_struct_init` added.
- **A19:** `emit_type_and_name` handles `distinct(?funcptr)`.
- **A20:** Module-qualified call rewrite unwraps distinct.

### ctags-Guided Audit (2026-04-14, second pass)
Used `make tags` (Universal Ctags) to query codebase structure instead of reading source. Found 3 bugs in ~5K tokens:
1. `resolve_type_for_emit` missing TYNODE_SLAB/BARRIER/SEMAPHORE/CONTAINER — fallback returned `ty_void`
2. `resolve_type_for_emit` volatile propagation without `type_unwrap_distinct` (same A11 class)
3. Duplicate `_comptime_global_scope` declaration (lines 1082/1570)

**Methodology:** Query ctags for functions returning `Type*`, cross-reference with TYNODE enum values, grep for missing switch cases. 40x more efficient than brute-force reading.

**For fresh sessions:** Run `make tags` first. Use `grep "function_name" tags` to find locations. Use `grep "pattern" file.c` to find specific code. Never read full files speculatively.

### Flag-Handler Matrix Audit (2026-04-14, automated)
`bash tools/audit_matrix.sh checker.c` — cross-references control-flow NODE_ handlers against context flags. Found 5 missing checks on first run:

| Node | Missing Flag | Why it's a bug |
|---|---|---|
| YIELD | `defer_depth` | yield in defer corrupts Duff's device state machine |
| YIELD | `critical_depth` | yield in @critical = lock held across suspend = deadlock |
| AWAIT | `defer_depth` | same as yield |
| AWAIT | `critical_depth` | same as yield |
| SPAWN | `in_interrupt` | pthread_create in ISR = unsafe |

**How it works:** Extracts each NODE_ handler body from check_stmt via grep+sed, scans for flag references. Missing reference where safety contract requires one = BUG. Takes <5 seconds, finds bugs that manual reading misses.

**Extend to other subsystems:** Define rows (handlers) and columns (flags/preconditions). Same grep-based cross-reference pattern works for emitter type dispatch, zercheck state checks, any flag×handler matrix.

**Run after adding:** any new NODE_ type, any new context flag, any new control-flow statement. The matrix exposes gaps automatically.

**Note:** The 5 matrix bugs are now FIXED by FuncProps (function summaries). The audit matrix remains useful for scope-exit flag checks (return/break/continue/goto) which FuncProps doesn't cover.

### Function Summaries — FuncProps (2026-04-14, tracking system #29)

Inferred function properties on Symbol, lazily computed via DFS with cycle detection.
Fixes the 5 matrix audit bugs (direct + transitive) and absorbs `has_atomic_or_barrier()`.

**Data structure:** `Symbol.props` — `{computed, in_progress, can_yield, can_spawn, can_alloc, has_sync}`

**Key functions:**
- `scan_func_props(c, node, parent_sym)` — recursive AST walker, sets all bools in one pass, follows callees transitively via `ensure_func_props`
- `ensure_func_props(c, sym)` — lazy compute + DFS cycle detection (`computed` + `in_progress` flags, same pattern as `FuncSharedTypes` deadlock DFS)
- `check_body_effects(c, body, line, ban_yield, msg, ban_spawn, msg, ban_alloc, msg)` — called at @critical, defer, interrupt entry points. Creates temp scan of the restricted body subtree.

**Where checks happen:**
- NODE_CRITICAL handler — bans can_yield, can_spawn (before `check_stmt`)
- NODE_DEFER handler — bans can_yield (before `check_stmt`)
- NODE_INTERRUPT in check_func_body — bans can_spawn, can_alloc (before `check_stmt`)
- NODE_SPAWN handler — bans `c->in_async` (direct flag, not FuncProps — spawn in async is about ownership, not transitivity)
- NODE_SPAWN handler — reads `func_sym->props.has_sync` (replaces `has_atomic_or_barrier()` call)

**Transitive following:** NODE_CALL in scanner → `scope_lookup` callee → `ensure_func_props(callee)` → merge callee props. Also follows module-qualified calls (`module__func` mangled name). Function pointer calls: conservative (assumed no effects).

**Ban decision framework:** Hardware/OS constraint → emission impossibility → needs runtime → needs type system → if none, track. All current bans justified. See CLAUDE.md.

**Design doc:** `docs/FunctionSummaries.md` — full problem statement, rejected approaches (flags, table-driven, effect annotations), architecture, edge cases, testing strategy.

### Why B5-B6 and B11 are deferred (NOT pure duplication):
- **B5-B6:** Pool alloc emits 6 inline args (`slots, sizeof(slots[0]), gen, used, count, &ok`). Slab emits 2 (`&slab, &ok`). A helper needs `is_pool` flag = same line count, worse readability. v0.4 table-driven solves this properly.
- **B11:** Pool uses `pool.elem`/`pool.count`, Slab uses `slab.elem` + ISR ban. Helper needs type flag + element accessor + optional count + ISR flag = more parameters than the code it replaces.

### Full list of unified helpers after this session (28 total)
| # | Helper | File | What |
|---|---|---|---|
| 1 | `vrp_invalidate_for_assign` | checker.c | VRP range invalidation (R1) |
| 2 | `emit_async_orelse_block` | emitter.c | Async orelse emission (R2) |
| 3 | `emit_shared_ensure_init` | emitter.c | Shared struct mutex+condvar init (R3) |
| 4 | `track_dyn_freed_index` | checker.c | Pool/Slab dynamic-index free tracking (B1) |
| 5 | `check_union_switch_mutation` | checker.c | Union switch lock check (B2) |
| 6 | `should_track_move` | zercheck.c | Move struct type detection |
| 7 | `is_handle_invalid` | zercheck.c | Handle use-check (FREED/MAYBE/TRANSFERRED) |
| 8 | `is_handle_consumed` | zercheck.c | Handle merge-check |
| 9 | `zc_report_invalid_use` | zercheck.c | Error message by handle state |
| 10 | `is_void_opt` | emitter.c | ?void type detection |
| 11 | `emit_opt_null_check` | emitter.c | Optional null check emission |
| 12 | `emit_opt_unwrap` | emitter.c | Optional value unwrap emission |
| 13 | `emit_opt_null_literal` | emitter.c | Optional null literal emission |
| 14 | `emit_return_null` | emitter.c | Return null for current function |
| 15 | `type_can_carry_pointer` | checker.c | Escape flag propagation filter |
| 16 | `propagate_escape_flags` | checker.c | Local/arena-derived flag copy |
| 17 | `check_isr_ban` | checker.c | ISR heap allocation ban |
| 18 | `find_or_create_auto_slab` | checker.c | Task.new auto-slab |
| 19 | `check_volatile_strip` | checker.c | Cast/intrinsic volatile check |
| 20 | `validate_struct_init` | checker.c | Designated init field validation |
| 21 | `find_handle_local` | zercheck.c | Scope-aware handle registration |
| 22 | `emit_zero_value` | emitter.c | Zero value for any return type |
| 23 | `scan_func_props` | checker.c | Recursive AST walker for function properties (FuncProps) |
| 24 | `ensure_func_props` | checker.c | Lazy DFS compute + cache for FuncProps |
| 25 | `check_body_effects` | checker.c | Context entry point effect checker (@critical/defer/interrupt) |

## Firmware Examples + Polish (2026-04-13)

3 new firmware examples exercising ALL v0.3 features. Zero bugs found — confirms compiler readiness.

| Example | Features Exercised |
|---|---|
| `async_sensor.zer` | async/yield, comptime, container(T), move struct, designated init, do-while |
| `concurrency_demo.zer` | shared struct, Semaphore, @once, @critical, spawn+ThreadHandle |
| `slab_registry.zer` | Slab(T), alloc_ptr/free_ptr, defer, comptime, enum switch |

### cinclude angle bracket fix
`cinclude "<stdio.h>"` now emits `#include <stdio.h>` (system header). Previously emitted `#include "<stdio.h>"` (double-quoted angles — GCC error). Detects `<` at start and `>` at end of path string. Local headers (`cinclude "myheader.h"`) unchanged.

**Found by:** writing firmware examples, not adversarial testing. Real usage finds feature gaps that red team rounds don't probe (they attack safety logic, not basic emission).

## Codebase Analysis Audit (2026-04-13) — 2 bugs found by code reading

Targeted analysis of 3 risk areas identified by 12 red team rounds. Read code flows to understand coupling before deciding what to fix.

### BUG-505: Optional enum switch emission
`is_opt_switch` path emitted `emit_expr(arm->values[j])` for enum dot values → bare ident `red` (undeclared in C). Regular enum switch path at line 4022 uses `EMIT_ENUM_NAME`. Optional path was a copy that diverged. Fix: optional path uses same `_ZER_EnumName_variant` pattern. Also: `type_unwrap_distinct` on detection, `opt_inner_enum` tracked.

### *opaque comparison unconditional
`emit_type(TYPE_POINTER)` at line 593 emits `_zer_opaque` unconditionally (not gated by `track_cptrs`). BUG-485 fix incorrectly gated `.ptr` comparison on `e->track_cptrs`. Found by reading the type emission code flow — the struct representation is always active.

**Method:** Read 3 targeted areas based on bug distribution data:
1. Async + untested features → code analysis showed all should work (recursive local collection covers for-init, blocks, etc.)
2. *opaque emission → found unconditional struct, incorrect guard
3. Optional enum switch → found bare ident divergence from regular enum path

**Lesson:** Code reading targeted by bug pattern data finds bugs that testing misses. The *opaque bug was invisible to tests because `--run` enables `track_cptrs` — the guard happened to be true in all test scenarios. Only reading the code reveals the unconditional struct emission.

## Refactors R1-R3 (2026-04-13) — Duplication elimination, 3 latent bugs found

Analysis of 12 red team rounds identified 3 areas where code duplication was CAUSING bugs (not just aesthetic debt). Each refactor extracted a helper that unified duplicated logic and fixed a latent inconsistency.

### R1: `vrp_invalidate_for_assign(c, key, key_len, op, value)` (checker.c)
**What:** Unified VRP range invalidation for assignments. Called with simple ident key AND compound key.
**Why:** Compound key path was missing compound op check (latent BUG-502: `s.idx += 20` didn't wipe `s.idx` range). Both paths now use identical logic.
**Impact:** 2 blocks (68 lines) → 1 helper (45 lines). Future assignment forms automatically covered.

### R2: `emit_async_orelse_block(e, expr, fallback, dest, len, type)` (emitter.c)
**What:** Unified async orelse emission. `dest=NULL` for expr-stmt (discard), `dest` set for var-decl (assign).
**Why:** 3 near-identical blocks had void check inconsistency (one used `checker_get_type`, another used local `type` variable). Helper uses `dest_type` parameter consistently.
**Impact:** 3 blocks (116 lines) → 1 helper (45 lines). Adding a new orelse context = one call.

### R3: `emit_shared_ensure_init(e, root, arrow)` (emitter.c)
**What:** Unified shared struct mutex+condvar initialization. Checks `is_condvar_type` internally.
**Why:** Auto-lock path used `_zer_mtx_ensure_init` (no condvar) for condvar-type shared structs. CAS winner set `inited=1` without condvar init → `@cond_wait` saw `inited=1` → skipped → uninitialized condvar.
**Impact:** 5 sites (57 lines) → 1 helper (20 lines). Adding a new condvar intrinsic = one call.

**Total: 10 duplicated sites → 3 helpers. ~80 lines removed. 3 latent bugs fixed by unification.**

**Lesson:** Code duplication in compilers isn't just debt — it's a bug factory. When two code paths implement the same concept independently, they WILL diverge. The refactor doesn't just clean up code — it makes future divergence impossible.

## Red Team Audit Fixes — Round 12 (2026-04-13, Gemini — 5 claims, 3 real bugs)

### BUG-502: VRP compound assignment range invalidation
Range invalidation was gated by `if (node->assign.op == TOK_EQ)`. Compound ops (`+=`, `-=`, `*=`, etc.) skipped — left stale proven ranges. Fix: ALL assignment ops trigger invalidation. `TOK_EQ` tries to derive new range from value. Compound ops wipe unconditionally.

**VRP architecture after BUG-475/478/479/502:**
1. `&var` → `address_taken=true`, permanently invalid (TOK_AMP handler)
2. Function call → global variable ranges wiped (NODE_CALL handler)
3. Direct assignment `=` → derive new range or wipe (NODE_ASSIGN handler)
4. Compound assignment `+=/-=/*=` → wipe unconditionally (NODE_ASSIGN handler)
5. Comptime calls exempt (pure)

### BUG-503: Async expr-stmt orelse restructured
NODE_EXPR_STMT handler intercepts orelse+block in async mode before `emit_expr`. Uses pre-scanned state struct temp. Separate statements (no GCC statement expression). Result discarded (expr-stmt).

**Async orelse emission summary (3 paths):**
- **Var-decl init** (BUG-481): `self->tmp = expr; if (!tmp.has_value) { block } self->x = tmp.value;`
- **Expr-stmt** (BUG-503): `self->tmp = expr; if (!tmp.has_value) { block }`
- **Expression level** (V46): GCC limitation — can't fix, GCC error is clear

### BUG-504: Condvar intrinsic initialization
All 4 condvar intrinsics (@cond_wait, @cond_timedwait, @cond_signal, @cond_broadcast) now call `_zer_mtx_ensure_init_cv` before `pthread_mutex_lock`. Previously, if the first access to a shared struct was via condvar intrinsic (no prior field access to trigger auto-lock init), the mutex/condvar were uninitialized.

## Red Team Audit Fixes — Round 11 (2026-04-13, Gemini — 5 claims, 4 real bugs)

### BUG-498: Packed struct sync primitive rejection
Semaphore/Barrier/shared struct fields banned inside `packed struct`. `pthread_mutex_t` requires natural alignment — packed offsets cause hard fault on ARM/RISC-V. Added to struct field validation alongside existing Pool/Ring/Slab rejection.

### BUG-499: Async param shadowing rejection
Variable shadowing of function params banned in async functions. Params and locals share state struct (`self->name`) — shadowing overwrites param value. Regular functions unaffected. Check at NODE_VAR_DECL: if `c->in_async` and name matches existing symbol at different line.

### BUG-500: shared(rw) read-only multi-type allowed
Deadlock check now skips when BOTH shared types are `is_shared_rw` AND statement is read-only. `pthread_rwlock_rdlock` allows concurrent readers — no deadlock. Write statements still trigger deadlock check.

### BUG-501: array.len emitter fix
Range-for desugaring generates `collection.len` for loop condition. For TYPE_ARRAY, emitter now emits the compile-time array size as literal (`4U`) instead of invalid C `.len` field access. Checker already handled `array.len` → `ty_usize` — emitter was the missing piece.

**Pattern:** Parser desugaring can generate AST that's semantically valid but emitter-invalid. The checker validates types correctly (array.len → usize), but the emitter must handle the C representation difference (slice = struct with .len, array = raw C array without .len).

## Red Team Audit Fixes — Rounds 9-10 (2026-04-13, Gemini — 10 claims, 5 real bugs)

### BUG-493: Packed struct atomic → compile error
@atomic_* on packed struct fields causes hard fault on ARM/RISC-V (misaligned atomics). Checker walks &field operand to root, checks `is_packed`.

### BUG-494: Move struct eager var-decl registration
Lazy registration (`zc_ensure_move_registered`) found outer handle for inner shadow. Fix: NODE_VAR_DECL eagerly registers move structs at current `scope_depth`. `find_handle` (highest depth) returns inner for inner use. First attempt (shadow logic in `zc_ensure_move_registered`) broke 7 loop tests — declaration-site is the correct location per rule #8.

### BUG-495: Async orelse prescan into expression trees
`prescan_expr_for_orelse` recursively scans NODE_BINARY, NODE_CALL, NODE_ASSIGN, etc. for nested orelse blocks. Registers state struct temp. Expression-level yield in orelse is a GCC limitation ("switch jumps into statement expression") — not fixable, GCC error message is clear. Var-decl level safe via BUG-481 restructured emission.

### BUG-496: Arena value escape + comptime cleanup
Arena VALUE (not pointer) stored in global → dangling buf pointer. Fix: checker checks TYPE_ARENA (and struct fields containing Arena), only rejects LOCAL source → global target. Global arena → global = safe.

Comptime `eval_comptime_block`: 6 early return paths skipped `ct_done` cleanup label. Array bindings leaked. All changed to `goto ct_done`.

### Async architecture summary after rounds 4-10

All async locals now promoted to state struct regardless of scope depth:
- **collect_async_locals**: fully recursive (NODE_BLOCK, NODE_IF, NODE_FOR, NODE_WHILE, NODE_SWITCH, NODE_DEFER, NODE_CRITICAL, NODE_ONCE)
- **prescan_async_temps**: recursive + `prescan_expr_for_orelse` for expression trees
- **State struct fields**: params (BUG-477) + all locals (BUG-490) + orelse temps (BUG-481/495) + static excluded (BUG-486)
- **Emission paths**: var-decl orelse = restructured statements. Expression-level orelse = GCC statement expression (yield = GCC error). Regular locals = `self->name`.

Same approach as Rust's MIR generator transform — promote everything that lives across yield.

## Red Team Audit Fixes — Round 8 (2026-04-13, Gemini — 4 claims, 3 real bugs)

**Summary:** Eighth Gemini audit. 3 real bugs (BUG-490/491/492), 1 false (V39 = per-statement-group locking, same as V12).

### BUG-490: Async sub-block locals — recursive collect_async_locals
`collect_async_locals` was top-level only. Sub-block, if-body, loop-body locals stayed on C stack — stale after yield. Fix: fully recursive scan into all block types. `add_async_local` helper with dedup by name. State struct field emission also recursive (iterative stack traversal). Every local in an async function is now promoted regardless of scope depth.

### BUG-491: Spawn qualifier validation
NODE_SPAWN validated shared vs non-shared but skipped const/volatile qualifier checks. Fix: after pointer safety check, resolve function param types and validate qualifiers. Same checks as NODE_CALL handler (const→mutable = error, volatile→non-volatile = error).

### BUG-492: Dynamic covered_ids (no fixed buffers)
`covered_ids[64]` silently dropped allocations 65+. Fix: stack-first dynamic pattern (`int stack[64]; int *arr = stack; int cap = 64;` with realloc). **Rule added to CLAUDE.md: NEVER use fixed-size buffers for dynamic data.** Same pattern as parser RF9.

## Red Team Audit Fixes — Round 7 (2026-04-12, Gemini — 5 claims, 2 real bugs)

**Summary:** Seventh Gemini audit. 2 real bugs (BUG-488/489), 3 false/design.

### BUG-488: Zercheck scope-aware handle tracking (refactor)
Variable shadowing caused false positive: inner `Handle h` freed → outer `Handle h` falsely FREED. Root cause: flat name matching in `find_handle` with no scope concept.

**Architecture after refactor:**
- `find_handle(ps, name, len)` — source lookup, returns highest `scope_depth` match. For UAF/alias checks. ~20 call sites.
- `find_handle_local(ps, name, len)` — destination registration, returns CURRENT scope match only. For var-decl alloc/alias. ~6 call sites. Returns NULL for outer-scope handles → `add_handle` creates new shadow.
- `scope_depth` on PathState: NODE_BLOCK increments on entry, decrements on exit.
- `scope_depth` on HandleInfo: set by `add_handle` from `ps->scope_depth`.
- Block exit: removes inner-scope handles shadowing outer handles. Propagates state only if same `alloc_id` (aliases of same allocation).
- `pathstate_copy` preserves `scope_depth` for if/else branch copies.

**Pattern:** Mirrors checker's `scope_lookup` (any scope chain) vs `scope_lookup_local` (current scope only). Same two-function separation, same semantics.

**Why multiple patch attempts failed:** The flat array assumption was in `find_handle`, called 25 times. Patching individual call sites (last-match, scope_depth flag on one path) broke other paths. The proper fix was the two-function separation — clean architectural boundary between "reading handle state" and "registering new handles."

### BUG-489: Runtime @inttoptr alignment check
Variable-address `@inttoptr` had range check but no alignment check. Fix: emitter emits `if (_zer_ma0 % align != 0) _zer_trap("unaligned")` after range check. Alignment from target type width.

### Not bugs (V33, V34, V37)
- V33: VRP conservative for compound keys — bounds check NOT eliminated
- V34: Async cancellation — design limitation, accepted for firmware
- V37: Union variant access requires switch — attack blocked

## Red Team Audit Fixes — Round 6 (2026-04-12, Gemini — 4 claims, 3 real bugs)

**Summary:** Sixth Gemini audit. 3 real bugs (BUG-485/486/487), 1 false.

### BUG-485: *opaque comparison — .ptr extraction for track_cptrs
With `--track-cptrs`, `*opaque` is `_zer_opaque` struct (not `void*`). C can't compare structs with `==`/`!=`. Fix: emitter NODE_BINARY detects `TYPE_POINTER(TYPE_OPAQUE)` and emits `.ptr` on both sides. Only fires when `e->track_cptrs` — without it, `*opaque` is plain `void*`.

### BUG-486: Async static locals — skip state struct promotion
`static u32 count` inside async must stay as C `static` in poll function (shared across instances). `collect_async_locals` and struct field emission now check `!is_static`.

### BUG-487: Union move variant — ban assignment to prevent resource leak
`m.k.fd = 42; m.id = 100;` silently overwrites move struct resource. Fix: checker NODE_ASSIGN scans union type for move struct variants → compile error. Same as Rust's enum Drop — must use switch for safe variant transitions.

**Pattern from rounds 5-6:** Feature interactions are the primary bug source. `*opaque` + `==` (emitter vs type representation), `async` + `static` (promotion vs persistence), `union` + `move struct` (variant vs ownership). Each feature works alone; bugs appear at intersections.

### Not a bug (V32): `u8[@size(usize)]` compiles correctly
`@size(usize)` resolves to `sizeof(size_t)` in emitted C. Target-portable.

## Red Team Audit Fixes — Round 5 (2026-04-12, Gemini — 5 claims, 3 real bugs)

**Summary:** Fifth Gemini audit. 3 real bugs (BUG-482/483/484), 2 false.

### BUG-482: Async struct names module-mangled
`_zer_async_init` collides when two modules have `async void init()`. Fix: `emit_async_func` builds mangled name `module__funcname` at top, uses throughout all `_zer_async_` emissions. Same pattern as `EMIT_MANGLED_NAME`.

### BUG-483: Condvar init inside CAS winner path
`_zer_mtx_ensure_init` sets `inited=1`, then `if (!inited) condvar_init()` is always false — condvar never initialized. Fix: `_zer_mtx_ensure_init_cv(mtx, inited, cond)` initializes condvar alongside mutex in CAS winner. `_zer_mtx_ensure_init` is wrapper calling `_cv(..., NULL)`. Semaphore acquire/release pass `&s->_zer_cond`.

### BUG-484: Move struct orelse fallback transfer
`Token b = opt orelse a` — `a` not marked HS_TRANSFERRED. Fix: after primary move_src transfer, also check `orelse.fallback` for move struct types. Handles direct ident, array element, struct field. Same type detection as primary path.

### Not bugs (V16, V20)
- V16: Union variant access requires switch + tag check — "corpse read" impossible
- V20: VRP unsigned clamp makes large u64 unprovable — bounds check stays

## Mutex Lazy Init Race Fix (2026-04-12)

Found during C interop testing (C library callback + shared struct). `_zer_mtx_ensure_init` uses CAS: `__atomic_compare_exchange_n(inited, &expected, 2, ...)`. States: 0=uninit, 2=in-progress, 1=ready. CAS 0→2 = winner initializes mutex (+condvar). Losers spin `while (inited != 1) {}`. Prevents double-init when two threads hit first shared struct access simultaneously.

## C Interop Safety Model — `cinclude` + `*opaque` + `shared struct`

Complete C library interop requires two keywords:
- `*opaque` for memory safety (pointer lifecycle tracking via zercheck + runtime type_id)
- `shared struct` for concurrency safety (auto-lock fires from ANY thread — ZER spawn, C pthread, OS callback)

The auto-lock is on the DATA (shared struct's mutex), not on the thread creation mechanism. C library threads calling ZER callbacks are automatically serialized when accessing shared struct fields. No annotations needed — same `shared struct` keyword used for ZER-to-ZER concurrency.

**Safety boundary:** C library internal bugs (their own data races, UAF) are outside ZER's scope — same as Rust's `unsafe extern`.

## Red Team Audit Fixes — Round 4 (2026-04-12, Gemini — 4 claims, 1 real bug)

**Summary:** Fourth Gemini audit. 1 real bug (async orelse stack ghost), 1 doc fix (ABA gen counter), 2 false (auto-lock group + VRP coercion).

### BUG-481: Async yield in orelse — state struct temp promotion (proper fix)
`u32 x = maybe_get() orelse { yield; 42; }` — `_zer_tmp0` is stack local, stale after yield+resume.

**Architecture (same as Rust's MIR generator transform):**
1. `prescan_async_temps(e, body)` — recursive pre-scan finds NODE_ORELSE with block fallback in async body. Records `AsyncTemp` entries with type + temp_id.
2. `emit_async_func` adds `_zer_async_tmpN` fields to state struct typedef.
3. In async var-decl orelse path: split GCC statement expression into separate statements using `self->_zer_async_tmpN`. Temp survives yield.

Non-async code unchanged (efficient GCC statement expression). No language restriction — yield inside orelse blocks now works correctly.

### Not bugs (V12, V15)
- V12: auto-lock group — per-statement locking, unlock before return
- V15: VRP array coercion — slice doesn't alias local variables

## Proper Fixes — BUG-474 Call Graph DFS + BUG-479 VRP 100% (2026-04-12)

### BUG-474 proper: Deadlock detection via call graph DFS
Replaced `_shared_scan_depth < 8` depth limit with memoized DFS.

**`FuncSharedTypes` cache on Checker struct:** `func_name → set of shared type_ids`. `compute_func_shared_types()` does DFS with `in_progress` (cycle detection) + `computed` (memoization). `scan_body_shared_types()` walks full AST. Each function computed once. `collect_shared_types_in_expr` NODE_CALL does O(1) cache lookup.

Handles: any call depth (tested at 20), mutual recursion (ping/pong cycles), no false positives on separate statements.

### BUG-479 proper: VRP address_taken at TOK_AMP handler
Moved from per-site invalidation to SINGLE check point at `check_expr(NODE_UNARY, TOK_AMP)`. Every `&var` expression marks root variable's VarRange as `address_taken=true`. `push_var_range` skips narrowing for `address_taken` entries.

**Correctness argument:** ZER has no pointer arithmetic. `&var` is the ONLY way to create a pointer to a variable. Single check point = 100% alias coverage without points-to analysis.

**VRP architecture after all fixes (BUG-475/478/479):**
1. `&var` in any expression → `address_taken=true`, range permanently invalid (TOK_AMP handler)
2. `&var` passed to function call → range wiped (NODE_CALL handler, redundant with #1 but defense-in-depth)
3. Any function call → global variable ranges wiped (NODE_CALL handler)
4. Comptime calls exempt (pure, no side effects)

## Red Team Audit Fixes — Round 3 (2026-04-12, Gemini — 4 claims, 4 real bugs)

**Summary:** Third Gemini red team audit. All 4 claims were real bugs.

### BUG-477: Async function parameters not in state struct
`async void worker(u32 x) { yield; u32 after = x; }` — `x` undeclared in poll function. `collect_async_locals` only scanned NODE_VAR_DECL. Fix: add params to async_locals list + state struct fields + init function signature. Checker init registration updated to include original params.

### BUG-478: VRP global variable ranges not invalidated after function call
`if (g_idx < 10) { sneaky(); arr[g_idx]; }` where `sneaky()` sets g_idx=100. Fix: after NODE_CALL, scan VRP stack for global variables and wipe their ranges. Skip comptime calls (pure).

### BUG-479: VRP address-taken flag for pointer aliasing
`*u32 p = &idx; if (idx >= 4) return; p[0] = 100; arr[idx]` — guard re-narrowed idx after invalidation. Fix: `address_taken` flag on `struct VarRange`. When `*T p = &var`, flag the aliased variable. `push_var_range` skips narrowing for `address_taken` entries — guards cannot override.

**VRP architecture after BUG-475/478/479:**
Three invalidation triggers:
1. `&var` passed to function call → wipe var's range (BUG-475)
2. Any function call → wipe all global variable ranges (BUG-478)
3. `*T p = &var` in var-decl → mark var as `address_taken`, permanently prevent narrowing (BUG-479)

### BUG-480: Move struct value capture in switch
`switch (m) { .k => |val| { ... } }` where val is move struct — creates copy, two owners. Fix: same check as V13 (if-unwrap): `type_unwrap_distinct(type)->kind == TYPE_STRUCT && is_move` → error "use |*val|". Applied to both union-switch and optional-switch capture paths.

## Red Team Audit Fixes — Round 2 (2026-04-12, Gemini — 7 claims, 3 real bugs)

**Summary:** Second Gemini red team audit. 7 attack vectors tested, 3 real bugs found (BUG-474/475/476), 4 false or already-handled.

### BUG-474: Transitive deadlock depth limit too shallow
`_shared_scan_depth < 4` → `< 8`. Deep call chains (5+ levels) between shared struct accesses now detected. Matches spawn transitive scan depth.

### BUG-475: VRP not invalidated on &variable passed to function call
When `&var` is passed as a function argument, `var`'s value range MUST be wiped — the callee may modify it through the pointer. Fix: NODE_CALL handler in `check_expr` scans args for `NODE_UNARY(TOK_AMP)`, walks to root ident, sets range to `[INT64_MIN, INT64_MAX]`. Without this fix, bounds checks were eliminated based on stale ranges.

**Pattern for future VRP additions:** Any code path where a variable's value could change through an alias (pointer arg, struct field mutation) must invalidate the VRP range. Currently handled: direct assignment, compound assignment, function call with `&var` arg.

### BUG-476: Move struct from array element / struct field not tracked
`Token copy = arr[0]; arr[0].kind` compiled — zercheck only tracked NODE_IDENT sources for move transfer. Extended to use `handle_key_from_expr()` for compound keys. Also detects move struct type through TYPE_ARRAY element type.

### Not Bugs (4 claims)
- V2 (Slot retirement DoS): Gen counter wraps (`gen++; if (gen==0) gen=1`), no retirement. Slots always reuse. Gemini fabricated "permanently retired" mechanism.
- V5 (Async defer bypass): Defer fires at function completion (after all yields), not per-yield. Correct by design — Duff's device places defer block at body end.
- V6 (Zero-handle collision): Gen starts at 1 (`if (gen[i]==0) gen[i]=1`). Auto-zeroed handle gen=0 never matches gen=1. Already fixed.
- V7 (Comptime budget global): Budget resets per top-level call (`if (depth==1) _comptime_ops=0`). Per-call, not per-compilation.

## Red Team Audit Fixes — Round 1 (2026-04-12, Gemini red team — 8 attempts, 5 real bugs)

**Summary:** External AI (Gemini) performed red team audit on ZER-LANG safety guarantees. 8 attack vectors tested: 5 were real bugs (V1,V3,V4,V5,V6), 3 were already caught by existing checks (V2,V7,V8).

### Transitive Deadlock Detection
`collect_shared_types_in_expr` now scans called function bodies transitively (depth 4) for shared type field accesses. Catches: `a.x = helper()` where `helper()` accesses `b.y` — different shared type in same statement = potential AB-BA deadlock. Required adding NODE_RETURN/NODE_EXPR_STMT/NODE_VAR_DECL handling in the expression scanner (callee body statements were being skipped).

### Comptime Global Instruction Budget
`_comptime_ops` counter in `eval_comptime_block`. Incremented per loop iteration (both for and while/do-while). Cap: 1,000,000 total operations per comptime call. Prevents nested loop DoS (10000×10000 = 100M iterations). Resets on depth==1 (top-level call). Returns CONST_EVAL_FAIL on budget exceeded.

### Naked Function Body Validation (V4)
Naked functions (`__attribute__((naked))`) must only contain `asm` and `return` statements. Non-asm code (var-decl, assignment, etc.) uses stack that was never allocated (no prologue). Checker scans body block and errors on first non-asm, non-return statement.

### Thread-Unsafe Slab/Pool/Ring from Spawn (V5)
Pool, Slab, Ring alloc/free/push/pop have non-atomic metadata access. Accessing global Pool/Slab/Ring from spawned thread is a data race. `scan_unsafe_global_access` no longer skips TYPE_POOL/TYPE_SLAB/TYPE_RING — only TYPE_ARENA (bump allocator, single-threaded reset) and TYPE_BARRIER (has own mutex) are safe. Also fixed: NODE_FIELD and callee expression scanning were missing from the spawn global scanner.

### Container Monomorphization Depth Limit (V6)
`_container_depth` static counter in TYNODE_CONTAINER resolution. Cap: 32. Prevents compiler hang/crash from self-referential containers like `container Node(T) { ?*Node(T) next; }`. Also: `subst_typenode()` recursive TypeNode substitution replaces 5 one-level pattern matches — handles T at any nesting depth (`?*Container(T)`, `[]*T`, etc.).

### Move Struct Value Capture Banned (V13)
`if (opt) |k|` where unwrapped type is move struct → compile error. Value capture copies the move struct, creating two owners. Fix: checker detects `should_track_move(unwrapped)` in non-pointer capture path of if-unwrap, forces `|*k|` pointer capture instead.

### Async Shared Struct Access Banned (V14)
Shared struct field access inside async function body → compile error. Lock acquired by auto-locking may be held across yield/await = deadlock. `c->in_async` flag set during async function body check. Same approach as Rust (MutexGuard not Send across .await).

### Already Caught (V2, V7, V8, V12, V15, V16)
- V2: Union mutation via *opaque inside switch arm → "cannot take address of union inside switch arm"
- V7: Union containing move struct → "cannot read union variant directly — must use switch"
- V8: @ptrtoint(&local) returned → "cannot return @ptrtoint of local" (fixed earlier this session)
- V12: Container type-id collision → each stamp gets unique `c->next_type_id++`, @ptrcast catches mismatch
- V15: Comptime @ptrtoint → eval fails (can't resolve pointer addresses at compile time)
- V16: Move struct partial field access → zercheck marks entire struct as HS_TRANSFERRED, any field access errors

### Not Bugs (V9, V11)
- V9: Async defer bypass → NOT a bug, defer fires correctly on async completion (Duff's device handles it)
- V11: Same-type instance deadlock → NOT a real deadlock. Per-statement locking means locks never overlap. Atomicity concern is design limitation (use single shared struct for atomic multi-field ops)

## Semaphore(N) Builtin Type (2026-04-12)

Counting semaphore. `Semaphore(3) slots;` declares with initial count 3. `Semaphore(0)` valid (producer-consumer pattern).

**Lexer:** `TOK_SEMAPHORE` (capital S, case 'S' with Slab). **Parser:** `TYNODE_SEMAPHORE` with optional `count_expr`. `(N)` optional — bare `Semaphore` allowed for pointer params (`*Semaphore p`).
**Types:** `TYPE_SEMAPHORE` with `uint32_t count`. **Checker:** `@sem_acquire`/`@sem_release` validate arg is `TYPE_SEMAPHORE` or `*TYPE_SEMAPHORE` (pointer unwrap). Skipped by spawn global scan (thread-safe by design — has own mutex/condvar).
**Emitter:** `_zer_semaphore` struct (count + pthread_mutex + condvar). `_zer_sem_acquire`/`_zer_sem_release` helper functions (same pattern as `_zer_barrier_init`/`_zer_barrier_wait`). Auto-zero emits `{ .count = N }`.

**Barrier/Semaphore pointer support:** Checker accepts both `TYPE_X` and `*TYPE_X`. Emitter conditionally adds `&` for direct access, omits for pointer. Enables: `void func(*Barrier b) { @barrier_wait(b); }`.

## Local Function Pointer Init Required (2026-04-12)

Local `void (*cb)(u32)` without initializer → compile error. Auto-zero creates NULL funcptr; calling it segfaults. Must either initialize (`= handler`) or use nullable `?void (*cb)(u32) = null`.

Global funcptrs exempt — commonly assigned in init functions following C convention. Matches existing `*T` pointer rule (BUG-239/253).

## Division by Zero — Function Call Divisors (2026-04-12)

Forced division guard extended to NODE_CALL divisors. `x / func()` where `func()` has no proven return range (min > 0) → compile error. Developer must store result in variable and add zero-guard. Proven nonzero return functions (via `find_return_range`) pass without error.

## Comptime Enum Values (2026-04-11)

`Color.red` resolves to the enum variant's integer value at compile time.

- `resolve_enum_field(c, node)` — looks up NODE_FIELD on enum type, returns `SEVariant.value` as int64.
- `eval_const_expr_scoped` extended: handles NODE_FIELD directly, and handles NODE_BINARY containing enum fields by recursing on left/right before delegating to `eval_const_expr_ex`.
- Works in: `static_assert(Color.red == 0)`, array index `table[Color.green]`, comptime function args (when type matches).
- Also available in `eval_const_expr_subst` (comptime evaluator) via `_comptime_global_scope` lookup.

## Comptime Float Arithmetic (2026-04-11)

`comptime f64 PI_HALF() { return 3.14159 / 2.0; }` — comptime functions with float return types.

**Architecture:** Parallel eval path at call site (same approach as comptime struct return). When `eval_comptime_block` returns CONST_EVAL_FAIL and return type is f32/f64:
1. `find_comptime_return_expr(body)` — finds the return expression
2. `eval_comptime_float_expr(expr, params, count)` — evaluates float expression tree. Handles: NODE_FLOAT_LIT, NODE_INT_LIT (cast to double), NODE_IDENT (param lookup — float bits stored as int64 via memcpy), NODE_UNARY(MINUS), NODE_BINARY(+, -, *, /).
3. Result stored as `node->call.comptime_float_value` (double) + `is_comptime_float` flag.
4. Emitter: `%.17g` format for full double precision.

**Float params:** At call arg collection, when `eval_const_expr_scoped` fails but arg is NODE_FLOAT_LIT (or negated float), the double value is stored as bits in int64 via `memcpy`. The float evaluator reconverts via `memcpy` on lookup.

**Design decision:** Does NOT extend ComptimeCtx to support float locals/arrays/loops. Only handles simple float expressions in return statements. This covers the embedded use case (compile-time math constants like PI, conversion factors, sensor calibration). Full float comptime (float locals, float arrays) would require a tagged ComptimeValue union — not needed yet.

## Value Range Propagation (checker.c)

Tracks `{min_val, max_val, known_nonzero}` per variable. Stack-based: newer entries shadow older, save/restore via count for scoped narrowing. `push_var_range()` intersects with existing (only narrows), clamps min to 0 for unsigned types.

**Narrowing events:** literal init (`u32 d = 5` → {5,5,true}), for-loop condition (`i < N` → {0,N-1}), guard pattern (`if (i >= N) return` → {0,N-1} after if), comparison in then-block, modulo (`x % N` → {0,N-1}), bitwise AND (`x & MASK` → {0,MASK}).

**Expression-derived ranges:** `derive_expr_range(c, expr, &min, &max)` handles `TOK_PERCENT` and `TOK_AMP` with constant RHS (including const global symbol lookup). Used at both var-decl init and NODE_ASSIGN reassignment paths. This eliminates false "index not proven" warnings for hash map patterns like `slot = hash % TABLE_SIZE; arr[slot]`.

**Proven nodes:** `mark_proven(c, node)` adds to `proven_safe` array. `checker_is_proven()` exposed to emitter. Emitter skips `_zer_bounds_check` for proven NODE_INDEX, skips div trap for proven NODE_BINARY.

**Inline call range (2026-04-06):** `arr[func()]` — at NODE_INDEX, if index is NODE_CALL with an ident callee that has `has_return_range`, and `return_range_max < array.size`, the index is proven safe. Enables hash map pattern `table[hash(key)]` with zero overhead.

**find_return_range enhancements (2026-04-06):**
1. Constant returns: `return 0`, `return N+1` via `eval_const_expr_scoped` — unions with `% N` ranges
2. Chained call returns: `return other_func()` inherits callee's `has_return_range` — enables multi-layer `get_slot() → raw_hash() → % N` chains
3. Guard-clamped ident returns: `if (idx >= N) { return 0; } return idx;` — `find_var_range()` on the ident uses the guard narrowing from check_stmt. Works because `find_return_range` runs immediately after `check_stmt(body)` while VarRanges are still on the stack.
4. NODE_SWITCH/FOR/WHILE/CRITICAL recursion — finds returns inside all control flow
5. Order-dependent: callee must be checked BEFORE caller (declaration order in ZER). Cross-module: imported functions checked first (topological order) so return ranges are available.

### C-Style Cast Syntax: (Type)expr (2026-04-07)

`NODE_TYPECAST` — parser detects `(TypeKeyword)expr` and `(*Type)expr`. Only keyword types are unambiguous (`u32`, `f32`, `*`, `?`, `const`, `volatile`). `(ident)expr` stays as parenthesized expression (ident could be a variable).

**Checker:** validates source→target conversion. Allows: int↔int, int↔float, float↔float, ptr↔ptr, ptr↔opaque, int→ptr, ptr→int, distinct typedef. Rejects: struct→int, invalid combinations.

**Emitter:** primitives emit as C cast `((uint16_t)(expr))`. `*opaque` round-trips use `_zer_opaque` wrap/unwrap with type_id check (same as `@ptrcast`).

**`@truncate`/`@ptrcast`/`@inttoptr` still work** — `(Type)expr` is sugar. `@saturate` and `@bitcast` remain the only REQUIRED explicit intrinsics.

### Block Defer Multi-Free Tracking (BUG-443, 2026-04-06)

`defer_scans_free` returned on FIRST match — `defer { free(a); free(b); }` only tracked `a`. Replaced with `defer_scan_all_frees` which walks ALL statements in defer blocks recursively, marking each found handle as FREED. Split into `defer_stmt_is_free` (single check) + `defer_scan_all_frees` (recursive walker with direct PathState mutation).

### Defer Before Return Expression — UAF (BUG-442, 2026-04-06)

`defer free(h); return get(h).val;` → emitted C called free BEFORE evaluating return expression. Fix: when NODE_RETURN has expression AND pending defers, hoist into typed temp: `RetType _ret = expr; defers; return _ret;`. Handles `?T` wrapping (optional return from non-optional expression). Skips trivial literals (no side effects).

**CRITICAL: Every `return expr` with `defer` that accessed deferred resources was silently broken.** This is the most common embedded pattern — `defer free(h); return h.field;`. All existing tests happened to return literal values or variables that didn't touch deferred resources.

### Keep Validation Variable Mismatch (BUG-441, 2026-04-06)

`arg_node` vs `karg` in keep parameter validation (NODE_CALL). The orelse-unwrap + intrinsic-walk loop produces `karg`, but line 3147 used `arg_node` (original unwrapped). `@ptrcast(*opaque, &global)` as arg → `arg_node` is NODE_INTRINSIC → `arg_node->unary.operand` segfaults.

**Debugging:** ASan (`gcc -g -fsanitize=address -O0`) pinpointed `checker.c:3148` instantly. Always use ASan for compiler crashes.

### Non-Keep Parameter Store Enforcement (BUG-440, 2026-04-06)

`keep` enforcement was caller-side only. The function side — storing a non-keep pointer param to global/static — was unchecked. Now NODE_ASSIGN checks: if target is global/static AND value is a non-keep pointer ident that's local-scope (not global, not static, not `is_local_derived`, not `is_arena_derived`), error. The heuristic identifies parameters as: pointer-typed, non-keep, non-global, non-static, non-flagged locals.

**Auto-guard for NODE_CALL indices NOT possible (2026-04-06 — attempted and reverted):**
`emit_auto_guards` evaluates the index expression to emit `if (idx >= size)`. For NODE_CALL, this calls the function TWICE (once in guard, once in access) — double-evaluating side effects. The inline `_zer_bounds_check` with GCC statement expression correctly handles single-eval for call indices. Auto-guard remains NODE_IDENT only.

**Forced division guard:** NODE_IDENT divisor not proven nonzero → compile error with fix suggestion. Resolves const global symbol init values (e.g., `const u32 N = 16; x / N` → proven nonzero). Complex expressions keep runtime check.

**Slice-to-pointer auto-coerce for extern C functions:** When arg is `[]T` and param is `*T` at a call site for a forward-declared function (no body), the checker allows it. The emitter auto-appends `.ptr` (already handled at line ~1265). ZER-to-ZER calls with bodies still require explicit `.ptr`. This is the C interop boundary convenience — `puts("hello")` works without `.ptr` when `puts` is declared as `i32 puts(const *u8 s);`.

## Bounds Auto-Guard (checker.c + emitter.c)

When array index is not proven by range propagation, compiler auto-inserts `if (idx >= size) { return <zero>; }` as invisible guard. Works for ALL cases: params, globals, volatile, computed.

**Checker:** `mark_auto_guard(c, node, array_size)` stores in `auto_guards` array. `checker_auto_guard_size()` exposed to emitter. Warning emitted so programmer can add explicit guard for zero overhead.

**Emitter:** `emit_auto_guards(e, node)` walks expression tree, finds auto-guarded NODE_INDEX, emits `if` guard as preceding statement. Called from emit_stmt for NODE_EXPR_STMT, NODE_VAR_DECL, NODE_RETURN. Uses `emit_zero_value()` for return type's zero value. Runtime `_zer_bounds_check` stays as belt-and-suspenders backup.

## Auto-Keep, @cstr Auto-Orelse, Provenance Extensions

**Auto-keep on fn ptr pointer-params:** In NODE_CALL keep validation, if callee is a function pointer (not direct call to named function), ALL pointer params treated as `keep` automatically. Invisible to programmer.

**@cstr auto-orelse:** In emitter @cstr handler, overflow check uses `return <zero_value>` instead of `_zer_trap`. Same pattern as auto-guard — device keeps running on overflow.

**Array-level *opaque provenance:** `prov_map_set()` auto-sets root key when key contains `[`. "callbacks[0]" → also sets "callbacks". Different type on same root → compile error "heterogeneous *opaque array."

**Cross-function provenance summaries:** `find_return_provenance()` scans function body for return expression provenance. `ProvSummary` stored on Checker. At NODE_VAR_DECL init from function call returning *opaque, `lookup_prov_summary()` sets `sym->provenance_type`.

## ZER vs Rust — Concurrency Primitives Comparison

Every ZER concurrency feature is a **language primitive** (keyword, intrinsic, or type). Rust implements most as library code (std). This table is the authoritative reference for what ZER has, what it doesn't, and why.

| Primitive | Rust (std library) | ZER (language) | Who's better |
|---|---|---|---|
| Mutex | `Mutex<T>` | `shared struct` (auto-lock) | ZER — no boilerplate |
| RwLock | `RwLock<T>` | `shared(rw) struct` | ZER — keyword |
| Thread spawn | `thread::spawn` | `spawn` keyword | ZER — compile-time safety |
| Thread join | `JoinHandle::join` | `ThreadHandle.join()` | Equal |
| Condvar | `Condvar` | `@cond_wait/signal/broadcast` | ZER — intrinsic |
| Condvar timeout | `wait_timeout` | `@cond_timedwait` | ZER — intrinsic |
| Barrier | `Barrier` (std) | `Barrier` keyword type | ZER — type-checked |
| Atomics | `AtomicU32` (std) | `@atomic_*` intrinsics | Equal |
| Thread-local | `thread_local!` macro | `threadlocal` keyword | ZER — no macro |
| Once init | `Once` (std) | `@once { }` intrinsic | ZER — block syntax |
| Async | `async/await` + `Future` + `Pin` | `async/await` (Duff's device) | ZER simpler — no Pin/Future |
| Channel | `mpsc::channel` (unbounded) | `Ring(T, N)` (bounded) | Different — ZER bounded by design |
| Send/Sync | Trait bounds | spawn checker | Equal — both compile-time |
| Deadlock detect | None | same-statement check | **ZER only** |
| Scoped threads | `thread::scope` (std) | `ThreadHandle` must-join | ZER — enforced at compile-time |

**Not in ZER (by design):**
- `Arc<T>` — Pool/Slab/Handle covers allocation, no ref counting needed
- `park/unpark` — condvar is the primitive, park is sugar
- `Future/Pin` — coroutines are concrete types, no trait abstraction needed
- Unbounded channel — bounded `Ring` prevents OOM on embedded
- Async runtime — user writes poll loop (freestanding) or library wraps epoll

## Test Files
| File | What | Count |
|---|---|---|
| `test_lexer.c` | Token scanning | 218 |
| `test_parser.c` | AST construction | 70 |
| `test_parser_edge.c` | Edge cases, func ptrs, overflow | 93 |
| `test_modules/` | Multi-file imports, typedefs, interrupts | 6 |
| `test_checker.c` | Type checking basic | 71 |
| `test_checker_full.c` | Full spec coverage + security + audit + safety | 510 |
| `test_extra.c` | Additional checker | 18 |
| `test_gaps.c` | Gap coverage | 4 |
| `test_emit.c` | Full E2E (ZER→C→GCC→run) | 229 |
| `test_zercheck.c` | Handle tracking, aliasing, params, leaks, cross-func | 49 |
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
85g. **Optional `== null` emits `.has_value` (BUG-257)** — In NODE_BINARY, when `==`/`!=` has a `NODE_NULL_LIT` side and the other is a struct-based optional (not null-sentinel), emit `(!x.has_value)` for `==` and `(x.has_value)` for `!=`. Null-sentinel optionals (`?*T`, `?FuncPtr`) still emit direct pointer comparison.
85h. **@ptrcast volatile check (BUG-258)** — In @ptrcast checker, if source is a volatile pointer (type-level `pointer.is_volatile` OR symbol-level `sym->is_volatile`), target must also be volatile. Prevents GCC from optimizing away writes through stripped-volatile pointers.
85i. **@cstr return escape (BUG-259)** — `return @cstr(local_buf, "hi")` returns pointer to stack buffer. NODE_RETURN checks NODE_INTRINSIC with name "cstr", walks buffer arg to root ident, rejects if local.
85j. **Deref-call assignment escape (BUG-260)** — `*pool.get(h) = &local` stores local pointer through function call result. NODE_ASSIGN walks target through deref/field/index; if root is NODE_CALL, rejects &local and local-derived values.
86. **@cstr const destination check** — Looks up destination ident symbol; if `is_const`, error. Separate from the compile-time overflow check (BUG-234) which validates sizes.
88. **Typemap is per-Checker struct (RF1)** — `type_map`, `type_map_size`, `type_map_count` moved from static globals into Checker struct. `typemap_init/set/get` take `Checker*`. `checker_get_type()` now takes `Checker *c, Node *node`. Emitter uses `checker_get_type(e->checker, node)`. Eliminates use-after-free risk in LSP multi-request scenarios.
89. **Unified emit_top_level_decl (RF2)** — `emit_file` and `emit_file_no_preamble` now share a single `emit_top_level_decl()` function. Previously two parallel switch statements that had to stay in sync (caused BUG-086/087). Adding a new NODE kind now requires updating only one place.
90. **Mangled name buffers use arena allocation (RF4)** — Fixed-size `char[256]` buffers for module name mangling replaced with arena-allocated buffers sized to actual need. Eliminates silent truncation on long module+symbol names.
91. **@cstr volatile detection walks field/index chains (RF7)** — `@cstr(reg_block.buf, src)` now correctly detects volatile by walking through NODE_FIELD/NODE_INDEX to root ident. Previously only handled direct NODE_IDENT destinations.
93. **resolve_type stores in typemap for emitter access (RF3)** — `resolve_type()` split into `resolve_type()` (wrapper with cache check + store) and `resolve_type_inner()` (actual resolution). Every resolved TypeNode is stored in typemap keyed by `(Node *)tn`. Emitter's `resolve_tynode()` tries `checker_get_type(e->checker, (Node *)tn)` first, falls back to `resolve_type_for_emit()` for uncached TypeNodes. This is the transitional step — `resolve_type_for_emit` becomes dead code once all TypeNode paths are cached.
92. **Null literal error messages improved (RF6)** — `u32 x = null` now says "'null' can only be assigned to optional types" instead of confusing "cannot initialize X with 'void'".
87. **Parser var-decl detection: lightweight lookahead for IDENT** — For TOK_IDENT-starting statements, the parser uses 2-3 token lookahead instead of full speculative `parse_type()`. Patterns: IDENT IDENT → var decl, IDENT `[`...`]` IDENT → array decl, IDENT `(*` → func ptr decl. Saves scanner+current+previous, scans tokens, restores. No AST allocation or error suppression. Non-IDENT type tokens (`*`, `?`, `[]`) still use speculative `parse_type()` since they unambiguously start types.
94. **eval_const_expr uses CONST_EVAL_FAIL sentinel (RF8)** — `INT64_MIN` replaces `-1` as the "not a constant" sentinel. Allows negative intermediate values in constant expressions. Callers check `== CONST_EVAL_FAIL` instead of `< 0`. Array size validation: `CONST_EVAL_FAIL` → "not a constant", `<= 0` → "must be > 0", else valid.
95. **Parser arrays are dynamic with stack-first pattern (RF9)** — All parse loops use stack arrays (32-64 elements) with arena-overflow doubling. OOM handling via `parser_alloc()` + `p->oom` flag. Parser depth limit of 64 prevents stack overflow. Token-before guards on ALL loops (block, file, struct, enum, union, switch) prevent infinite loops when `consume()` fails without advancing.
96. **`is_func_ptr_start()` helper (RF10)** — Saves scanner+current+previous, peeks past `(` to check for `*`, restores. Replaces 5 duplicated detection sites. All func ptr declaration paths (local, global, struct field, func param, typedef) use this helper.
7. **Defer stack scoping** — return emits ALL defers, break/continue emit only loop-scope defers
8. **Type arg parsing** — intrinsics use `type_arg`, but method calls pass types as NODE_IDENT expression args. Primitive type keywords (`u32`) can't be passed as args (only struct/enum names work as NODE_IDENT).
97. **Union switch lock checks pointer alias types (BUG-261)** — `union_switch_type` stored on Checker. In field mutation check, if root ident doesn't name-match the locked variable, check if its type is `*UnionType` matching `union_switch_type`. Only applies to pointer types (might alias external memory). Direct local variables of the same type are safe.
98. **Slice start/end hoisted into temps (BUG-262)** — The runtime check path (`slice_needs_runtime_check`) now uses `size_t _zer_ss = start; size_t _zer_se = end;` inside a GCC statement expression. Eliminates double/triple evaluation of side-effecting start/end expressions.
99. **Volatile pointer stripping rejected at call sites (BUG-263)** — In NODE_CALL arg checking, after const checks, also check if arg pointer is volatile (type-level OR symbol-level) and param is non-volatile. Same pattern as const check but for volatile qualifier.
100. **If-unwrap `|*v|` rvalue hoist (BUG-264)** — When condition is `NODE_CALL` (rvalue), hoist into typed temp before taking address: `T _tmp = expr; T *_ptr = &_tmp;`. Lvalue conditions (NODE_IDENT etc.) still use direct `&(expr)` for mutation semantics.
101. **Multi-dimensional arrays** — Parser chains TYNODE_ARRAY: `u8[10][20]` → ARRAY(ARRAY(u8, 10), 20). `emit_type_and_name` collects all dims by walking TYPE_ARRAY chain to base type, then emits `base name[dim1][dim2]...`. Statement disambiguator loops through multiple `[N]` bracket pairs. Bounds checks emit per-dimension.
102. **Recursive union rejected (BUG-265)** — Same check as struct BUG-227. In `register_decl(NODE_UNION_DECL)`, after resolving each variant type, walk through arrays to base and check `inner == t`. Catches `union U { U x; }` and `union U { U[4] arr; }`.
103. **Arena alloc_slice overflow-safe (BUG-266)** — Emitted C uses `__builtin_mul_overflow(sizeof(T), n, &total)` instead of raw `sizeof(T) * n`. If overflow, returns `(void*)0` (null) — arena alloc skipped, optional slice `.has_value` stays 0. Prevents heap OOB from overflowed `.len`.
104. **If-unwrap uses emit_type_and_name (BUG-267)** — Replaces `__auto_type` with explicit type for initial copy in if-unwrap. Uses `emit_type_and_name` to handle func ptr name placement correctly. Preserves volatile qualifier.
105. **Union switch lvalue detection (BUG-268)** — For mutable capture `|*v|`, detects if switch expression is lvalue (not NODE_CALL). Lvalue: use direct `&(expr)` so mutations write through to original. Rvalue (NODE_CALL): hoist into temp (mutation is to copy — semantically correct for temporaries).
106. **Const expr div-by-zero (BUG-269)** — Checker uses `eval_const_expr(divisor)` instead of checking only `NODE_INT_LIT`. Catches `10 / (2 - 2)` at compile time.
107. **Array return type rejected (BUG-270)** — In `check_func_body`, if resolved return type is TYPE_ARRAY, error. C forbids array returns. Suggest struct wrapper or slice.
108. **Distinct union/enum switch unwrap (BUG-271)** — Both checker and emitter call `type_unwrap_distinct` on switch expression type before TYPE_UNION/TYPE_ENUM dispatch. `expr_eff` / `sw_eff` used for all variant lookups and tag emission.
109. **Volatile if-unwrap copy (BUG-272)** — Checks if condition ident's symbol has `is_volatile`. If so, emits `volatile` before the typed copy. Uses `emit_type_and_name` for correct func ptr name placement.
110. **Volatile array assign byte loop (BUG-273)** — Array assignment checks target root symbol for `is_volatile`. If volatile, emits `for(_i) vd[_i] = vs[_i]` byte loop instead of memcpy. Same pattern as @cstr volatile (BUG-223).
111. **Volatile union capture pointer (BUG-274)** — `sw_volatile` flag detected from switch expression root symbol. When set, mutable capture `|*v|` emits `volatile T *v` instead of `T *v`.
132. **@ptrcast const stripping (BUG-304)** — Mirrors BUG-258 volatile check. If source `pointer.is_const` and target is not, error.
133. **Const capture bypass (BUG-305)** — In if-unwrap |*v|, walks condition to root ident. If `is_const`, forces `cap_const = true` and `cap_type->pointer.is_const = true`.
134. **memmove for array assign (BUG-306)** — Both NODE_ASSIGN and NODE_VAR_DECL array paths use `memmove` instead of `memcpy`. Handles self-assignment overlap safely.
135. **@saturate u64 upper bound (BUG-308)** — u64 saturate path adds `> 18446744073709551615.0 ? UINT64_MAX` check when source could be f64.
131. **Rvalue field assign lvalue check (BUG-302)** — NODE_ASSIGN walks field/index chains to base. If base is NODE_CALL, checks return type: non-pointer (value type) → "not an lvalue". Pointer return → valid lvalue via auto-deref. Literals also rejected.
128. **`type_unwrap_distinct` recursive (BUG-295)** — Changed from single `if` to `while` loop. Handles `distinct typedef (distinct typedef T)` at any depth. All callers benefit automatically.
129. **Const fold INT_MIN / -1 guard (BUG-296)** — `eval_const_expr` TOK_SLASH and TOK_PERCENT check `l == INT64_MIN && r == -1` → CONST_EVAL_FAIL. Prevents signed overflow UB in the compiler.
130. **`emit_type(TYPE_ARRAY)` emits dimensions (BUG-297)** — Walks array chain to base type, emits all `[N]` suffixes. `sizeof(u32[10])` emits `sizeof(uint32_t[10])` = 40. Multi-dim: `sizeof(u8[10][20])` emits `sizeof(uint8_t[20][10])`.
126. **Volatile |*v| capture pointer (BUG-292)** — `expr_is_volatile` check added to mutable capture branch. When volatile, emits `volatile T *_zer_uwp` and `volatile T _zer_uwt` (rvalue path).
127. **Assign to non-lvalue rejected (BUG-294)** — NODE_ASSIGN checks target kind. NODE_CALL, NODE_INT_LIT, NODE_STRING_LIT, NODE_NULL_LIT, NODE_BOOL_LIT → "not an lvalue" error.
124. **Orelse volatile via __typeof__ (BUG-289)** — All 3 orelse `__auto_type _zer_tmp` sites replaced with `__typeof__(expr) _zer_tmp`. `__typeof__` preserves volatile/const from the expression type. `__auto_type` strips qualifiers.
125. **Local escape via *param deref (BUG-290)** — Target walk in &local escape check extended: `NODE_UNARY(TOK_STAR)` added alongside `NODE_FIELD`/`NODE_INDEX`. `*p = &local` where `p` is a pointer parameter now caught. `target_is_param_ptr` detection broadened to any deref/field/index on a pointer-typed local.
121. **Arena.over slice single-eval (BUG-286)** — Hoists slice arg into `__auto_type _zer_ao` temp before extracting `.ptr` and `.len`. Array args unchanged (sizeof doesn't evaluate).
122. **Pool/Ring struct field rejected (BUG-287)** — In struct field registration, checks TYPE_POOL/TYPE_RING and errors. These types use C macros that can't be inside struct definitions.
123. **Bit extract hi < lo rejected (BUG-288)** — In NODE_SLICE integer path, when both hi and lo are constant and hi < lo, compile error. Prevents silent negative-width extraction.
119. **Volatile return stripping (BUG-281)** — NODE_RETURN checks if return expression is volatile (type-level or symbol-level) and function return type is non-volatile pointer. Same pattern as const return check.
120. **Volatile init/assign stripping (BUG-282)** — NODE_VAR_DECL and NODE_ASSIGN check source ident's `is_volatile` symbol flag when type-level `pointer.is_volatile` is not set. Assignment also checks target symbol volatile to allow volatile-to-volatile.
116. **Volatile array var-decl init byte loop (BUG-278)** — NODE_VAR_DECL array init path checks `var_decl.is_volatile`. If volatile, emits byte-by-byte loop instead of memcpy.
117. **`is_null_sentinel` recursive distinct unwrap (BUG-279)** — Changed from single `if (TYPE_DISTINCT)` to `while (TYPE_DISTINCT)` loop. Handles `distinct typedef (distinct typedef *T)` chains at any depth.
145. **Const return type parsing** — `const` at global scope uses same lookahead as volatile (scanner save/restore, skip type tokens, check if IDENT followed by `(`). If function detected, routes to `parse_func_or_var`. Otherwise, re-consumes `const` and routes to `parse_var_decl`. Enables `const []u8 get_name() { ... }` pattern needed by stdlib.
142. **Return orelse @ptrcast(&local) (BUG-317)** — NODE_RETURN orelse root walk now inspects NODE_INTRINSIC (ptrcast/bitcast) and NODE_UNARY(&) in fallback. Only when `ret_type->kind == TYPE_POINTER` — value bitcasts are safe. Walks into the intrinsic's last arg, then into & operand with field/index chain walk.
143. **Orelse fallback bidirectional flag propagation (BUG-318)** — Var-decl init flag propagation splits NODE_ORELSE into two checks: `orelse.expr` AND `orelse.fallback`. Both checked for local/arena-derived idents via field/index walk. Previously only checked expr side.
144. **@size(distinct void) (BUG-320)** — `@size` handler checks both `type_arg` (for keyword types like `void`) AND expression args (for named types like `distinct typedef void MyVoid` which parse as NODE_IDENT). Calls `type_unwrap_distinct` before TYPE_VOID/TYPE_OPAQUE check.
139. **Orelse assignment escape (BUG-314)** — NODE_ASSIGN flag propagation now walks into NODE_ORELSE: checks fallback for `&local` (sets `is_local_derived`) and for local-derived idents. Direct escape check added: if value is NODE_ORELSE, inspects fallback for `&local`/local-derived, walks target to root, rejects if global/static. Complements BUG-251 (return orelse) and BUG-205 (assign local-derived).
140. **Distinct slice/array comparison (BUG-315)** — Binary ==/!= comparison check calls `type_unwrap_distinct()` on both operands before checking TYPE_SLICE/TYPE_ARRAY. Without this, `distinct typedef []u8 Buffer; a == b` passes checker but GCC rejects (struct comparison). Same pattern as BUG-271 (distinct union/enum switch).
141. **Bit-set index hoisting (BUG-316)** — Runtime bit-set assignment (`reg[hi..lo] = val` with non-constant hi/lo) hoists indices into `_zer_bh`/`_zer_bl` uint64_t temps inside the statement expression. Constant path unchanged (precomputed masks, no temps needed). Prevents double/triple evaluation of side-effecting index functions.
137. **Volatile slices — `volatile []T` (BUG-310)** — `TYPE_SLICE` has `bool is_volatile`. `type_volatile_slice()` constructor in types.c. `type_equals` checks `is_volatile` match. `can_implicit_coerce` blocks volatile→non-volatile stripping, allows non-volatile→volatile widening. Emitter: preamble defines `_zer_vslice_T` typedefs for all primitives (with `volatile T *ptr`). `emit_type(TYPE_SLICE)` picks `_zer_vslice_` or `_zer_slice_` prefix based on `is_volatile`. `emit_array_as_slice()` adds `volatile` cast when slice is volatile. Struct/union emission includes `_zer_vslice_` typedefs after `_zer_slice_`. Checker: `TYNODE_VOLATILE` resolver now handles TYPE_SLICE (was pointer-only). Var-decl `volatile` qualifier propagates to slice type. Volatile array → non-volatile slice rejected at call/var-decl/assign with helpful error messages suggesting `volatile []T`.
136. **usize width matches host platform** — `type_width(TYPE_USIZE)` returns `sizeof(size_t) * 8`. On 64-bit: usize = 64-bit, coercion/truncation rules correct. `is_literal_compatible` uses `sizeof(size_t) == 8 ? UINT64_MAX : 0xFFFFFFFF`. Cross-compilation: build zerc for target platform, or use emitted `size_t` which GCC cross-compiler resolves.
138. **Slab(T) dynamic growable pool** — Full pipeline: `TOK_SLAB` (lexer), `TYNODE_SLAB` (parser, takes one type param, no count), `TYPE_SLAB` (checker, `type_slab()` constructor). Same Handle(T) API as Pool: `alloc() → ?Handle(T)`, `get(h) → *T` (non-storable), `free(h)`. Checker: mirrors Pool method validation at all 4 sites (NODE_CALL methods, assignment rejection, stack rejection, struct field rejection). Emitter: `_zer_slab` typedef in preamble (pages/gen/used/page_count/page_cap/total_slots/slot_size). Runtime: `_zer_slab_alloc` scans for free slot then grows via `calloc` (64 slots/page); `_zer_slab_get` checks generation counter, computes `pages[idx/64] + (idx%64)*slot_size`; `_zer_slab_free` clears used + bumps gen. Global var emits `_zer_slab name = { .slot_size = sizeof(T) }`. Method interception passes `&name` (unlike Pool which passes individual arrays). ZER-CHECK: extended TYPE_POOL guards to accept TYPE_SLAB — same handle tracking for use-after-free/double-free.
118. **`@size(usize)` target-dependent (BUG-280)** — `compute_type_size` returns `CONST_EVAL_FAIL` for TYPE_USIZE (before `type_width` which hardcodes 32). Emitter uses `sizeof(size_t)`.
115. **`keep` in function pointer types (BUG-277)** — `TYPE_FUNC_PTR` now has `bool *param_keeps` array (NULL if no keep params). Parser parses `keep` before param types in `parse_func_ptr_after_ret`. `TYNODE_FUNC_PTR` carries `param_keeps`. Checker resolves from both TYNODE (func ptr declarations) and `NODE_FUNC_DECL` (regular functions). `type_equals` checks per-param keep mismatch. Call-site `keep` validation uses `effective_callee->func_ptr.param_keeps` — works for both direct and func ptr calls.
113. **@size target-portable via sizeof_type (BUG-275)** — `compute_type_size` returns `CONST_EVAL_FAIL` for TYPE_POINTER, TYPE_SLICE, and structs/unions containing them. Array Type stores `sizeof_type` (the resolved @size target Type). `emit_array_size()` emits `sizeof(T)` when `sizeof_type` is set, numeric otherwise. `emit_type_and_name` also checks `sizeof_type` per dimension.
114. **`_zer_` prefix reserved (BUG-276)** — `add_symbol` rejects identifiers starting with `_zer_` (5-char prefix check via `memcmp`). Prevents collision with compiler temporaries. Applies to all scopes (local, global, params).
112. **RF11: `expr_is_volatile()` / `expr_root_symbol()` helpers** — Walks any expression through NODE_FIELD/NODE_INDEX/NODE_UNARY(STAR) to root NODE_IDENT, looks up symbol in current then global scope. Returns symbol or NULL. `expr_is_volatile()` wraps this to check `sym->is_volatile`. Used by array assign (BUG-273/320), if-unwrap (BUG-272), switch capture (BUG-274), @cstr (BUG-223), array copy source (BUG-320). New volatile emission sites should use this instead of inline walks.
146. **RF12: `build_expr_key_a()` arena-allocated expression keys** — All expression key building in checker.c uses `ExprKey build_expr_key_a(Checker *c, Node *expr)` instead of `char buf[128]` stack buffers. Returns `{str, len}` struct. Uses 512-byte stack buffer internally, arena-copies the result. No external size limit — deeply nested field chains (`a.b.c.d.e.f.g[0].h`) that exceeded 128 bytes previously returned 0 (silently skipping the check). Now always succeeds. All 13 call sites converted: division guard, union switch lock (3 sites), provenance compound keys (4 sites), range propagation (2 sites), mangled keep key, union switch key setup. The underlying `build_expr_key()` function still exists (used internally by `build_expr_key_a` and by zercheck.c).
147. **RF12: Dynamic `ComptimeParam` arrays** — All 3 `ComptimeParam` allocation sites use stack-first pattern: `ComptimeParam stack[8]; ComptimeParam *cparams = pc <= 8 ? stack : arena_alloc(...)`. Previously `cparams[32]` silently truncated functions with >32 params. The `eval_comptime_call_subst` site uses `malloc`+`free` (no checker arena available in recursive static context).

148. **`find_unique_allocator` must skip dual-registered symbols (BUG-416)** — Imported globals exist under BOTH raw name and mangled name in global scope (BUG-233 design). Scanning for "unique allocator for element type T" finds two matches — same `Type*`, different symbol names. Without `if (found && found->type == t) continue;`, returns NULL (ambiguous). This pattern applies to ANY function scanning global scope for unique matches.
149. **POSIX functions need `_POSIX_C_SOURCE` on strict C99 (BUG-417)** — `popen`/`pclose` in `zerc_main.c` get implicit `int` return without the feature test macro. On 64-bit systems, this truncates `FILE*` → segfault. Fix: `#define _POSIX_C_SOURCE 200809L` before `<stdio.h>`. Watch for `-Wall` warnings about `implicit declaration of function`.

### Const Global Division Guard (BUG-395)
`const u32 MAP_SIZE = 16; h % MAP_SIZE` falsely errored "not proven nonzero." Two root causes: (1) `eval_const_expr` (in ast.h) doesn't resolve `NODE_IDENT` — it only handles literals and binary ops. (2) `sym->func_node` was never set for `NODE_GLOBAL_VAR` in `register_decl`, so the const init lookup had nothing to read. Fix: add const symbol init lookup in both `/` `%` (NODE_BINARY) and `/=` `%=` (NODE_ASSIGN) division guard paths. Also set `sym->func_node = node` for global vars in `register_decl`. **Pattern for future const lookups:** scope_lookup the ident → check `is_const` + `func_node` → read `func_node->var_decl.init` → `eval_const_expr(init)`.

### #line Directive in Orelse Defer (BUG-396)
`emit(e, "{ "); emit_defers(e);` emitted `#line` on the same line as `{` — GCC requires `#line` at the start of a line. Fix: change all `"{ "` to `"{\n"` before `emit_defers`/`emit_defers_from`. 6 sites: var-decl orelse return (1), auto-guard return (2), orelse break (1), orelse continue (1). **Pattern:** never emit code + `emit_defers` on the same line when `source_file` is set (defers call `emit_stmt` which emits `#line`).

### Windows `zerc --run` GCC Quoting (BUG-397)
`system("\"gcc\" -std=c99 ...")` fails on Windows `cmd.exe` — outer quotes treated as the entire command string. Fix: only quote gcc_path when it contains spaces (bundled GCC path). Plain `gcc` from system PATH emitted without quotes. Same fix for run command: `.\hash_map.exe` not `.\"hash_map.exe"`.

### ?Handle(T) Struct Field Double-Wrap (BUG-398)
Emitter var-decl init for `?T` target wraps value in `{value, 1}`. But if init expression is NODE_FIELD returning `?Handle(T)` (already `_zer_opt_u64`), it double-wraps. Fix: in the `else` branch (non-IDENT, non-CALL, non-NULL init), check `checker_get_type(init)->kind == TYPE_OPTIONAL` — if already optional, assign directly. Same pattern as BUG-032 (NODE_IDENT already-optional check).

### Cross-Function Range Propagation (BUG-399)
`find_return_range(c, body, &min, &max, &found)` walks function body for NODE_RETURN expressions with derivable ranges (% N, & MASK). If ALL returns have ranges, stores `return_range_min/max` + `has_return_range` on Symbol (types.h). At call sites (var-decl init + assignment), if `derive_expr_range` fails and value is NODE_CALL to a function with `has_return_range`, propagates the range. Eliminates false "index not proven" warnings for `slot = hash(key)` where `hash()` returns `h % TABLE_SIZE`.

### []T → *T Extern Const Safety (BUG-400)
The `[]T → *T` auto-coerce for extern functions must check const: if arg is string literal or const slice, param must be `const *T`. Without this, `puts(*u8 s)` accepted string literals — would allow writes through `.rodata` pointer. Check is before the `slice_to_ptr_ok` flag, so const rejection takes priority over extern allowance.

### *opaque Level 1-5 Safety Tracking

**Level 1 — zercheck compile-time (zercheck.c):**
- `is_alloc_call(zc, call)` — detects extern functions returning `*opaque`/pointer with no body (malloc, calloc, strdup, any extern returning pointer)
- `is_free_call(call, key, keylen, bufsize)` — detects `free()` by name (4 chars, 1 arg)
- `zc_check_var_init`: registers `*opaque p = malloc(...)` as ALIVE with `pool_id = -2` (malloc'd, not pool/param)
- `zc_check_expr(NODE_CALL)`: detects `free(p)` → marks FREED, propagates to aliases
- `zc_check_expr(NODE_INTRINSIC)`: checks `@ptrcast` source against freed *opaque
- `zc_check_expr(NODE_UNARY/TOK_STAR)`: checks deref `*p` against freed *opaque
- Reuses existing HandleInfo/PathState machinery — same state machine as Handle tracking

**Level 2 — poison-after-free (emitter.c):**
- In `NODE_EXPR_STMT`, after emitting `free(p);`, auto-inserts `p = (void*)0;`
- Only fires for direct `free()` calls with NODE_IDENT argument (not pool.free)

**Level 3+4+5 — inline header + global wrap (emitter.c preamble + zerc_main.c):**
- Preamble emits `__wrap_malloc/free/calloc/realloc/strdup/strndup` when `e->track_cptrs` is set
- 16-byte inline header: `[gen:4][size:4][magic:4][alive:4]` prepended to every allocation
- Magic `0x5A455243` ("ZERC") identifies tracked allocations — untracked pointers pass through
- `_zer_check_alive(ptr, file, line)` — emitted before `@ptrcast` from `*opaque` via comma operator
- `extern void *__real_malloc(size_t)` etc. — forward declarations for linker-provided originals
- GCC invocation adds `-Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc`

**Flags (zerc_main.c):**
- `--track-cptrs`: explicitly enable Level 3+4+5
- `--release`: disable Level 3+4+5 (Level 1+2 always active)
- `--run` without `--release`: Level 3+4+5 enabled by default (`track_cptrs || (!release_mode && do_run)`)
- `emitter.track_cptrs` flag on Emitter struct controls preamble emission

**Full design document:** `docs/ZER_OPAQUE.md` — 601 lines covering all levels, edge cases, performance, Ada/SPARK comparison, implementation plan.

### Pitfalls Found Writing Real .zer Code (2026-04-02)

These caused test failures and are not obvious from reading C source tests:

- **Arena.over() at global scope is rejected** — "initializer must be constant expression." Must create Arena inside a function: `u8[4096] backing; Arena scratch = Arena.over(backing);`
- **`?void` function must have explicit `return;`** — `?void validate(u32 x) { if (x > 100) { return null; } }` errors "not all paths return." Add `return;` at end for the success path.
- **`!ptr` on `*opaque` is rejected** — ZER's `!` requires bool, not pointer. Use `if (ptr) |p| { }` pattern for optional pointers, or declare as `?*opaque` and unwrap with orelse.
- **`@size(T)` returns `usize`, not `u32`** — function params declared as `u32 size` won't accept `@size(T)`. Use `usize size` for C interop with size_t.
- **`*opaque` emits as `_zer_opaque` (struct with type_id)** — NOT `void*`. Can't directly pass to C's `free(void*)` via forward declaration. Use Slab instead, or wrap C allocation in helper functions.
- **`cinclude "stdlib.h"` + declaring `free(*opaque)` conflicts** — GCC sees both `free(void*)` from header and `free(_zer_opaque)` from ZER declaration. Don't cinclude headers for functions you declare manually.
- **`arena.reset()` outside defer warns** — use `arena.unsafe_reset()` to suppress the warning in tests/non-safety-critical code.

### Audit Findings Verified (2026-04-03)

7-point external audit verified against codebase:

1. **Type ID 0 (provenance bypass)** — Real, by design. `@ptrcast` skips type_id==0 for C interop. Not a bug — C boundary is inherently untracked. Future `--strict-interop` flag possible.
2. **Pointer arithmetic** — False finding. ZER deliberately rejects `ptr + N`. Use `ptr[N]` (indexing). This is a safety feature.
3. **Slab uint32_t total_slots** — Real, fixed. Changed to `size_t` (page_count, page_cap, total_slots, local vars in slab functions).
4. **MAYBE_FREED conservatism** — Partially fixed (2026-04-05). `if (err) { free(h); return; }` no longer causes false MAYBE_FREED — zercheck now detects that the freeing branch always exits (return/break/continue/goto), so post-if the handle is still ALIVE. Remaining case: `if (cond) { free(h); }` without return — this is genuine MAYBE_FREED (developer should add `return;` after free).
5. **Atomic width unchecked** — Real bug, fixed. Added width validation: must be 8/16/32/64 bits. 64-bit targets get warning about libatomic on 32-bit platforms.
6. **Large preamble** — Real debt, acknowledged. ~300 lines of C strings in emitter. Works but hard to maintain.
7. **Comptime nested calls** — Partially real. Recursion depth guard works (max 16). Error message is generic. Two-pass registration handles most ordering issues.

### Second External Audit (2026-04-03)

4-point audit — pointer indexing, ISR alloc, type-ID shadowing, ghost handle:

1. **Pointer indexing no bounds check** — Real. `p[N]` on `*T` emitted raw C. Fix: non-volatile `*T` indexing now warns "use slice." Volatile `*T` from `@inttoptr` gets compile-time bounds from `mmio` range (NEW — `mmio_bound` on Symbol). No other language bounds-checks MMIO indexing at compile time.
2. **Slab.alloc in ISR** — Real. Slab uses calloc internally which may deadlock with global mutex. Fix: `c->in_interrupt` check before Slab alloc → compile error. Pool is safe (static, no malloc).
3. **Type-ID shadowing** — False. ZER compiles all modules in one invocation with one `next_type_id` counter. No incremental compilation = no collision possible.
4. **Ghost handle** — Real. `pool.alloc()` as bare expression → handle leaked silently. Fix: NODE_EXPR_STMT detects pool/slab alloc without assignment → compile error.

### MMIO Pointer Index Bounds (5-Layer MMIO Safety)

When `volatile *T ptr = @inttoptr(*T, addr)` is assigned:
1. Look up which `mmio` range contains `addr`
2. Calculate `bound = (range_end - addr + 1) / sizeof(T)`
3. Store as `sym->mmio_bound` on the Symbol
4. In NODE_INDEX for TYPE_POINTER, if `mmio_bound > 0` and index is constant, check `idx < mmio_bound`
5. If proven, `mark_proven(c, node)` — emitter skips bounds check

```
mmio 0x40020000..0x4002001F;              → 32 bytes
volatile *u32 gpio = @inttoptr(*u32, 0x40020000);
                                            → bound = 32/4 = 8
gpio[7] = 0xFF;                           → 7 < 8 → proven safe
gpio[8] = 0xFF;                           → 8 >= 8 → compile error
```

Both var-decl (local) and global var paths set `mmio_bound`. Works on any architecture — math is `(range_end - addr + 1) / type_width`.

**Direct `@inttoptr[N]` indexing:** NODE_INDEX also checks when object is `NODE_INTRINSIC(inttoptr)` — performs mmio range lookup inline without needing a Symbol. Closes the "no variable" bypass.

**Variable index auto-guard:** When MMIO pointer has `mmio_bound > 0` and index is not constant, `mark_auto_guard(c, node, mmio_bound)` inserts runtime `if (i >= bound) return;`. Same auto-guard mechanism as arrays.

**Handle design: why `tasks.get(h).id` is verbose but correct:** Handle is `u64` (index+gen). It carries no reference to which Pool/Slab allocated it. `h.id` shortcut was proposed but rejected — compiler can't resolve which pool when multiple exist for the same type. The explicit `pool.get(h)` tells the emitter which `_zer_slab_get()` to call. This is a fundamental design constraint of Handle-based allocation, not fixable without making Handle a fat struct.

5 layers total:
1. Compile-time: address in declared range
2. Compile-time: address aligned
3. **Compile-time: index within range-derived bound (NEW)**
4. Boot-time: @probe verifies hardware
5. Runtime: fault handler traps

### `[*]T` Syntax (v0.3 — dynamic pointer to many)

Parser accepts `[*]T` as alias for `[]T`. Both resolve to `TYNODE_SLICE`. Same internal `TYPE_SLICE` = `{ptr, len}`.

**Parser (parser.c line ~380):** `TOK_LBRACKET` → check `TOK_STAR` → `TOK_RBRACKET` → `TYNODE_SLICE`. Falls through to existing `[]T` path if no `*`.

**Why `[*]T` instead of `[]T`:** C devs read `[]` as "empty array, fill in size." `[*]` reads as "pointer (`*`) to many items (`[]`)." Same type, better name for C audience.

**Type system after v0.3:**
- `T[N]` → fixed array (compile-time size)
- `[*]T` → dynamic pointer to many (bounds checked, replaces `[]T`)
- `*T` → pointer to one (non-null)
- `?*T` → pointer to one (nullable)
- `[]T` → **deprecated** (parser warns: "use [*]T instead"), same as `[*]T` internally

**Deprecation warning:** Parser emits `warn()` when `[]T` is used. Warning suppressed when `parser.source == NULL` (test harness mode) to avoid noise from 200+ test strings that still use `[]T`. Real `.zer` files always see the warning. The `warn()` function (parser.c line ~57) uses same `print_source_line_p()` as errors.

**Full design documents:** `docs/ZER_STARS.md` (syntax), `docs/ZER_SUGAR.md` (Handle auto-deref + Task.new())

### Why ZER doesn't need a borrow checker

ZER's memory model is fundamentally simpler than Rust's:
- **Handles are indices (u64), not references** — `pool.get(h)` does a fresh lookup each call, never caches a pointer
- **Pool is fixed, Slab uses paged allocation** — existing pages never move, no invalidation
- **No `&mut` vs `&` distinction** — no shared mutable reference problem
- **ALIVE/FREED/MAYBE_FREED** covers ownership at compile-time, generation counter catches UAF at runtime
- **Cost of borrow checker:** lifetime annotations on every function, 6-month learning curve, "fighting the checker" friction
- **Cost of ZER's approach:** occasional `return;` after conditional free — 10 seconds to fix, produces better code

### @bitcast Struct Width Validation (BUG-325)
`type_width()` returns 0 for TYPE_STRUCT, TYPE_UNION, TYPE_ARRAY. The @bitcast width check `if (tw > 0 && vw > 0 && tw != vw)` was silently skipped for structs. Fix: when `type_width()` returns 0, fall back to `compute_type_size(t) * 8`. This catches `@bitcast(Big, small)` where structs have different memory sizes. `compute_type_size` returns `CONST_EVAL_FAIL` for types with target-dependent size (pointers, slices) — those still skip the check (GCC validates at C level).

### Switch Capture Const Safety (BUG-326)
The if-unwrap path (BUG-305) walks the condition expression to its root ident and checks `is_const`. The switch capture path was missing this check — `cap_const = false` was hardcoded for mutable `|*v|` captures. Fix: before the union/optional dispatch in NODE_SWITCH, walk `node->switch_stmt.expr` through field/index chains to root, check `is_const`. Set `cap_const = switch_src_const` and `cap_type->pointer.is_const = true` when source is const. Applies to BOTH union switch and optional switch capture paths.

### Array Copy Source Volatile (BUG-320)
Array assignment and var-decl init paths only checked `expr_is_volatile(target)` for the destination. The source can also be volatile (e.g., reading from MMIO). Fix: `arr_volatile = expr_is_volatile(target) || expr_is_volatile(value)`. Source pointer cast uses `const volatile uint8_t*` to preserve volatile reads. Both NODE_ASSIGN and NODE_VAR_DECL array init paths updated.

### Module Name Mangling — Double Underscore Separator (BUG-332)
Module-qualified symbols use `module__name` (double underscore `__`) as the separator in both checker mangled keys and emitter C output. Previously used single `_` which caused collisions: `mod_a` + `b_c` and `mod_a_b` + `c` both mangled to `mod_a_b_c`. With `__`: `mod_a__b_c` vs `mod_a_b__c` are always distinct.

8 sites updated:
- **Checker:** 3 mangled key registrations in `checker_register_file` and `checker_push_module_scope` (arena-allocated, `+2` for separator)
- **Checker:** 1 mangled key lookup in `keep` validation fallback
- **Emitter:** `emit_user_name()` helper (used by `EMIT_STRUCT_NAME`/`EMIT_UNION_NAME`/`EMIT_ENUM_NAME`)
- **Emitter:** `EMIT_MANGLED_NAME` macro (function declarations)
- **Emitter:** NODE_IDENT primary mangled lookup + fallback raw lookup (both emit `%.*s__%.*s`)
- **Emitter:** Global var declaration mangling

### keep Validation Completeness (BUG-334/336)
The `keep` parameter escape check in NODE_CALL must block ALL sources of short-lived memory:
1. `&local` — direct address of local (original check)
2. `is_local_derived` — aliased local pointers (BUG-221)
3. `is_arena_derived` — arena-allocated pointers (BUG-336)
4. Local `TYPE_ARRAY` → slice coercion (BUG-334) — stack array creates slice pointing to stack
Each check is a separate `if` block in the keep validation loop. All use `arg_node->kind == NODE_IDENT` to find the argument symbol.

### zercheck Capture Tracking (BUG-335)
If-unwrap captures (`if (pool.alloc()) |h| { ... }`) must register `h` as HS_ALIVE in the then-branch PathState. Without this, use-after-free (`pool.free(h); pool.get(h)`) inside capture blocks is invisible to zercheck. Detection: NODE_IF with `capture_name` + condition is NODE_CALL with callee NODE_FIELD where field name is "alloc".

### Union Variant Lock via Pointer Alias (BUG-337)
The variant lock must detect mutations through struct field chains: `s.ptr.b = 10` where `s.ptr` is `*U` (pointer to locked union). During the assignment target walk, check if any NODE_FIELD's object has type `TYPE_POINTER` whose inner type matches `union_switch_type`. Only triggers for pointer-typed fields (could alias external memory), not direct local variables of same union type (those are distinct values).

### Constant-Indexed Handle Array Aliasing (BUG-462, 2026-04-08)

`Handle(T)[4] ents; ents[0] = m0 orelse return;` — the assignment aliasing code at NODE_ASSIGN (line ~1102) called `handle_key_from_expr(node->assign.value)` but value was NODE_ORELSE. `handle_key_from_expr` returns 0 for NODE_ORELSE → alias never created → `m0` seen as leaked.

**Fix:** Unwrap NODE_ORELSE/NODE_INTRINSIC/NODE_TYPECAST before extracting key. Same unwrap chain as the var-decl alias path (line 811-818). Now `ents[0]` gets same alloc_id as `m0` — free propagates correctly.

**Pattern:** Any zercheck code extracting source keys from assignment values must unwrap orelse/intrinsic/typecast first. The var-decl path already did this; the NODE_ASSIGN path was missing it.

### Struct Field Pointer Alias UAF (BUG-463, 2026-04-08)

`h.inner = w; free_ptr(w); h.inner.data` — NODE_FIELD UAF check walked to root ident (`h`) and only checked that. Root `h` is an untracked stack struct. The tracked key `"h.inner"` (alias of `w` with same alloc_id) was never checked.

**Fix:** Walk EVERY prefix of the field/index chain, not just root. For `h.inner.data`, check: `"h.inner.data"`, `"h.inner"`, `"h"`. Any tracked prefix that is FREED catches the UAF. Added `free_line < cur_line` guard to exclude the free call's own arguments (pool.free(s.h) marks s.h FREED, then expression check re-visits s.h on same line → false positive without the guard).

**Pattern:** NODE_FIELD UAF now catches pointer aliasing through struct fields at any depth. The prefix walk handles `a.b.c.d` by checking `a.b.c.d`, `a.b.c`, `a.b`, `a` — first tracked FREED prefix triggers the error.

### Deadlock Detection Redesign (BUG-464, 2026-04-08)

**Old model (overly conservative):** `check_block_lock_ordering` tracked `last_shared_id` across an entire block. `a.x = 10; b.y = 20; if (a.x != 10)` → false deadlock error. The third statement accesses Alpha after Beta — "descending order."

**Why old model was wrong:** The emitter does lock→op→unlock PER STATEMENT GROUP. No two different shared types are ever locked simultaneously. The ABBA deadlock pattern requires NESTED locks (lock A, then lock B while A still held). ZER's emitter never nests locks.

**New model:** Only detect same-statement multi-lock — when ONE expression/statement accesses TWO DIFFERENT shared types. The emitter can only lock ONE type per group, so the second type is unprotected. `collect_shared_types_in_expr()` finds ALL shared types in an expression tree. If ≥2 distinct types found → compile error.

**What changed:**
- Removed: cross-statement ordering check (safe by construction — locks released between statements)
- Added: `collect_shared_types_in_expr()` and `collect_shared_types_in_stmt()` — walk expression tree, collect all shared types
- Updated: 3 existing deadlock tests to use same-statement pattern (`a.x = b.y;` instead of `b.y = 1; a.x = 2;`)

**Test patterns:**
- `a.x = 10; b.y = 20; if (a.x != 10)` → **OK** (cross-statement, independent locks)
- `a.x = b.y;` → **ERROR** (same statement, two shared types)
- `a.x = 10; a.x = 20;` → **OK** (same type, grouped under one lock)

### Function Pointer as Spawn Argument (BUG-465, 2026-04-08)

Spawn wrapper arg struct emitted funcptr field name outside parens: `uint32_t (*)(uint32_t) a1;` instead of `uint32_t (*a1)(uint32_t);`. The `emit_spawn_wrappers` function used `emit_type_and_name(e, at, NULL, 0)` then `emit(e, " a%d; ")` — separate name placement breaks function pointer syntax.

**Fix:** Pass the field name (`"a0"`, `"a1"`, etc.) to `emit_type_and_name` directly instead of NULL. Same class of bug as BUG-412 (funcptr array emission). **Pattern:** Any site emitting a type+name pair must use `emit_type_and_name()` with the actual name — never emit type and name separately. Function pointers and arrays require the name inside the type syntax.

**Found by:** Auditing existing tests — `rt_sendfn_spawn_with_fn_arg.zer` had been rewritten to use integer dispatch instead of funcptr args to work around the bug.

### Heterogeneous *opaque Array Constant-Index Exemption (BUG-466, 2026-04-08)

`Op[2] ops; ops[0].ctx = @ptrcast(*opaque, &adder); ops[1].ctx = @ptrcast(*opaque, &multiplier);` was rejected with "heterogeneous *opaque array." This is the fundamental vtable array pattern (Linux kernel `file_operations[]`).

**Root cause:** `prov_map_set` forced root-level homogeneous provenance for ALL array keys containing `[`. Didn't distinguish constant indices (compiler knows which element) from variable indices (can't distinguish at compile time).

**Fix:** In `prov_map_set`, check if bracket content is all digits. Constant index → skip root homogeneity check (per-element provenance tracked individually). Variable index → enforce homogeneity (can't know which element at compile time).

**Found by:** Auditing `rt_opaque_multi_dispatch.zer` which had been rewritten to use separate `op1`, `op2` variables instead of an `Op[2]` array.

### `?*T[N]` Parser Precedence (BUG-467, 2026-04-08)

`?*Device[2] slots;` produced `struct Device[2]* slots` in emitted C — parsed as `OPTIONAL(POINTER(ARRAY(Device,2)))` instead of `ARRAY(OPTIONAL(POINTER(Device)),2)`.

**Root cause:** Nested `parse_type` calls consumed `[N]` inside the `*T` handler before the `?` handler could swap. Flow: `?` → `parse_type` → `*` → `parse_type` → base type `Device` + `[2]` → `ARRAY(Device,2)` → `*` wraps as `POINTER(ARRAY)` → `?` wraps as `OPTIONAL(POINTER(ARRAY))`.

**Fix:** In `?` handler, after getting inner type, check for `POINTER(ARRAY(...))` pattern and restructure: unwrap pointer, extract array, rewrap as `ARRAY(OPTIONAL(POINTER(elem)),N)`. Same swap pattern as BUG-413 (`?Handle(T)[N]`) but through the pointer wrapper. Also handles `?const *T[N]` and `?volatile *T[N]`.

**Found by:** Auditing `rt_opaque_array_homogeneous.zer` which used struct fields (`slot0`, `slot1`) instead of `?*Device[2]` array syntax.

### Move Struct — Compile-Time Ownership Transfer (2026-04-09)

`move struct FileHandle { i32 fd; }` — passing to function or assigning transfers ownership. Use after transfer = compile error. Closes the gap for stack-allocated value types representing unique resources (file descriptors, hardware handles, DMA buffers, one-shot tokens).

**Parser:** Contextual keyword `move` (4 chars, not reserved). Scanner save/restore to peek: if `move` followed by `struct`, parse as move struct. Otherwise restore and continue as ident.

**AST/Types:** `bool is_move` on `struct_decl` (ast.h) and `struct_type` (types.h). Checker propagates in `register_decl`.

**zercheck:** Reuses existing `HS_TRANSFERRED` state (originally for spawn). Key design decisions:
- `pool_id = -3` distinguishes move structs from pool handles (-1), malloc (-2), params (-1)
- **Lazy registration:** Move struct vars registered at first use via `checker_get_type()` on NODE_IDENT, not at var-decl time (checker scopes are gone when zercheck runs — typemap is the only reliable source)
- **Two-pass transfer in NODE_CALL:** First pass checks UAF + use-after-transfer. Second pass marks args as TRANSFERRED. This prevents false positives where the arg NODE_IDENT check fires on the same line as the transfer.
- **Var-decl transfer after zc_check_expr:** `Token b = a;` — transfer of `a` happens AFTER `zc_check_expr(init)` recurses into `a`. Otherwise NODE_IDENT sees `a` as already-transferred (false positive).
- **Excluded from leak detection:** `pool_id == -3` skipped in leak check. Move structs are stack values — auto-cleaned, no free needed.
- **HS_TRANSFERRED counted as "covered"** in alloc_id grouping — prevents false leak errors.

**Design choice: copy-by-default, move opt-in.** Opposite of Rust. ZER copies all types by default. `move struct` opts into ownership tracking for types where copying would be a bug (double-close, double-free, hardware aliasing). This means MOST structs don't need move — only the ~5% that wrap unique resources.

**Why not add other Rust features:**
- Closures: would need captured environment + 3 closure kinds + lifetime interaction → O(N²) coupling
- Generics: `container` keyword (v0.4) covers it via text substitution → O(1) per stamp
- Traits: function pointer vtable + `*opaque` already covers dispatch → 0 new features
- Destructuring: `|v|` capture + `v.field` does the same thing in 1 extra line → not worth compiler complexity
- Lifetimes: that IS the borrow checker → never add

## C-to-ZER Conversion Tools (moved from CLAUDE.md)

### C-to-ZER Conversion Tools (implemented v0.2)

Two tools + one library for automated C-to-ZER migration:

**`tools/zer-convert.c`** — Phase 1: C syntax → ZER syntax (token-level transform)
- Types: `int`→`i32`, `unsigned int`→`u32`, `char`→`u8`, `size_t`→`usize`
- Operators: `i++`→`i += 1`, `NULL`→`null`, `->`→`.`
- Memory: `malloc`→`zer_malloc_bytes`, `free`→`zer_free`
- Strings: `strlen`→`zer_strlen`, `strcmp`→`zer_strcmp`, `memcpy`→`zer_memcpy`
- Casts: `(Type *)expr`→`@ptrcast(*Type, expr)`, `(int)x`→`@truncate(i32, x)`, `(uintptr_t)ptr`→`@ptrtoint(ptr)`
- MMIO casts: `(uint32_t*)0x40020000`→`@inttoptr(*u32, 0x40020000)`, `(volatile uint32_t*)0xADDR`→`@inttoptr(*u32, 0xADDR)`
- sizeof: `sizeof(Type *)`→`@size(*Type)`, inside cast args too
- Qualifiers: `volatile` preserved and reordered (`volatile uint32_t *`→`volatile *u32`), `extern`/`inline`/`restrict`/`register`/`__extension__` stripped
- Preprocessor: `#include`→`cinclude`, `#define N 42`→`const u32 N = 42;`
- Struct: `struct Node` in usage→`Node` (keeps `struct` in declarations)
- Enum: `enum State` in usage→`State` (keeps `enum` in declarations)
- void*: `void *`→`*opaque`, `void **`→`**opaque`, `(void *)expr`→`@ptrcast(*opaque, expr)`
- Arrays: `int arr[10]`→`i32[10] arr` (ZER size-before-name reorder)
- switch: `case VAL: ... break;`→`.VAL => { ... }`, `default:`→`default => {`
- do-while: `do { body } while(cond);`→`while (true) { body if (!(cond)) { break; } }`
- typedef struct: `typedef struct { ... } Name;`→`struct Name { ... }`
- Tag mapping: `typedef struct node { ... } Node;` records `node`→`Node`, applies in body and usage
- I/O functions: `printf`, `fprintf` etc. stay as-is (used via cinclude, not compat)
- Auto-imports `compat.zer` when unsafe patterns detected

**zer-convert architecture notes for fresh sessions:**
- `map_type()` covers BOTH the type_map table AND standalone keywords (`int`, `long`, `short`, `unsigned`). Without this, `(int)x` cast detection fails — the cast handler calls `map_type` to check if a token is a type.
- `try_reorder_array(i)` called after every type emission to detect C-style `type name[N]` and reorder to ZER's `type[N] name`.
- `in_switch_arm` static flag tracks open switch arms. When `case` or `default` is encountered while an arm is open, auto-closes with `}`. When `break;` is encountered inside a switch arm, emits `}` instead.
- `tag_maps[]` records typedef struct tag→name mappings. `lookup_tag()` resolves bare tag names in body and usage. Both `struct tag_name` usage (via struct handler) and bare `tag_name` usage (via identifier fallthrough) are resolved.
- `typedef struct` handler neutralizes the post-`}` typedef name and `;` tokens, then jumps `i` to `{` so the normal transform loop handles body contents (enabling type transforms inside struct bodies).
- do-while body is emitted with inline transforms (++, --, ->, NULL, type mapping) since the token-level approach can't easily recurse the full transform on a sub-range.
- Number suffixes: C suffixes (U, L, UL, ULL, u, l) stripped from numeric literals during emission
- Include guards: `#ifndef FOO_H / #define FOO_H` pattern detected and stripped (ZER uses import)
- Stringify/token-paste macros (`#`, `##`): detected in macro body, emit as `// MANUAL:` comment instead of invalid comptime
- Variadic macros (`...`, `__VA_ARGS__`): detected in params/body, emit as `// MANUAL:` comment
- Types: `uintptr_t`→`usize`, `intptr_t`→`usize` added to type_map
- Keyword stripping (`extern`, `inline`, `restrict`, `register`, `__extension__`, `__inline__`, `__restrict__`) is early in the main loop — before type mapping. Just skips the token.
- `volatile` handling is also before type mapping. Detects `volatile TYPE *` → reorders to `volatile *TYPE`. For non-pointer `volatile TYPE` → emits `volatile TYPE`.
- MMIO detection in cast handler: after recognizing `(TYPE*)` or `(volatile TYPE*)`, peeks at the operand. If numeric literal → emits `@inttoptr` instead of `@ptrcast`. Works for both direct `0x40020000` and parenthesized `(0x40020000)`.
- `(uintptr_t)` cast uses `use_ptrtoint` flag — emits `@ptrtoint(expr)` instead of `@truncate(usize, expr)`. Requires `uintptr_t` in type_map (len=9, not 10!).
- Include guard detection runs before `#ifndef` handler. Peeks ahead for `#define SAME_NAME` with empty body on next line. If found, emits comment and skips both lines. Otherwise falls through to normal `#ifndef` → `comptime if`.
- Stringify/variadic detection runs before comptime emission. Scans past params to `)`, then scans body for `CT_HASH` or `__VA_ARGS__`. Also scans params for `...` (three `CT_DOT` tokens). If found, auto-extracts the `#define` line to companion `_extract.h` file (same mechanism as ternary/goto/bitfields). Sets `needs_extract = true`, writes macro via `extract_str`/`extract_tok`, handles line continuation (`\\`). The `.zer` file gets a `// extracted to .h:` comment. Zero manual work — macro works through GCC via cinclude.
- `emit_tok()` strips C number suffixes (U/L/UL/ULL) from `CT_NUMBER` tokens by trimming trailing u/U/l/L chars before writing.
- 139 regression tests in `tests/test_convert.sh`, integrated into `make check`.

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
- **`scan_handle_params` must NOT reset `handle_param_count`** — `scan_local_slab_vars` runs first and adds entries. Resetting wipes them. This was the root cause of bare references (`a` instead of `a_h`) not being rewritten in `main()`.
- **String literal skipping:** `upgrade()` must skip content inside `"..."` to prevent replacing identifiers inside strings. `"a=%d"` was becoming `"a_h=%d"` without this.
- **Double `_h` prevention:** bare reference handler must detect var declarations (`Type *name = expr`) and skip them — `rewrite_signatures` already adds `_h` to declarations. The handler checks: if preceded by `*` and followed by `=`, it's a declaration → emit as-is.
- **Local slab var tracking:** `scan_local_slab_vars()` finds `SlabType *var = expr` in function bodies (e.g., `Task *a = task_create()`). Registers as handle variable with function bounds. Combined with `scan_handle_params` (function params) and `scan_allocs` (malloc sites), covers all three sources of handle variables.
- **Struct field rewriting:** `rewrite_signatures()` also detects `SlabType *field;` in struct definitions and rewrites to `?Handle(SlabType) field;`. Handles linked list `next`/`prev` and parent pointers.
- **Local var declaration rewriting:** `rewrite_signatures()` detects `SlabType *var = expr;` and rewrites to `?Handle(SlabType) var_h = expr;`.
- **Primitive type exclusion:** `is_primitive_type()` checks if a type name is a primitive (`u8`, `i32`, etc.). `scan_allocs` skips primitives — raw byte malloc (e.g., `u8 *buf = malloc(100)`) stays as compat, not Slab. The check is in BOTH `scan_allocs` (pre-scan) AND `upgrade()` (line-based replacement) — the line-based path also calls `get_slab_name()` which creates slab types if not guarded.

**`lib/compat.zer`** — Scaffolding library (NOT part of ZER). Wraps C stdlib via `cinclude`. Tagged `zer_` prefix for Phase 2 detection. Removed after full upgrade.

**Pipeline:** `input.c → zer-convert → input.zer → zer-upgrade → input_safe.zer`

**Multi-file conversion and cinclude → import migration:**
- Each `.c` file converts independently. Types are shared via `cinclude "header.h"` (passes through to GCC).
- `cinclude` is safe scaffolding — GCC handles type resolution across C headers. Converted `.zer` files compile correctly with `cinclude`.
- For **fully safe ZER** (Handle-based, bounds-checked across modules): replace `cinclude "module.h"` with `import module;`. This requires converting the header into a `.zer` module file with ZER type signatures (e.g., `?Handle(Connection)` instead of `Connection *`).
- Both approaches are valid: `cinclude` for mixed C/ZER projects (incremental migration), `import` for pure ZER projects (full safety).
- The converter does NOT auto-convert `cinclude` → `import` — this is a manual step when the user is ready to make the module fully ZER.

**zer-upgrade additional rules (post-agent-audit):**
- `zer_memset(var, 0, sizeof(T))` and `bytes_zero(var)` after Slab alloc are auto-removed (Slab uses calloc, already zeroed). Loops up to 3 consecutive redundant lines (null check + memset + bytes_zero).
- `zer_strcpy(dst, src)` → `@cstr(dst, src)`, `zer_strncpy(dst, src, n)` → `@cstr(dst, src)`.
- Bare `zer_strcmp`/`zer_strncmp`/`zer_memcmp` without `== 0`/`!= 0` → `bytes_compare()` (returns `i32`, same as C's strcmp). With `== 0` → `bytes_equal()` (returns `bool`). Both are from `str.zer`.
- Comments (`//` and block comments) skipped — `zer_` names inside comments preserved.
- `== 0` stripping uses exact advancement (past `==` + spaces + `0`) instead of greedy char loop. Preserves space before `&&`/`||`.
- `out_write_with_handles()` routes ALL Layer 1 replacement args through Layer 2 handle rewriting. Fixes `@cstr(c.host, ...)` → `@cstr(slab.get(c_h).host, ...)`. Applied to all compat function replacements (strlen, strcmp, memcpy, memset, strcpy, strncpy, memcmp).

**zer-convert usage scanner (classify_params):**
- Pre-scan pass runs after tokenizing, before transform. Classifies `char *` / `const char *` params.
- Detects string usage: `strlen(param)`, `strcmp(param, ...)`, `param[i]` indexing, passed to string functions.
- Detects nullable: `param == NULL`, `param = NULL`, `if (param)`, `if (!param)`.
- `const char *` without counter-evidence defaults to `const []u8` (convention: ~99.9% correct).
- Non-const `char *` with string usage → `[]u8`, with null checks → `?[]u8` or `?*u8`.
- Write-through pattern (`*p = val`) stays as `*u8`.
- Ambiguous (no usage clues, non-const) stays as `*u8` + compat (safe fallback, compiles).
- Nested switch uses depth counter array (16 levels max) instead of single boolean.

**Pointer arithmetic conversion:**
- `ptr + N` → `ptr[N..]` (sub-slice from offset) for classified slice params.
- `*(ptr + N)` → `ptr[N]` (index at offset) via lookahead at `*` token — detects `*(IDENT + expr)` where IDENT is a classified slice.
- Only fires for params classified as slices by `classify_params`. Non-classified `*u8` pointers pass through — zerc catches invalid arithmetic with clear error.

**sizeof(variable) vs sizeof(type):**
- `sizeof(Type)` → `@size(Type)` when arg is a mapped C type or starts with uppercase.
- `sizeof(var)` → kept as `sizeof(var)` when arg starts with lowercase (likely a variable). GCC resolves via `cinclude`.

**Reassignment malloc:**
- `var = @ptrcast(*Type, zer_malloc_bytes(...))` (no `Type *` prefix) now detected and converted to `var_h = slab.alloc() orelse return;` (single line, no intermediate `_maybe`).
- Declaration check for double `_h` prevention fires for BOTH `ai` and `hp` variables (was `hp` only — caused `t_h_h` on declarations when `t` was also an alloc target from reassignment).

**Pointer declaration rearrangement:**
- `int *ptr` → `*i32 ptr`, `int **pp` → `**i32 pp` (multi-level).
- `try_ptr_rearrange()` called after every type emission. Detects declaration context: name followed by `=`, `;`, `,`, `)`, `[`, `(`.
- Works for all mapped types (uint8_t, float, etc.), combos (unsigned int), and standalone keywords (int, long, short).
- Return type pointers: `int *func()` → `*i32 func()`.

**Auto-extraction to .h via cinclude (unconvertible C constructs):**
- `scan_for_extractions()` pre-scan detects functions/structs that can't be expressed in ZER.
- Ternary (`? :`), goto/labels, inline asm (`__asm__`): entire function extracted to companion `_extract.h`.
- Bit fields (`: N` in struct): entire struct extracted to `_extract.h`.
- Ambiguous `char *` with zero usage clues: function extracted to `_extract.h`.
- `.zer` file gets `cinclude "name_extract.h";` + function declarations with ZER type mapping.
- Zero `// MANUAL:` comments needed. Everything compiles via GCC.

**Preprocessor → comptime conversion (zero // MANUAL:):**
- `#define NAME(params) expr` → `comptime u32 NAME(u32 param, ...) { return expr; }` — function-like macros become comptime functions. All params typed as `u32` (user refines types manually if needed). Line continuation (`\`) handled.
- `#define NAME expr` (non-numeric) → `comptime u32 NAME() { return expr; }` — expression macros become zero-arg comptime functions.
- `#define GUARD` (empty) → `const bool GUARD = true;` — guard/flag macros become bool constants.
- `#define NAME 42` (numeric) → `const u32 NAME = 42;` — unchanged from before.
- `#ifdef NAME` → `comptime if (NAME) {`
- `#ifndef NAME` → `comptime if (!NAME) {`
- `#if expr` → `comptime if (expr) {`
- `#elif expr` → `} else { comptime if (expr) {`
- `#else` → `} else {`
- `#endif` → `}`
- `#pragma`, `#error`, `#warning`, `#line` → `// #pragma ...` (comment, harmless)

**Remaining design limitations:**
- `cinclude` → `import` migration — manual step (one line per file). Required for full ZER safety across modules.
- Comptime function-like macros default all params to `u32` — user must refine types for non-integer macros.
- `#elif` emits nested `} else { comptime if (...) {` which may need extra `}` at `#endif` — manual fixup for complex multi-branch `#if/#elif/#else/#endif` chains.
- 108 regression tests covering all conversion patterns.

### Red Team Audit Fixes (BUG-314 through BUG-318)

**170. Recursive struct/union via distinct unwrapped.** `type_unwrap_distinct(inner)` before self-reference check. (BUG-314)

**171. Array return via distinct unwrapped.** `type_unwrap_distinct(ret)` before `TYPE_ARRAY` rejection. (BUG-315)

**172. Intrinsic named types: `force_type_arg` for @bitcast/@truncate/@saturate.** Not just @cast. (BUG-316)

**173. keep validation: mangled key fallback.** Try `module_name` when raw lookup fails for imported globals. (BUG-317)

**174. Constant folder shift UB.** `(uint64_t)l << r` prevents signed overflow in the compiler. (BUG-318)

### Red Team Audit Fixes (BUG-319 through BUG-322)

**175. Volatile orelse var-decl: `__typeof__` instead of `__auto_type`.** GCC's `__auto_type` drops volatile. `__typeof__(expr) tmp = expr` preserves it. (BUG-319)

**176. Array copy source volatile check.** `arr_volatile` now checks BOTH `expr_is_volatile(target)` AND `expr_is_volatile(value)`. Source cast uses `const volatile uint8_t*`. Same fix in var-decl init path. (BUG-320)

**177. Volatile in mutable capture `|*v|`.** Emit `volatile` prefix on capture pointer when source expression is volatile. Was only propagating `const`, not `volatile`. (BUG-321)

**178. `__typeof__` in ALL capture declarations.** Three sites in if-unwrap replaced `__auto_type` with `__typeof__()`: mutable `|*v|` pointer, null-sentinel `|v|` copy, struct optional `|v|` value extraction. Preserves both volatile and const qualifiers. (BUG-322)

### Red Team Audit Fixes (BUG-325, BUG-326)

**179. @bitcast struct width validation.** `type_width()` returns 0 for structs — falls back to `compute_type_size() * 8`. Prevents mismatched-size struct bitcasts. (BUG-325)

**180. Switch capture const safety.** Walk switch expression to root ident, check `is_const`. Mutable capture `|*v|` on const source now rejected. Applies to both union and optional switch paths. (BUG-326)

**181. Module name mangling uses double underscore `__` separator.** `module__name` instead of `module_name`. Prevents collisions when module names contain underscores (`mod_a` + `b_c` vs `mod_a_b` + `c`). All 8 sites updated: checker (3 registrations + 1 lookup), emitter (`emit_user_name`, `EMIT_MANGLED_NAME`, NODE_IDENT primary + fallback, global var). (BUG-332)


## Emitter Patterns, Bug Fixes, and Refactors (moved from CLAUDE.md)

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
- **ABA mitigation:** gen counter wraps via uint32_t overflow: `gen[idx]++; if (gen[idx] == 0) gen[idx] = 1;` (skip 0 — reserved for null handle). After ~4 billion alloc/free cycles per slot, gen wraps back to 1, creating a theoretical ABA window. Acceptable for ZER's target (firmware/embedded — 4B cycles at 1MHz = ~71 minutes continuous alloc/free on one slot). No slot retirement, no permanent loss. Applied to both Pool and Slab.
- **Zero-handle safety:** Alloc bumps gen from 0 to 1 on first use: `if (gen[i] == 0) gen[i] = 1`. Zero-initialized `Handle(T) h;` has gen=0, which never matches any valid allocation (all start at gen≥1). Prevents silent access via uninitialized handles.

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

**RF13: `eval_const_expr_ex` with callback-based ident resolution (2026-04-06).**
`eval_const_expr_scoped` in checker.c duplicated all binary/unary math from `eval_const_expr` in ast.h (~60 lines). Refactored: `eval_const_expr_ex(Node *n, int depth, ConstIdentResolver resolve, void *ctx)` in ast.h takes an optional function pointer callback. When `NODE_IDENT` is encountered and `resolve != NULL`, calls `resolve(ctx, name, len)` to look up the value. Checker provides `resolve_const_ident` which walks scope chain for const symbols. `eval_const_expr_d` delegates to `eval_const_expr_ex(n, depth, NULL, NULL)`. `eval_const_expr_scoped` is now 2 lines: `return eval_const_expr_ex(n, 0, resolve_const_ident, c)`. Zero code duplication, no circular includes. Kernel-style callback pattern.

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

### Known Technical Debt (resolved)
- **Global Compiler State:** `non_storable_nodes` moved into Checker struct (BUG-346/RF12). `type_map` moved in RF1. All compiler state is now per-instance — thread-safe for LSP.
- **Static vars in imported modules:** Fixed in BUG-222/229/233. All imported symbols (static and non-static) register under mangled keys. Cross-module same-named symbols work correctly. No qualified call syntax yet (unqualified calls resolve to last import).


### Intrinsic Flag Propagation (BUG-338)
`is_local_derived` and `is_arena_derived` flags must propagate THROUGH intrinsics. `*opaque p = @ptrcast(*opaque, &x)` — the init root walk must enter NODE_INTRINSIC (take last arg) and NODE_UNARY(&) (take operand) to reach the actual `x` symbol. Two sites in checker.c NODE_VAR_DECL: (1) alias propagation loop at ~line 2880, (2) &local detection at ~line 2906. Both now walk into intrinsics and & unary.

### keep Orelse Fallback Check (BUG-339)
Call-site keep validation must unwrap NODE_ORELSE before checking for &local. `reg(opt orelse &x)` — the orelse fallback provides a local address. Fix: split arg into `keep_checks[2]` (expr + fallback for orelse, or just arg itself). Each check also walks into intrinsics before looking for NODE_UNARY(&).

### Union Variant Assignment Single-Eval (BUG-340)
`msg.sensor = val` emits tag update + value assignment. If `msg` is a function call, the old comma expression `(get_msg()._tag = 0, get_msg().sensor = val)` evaluated it twice. Fix: hoist into pointer temp: `({ __typeof__(obj) *_zer_up = &(obj); _zer_up->_tag = N; _zer_up->variant = val; })`. Single evaluation via `&(obj)`.

### @bitcast Volatile Check (BUG-341)
Same pattern as @ptrcast volatile check (BUG-258). In the @bitcast handler, after width validation, check if source is a volatile pointer and target is not. Checks both type-level `pointer.is_volatile` and symbol-level `is_volatile`. Prevents GCC from optimizing away hardware register writes through the bitcasted pointer.

### @cast Volatile/Const Check (BUG-343)
Same pattern as @ptrcast (BUG-258) and @bitcast (BUG-341), applied to @cast (distinct typedef conversion). After the type_equals validation, unwrap distinct on both source and result types. If both are pointers, check volatile (type-level + symbol-level) and const (`pointer.is_const`). Prevents `distinct typedef *u32 SafePtr; @cast(SafePtr, volatile_reg)` from silently stripping volatile.

### compute_type_size Overflow Guard (BUG-344)
`elem_size * (int64_t)t->array.size` could wrap to a small positive via -fwrapv for massive multi-dim arrays. Guard: `if (count > 0 && elem_size > INT64_MAX / count) return CONST_EVAL_FAIL`. Falls back to emitter's `sizeof()` which GCC handles correctly.

### Handle(T) Always u32 (BUG-345 — spec fix)
Handle is always `uint32_t` (16-bit index + 16-bit generation). Max 65,535 slots per Pool/Slab. The spec (ZER-LANG.md) previously claimed platform-width handles, but the entire Pool/Slab runtime hardcodes `uint32_t` and `0xFFFF` masks. Spec updated to match implementation. Future 64-bit handle support would require runtime rewrite.

### non_storable_nodes Moved to Checker Struct (BUG-346, RF12)
`non_storable_nodes`, `non_storable_count`, `non_storable_capacity` moved from static globals into Checker struct. All helpers (`non_storable_init`, `mark_non_storable`, `is_non_storable`) now take `Checker *c`. Arena pointer uses `c->arena`. Eliminates last known static global state — compiler is now thread-safe for LSP concurrent requests.

### Ring Memory Barriers (BUG-348)
Ring push emits `__atomic_thread_fence(__ATOMIC_RELEASE)` between data write (`memcpy`) and head pointer update. Ring pop emits `__atomic_thread_fence(__ATOMIC_ACQUIRE)` after data read and before tail pointer update. This fulfills the spec promise "Ring handles barriers INTERNALLY" — data is guaranteed visible before the pointer update on out-of-order processors (ARM Cortex-A, modern x86, RISC-V). Without these, an interrupt handler reading head/tail could see stale data.

### Topological Registration Order (BUG-349)
Module registration must use topological order (dependencies before dependents), not BFS discovery order. `register_decl` for structs resolves field types immediately via `resolve_type` — if a dependency module's types aren't registered yet, field types resolve to `ty_void`. The topo sort is computed once in `zerc_main.c` and reused for: (1) registration, (2) body checking, (3) emission. Eliminates the old duplicate topo sort for emission.

### Array Alignment in compute_type_size (BUG-350)
Array member alignment = element type alignment, NOT total array size. `u8[10]` has alignment 1, not 8. For multi-dim arrays, recurse to innermost element. Struct member alignment = max field alignment. The generic formula `min(fsize, 8)` only applies to scalar types. Without this fix, `@size` over-estimates padding for structs containing arrays, causing binary layout mismatch with C (broke C interop via cinclude).

### mmio Range Registry (Safe @inttoptr)
New top-level declaration: `mmio 0x40020000..0x40020FFF;`. Stores address ranges in Checker struct (`mmio_ranges` array of `[start, end]` pairs). When mmio ranges are declared:
- **Constant addresses** in `@inttoptr` validated at compile time — must fall within at least one range. Outside all ranges → compile error.
- **Variable addresses** in `@inttoptr` get runtime range check in emitter: `if (!(addr >= range1_start && addr <= range1_end) && ...) _zer_trap(...)`. Emitted as GCC statement expression with hoisted temp for single-eval.
- **No mmio declarations + @inttoptr** → compile error "mmio range declarations required". Strict by default — no existing ZER code to break. Opt-out: `--no-strict-mmio` flag on zerc (sets `checker.no_strict_mmio = true`, skips the mandatory check). For tests: `mmio 0x0..0xFFFFFFFFFFFFFFFF;` allows all addresses without needing the flag.
- Lexer: `TOK_MMIO`. Parser: `mmio` → consume INT → consume `..` → consume INT → `;`. AST: `NODE_MMIO` with `range_start`/`range_end` (uint64_t). Emitter: emits as comment `/* mmio 0x...–0x... */`.

### @ptrcast Type Provenance Tracking
Symbol gains `provenance_type` field — tracks the original Type before a `@ptrcast` to `*opaque`. Set when:
1. `*opaque ctx = @ptrcast(*opaque, sensor_ptr)` → provenance = type of sensor_ptr
2. Alias propagation: `*opaque q = p` where p has provenance → q inherits
3. Clear + re-derive on assignment (same pattern as `is_local_derived`)

Checked in `@ptrcast` handler: when casting FROM `*opaque` TO `*T`, if source symbol has `provenance_type`, unwrap both and compare inner types. Mismatch → compile error "source has provenance X but target is Y". Unknown provenance (params, cinclude) → allowed (can't prove wrong).

### @container Field Validation + Provenance
Two new checks in the `@container` handler:
1. **Field exists** — resolves type_arg to struct type, looks up field name in fields. Missing field → compile error. This was previously unchecked (GCC caught it via `offsetof` in emitted C).
2. **Provenance** — Symbol gains `container_struct`, `container_field`, `container_field_len`. Set when `*T ptr = &struct.field` (NODE_UNARY(AMP) → NODE_FIELD, walk to struct type). In `@container`, if source has provenance: target struct must match `container_struct` (pointer identity), field must match `container_field`. Mismatch → compile error. Unknown provenance → allowed.

Both provenance systems propagate through aliases and clear+re-derive on assignment, following the same pattern as `is_local_derived`/`is_arena_derived`.

### Comptime Functions (compile-time evaluation)

**Keyword:** `TOK_COMPTIME` in lexer. Parser handles `comptime` prefix before `parse_func_or_var()` — sets `func_decl.is_comptime = true`. Symbol gets `is_comptime = true` during `register_decl`.

**Evaluation mechanism:** NOT a full interpreter. Uses inline expansion + constant folding:
1. At call site (NODE_CALL), checker detects `callee_sym->is_comptime`
2. All args verified as compile-time constants via `eval_const_expr()`
3. `eval_comptime_block()` walks the function body with parameter substitution:
   - `eval_const_expr_subst()` — like `eval_const_expr` but replaces NODE_IDENT matching param names with mapped int64_t values
   - NODE_RETURN → evaluate return expression with substitution
   - NODE_IF → evaluate condition, recurse into taken branch only
   - NODE_BLOCK → walk statements sequentially, return first result
4. Result stored on `node->call.comptime_value` + `node->call.is_comptime_resolved = true`

**Emitter:** `emit_top_level_decl` skips comptime functions entirely (no C output). `emit_expr(NODE_CALL)` checks `is_comptime_resolved` and emits the constant value directly as `%lld` literal.

**Extended eval_const_expr (ast.h):** Added comparison operators (`> < >= <= == !=`), logical (`&& ||`), XOR (`^`), bitwise NOT (`~`), logical NOT (`!`). These support comptime function bodies with if/else branching.

**Nested calls:** Comptime functions can call other comptime functions — `QUAD(x) { return DOUBLE(DOUBLE(x)); }` works. BUG-425 fixed the checker rejecting params as non-constant during body type-checking (`in_comptime_body` flag). Array sizes also work: `u8[QUAD(2)]` (BUG-391/423 fixed `resolve_type_inner` to call `eval_comptime_block`).

### Comptime If (conditional compilation)

**Syntax:** `comptime if (CONST) { ... } else { ... }` — parsed as regular NODE_IF with `if_stmt.is_comptime = true`. Parser detects `TOK_COMPTIME` followed by `TOK_IF` at statement level.

**Checker:** Evaluates condition via `eval_const_expr()`. Only the taken branch is type-checked. Dead branch is completely ignored — can contain undefined types/functions without error. This matches C `#ifdef` behavior where the dead branch is never compiled.

**Emitter:** Checks `if_stmt.is_comptime`, evaluates condition, only emits the taken branch. Dead branch produces zero C output.

**Use case:** Platform-specific code:
```zer
comptime if (ARM) {
    mmio 0x40020000..0x40020FFF;
    volatile *u32 reg = @inttoptr(*u32, 0x40020014);
}
```

### @cast Added to All Escape Check Sites (BUG-351)
The return escape and orelse fallback escape checks walked into `@ptrcast` and `@bitcast` intrinsics to find `&local` patterns, but missed `@cast`. Since `@cast` converts between distinct typedefs (which can wrap pointer types), `return @cast(SafePtr, &x)` bypassed escape analysis. Fix: added `(ilen == 4 && memcmp(iname, "cast", 4) == 0)` to both sites (return check ~line 3930 and orelse fallback check ~line 4006).

### Union Switch Rvalue Volatile (BUG-352)
The rvalue union switch temp at emitter.c line 2455 used `__auto_type` which strips volatile. Changed to `__typeof__(expr)` (same fix pattern as BUG-319/322 for captures and orelse). Only fires when switch expression is NODE_CALL (rvalue). Lvalue path already used `__typeof__` correctly.

### all_paths_return Respects comptime if (BUG-354)
`all_paths_return(NODE_IF)` now checks `is_comptime`. If true, evaluates the condition via `eval_const_expr` and only requires a return from the taken branch. `comptime if (1) { return 42; }` without else now passes return analysis — the dead branch is irrelevant.

### Assignment Escape Walks Through Intrinsics (BUG-355)
The BUG-205 assignment escape check (`g_ptr = local_derived_ptr`) only fired when the value was direct `NODE_IDENT`. Now walks through `NODE_INTRINSIC` chain to find the root ident: `while (vnode->kind == NODE_INTRINSIC) vnode = vnode->intrinsic.args[last]`. Catches `g_ptr = @ptrcast(*u32, p)` and `g_ptr = @cast(GPtr, p)` where `p` is local-derived.

### Backward Goto UAF Detection (2026-04-06)

Backward goto is now tracked by zercheck. In NODE_BLOCK, labels are scanned with their statement indices. When NODE_GOTO targets a label at an earlier index, zercheck re-walks from label to goto with the current PathState. If any handle state changed → widen to MAYBE_FREED (same pattern as for/while loop 2-pass). This closes the previously-documented "zercheck is linear" gap.

**Limitation:** Only detects backward gotos within the same block (covers ~99% of cases). Cross-block backward jumps (e.g., goto from inside an if-body to a label in the parent block) are NOT detected. Runtime gen counter handles those.

### Auto-Guard for if Conditions (BUG-439, 2026-04-06)

`emit_auto_guards` now called for NODE_IF conditions (both regular and if-unwrap). NOT added for while/for conditions because:
- Loop conditions are re-evaluated every iteration — auto-guard before the loop only checks the initial value
- OOB condition data causes wrong-branch execution — trap is the correct behavior (stop immediately, don't make decisions on garbage)
- Inline `_zer_bounds_check` handles loop conditions correctly at every iteration

### Known Technical Debt (updated)
- **~~No qualified module call syntax~~** — RESOLVED (BUG-416 session). `config.func()` now works via mangled lookup rewrite.
- **~~Comptime in array sizes~~** — RESOLVED (BUG-391/423). `u8[BIT(3)]` works via `eval_comptime_block` in `resolve_type_inner`. Nested calls like `u8[QUAD(2)]` also work (BUG-425).
- **~~Backward goto UAF~~** — RESOLVED (2026-04-06). zercheck re-walks backward goto ranges. Only same-block gotos tracked.
- **zercheck variable-index handles:** `arr[i]` with variable index still untrackable — falls back to runtime generation counter traps. Constant indices (`arr[0]`, `s.h`) now tracked via compound string keys (BUG-357 fix).
- **Dual symbol registration for imported globals:** Imported non-static globals/functions are registered TWICE in global scope — raw name + mangled name (BUG-233). This is intentional (emitter needs both), but any code that scans global scope for unique matches (like `find_unique_allocator`) must handle duplicates. Pattern: check `found->type == candidate->type` before declaring ambiguity.

### @critical Control Flow Ban (BUG-436, 2026-04-06)

`return`, `break`, `continue`, and `goto` are banned inside `@critical` blocks. Jumping out skips the interrupt re-enable code (emitted after the body), leaving the system with interrupts permanently disabled. Same pattern as `defer` control flow ban (BUG-192) using `critical_depth` counter on Checker struct.

**When adding new control flow nodes:** check BOTH `defer_depth > 0` AND `critical_depth > 0`.

### emit_auto_guards Walker Completeness (BUG-433, 2026-04-06)

`emit_auto_guards` must recurse into ALL expression node types that can contain NODE_INDEX children. Missing node types silently skip auto-guards (graceful return not emitted, hard trap fires instead).

**Nodes handled:** NODE_INDEX, NODE_FIELD, NODE_ASSIGN, NODE_BINARY, NODE_UNARY, NODE_CALL, NODE_ORELSE (expr + value fallback), NODE_INTRINSIC (all args), NODE_SLICE (object/start/end).

**When adding new expression nodes:** add a case to `emit_auto_guards` that recurses into all child expressions.

### NODE_CRITICAL in AST Walkers (BUG-434/435/437, 2026-04-06)

NODE_CRITICAL must be handled in ALL recursive AST walkers:
- `contains_break` — recurse into body (break inside @critical targets outer loop)
- `all_paths_return` — recurse into body (return inside @critical counts)
- `zc_check_stmt` (zercheck) — recurse into body (handle ops inside @critical must be tracked)
- `block_always_exits` (zercheck) — recurse into body
- `collect_labels` — already handled
- `validate_gotos` — already handled

**When adding new AST walkers that recurse through statements:** add NODE_CRITICAL case.

### Distinct Union Variant Assignment (BUG-438, 2026-04-06)

`distinct typedef union Msg SafeMsg` — variant assignment `sm.sensor = 42` must update `_tag`. The emitter's NODE_ASSIGN handler checks `obj_type->kind == TYPE_UNION`, but `checker_get_type` returns TYPE_DISTINCT wrapping TYPE_UNION. Fix: `type_unwrap_distinct(obj_type)` before the check. **Same pattern as BUG-409/410 (35+ sites fixed), but this one was missed.**

### Deref Walk in Flag Propagation (BUG-356)
The is_local_derived/is_arena_derived propagation walk now handles `NODE_UNARY(TOK_STAR)` — pointer dereference. `*u32 p2 = *pp` where `pp` is a double pointer to a local-derived pointer — the walk goes through the deref to find `pp`, checks its flags, propagates to `p2`. Without this, double pointers "washed" the safety flag. Same walk location as BUG-338 (intrinsic args) at ~line 3232.

### Provenance Propagation Through All Intrinsics (BUG-358)
Provenance alias propagation in NODE_VAR_DECL now walks through ALL intrinsics (not just @ptrcast) to find the root ident and copy its provenance_type and container_struct/field. `*opaque q = @bitcast(*opaque, ctx)` preserves `ctx`'s provenance. Uses same `while (prov_root->kind == NODE_INTRINSIC) walk` pattern as BUG-355 (assignment escape).

### Identity Washing — Call-Site Escape Check (BUG-360)
`return func(&local)` where func returns a pointer — conservatively rejected. In NODE_RETURN, if return expr is NODE_CALL with pointer return type, scan all arguments for `&local` patterns and local-derived idents. Same check added in NODE_VAR_DECL init: `*u32 p = func(&x)` where `x` is local marks `p` as local-derived. This is an overapproximation (safe functions that don't return their args get false positives) but prevents identity-washing escape. `keep` parameters are the programmer's opt-in for "I know this function stores but doesn't return the pointer."

### zercheck Assignment Handle Tracking (BUG-361)
zercheck NODE_ASSIGN handler now detects `g_h = pool.alloc() orelse return` — registers the handle in PathState same as NODE_VAR_DECL. Covers global handles assigned within function bodies. Without this, `pool.free(g_h); pool.get(g_h)` use-after-free was invisible to zercheck (fell back to runtime generation traps only).

### usize Target Width (BUG-363) + GCC Auto-Detect
`zer_target_ptr_bits` global in types.c (default 32). `type_width(TYPE_USIZE)` returns this instead of host `sizeof(size_t) * 8`. The emitted C always uses `size_t` — GCC resolves the actual width per target.

**Auto-detection:** `zerc_main.c` probes GCC at startup via `echo '' | gcc -dM -E -` and parses `__SIZEOF_SIZE_T__` to set `zer_target_ptr_bits` automatically. For cross-compilers: `--gcc arm-none-eabi-gcc` uses the specified compiler for the probe. `--target-bits N` is still available as explicit override (skips probe). If probe fails (no GCC in PATH), falls back to default 32.

**Test suite:** does NOT use the probe — tests use the default 32 directly. For 64-bit tests, `zer_target_ptr_bits` is set/restored explicitly: `{ int saved = zer_target_ptr_bits; zer_target_ptr_bits = 64; ... zer_target_ptr_bits = saved; }`. This keeps tests platform-independent.

**Coercion rule:** `can_implicit_coerce` allows same-width coercion when TYPE_USIZE is involved (u32↔usize on 32-bit). On 64-bit, u32→usize is widening (allowed), usize→u32 is narrowing (requires @truncate).

### Union Alignment Element-Based (BUG-364)
Same fix as BUG-350 (struct alignment) applied to union path in `compute_type_size`. Union data alignment now computed per-variant: arrays use element alignment, structs use max field alignment, scalars use `min(size, 8)`. Prevents binary layout mismatch with C for unions containing byte arrays.

### Nested Orelse in keep Validation (BUG-370)
Keep parameter validation now recursively walks orelse chains. `reg(a orelse b orelse &x)` — collects up to 8 branches and checks each for `&local` patterns. Also added orelse expr walk for local-derived ident check: `reg(local_derived orelse opt orelse &x)` — walks to the expr root through orelse chain, checks `is_local_derived`. Two separate checks: (1) recursive branch collection for `&local` in keep_checks loop, (2) orelse expr root walk for local-derived idents before the BUG-221 check.

### MMIO Constant Expression Validation (BUG-371)
`@inttoptr` mmio range check now uses `eval_const_expr()` instead of only checking `NODE_INT_LIT`. `@inttoptr(*u32, 0x50000000 + 0)` is now validated at compile time against declared ranges. Any constant expression that `eval_const_expr` can fold (arithmetic, bitwise, shifts on literals) gets compile-time range checking. Non-constant expressions still get runtime range traps in the emitter.

### Void as Compound Inner Type Rejected (BUG-372)
`*void` and `[]void` now produce compile errors in `resolve_type`. `*void` → "use *opaque for type-erased pointers". `[]void` → "void has no size". `*opaque` (TYPE_OPAQUE) is unaffected — it's the correct way to express type-erased pointers. `?void` is also unaffected — it has valid semantics (`has_value` flag only, no `.value` field).

### Provenance: 3-Layer System (BUG-393)

**Layer 1 — Compile-time Symbol-level (simple idents):**
`provenance_type` on Symbol. `ctx = @ptrcast(*opaque, &s)` sets `ctx.provenance_type`. @ptrcast CHECK looks up source ident's Symbol. Covers simple variable round-trips.

**Layer 2 — Compile-time compound key map (struct fields, constant array indices):**
`prov_map` on Checker — `{key, provenance}` entries. `h.p = @ptrcast(*opaque, &s)` stores provenance under key `"h.p"` via `build_expr_key`. @ptrcast CHECK calls `prov_map_get` when source isn't a simple ident. `prov_map_set` called in NODE_ASSIGN when value is @ptrcast or provenance-carrying ident. Same `build_expr_key` helper used by union lock (BUG-392) and zercheck (BUG-357).

**Layer 3 — Runtime type tags (everything else):**
`*opaque` in emitted C is now `_zer_opaque` struct (`{ void *ptr; uint32_t type_id; }`), not `void*`. Each struct/enum/union gets a unique `type_id` assigned in `register_decl` via `c->next_type_id++` (0 = unknown/external).

**Emitter changes:**
- `emit_type(TYPE_POINTER)`: when inner is TYPE_OPAQUE, emits `_zer_opaque` (no star)
- `is_null_sentinel`: excludes TYPE_OPAQUE inner — `?*opaque` is now struct optional `_zer_opt_opaque`
- `@ptrcast` TO `*opaque`: emits `(_zer_opaque){(void*)(expr), TYPE_ID}` where TYPE_ID is the source type's `type_id`
- `@ptrcast` FROM `*opaque`: emits `({ _zer_opaque tmp = expr; if (tmp.type_id != EXPECTED && tmp.type_id != 0) trap; (T*)tmp.ptr; })`
- Preamble: `_zer_opaque` and `_zer_opt_opaque` typedefs after `_zer_opt_void`

**Checker changes:**
- `Symbol.provenance_type` REMOVED — no longer needed, runtime handles it
- All provenance SET sites removed (NODE_ASSIGN, NODE_VAR_DECL)
- All provenance CHECK sites removed (@ptrcast handler)
- `@container` field/struct provenance (`container_struct/field` on Symbol) KEPT — orthogonal system
- `next_type_id` counter on Checker, initialized to 1

**Coverage:** 100% of `*opaque` round-trips. Type_id embedded in data, not compiler metadata. Struct fields, array elements, function returns all carry provenance. Unknown (params, cinclude) = type_id 0 = allowed through.

### Comptime Array Sizes (BUG-391)
`u8[BIT(3)]` now works. In `resolve_type_inner(TYNODE_ARRAY)`, when `eval_const_expr` fails and the size expr is `NODE_CALL` with a comptime callee (`is_comptime && func_node`), evaluates via `eval_comptime_block`. `ComptimeParam` and `eval_comptime_block` forward-declared above `resolve_type_inner` for this purpose. Nested comptime calls in array sizes work — `eval_comptime_block` + `eval_const_expr_subst` recursively resolves calls. BUG-425 fixed the checker rejecting nested calls during body type-checking.

### Union Array Lock Precision (BUG-392)
`union_switch_key` added to Checker — full expression key (e.g., `"msgs[0]"`) built via `build_expr_key()` helper. Mutation check compares assignment target's object key against the switch key. Different array elements are independent — `msgs[1].data = 20` inside `switch(msgs[0])` is allowed. Same element (`msgs[0].cmd = 99`) and pointer aliases still blocked. `build_expr_key()` handles NODE_IDENT, NODE_FIELD, NODE_INDEX(constant), NODE_UNARY(STAR) — same pattern as zercheck's `handle_key_from_expr`. Three mutation check sites updated: NODE_ASSIGN direct, NODE_FIELD pointer auto-deref, NODE_FIELD direct union.

### eval_const_expr Depth Limit (BUG-389)
`eval_const_expr` renamed to `eval_const_expr_d(Node *n, int depth)` with `depth > 256 → CONST_EVAL_FAIL` guard. Wrapper `eval_const_expr(Node *n)` calls with depth 0. Prevents stack overflow on pathological deeply-nested constant expressions.

### Handle u64 with u32 Generation (BUG-390)
`Handle(T)` changed from `uint32_t` to `uint64_t`. Encoding: `(uint64_t)gen << 32 | idx`. Gen counter changed from `uint16_t` to `uint32_t`. 4 billion cycles per slot before potential ABA wrap (was 65,536).

Sites updated:
- `emit_type(TYPE_HANDLE)` → `uint64_t`
- `emit_type(TYPE_OPTIONAL > TYPE_HANDLE)` → `_zer_opt_u64`
- Pool struct: `uint32_t gen[N]` (was `uint16_t`)
- Slab struct: `uint32_t *gen` (was `uint16_t *`)
- `_zer_pool_alloc/get/free`: u64 handle, u32 gen params, `handle & 0xFFFFFFFF` / `handle >> 32` decode
- `_zer_slab_alloc/get/free`: same changes
- Pool/Slab alloc call emission: `uint64_t _zer_ah`, `_zer_opt_u64` result

### Pool/Ring/Slab in Union Rejected (BUG-386)
Same check as BUG-287 (struct fields) added to NODE_UNION_DECL variant registration. Pool/Ring/Slab types use C macros that can't be inside union definitions.

### Orelse Keep Fallback Check (BUG-387)
Keep parameter orelse validation now collects ALL terminal nodes from orelse chain — both `orelse.expr` and `orelse.fallback` sides, up to 8 branches. Previously only walked `orelse.expr`, missing fallback local-derived idents. `reg(opt orelse local_ptr)` where `local_ptr` is local-derived now caught.

### Comptime Optional Emission (BUG-388)
Emitter comptime path checks `checker_get_type` on call node. If TYPE_OPTIONAL, emits `(type){value, 1}` instead of raw `%lld`. `comptime ?u32 maybe(u32 x)` now emits `(_zer_opt_u32){10, 1}` correctly.

### Struct Wrapper Escape (BUG-383)
`return wrap(&x).p` — walks return expression through NODE_FIELD/NODE_INDEX chains to find root NODE_CALL. If that call has local-derived args (via `call_has_local_derived_arg`) and the final return type is TYPE_POINTER, rejected. Same walk in NODE_VAR_DECL init: `*u32 p = wrap(&x).p` marks `p` as local-derived. Covers the pattern where a function wraps a pointer in a struct and the caller extracts it via field access.

### @cstr Source Volatile (BUG-384)
`@cstr` byte-loop now triggers when EITHER destination OR source is volatile. Previously only checked `dest_volatile`. Added `src_volatile` via `expr_is_volatile` on source arg. When source is volatile, the source pointer is cast to `volatile const uint8_t*` in the byte loop. Also fixed `expr_root_symbol` to walk through NODE_SLICE — `mmio_buf[0..4]` now correctly resolves to the `mmio_buf` root symbol.

### zercheck Struct Parameter Handle Fields (BUG-385)
`zc_check_function` now scans TYNODE_NAMED params by resolving via `checker->global_scope`, then walking struct fields for TYPE_HANDLE. Builds compound keys `"param.field"` and registers as HS_ALIVE. `void f(State s) { pool.free(s.h); pool.get(s.h); }` now detected as UAF.

### @container Volatile Propagation (BUG-381)
`@container(*T, ptr, field)` now checks volatile on source pointer — same pattern as @ptrcast (BUG-258). Checker validates: if source is volatile (type-level `pointer.is_volatile` OR symbol-level `is_volatile`), target must also be volatile pointer. Emitter: `expr_is_volatile(e, args[0])` check, prepends `volatile ` before the cast type in the emitted `((volatile T*)((char*)(ptr) - offsetof(T, field)))`.

### zercheck Compound Handle Keys (BUG-357)
`handle_key_from_expr()` helper builds string keys from handle expressions: `NODE_IDENT` → `"h"`, `NODE_FIELD` → `"s.h"`, `NODE_INDEX(constant)` → `"arr[0]"`. Recursive — handles `s.arr[1]` etc. Returns 0 for untrackable expressions (variable index `arr[i]`).

All handle tracking sites updated to use compound keys:
- `zc_check_call` free/get: builds key from `args[0]` via `handle_key_from_expr`, looks up with `find_handle`
- `zc_check_var_init` aliasing: builds key from init expression (was NODE_IDENT only)
- Assignment alloc tracking: builds key from `assign.target`, arena-allocates for storage in HandleInfo
- Assignment aliasing: builds key from both target and value expressions

Variable indices (`arr[i]`) remain untrackable — fall back to runtime generation counter traps. Constant indices (`arr[0]`, `arr[1]`) are fully tracked with independent state (freeing arr[0] doesn't affect arr[1]).

### Integer Literal Range Uses Target Width (BUG-373)
`is_literal_compatible` for TYPE_USIZE now uses `zer_target_ptr_bits == 64` instead of `sizeof(size_t) == 8`. Additionally, all integer literal initializations (var_decl, assign, global var) now run `is_literal_compatible` AFTER coercion passes — catches oversized values that slip through `can_implicit_coerce` because literals default to `ty_u32`. `usize x = 5000000000` rejected on 32-bit target, accepted on 64-bit.

### Nested Identity Washing Blocked (BUG-374)
`call_has_local_derived_arg()` recursive helper added at checker.c top. Scans call args for `&local`, `is_local_derived` idents, AND recurses into pointer-returning `NODE_CALL` args (max depth 8). Used by both NODE_RETURN and NODE_VAR_DECL BUG-360 paths. `return identity(identity(&x))` now caught — previously only the outermost call's direct args were checked.

### Intrinsic Target Type Validation (BUG-375)
Three intrinsics now validate target/source pointer types:
- `@inttoptr(T, addr)` — `T` must be TYPE_POINTER (was unchecked, could emit broken C with non-pointer target)
- `@ptrcast(T, expr)` — `T` must be TYPE_POINTER or TYPE_FUNC_PTR (source already validated, target was not)
- `@container(*T, ptr, field)` — `ptr` arg must be TYPE_POINTER (was unchecked)
All checks unwrap TYPE_DISTINCT before comparing.

### Orelse Array Escape to Global (BUG-377)
Both NODE_ASSIGN (BUG-240 path) and NODE_VAR_DECL (BUG-203 path) now check orelse fallback for local array roots. `g_slice = opt orelse local_buf` — the assignment path collects both direct value AND `orelse.fallback` into `arr_checks[]`, iterates each. Var_decl path does the same with `arr_roots[]`. Catches local array provided as orelse fallback being stored in global/static slices.

## Value Range Propagation

### Overview
The checker maintains a `VarRange` stack on the `Checker` struct: `{name, min_val, max_val, known_nonzero}`. This enables eliminating redundant runtime checks at compile time, and inserting invisible safety guards only when the index cannot be proven safe.

### VarRange Stack API
- `push_var_range(c, name, min, max, nonzero)` — adds entry, intersects with existing range for same name (narrowing only, never widens). Clamps `min` to 0 for unsigned types.
- Save/restore via `c->range_count` snapshot for scoped narrowing (if-body, for-body).
- `proven_safe` array on Checker: set of `Node *` pointers proven bounds/div safe. `checker_is_proven(c, node)` returns bool.

### Patterns Tracked
| Pattern | What's learned |
|---|---|
| `u32 d = 5;` | d in [5,5], known_nonzero=1 |
| `for (u32 i = 0; i < N; i += 1)` | i in [0, N-1] inside body |
| `if (i >= arr_len) { return; }` | i < arr_len after the if |
| `if (d == 0) { return; }` | d known_nonzero after the if |
| Literal index `arr[3]` where size > 3 | node added to proven_safe |
| Literal divisor `x / 4` | node added to proven_safe |

### Emitter Integration
- `NODE_INDEX` checks `checker_is_proven(e->checker, node)` → emits plain `arr[idx]` (no `_zer_bounds_check` wrapper).
- `NODE_BINARY TOK_SLASH/TOK_PERCENT` checks → emits plain `(a / b)` (no runtime division-by-zero trap).
- Belt and suspenders: proven-safe paths are zero overhead. Unproven paths still get bounds check OR auto-guard.

## Forced Division Guard

For `NODE_BINARY TOK_SLASH/TOK_PERCENT`: if divisor is `NODE_IDENT` and NOT in `proven_safe`, the checker emits a **compile error** with a fix suggestion. This prevents silent division-by-zero UB for the most common case.

- Error: `divisor 'd' not proven nonzero — add 'if (d == 0) { return; }' before division`
- Complex divisors (struct fields, function calls, array elements) keep the existing runtime trap — they're too hard to prove statically.
- Proof methods: literal nonzero init, zero-guard before use, for-loop range starting at 1+.

## Bounds Auto-Guard

### Design Decision (why not "forced bounds guard")
An earlier design tried to require the programmer to add an explicit bounds guard before any array index. This was rejected because:
1. It broke hundreds of existing tests.
2. It required guard syntax that doesn't exist in ZER.
3. It was too invasive — every array access needed ceremony.

### Final Design: Compiler-Inserted Invisible Guard
When an array index is NOT proven safe by range propagation, the compiler inserts an invisible guard **before** the containing statement:

```c
// ZER: arr[i] = 5;
// Emitted C (when i not proven):
if (i >= arr_size) { return <zero_value>; }   // auto-guard
arr[i] = 5;                                    // _zer_bounds_check still present (belt+suspenders)
```

A **warning** is emitted so the programmer knows: `"auto-guard inserted for arr[i] — add explicit guard for zero overhead"`.

### Implementation
- `AutoGuard` struct in `checker.h`: `{node, size, type_tag}`. Array `auto_guard_count/capacity` on Checker.
- `checker_mark_auto_guard(c, node, size)` — called in checker when index is unproven.
- `emit_auto_guards(e, stmt)` — walks statement expression tree, finds auto-guarded NODE_INDEX nodes, emits if-return before the statement.
- `emit_zero_value(e, type)` — emits the appropriate zero for the function's return type: `void` → nothing (just `return`), integer → `0`, bool → `0`, pointer → `NULL`, optional → `{0}`.
- `checker_auto_guard_size(c, node)` — API for emitter to query guard size for a given node.

### Interaction with Range Propagation
1. Checker runs range propagation for each statement.
2. Proven-safe indices → `proven_safe` set → emitter skips `_zer_bounds_check`.
3. Unproven indices → `auto_guard` set → emitter inserts if-return guard + keeps `_zer_bounds_check`.
4. Net effect: safe code is zero overhead; unsafe code is safe with a warning.

## Auto-Keep for Function Pointer Calls

In `NODE_CALL` keep-parameter validation: when the callee is a **function pointer** (not a direct call to a named function), ALL pointer parameters are automatically treated as `keep`. This is because the compiler cannot see the function body to know whether the pointer escapes.

Detection: callee is `NODE_IDENT` but the resolved symbol `is_function == false` (it's a function pointer variable), OR callee is not `NODE_IDENT` at all (e.g., `ops.fn(ptr)`).

This is invisible to the programmer — no annotation needed. The compiler enforces conservatively for all function pointer calls.

## @cstr Overflow Auto-Return

Previously, `@cstr` buffer overflow (source slice too long for destination) called `_zer_trap()`. Now it uses the same `emit_zero_value()` pattern as bounds auto-guard:
- If destination buffer too small: emit `if (src.len + 1 > dest_size) { emit_defers(); return <zero_value>; }` instead of trap.
- `emit_defers()` is called before the return so pending defers fire on this path.
- Applies to both array destination and slice destination overflow checks.

## *opaque Array Homogeneous Provenance

`prov_map_set()` has an additional check: when the key contains `[` (array element), the root key (prefix before `[`) is also checked. If the root already has a DIFFERENT provenance, it's a compile error: `"heterogeneous *opaque array — all elements must have the same type"`.

This enforces that `*opaque arr[4]` cannot have `arr[0]` pointing to `Sensor` and `arr[1]` pointing to `Motor`. All elements must be the same concrete type.

## Cross-Function *opaque Provenance Summaries

When a function returns `*opaque`, the checker scans the function body for `NODE_RETURN` nodes that contain `@ptrcast` or provenance-carrying idents. This return provenance is recorded in `ProvSummary` entries on the Checker struct.

- `find_return_provenance(c, func_node)` — walks function body for returns with ptrcast source type or ident provenance.
- `add_prov_summary(c, func_name, type)` / `lookup_prov_summary(c, func_name)` — summary table API.
- Built after checking each function body (if return type is `*opaque`).
- Used in `NODE_VAR_DECL`: if init is a call to a function with a known prov summary and target is `*opaque`, sets `sym->provenance_type` automatically.

This means `*opaque p = get_sensor();` can be typed as `Sensor`-provenance without any `@ptrcast` annotation at the call site, if `get_sensor()` always returns a `Sensor`-casted pointer.

## Whole-Program *opaque Param Provenance (checker.c)

Post-check pass validates that call-site arguments match what the callee expects.

**Building:** `find_param_cast_type(c, body, param_name)` scans function body for `@ptrcast(*T, param)` — returns the target type *T from typemap. Stored in `ParamExpect` entries on Checker.

**Validation:** `check_call_provenance(c, node)` runs after all bodies are checked (Pass 3 in `checker_check`). At each call to a function with `*opaque` params, extracts argument provenance (from @ptrcast source type or ident provenance_type) and compares against expected type. Mismatch → compile error.

**Limitation:** only works for ZER-to-ZER calls. `cinclude` functions have no body to analyze — runtime `type_id` handles those.

## Struct Field Range Propagation (checker.c)

Value range propagation extended to handle struct fields via `build_expr_key()`:
- `if (cfg.divisor == 0) { return; }` → range `"cfg.divisor"` set to known_nonzero
- `total / cfg.divisor` → lookup `"cfg.divisor"` in var_ranges → proven
- Both NODE_IF condition extraction and NODE_BINARY division check handle NODE_FIELD

**Critical fix:** compound key strings MUST be arena-allocated, not stack-allocated. The `cmp_key_buf` in NODE_IF is stack-local — `push_var_range` stores a pointer to it. After the if-block scope ends, the pointer dangles. Fix: `arena_alloc` + `memcpy` before pushing.

## @probe Intrinsic (checker.c + emitter.c)

Safe MMIO hardware discovery. `@probe(addr)` tries reading a memory address, returns `?u32` — null if the address faults.

**Checker:** validates 1 integer arg, result type = `type_optional(ty_u32)`.

**Emitter:** emits `_zer_probe((uintptr_t)(addr))`. Uses universal C `signal()` + `setjmp`/`longjmp` — NO platform-specific `#ifdef`. Works on any platform with C libc. `__STDC_HOSTED__` guard for freestanding compatibility (no signal/setjmp available → @probe does direct read, same as C).

**Universal fault handler (dual-mode):**
- `_zer_fault_handler(int sig)` installed at startup via `__attribute__((constructor))`
- `_zer_in_probe` flag distinguishes probe vs normal code
- During `@probe()`: `_zer_in_probe = 1` → fault handler calls `longjmp` → probe returns null
- During normal code: `_zer_in_probe = 0` → fault handler calls `_zer_trap("memory access fault")` → catches bad MMIO register access at runtime
- **Critical:** `signal()` must be re-installed after `longjmp` recovery — System V semantics reset handler to `SIG_DFL` after delivery. Without re-install, second probe crashes.
- Zero per-access overhead — handler is dormant until CPU faults
- Catches bad registers WITHIN declared mmio ranges (the gap that compile-time + boot probe can't cover)

**Important:** `NODE_INTRINSIC` returning `?T` must be handled in var-decl optional init path — added to the `NODE_CALL || NODE_ORELSE` check that assigns directly (without `{ val, 1 }` wrapping).

## MMIO Validation (emitter.c)

**5-phase auto-discovery REMOVED (2026-04-01 decision).** See `docs/safety-roadmap.md` for full rationale. Summary: auto-discovery couldn't find locked/gated/write-only peripherals (~80% coverage presented as 100%), was chip-family-specific (STM32-centric), and `_zer_mmio_valid()` false-blocked legitimate MMIO accesses.

**Replaced with: mmio declaration startup validation.** When `mmio` ranges are declared, emitter generates `_zer_mmio_validate()` as `__attribute__((constructor))`. Probes the start address of each declared range via `_zer_probe()`. If hardware doesn't respond → trap with "no hardware detected" message. Catches wrong datasheet addresses at first power-on.

**Skips:**
- Wildcard ranges (`mmio 0x0..0xFFFFFFFFFFFFFFFF;`) — clearly test/dev, not real hardware
- Hosted user-space (`#if !defined(__linux__) && !defined(__APPLE__) && !defined(_WIN32)`) — can't probe physical MMIO. x86 bare-metal gets validation.

**Flags:**
- (none): strict mode, `mmio` required, @inttoptr without declaration = compile error. Declared ranges validated at boot.
- `--no-strict-mmio`: allows @inttoptr without `mmio` declarations — plain cast, no validation (programmer's choice, like C)

**@probe remains as standalone intrinsic** for manual hardware discovery. `@probe(addr)` → `?u32`, safe read. Takes `uintptr_t` (was `uint32_t` — fixed for 64-bit systems).

**4-layer MMIO safety (final design):**
1. **Compile-time:** `mmio` declarations validate `@inttoptr` addresses (100%, zero cost)
2. **Compile-time:** alignment check — `@inttoptr(*u32, 0x40020001)` rejected (u32 needs 4-byte alignment). Universal, based on `type_width()`, works for all integer/float types. u8=any, u16=2, u32=4, u64=8.
3. **Boot-time:** `_zer_mmio_validate()` probes declared range starts (catches wrong datasheet)
4. **Runtime:** universal `signal()` fault handler catches bad registers within ranges (zero per-access cost, fires only on CPU fault)

## Test Counts (v0.2.1 current)

| File | What | Count |
|---|---|---|
| `test_lexer.c` | Token scanning | 218 |
| `test_parser.c` | AST construction | 70 |
| `test_parser_edge.c` | Edge cases, func ptrs, overflow | 98 |
| `test_modules/` | Multi-file imports, typedefs, interrupts | 11 |
| `test_checker.c` | Type checking basic | 72 |
| `test_checker_full.c` | Full spec + safety + provenance + @probe + ISR + stack + escape | 584 |
| `test_extra.c` | Additional checker | 18 |
| `test_gaps.c` | Gap coverage | 4 |
| `test_emit.c` | Full E2E (ZER→C→GCC→run) + signal() fault handler | 238 |
| `test_zercheck.c` | Handle tracking, leaks, cross-func | 54 |
| `test_fuzz.c` | Parser adversarial inputs | 491 |
| `test_firmware_patterns.c` | Round 1 firmware | 39 |
| `test_firmware_patterns2.c` | Round 2 firmware | 41 |
| `test_firmware_patterns3.c` | Round 3 firmware | 22 |
| `test_production.c` | Production firmware E2E | 14 |

## Comptime Nested Calls + Global Init (checker.c)

**Nested calls:** `eval_const_expr_subst` handles NODE_CALL via `eval_comptime_call_subst()`. Uses `_comptime_global_scope` to look up callees. Enables `BUF_SIZE()` calling `BIT(3)`.

**Global init:** `u32 mask = BIT(3);` at global scope allowed — check skips `NODE_CALL` when `is_comptime_resolved` is true.

**Recursion guard:** `eval_comptime_block` has `static int depth` (limit 32). `eval_comptime_call_subst` has `_comptime_call_depth` (limit 16). `eval_const_expr_subst` NODE_CALL handler has `_subst_depth` (limit 32). Three-layer guard prevents compiler crash from `comptime u32 f() { return f(); }`.

**Limitation:** comptime returns `int64_t` only. No structs/slices/pointers.

## zercheck Struct Copy Aliasing (zercheck.c)

`State s2 = s1` propagates handle tracking. Scans PathState for `"s1.*"` keys, creates `"s2.*"` aliases. Freeing `s1.h` marks `s2.h` FREED via alias propagation. UAF caught.

## Cross-Platform Portability Warning (checker.c)

`u32 addr = @ptrtoint(ptr)` works on 32-bit but silently loses upper bits on 64-bit. Warning emitted in NODE_VAR_DECL when init is `NODE_INTRINSIC("ptrtoint")` and target type is a fixed-width integer (not `TYPE_USIZE`). Uses `checker_warning()` — code still compiles. Message suggests using `usize` for portability.

Detection: check `type_unwrap_distinct(type)->kind != TYPE_USIZE && init is @ptrtoint`. Fires even when types match on current target (e.g. 32-bit ARM where usize == u32) — the whole point is catching code that WILL break when ported.

## Interrupt Safety Analysis (checker.c — Pass 4)

Detects unsafe shared state between interrupt handlers and regular code. No other language does this at compile time.

**Mechanism:** `in_interrupt` flag on Checker. When checking NODE_INTERRUPT body, flag is set. NODE_IDENT handler checks if ident is a global (`scope_lookup(global_scope, name)` == found symbol). If so, calls `track_isr_global()` which records access in `IsrGlobal` array with `from_isr`/`from_func` flags.

**NODE_ASSIGN handler:** compound assignments (op != TOK_EQ) on global targets also tracked via `track_isr_global(c, name, len, true)` setting `compound_in_isr`/`compound_in_func`.

**Post-check Pass 4 (`check_interrupt_safety`):** scans `isr_globals` array. For each entry where `from_isr && from_func`:
- If `!sym->is_volatile` → compile error: "must be declared volatile"
- If volatile but `compound_in_isr || compound_in_func` → compile error: "read-modify-write is not atomic"

**What it catches:** #1 and #2 most common embedded bugs — missing volatile on ISR-shared state and non-atomic read-modify-write races. Both are invisible in C — code compiles, runs for days, then randomly corrupts data.

## Stack Depth Analysis (checker.c — Pass 5)

Builds call graph, estimates frame sizes, detects recursion. MISRA tools charge $10K+/year for this.

**Mechanism:** `StackFrame` struct tracks per-function: `frame_size` (bytes), `callees` (function names), `is_recursive`.

**`check_stack_depth()`:** walks all NODE_FUNC_DECL and NODE_INTERRUPT, builds frames via `scan_frame()` which:
- Counts NODE_VAR_DECL sizes via `estimate_type_size()` (walks type tree)
- Records NODE_CALL targets (direct calls to NODE_IDENT that resolve to functions in global scope)

**`compute_max_depth()`:** DFS with visited array. Cycles detected = recursion. Accumulates frame sizes along deepest path.

**Recursion = warning (not error):** recursive code is valid ZER, but dangerous on embedded (unbounded stack). Warning lets programmer decide.

**`estimate_type_size()`:** conservative estimates: u8=1, u16=2, u32/usize=4, u64=8, pointer=4, handle=8, array=elem*count, struct=sum of fields, optional=inner+1 (or pointer size for null sentinel).

## Forward Declaration Emission Fix (emitter.c)

Bodyless forward decls emit C prototypes for mutual recursion + extern functions. Skip C stdlib names (puts, printf) to avoid conflicts.

## Session v0.2.1 Final (2026-03-30/31)

525 checker + 233 E2E + 50 zercheck. 4 audit rounds, 4 bugs fixed. Only 2 runtime cases: *opaque from cinclude + INT_MIN/-1. Auto-discovery removed (2026-04-01) — replaced with mmio startup validation via @probe of declared ranges.

## Session v0.2.2 (2026-04-01)

**MMIO redesign:**
- Removed 5-phase auto-discovery (~150 lines, unreliable, chip-specific)
- Added mmio startup validation: @probe declared range starts at boot
- Universal signal() fault handler: replaces all platform-specific handlers. Dual-mode: @probe recovers, normal code traps. Re-installs signal after longjmp (SysV resets to SIG_DFL).
- @probe takes uintptr_t (was uint32_t — broken on 64-bit)
- __STDC_HOSTED__ guard: freestanding compiles don't fail (no signal/setjmp)
- x86 bare-metal included in mmio validation (was ARM/RISC-V/AVR only)
- @inttoptr alignment check: address must match type alignment (u32=4, u16=2, u64=8)
- 4-layer MMIO safety: range + alignment + boot probe + runtime fault handler

**New safety categories (13 → 17):**
- Interrupt safety: shared globals between ISR and main without volatile → error. Compound assign on shared volatile → error (non-atomic race). Pass 4 post-check.
- Stack depth: call graph DFS, frame size estimation, recursion detection → warning. Pass 5 post-check.
- @inttoptr alignment: compile-time check, universal
- Cross-platform @ptrtoint portability: warning when stored in fixed-width type

**zer-convert P0+P1 fixes (108 → 139 tests):**
- volatile qualifier preserved and reordered
- extern/inline/restrict/register/__extension__ stripped
- #if defined(X) → comptime if (X) (expand defined() operator)
- Number suffixes (U/L/UL/ULL) stripped from literals
- MMIO casts → @inttoptr (numeric address detection)
- (uintptr_t)ptr → @ptrtoint (was @truncate)
- uintptr_t/intptr_t added to type_map
- Include guard (#ifndef/#define) detection and stripping
- void ** → **opaque
- Stringify (#), token paste (##), variadic (__VA_ARGS__) macros → auto-extracted to companion .h

559 checker + 238 E2E + 50 zercheck + 139 convert = ~1,700+ tests. All passing.

**ASM implementation (all 5 phases):**
- `@critical { }`: NODE_CRITICAL in parser/checker/emitter. Per-arch interrupt disable/enable via #if defined. Hosted x86 uses __atomic_thread_fence (CLI needs ring 0).
- `@atomic_*`: Checker validates ptr-to-integer first arg. Emitter emits `__atomic_fetch_add` etc. All 8 operations: add/sub/or/and/xor/cas/load/store.
- Extended asm: Parser bypasses lexer — scans raw source for matching `)` (colon not a ZER token). Emits `__asm__ __volatile__(...)` verbatim.
- `naked`: `func_decl.is_naked` flag. Emitter prepends `__attribute__((naked))`.
- `section("name")`: stored on func_decl and var_decl. Emitter prepends `__attribute__((section(...)))`.
- Parser detects `section(...)` and `naked` as contextual keywords at top-level before parse_func_or_var.
- **MISRA Dir 4.3 enforcement:** NODE_ASM in check_stmt checks `c->in_naked`. If false → compile error. `in_naked` flag set when entering naked function body in check_func_body, cleared on exit. Asm only in naked functions — regular functions must use @critical or @atomic_*.

**Session 2026-04-01 audit summary (11 rounds, 15 bugs fixed):**
Comptime recursion depth guard, Pool/Slab zero-handle collision, Pool/Slab ABA wrap-to-1, struct wrapper escape (2 patterns), slab calloc 64-bit overflow, stale range on reassignment, compound /= division guard, orelse identity washing, @cstr local-derived (var-decl + direct arg), slice struct escape, nested orelse recursion, field of local-derived, struct field range invalidation, partial struct mutation flag preservation, Handle type_width=64.

**Additional fixes (audit rounds 4-5):**
- Range propagation stale guard: reassignment directly overwrites VarRange entry (not intersect). `i = get_input()` wipes range.
- Compound /= %= forced division guard: added to NODE_ASSIGN alongside NODE_BINARY.
- Struct wrapper escape: `call_has_local_derived_arg` walks NODE_FIELD chains to call root. Var-decl marks struct results as local-derived.
- Slab calloc overflow: `(size_t)nc * _ZER_SLAB_PAGE_SLOTS` prevents 32-bit wrap.
- Pool/Slab ABA: gen capped at 0xFFFFFFFF, retired slots skipped in alloc.
- Pool/Slab zero-handle: gen starts at 1 (not 0), zero Handle never matches.
- Comptime recursion: 3-layer depth guard in eval_comptime_block/call_subst/const_expr_subst.
- Identity washing via orelse: `call_has_local_derived_arg` checks NODE_ORELSE fallback for &local and local-derived idents.
- @cstr local-derived: var-decl marks local-derived when init is @cstr(local_buf,...). `call_has_local_derived_arg` also checks NODE_INTRINSIC(@cstr) args directly.
- Slice struct escape: BUG-360/383 return checks extended to TYPE_SLICE. `call_has_local_derived_arg` detects local TYPE_ARRAY args (array→slice coercion). Var-decl local-derived marking includes TYPE_SLICE.
- Struct field range invalidation: NODE_ASSIGN uses `build_expr_key` on NODE_FIELD targets to invalidate compound key ranges ("s.x") on reassignment.
- Compound /= %= forced division guard: added to NODE_ASSIGN alongside NODE_BINARY.

- Nested orelse recursion: walks through `o1 orelse o2 orelse &x` chains to any depth.
- Field of local-derived struct: `identity(h.p)` where `h` has `is_local_derived` — walks NODE_FIELD to root NODE_IDENT, checks symbol flag.
- Partial struct mutation: field assignments (`h.val = 42`) no longer clear `is_local_derived` on root. Only whole-variable replacement (`h = ...`) clears flags.
- Handle type_width: TYPE_HANDLE returns 64 (was 0 — broke @size and stack estimates).

**`call_has_local_derived_arg` — the escape analysis walker (checker.c line ~383):**
Central function for detecting local pointer escape through function calls. Checks all args of a NODE_CALL for local-derived sources. **9 cases handled:**
1. `&local` — NODE_UNARY(TOK_AMP) with local root ident
2. Local-derived ident — symbol has `is_local_derived` or `is_arena_derived`
3. Local array — TYPE_ARRAY ident that's not global/static (array→slice coercion)
4. Nested call — NODE_CALL arg returning pointer, recurse
5. Orelse fallback — NODE_ORELSE, check fallback for &local or local-derived ident
6. Nested orelse — recurse through chained NODE_ORELSE fallbacks to any depth
7. @cstr — NODE_INTRINSIC("cstr"), check first arg (buffer) for local root
8. Struct field from call — NODE_FIELD chain walking to NODE_CALL root, recurse
9. Field of local-derived — NODE_FIELD chain to NODE_IDENT root, check `is_local_derived`
If adding a new expression type that can carry a local pointer, ADD A CASE HERE. This is the most-patched function in the checker (9 cases added across audit rounds 5-11).

### Interior Pointer UAF Tracking (zercheck.c, 2026-04-07)

**Problem:** `*u32 p = &b.field; free_ptr(b); p[0]` — field-derived pointer used after parent allocation freed. zercheck tracked `b` as FREED but didn't know `p` was derived from `b`.

**Fix — two changes:**

1. **Interior pointer alias tracking in `zc_check_var_init` and NODE_ASSIGN:**
   When init/value is `NODE_UNARY(TOK_AMP)`, walk the operand through field/index/deref chains to the root ident. If root is a tracked handle, assign the same `alloc_id` to the new variable. Uses the same alloc_id mechanism as handle aliasing (`h2 = h1`).

2. **NODE_INDEX UAF check in `zc_check_expr`:**
   Added `case NODE_INDEX:` — checks if the indexed object (`p` in `p[0]`) is a freed handle. Same pattern as existing NODE_FIELD and NODE_UNARY/TOK_STAR UAF checks. Also recurses into object and index sub-expressions.

**Why NODE_INDEX was missing:** zercheck had UAF checks on NODE_FIELD (`b.x`), NODE_UNARY/deref (`*p`), and NODE_CALL args (`func(p)`), but pointer indexing (`p[0]`) fell through to the default case which doesn't check handle state.

**Design note:** Holding a dangling pointer is not itself UB — only dereferencing it is. So NODE_IDENT doesn't need a UAF check. The three dereference paths (field, deref, index) plus the call-arg path cover all actual memory access.

**Tests:** `interior_ptr_safe.zer` (field ptr used before free — compiles), `interior_ptr_uaf.zer` (field ptr used after free — rejected), `interior_ptr_func.zer` (field ptr passed to function after free — rejected).

**Remaining gap:** `@ptrtoint` + integer math + `@inttoptr` creates a pointer with no link to the original allocation. This is guarded by the `mmio` declaration requirement — `@inttoptr` without mmio ranges is a compile error. The `mmio` requirement is the defense, not pointer tracking.

### C-Style Cast Safety Audit (checker.c, 2026-04-07)

**Problem:** NODE_TYPECAST handler only validated cast direction (int↔int OK, struct→int NO) but didn't apply the safety checks that `@ptrcast` enforces for pointer casts.

**7 bugs found in audit (BUG-445 through BUG-451):**
All same root cause — NODE_TYPECAST was a "dumb cast" that bypassed ZER's safety layer.

**Fix — one unified block in NODE_TYPECAST handler:**
When source and target are both pointers:
1. **Qualifier preservation** — const/volatile cannot be stripped (same as @ptrcast BUG-258/304)
2. **Provenance check** — *opaque with known provenance must match target type (same as @ptrcast BUG-393)
3. **Direct *A→*B rejection** — must go through *opaque round-trip for provenance tracking
4. **Provenance propagation** — `(*opaque)sensor` sets provenance on source symbol, AND var-decl walker now follows NODE_TYPECAST (was only following NODE_INTRINSIC)

When crossing pointer/integer boundary:
5. **int→ptr rejected** — must use `@inttoptr` (mmio range validation)
6. **ptr→int rejected** — must use `@ptrtoint` (portability warning)

**Key principle:** C-style cast is syntax sugar for safe operations. It must NEVER bypass a safety check that `@ptrcast`/`@inttoptr`/`@ptrtoint` enforce. If a cast needs to bypass safety, use the explicit intrinsic.

**Allowed C-style casts:** int↔int (widening/narrowing), int↔float, float↔float, bool↔int, *T↔*opaque (with provenance), distinct↔base (with @cast semantics).

**Provenance propagation for NODE_TYPECAST (BUG-454):**
Two sites needed for C-style cast provenance to work:
1. **NODE_TYPECAST handler** — when casting `*T → *opaque`, walk through `&`/field/index to find root ident, set `provenance_type = source` on its symbol.
2. **Var-decl init handler** — when init is `NODE_TYPECAST` producing `*opaque`, extract source type via `typemap_get(c, init->typecast.expr)` and set as `sym->provenance_type`. Same pattern as @ptrcast's `typemap_get(c, init->intrinsic.args[0])`.

Without (2), `*opaque raw = (*opaque)&a` didn't propagate `*A` provenance to `raw`, so `(*B)raw` wasn't caught. Both `@ptrcast` and C-style cast now use the same provenance mechanism.

**Parser limitation:** `(Celsius)raw` where `Celsius` is a user-defined type — parser can't distinguish from `(variable) * expr`. Use `@cast(Celsius, raw)` for distinct typedefs. Not a safety bug — @cast is the correct syntax for distinct types.

**Tests:** `typecast_safe_complex.zer` (multi-layer safe patterns), `typecast_provenance.zer`, `typecast_volatile_strip.zer`, `typecast_const_strip.zer`, `typecast_direct_ptr.zer`, `typecast_int_to_ptr.zer`, `typecast_ptr_to_int.zer`.

### checker_post_passes Not Called (BUG-453, 2026-04-07)

**Problem:** `zerc_main.c` called `checker_check_bodies` (Pass 2 only), not `checker_check` (all passes). Pass 3 (whole-program provenance), Pass 4 (interrupt safety), Pass 5 (stack depth) never ran in the real compiler — only in unit tests.

**Fix:** Added `checker_post_passes()` function that runs Pass 3+4+5, called from `zerc_main.c` after body checking. Same class of integration bug as zercheck (2026-04-03).

### scan_frame Missing Expression Nodes (BUG-452, 2026-04-07)

**Problem:** `return fibonacci(n-1) + fibonacci(n-2)` — function calls inside NODE_BINARY invisible to stack depth analysis. No recursion warning emitted.

**Fix:** Added NODE_BINARY (recurse left+right), NODE_UNARY (recurse operand), NODE_ORELSE (recurse expr) to `scan_frame`. Now all function calls in any expression position are found.

### Arena Global Escape (BUG-455, 2026-04-07)

**Problem:** Global arena pointer stored in global variable not caught. `Arena scratch;` (global) → `*Cfg c = scratch.alloc(Cfg) orelse return; global_cfg = c;` compiles without error. After `scratch.reset()`, `global_cfg` is dangling.

**Root cause:** `is_arena_derived` flag was only set for LOCAL arena allocs (`!arena_is_global` guard). Global arena allocs were considered "safe" — wrong, because `arena.reset()` invalidates all pointers regardless of arena scope.

**Fix:** Added `is_from_arena` flag on Symbol (types.h). Set for ALL arena allocs (global or local). Assignment-to-global check uses `is_from_arena || is_arena_derived`. Return/keep/call checks still use only `is_arena_derived` (local arenas — global arena pointers CAN be returned from functions safely because global arena outlives function scope).

**Design distinction:**
- `is_arena_derived` = pointer from LOCAL arena → cannot return, cannot pass to keep params, cannot store in global
- `is_from_arena` = pointer from ANY arena → cannot store in global (but CAN return, CAN pass to functions)

Both propagate through aliases, if-unwrap captures, switch captures, orelse unwrap, struct copy.

### Allocation Coloring (zercheck.c, 2026-04-07)

**Problem:** Wrapper functions returning `?*T` from arena triggered false "handle never freed" errors. Chained wrappers (app → driver → hal → arena.alloc) made it worse — each layer hid the arena source.

**Solution — three-layer coloring system:**

1. **`source_color` on HandleInfo** (`ZC_COLOR_UNKNOWN/POOL/ARENA/MALLOC`): set at allocation, propagated through all alias paths (alloc_id copies, struct copies, if-unwrap, interior pointers). Leak check skips `ZC_COLOR_ARENA`.

2. **`func_returns_color_by_name()`**: recursive body scan. Walks return statements — direct `arena.alloc()` → ARENA, `pool.alloc()` → POOL, call to function with known color → inherit transitively. Depth-limited to 8. Cached on `Symbol.returns_color_cached/value`. Handles chained wrappers: `app()` returns `driver()` returns `hal()` returns `arena.alloc()` → all ARENA.

3. **Param color inference** (`Symbol.returns_param_color`): when a function returns a cast of its parameter (e.g., `*Block opaque_to_block(*opaque raw) { return (*Block)raw; }`), the summary records "return inherits param[0]'s color." At call site, the arg's `source_color` is copied to the result. This makes freelist type-punning work: `opaque_to_block(raw)` where `raw` is ARENA-colored → result is ARENA-colored.

**Alias walker updated:** `NODE_TYPECAST` added to the alias source walker (alongside `NODE_INTRINSIC` and `NODE_ORELSE`). `*opaque raw = (*opaque)b` now copies `b`'s alloc_id AND source_color to `raw`.

**Coverage:**
- Direct arena wrapper: 100%
- Chained arena wrapper (any depth up to 8): 100%
- Freelist type-punning through `*opaque` adapter functions: 100%
- Pool/malloc leaks: still caught
- Mixed-source functions (one path arena, one path pool): conservative → tracked

### Exhaustive Switch on NodeKind (RF14, 2026-04-07)

**Problem:** The most common bug class across 12 audit sessions was "AST walker doesn't handle NODE_X" (BUG-433, 434, 435, 437, 452). Each is 2 lines to fix but 10-30 minutes to find. Root cause: `default: break;` in switch statements silently skips new node types.

**Fix:** Converted 5 critical walker functions from `default: break;` to exhaustive case lists covering all 35 NodeKind values. No `default:` means GCC `-Wswitch` (enabled by `-Wall`) warns immediately when a new NODE_ type is added and any walker doesn't handle it.

**Functions converted:**
1. `scan_frame` (checker.c) — stack depth / recursion detection. Also gained recursion into NODE_SWITCH, NODE_DEFER, NODE_CRITICAL, NODE_INTRINSIC, NODE_FIELD, NODE_INDEX, NODE_TYPECAST, NODE_SLICE (all were previously missed via `default: break;`).
2. `collect_labels` (checker.c) — converted from if/else chain to switch.
3. `validate_gotos` (checker.c) — converted from if/else chain to switch.
4. `zc_check_expr` (zercheck.c) — gained NODE_TYPECAST, NODE_SLICE recursion.
5. `zc_check_stmt` (zercheck.c) — all statement nodes explicit.

**Pattern for new walkers:** Always use `switch (node->kind)` with NO `default:`. List every NodeKind explicitly — active cases with logic, inactive cases grouped with `break;`. GCC enforces completeness.

**Also converted (emitter.c):**
6. `emit_auto_guards` — bounds check guard emission
7. `emit_top_level_decl` — top-level declaration emission

**Remaining with intentional `default:`:** `emit_expr` (emits `/* unhandled expr */` diagnostic), `emit_stmt` (emits `/* unhandled stmt */` diagnostic), `resolve_type_for_emit` (returns `ty_void` fallback). These are safe — unknown nodes produce visible output in emitted C, not silent skips.

**Total: 7 exhaustive walkers.** GCC `-Wswitch` warns on ALL of them when a new NODE_ type is added.

**Impact:** Prevents ~10-12 bugs per version. The single highest-value refactor for long-term maintainability.

### Semantic Fuzzer (tests/test_semantic_fuzz.c, 2026-04-07)

**Purpose:** Generates random valid ZER programs combining safety-critical patterns, compiles with zerc, runs, and verifies: safe programs must compile AND run (exit 0), unsafe programs must be REJECTED by zerc.

**32 generators (safe + unsafe):**
- Memory: Pool+defer (1-4 handles), arena wrappers (1-4 depth), *opaque roundtrip, interior ptr, Task.new/delete, handle alias, Slab alloc_ptr
- Casts: narrowing chain, bool↔int, signed↔unsigned, wrong-type, wrong-provenance, direct *A→*B
- Control: goto+defer, while+break, enum switch, defer+orelse block
- Types: union mutable capture, packed struct, distinct typedef, function pointer callback, nested struct deref, bit extraction, slice subslice, ring buffer
- Unsafe: UAF (handle, interior, alias, goto backward), double free, leak, non-keep param escape, arena global escape

**Architecture:** C program with generator functions (`gen_safe_*`, `gen_unsafe_*`). RNG selects from 32 patterns, generates ZER source, writes to `/tmp/_zer_fuzz.zer`, runs `zerc --run` or `zerc -o /dev/null`. 200 tests per `make check`, deterministic via seed. Auto-detects zerc binary or builds from source.

**Found bugs:** Param color aliasing for `*opaque` adapter functions — `unwrap(*opaque raw)` returning `(*T)raw` was treated as new allocation instead of alias. Fixed: param color inference copies alloc_id from arg to result.

**When adding new features:** Add `gen_safe_<feature>()` + `gen_unsafe_<feature>()` to the fuzzer. Add case to the switch in main. The RNG automatically combines new patterns with existing ones — combinatorial coverage grows with each feature.

**Run:** Part of `make check`. Also: `./test_semantic_fuzz [seed] [count]` for custom runs. Default: seed=42, count=200. Verified with 2,500 tests across 5 seeds — zero failures.

### Param Color Aliasing (zercheck.c, 2026-04-07)

**Problem:** `*Src back = unwrap(raw)` where `unwrap(*opaque r) { return (*Src)r; }` — zercheck treated `back` as a new allocation needing free, even though it's the SAME memory as the arg (just re-cast).

**Fix:** When param color inference detects "function returns cast of param[N]", at the call site: instead of just copying color, create a full ALIAS — copy alloc_id, state, pool_id, free_line, source_color from the arg's HandleInfo. The result shares the same allocation identity as the arg.

**This means:** `defer slab.free_ptr(s); ... *Src back = unwrap((*opaque)s);` — `back` has same alloc_id as `s`, so when `s` is freed by defer, `back` is covered. No false "never freed" error.

**Alias walker updated:** NODE_TYPECAST now followed in the alias source walker (same loop as NODE_INTRINSIC and NODE_ORELSE). `*opaque raw = (*opaque)b` copies alloc_id + source_color from `b` to `raw`.

### shared struct — Auto-Locked Thread-Safe Data (2026-04-07)

**Keyword:** `shared struct Name { fields... }` — adds hidden `uint32_t _zer_lock` field. Every field access auto-locked via spinlock.

**Lexer:** `TOK_SHARED` keyword. **Parser:** `shared struct` prefix (same pattern as `packed struct`). **AST:** `is_shared` on struct_decl. **Types:** `is_shared` on TYPE_STRUCT.

**Emitter — block-level grouping:** NODE_BLOCK emitter scans consecutive statements with `find_shared_root_in_stmt()`. When consecutive statements access the same shared variable, ONE lock scope wraps the group. When a non-matching statement is encountered, the lock is released. Covers NODE_EXPR_STMT, NODE_VAR_DECL, NODE_RETURN, NODE_IF, NODE_WHILE, NODE_FOR, NODE_SWITCH.

**Emitter — lock primitives:** `_zer_lock_acquire` (atomic exchange spinlock), `_zer_lock_release` (atomic store). Emitted in preamble.

**Checker — &shared.field ban:** In NODE_UNARY TOK_AMP handler, if operand is NODE_FIELD on shared struct → compile error "pointer would bypass auto-locking."

**Design doc:** `docs/SHARED_STRUCT_DESIGN.md` — full specification including interactions with defer, goto, Handle, orelse, const, arrays, function parameters.

### spawn — Thread Creation (2026-04-07)

**Contextual keyword:** `spawn` is NOT a reserved keyword (not TOK_SPAWN in lexer). Detected by ident match at statement position in parser. This allows `spawn` as a method name (e.g., `ecs_world.spawn()`).

**Parser:** `spawn func(args);` → NODE_SPAWN with func_name + args array (arena-allocated).

**Checker validation:**
- Function must exist and be is_function
- Pointer args: only `*shared_struct` allowed (auto-locked, safe)
- Non-shared pointer args → compile error "data race"
- Handle args → compile error "pool.get() not thread-safe"
- Value args (u32, bool, struct by value) → OK, copied
- Banned inside @critical → compile error

**Emitter:** Generates pthread_create call with arg struct:
```c
{ struct _zer_spawn_args_N { ... }; ... pthread_create(...); pthread_detach(...); }
```
`#include <pthread.h>` added to preamble (guarded by `__STDC_HOSTED__`).

**NODE_SPAWN in exhaustive switches:** Added to all 7 walkers + scan_frame. CRITICAL: NODE_SPAWN must NOT fall through from leaf nodes (NODE_SIZEOF, NODE_CONTINUE, etc.) — caused segfault crash (accessing spawn_stmt union member on wrong node type).

### HS_TRANSFERRED — Ownership Transfer (zercheck.c, 2026-04-07)

**New HandleState:** `HS_TRANSFERRED` — set when a non-shared pointer is passed to spawn (if the checker somehow allows it via future changes).

**Check sites:** NODE_FIELD, NODE_INDEX, NODE_UNARY/deref, NODE_INTRINSIC/ptrcast — all check `h->state == HS_TRANSFERRED` alongside HS_FREED/HS_MAYBE_FREED. Error: "use after transfer: ownership transferred to thread at line N."

**Note:** Currently, the CHECKER prevents non-shared pointers from reaching spawn, so HS_TRANSFERRED is rarely triggered. It exists as defense-in-depth for future features that might relax the checker constraint.

### Deadlock Detection — Lock Ordering (checker.c, 2026-04-07)

**Pass 7:** `check_lock_ordering()` runs after stack depth analysis. Walks each function body's blocks.

**Algorithm:** Within any block, track the highest `type_id` of shared structs accessed. If a subsequent access has a LOWER type_id → compile error "deadlock: always access shared structs in consistent ascending order."

**Helpers:** `find_shared_type_in_expr()` walks expression tree to find shared struct type. `find_shared_type_in_stmt()` dispatches by statement kind. `check_block_lock_ordering()` walks block statements and recurses into nested blocks/if/for/while.

**Declaration order = lock order:** `type_id` is assigned during `register_decl` in declaration order. Earlier declared struct = lower ID = must be locked first.

**Mathematically proven:** If all code acquires locks in the same total order, deadlock is impossible (Dijkstra, 1965). Same principle as Linux kernel lockdep.

**Post-passes error check:** `zerc_main.c` now checks `checker.error_count` after `checker_post_passes()`. Deadlock errors prevent compilation.

### Scoped Spawn — ThreadHandle + join (2026-04-08)

**Syntax:** `ThreadHandle th = spawn worker(&data);` + `th.join();`

**Key rule:** Scoped spawn (with ThreadHandle capture) allows `*T` (non-shared pointer) args because the thread is guaranteed to be joined before the scope exits. Fire-and-forget spawn still requires `*shared_struct` or value args.

**Parser:** `ThreadHandle` is a contextual ident (12 chars), detected at statement position. Parsed as NODE_SPAWN with `handle_name`/`handle_name_len` fields. `yield` and `await` are also contextual idents (5 chars).

**Checker:** Registers ThreadHandle variable in scope as `u64` with `sym->is_thread_handle = true`. `th.join()` intercepted in builtin method dispatch (checks `is_thread_handle` on symbol). Scoped spawn skips the non-shared pointer error.

**Emitter — proper spawn wrappers:** Pre-scan phase (`prescan_spawn_in_node`) walks entire AST, assigns unique IDs to NODE_SPAWN nodes. Wrapper functions (`_zer_spawn_wrap_N`) emitted at file scope between struct declarations and user functions. Forward declarations emitted for target functions. At NODE_SPAWN emission site, references the wrapper by ID. Scoped spawn emits `pthread_t handle_name;` + `pthread_create` (no detach). Fire-and-forget emits local `pthread_t` + detach.

**zercheck:** ThreadHandle registered as ALIVE with `is_thread_handle = true`. `th.join()` detected in NODE_CALL handler — marks FREED. At function exit, ALIVE ThreadHandle → error "thread not joined: 'th' spawned but never joined — add 'th.join()' before function returns."

**Previous spawn UB fixed:** Old emitter cast function pointer `(void*(*)(void*))func_name` — UB for multi-arg functions. New approach: proper wrapper function unpacks arg struct and calls the real function with typed args.

### Condvar — @cond_wait / @cond_signal / @cond_broadcast / @cond_timedwait (2026-04-08)

**Intrinsics:** `@cond_wait(shared_var, condition)`, `@cond_signal(shared_var)`, `@cond_broadcast(shared_var)`, `@cond_timedwait(shared_var, condition, timeout_ms)`.

**Smart lock upgrade:** Pre-scan phase detects which shared struct types are referenced by @cond_* intrinsics. Those types get `pthread_mutex_t _zer_mtx` + `pthread_cond_t _zer_cond` instead of `uint32_t _zer_lock`. Lock/unlock functions (`emit_shared_lock_mode`, `emit_shared_unlock`) check `shared_needs_condvar()` to emit the correct lock type.

**@cond_wait emission:** `({ pthread_mutex_lock(&var._zer_mtx); while (!(condition)) { pthread_cond_wait(&var._zer_cond, &var._zer_mtx); } pthread_mutex_unlock(&var._zer_mtx); (void)0; })`

**@cond_timedwait emission:** Same pattern but uses `clock_gettime` + `pthread_cond_timedwait`. Returns `_zer_opt_void` — `.has_value = 1` if condition met, `.has_value = 0` on timeout. Requires `#include <time.h>` in preamble.

**Checker:** Validates first arg is shared struct variable. @cond_wait requires 2 args, @cond_timedwait requires 3 args (last is integer timeout_ms). @cond_timedwait returns `?void`.

### shared(rw) struct — Reader-Writer Locks (2026-04-08)

**Syntax:** `shared(rw) struct Config { u32 threshold; }` — uses `pthread_rwlock_t` instead of spinlock.

**Parser:** After consuming `TOK_SHARED`, checks for `TOK_LPAREN` + ident "rw" + `TOK_RPAREN`. Sets `is_shared_rw = true` on struct_decl.

**AST/Types:** `bool is_shared_rw` added to both `struct_decl` (AST) and `struct_type` (Type).

**Emitter — auto read/write detection:** Block-level grouping pre-scans the statement group to determine if ANY statement writes. `stmt_writes_shared()` checks for NODE_ASSIGN targets and NODE_CALL (conservative: any call might mutate). If group has writes → `pthread_rwlock_wrlock`. All reads → `pthread_rwlock_rdlock`. Unlock is always `pthread_rwlock_unlock`.

**Preamble:** `#define _POSIX_C_SOURCE 200112L` added before includes for `pthread_rwlock_t` support.

### @once { body } — Thread-Safe Init (2026-04-08)

**Syntax:** `@once { body }` — executes body exactly once, thread-safe.

**Parser:** Detected alongside `@critical` in the `@` + ident pattern. Creates NODE_ONCE with `once.body`.

**Emitter:** `static uint32_t _zer_once_N = 0; if (!__atomic_exchange_n(&_zer_once_N, 1, __ATOMIC_ACQ_REL)) { body }` — atomic CAS ensures only one thread enters.

**NODE_ONCE added to ALL exhaustive walkers:** collect_labels, validate_gotos, contains_break, all_paths_return, scan_frame, find_return_range, emit_auto_guards, prescan_spawn_in_node, emit_top_level_decl, zc_check_expr, zc_check_stmt, block_always_exits, defer free scan.

### @barrier_init / @barrier_wait — Thread Barrier (2026-04-08)

**Intrinsics:** `@barrier_init(var, N)` + `@barrier_wait(var)` — N threads wait until all arrive.

**Implementation:** Portable (mutex + condvar, same as Rust's `std::sync::Barrier`). `_zer_barrier` struct emitted in preamble with `pthread_mutex_t`, `pthread_cond_t`, `count`, `target`, `generation` fields. `_zer_barrier_wait` increments count, broadcasts when target reached, otherwise waits on condvar with generation check for spurious wakeup.

### async/await — Stackless Coroutines (2026-04-08)

**Syntax:** `async void func() { yield; await cond; }` — zero-cost cooperative multitasking.

**Keyword:** `TOK_ASYNC` in lexer. `yield` and `await` are contextual idents (not reserved keywords).

**AST:** `func_decl.is_async` flag. NODE_YIELD (leaf, no operands). NODE_AWAIT with `await_stmt.cond` expression.

**Checker — auto-registration:** When an async function is registered, checker also registers:
- `_zer_async_NAME` — struct type (state machine)
- `_zer_async_NAME_init` — function taking `*_zer_async_NAME`, returns void
- `_zer_async_NAME_poll` — function taking `*_zer_async_NAME`, returns i32
Uses `add_symbol_internal()` to bypass BUG-276 `_zer_` prefix check.

**Emitter — Duff's device transformation:**
1. `collect_async_locals()` — scans body for NODE_VAR_DECL, collects names + lengths
2. Emits `typedef struct { int _zer_state; type field1; type field2; ... } _zer_async_NAME;`
3. Emits `_init` function (memset 0)
4. Emits `_poll` function with `switch (self->_zer_state) { case 0:; body }`
5. NODE_YIELD emits: `self->_zer_state = N; return 0; case N:;`
6. NODE_AWAIT emits: `case N:; if (!(cond)) { self->_zer_state = N; return 0; }`
7. NODE_VAR_DECL in async mode: emits `self->name = init;` (no type declaration)
8. NODE_IDENT in async mode: `is_async_local()` check → emits `self->name`

**Duff's device:** The switch/case labels can jump INTO while/for loops — this is valid C since 1983. Enables yield/await inside loops without special loop handling. The compiler's normal while/for emission works unchanged.

**TYPE_STRUCT for async types:** Emitter checks `_zer_async_` prefix in TYPE_STRUCT emission — skips `struct` keyword (uses typedef name directly).

**Zero overhead:** No heap allocation, no runtime scheduler. Each task is a stack-allocated struct (~4-50 bytes). Poll function is a flat switch — branch predictor handles it efficiently.

### Ring Channel Pointer Warning (2026-04-08)

**Checker:** In Ring.push() and Ring.push_checked() method handling, checks if the Ring's element type is TYPE_POINTER or TYPE_OPAQUE. If so, emits warning: "pushing pointer through Ring channel — pointer may not be valid in receiver context."

### threadlocal Keyword (2026-04-07)

**Keyword:** `TOK_THREADLOCAL` in lexer, recognized for 't' case, 11 chars.

**AST:** `bool is_threadlocal` on var_decl.

**Parser:** `threadlocal` prefix handled before `const` in `parse_declaration`. Sets `is_threadlocal = true`.

**Emitter:** `emit_global_var` emits `__thread` prefix for threadlocal globals.

### Const Global Pre-Evaluation (BUG-461, 2026-04-08)

**Problem:** `const u32 X = 1 << 2;` at global scope fails because `_zer_shl` safety macro uses GCC statement expression `({...})` — invalid in global initializer context.

**Fix:** In `emit_global_var`, when `node->var_decl.is_const`, try `eval_const_expr(node->var_decl.init)` first. If evaluation succeeds (returns != CONST_EVAL_FAIL), emit the pre-computed numeric result directly: `uint32_t X = 4;`. Falls back to `emit_expr()` for non-evaluable expressions.

**Safety:** Compile-time evaluation is SAFER than runtime macro — shift amount verified at compile time, not runtime. All const global initializers with arithmetic (including <<, >>) are now pre-evaluated.

### Rust Test Suite Coverage (2026-04-08)

**400 Rust-equivalent tests** in `rust_tests/`, all passing, integrated into `make check` via `rust_tests/run_tests.sh`.

**Directories fully covered:**
- `tests/ui/threads-sendsync/` — COMPLETE (51/67 translated, 16 not-applicable)

**Directories partially covered:**
- `tests/ui/borrowck/` — 8 patterns (cross-func UAF, interior ptr, alias, autoref)
- `tests/ui/moves/` — 8 patterns (move chain, guard, loop, arc reuse)
- `tests/ui/drop/` — 8 patterns (scope exit, LIFO order, conditional, count, trait object)
- `tests/ui/unsafe/` — 4 patterns (@inttoptr safety, pointer deref, assignability)
- `tests/ui/consts/` — 10 patterns (const fn, binops, array OOB, enum values, comptime if)

**Test detection:** Negative tests auto-detected by "EXPECTED: compile error" in file content. run_tests.sh handles both positive (compile+run+exit 0) and negative (must fail to compile).

**Critical zercheck edge cases verified (15 tests):**
- Cross-function free chain (A→B→C frees)
- MAYBE_FREED in loop, nested if, switch arm
- Interior pointer to nested field chain
- Handle alias tracking (free one var, use other = error)
- *opaque provenance through free call
- defer + return expression evaluation order
- goto fires pending defers
- if-exit-not-MAYBE_FREED (free+return in branch = safe after)
- Double free via alias, ghost handle, handle leak overwrite
- Cross-type slab free, scope escape via struct field return
