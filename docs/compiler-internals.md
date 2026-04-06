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

### Leak Detection
At function exit, any handle that is `HS_ALIVE` or `HS_MAYBE_FREED` and was allocated inside the function (not a parameter) triggers a warning:
- `HS_ALIVE` → "handle leaked: never freed"
- `HS_MAYBE_FREED` → "handle leaked: may not be freed on all paths"
- Parameter handles (pool_id == -1, alloc_line == func start) are excluded — caller is responsible.

**Defer free scanning (2026-04-05):** Before leak detection, zercheck scans all top-level `NODE_DEFER` statements in the function body. `defer_scans_free()` checks for pool.free, slab.free_ptr, Task.delete, Task.delete_ptr, and bare free() calls inside defer bodies. Matched handles are marked HS_FREED, suppressing the false "never freed" warning.

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

### Scope
- Looks up Pool types via `checker_get_type()` first, then `global_scope` fallback
- Checks `NODE_FUNC_DECL` and `NODE_INTERRUPT` bodies

## Value Range Propagation (checker.c)

Tracks `{min_val, max_val, known_nonzero}` per variable. Stack-based: newer entries shadow older, save/restore via count for scoped narrowing. `push_var_range()` intersects with existing (only narrows), clamps min to 0 for unsigned types.

**Narrowing events:** literal init (`u32 d = 5` → {5,5,true}), for-loop condition (`i < N` → {0,N-1}), guard pattern (`if (i >= N) return` → {0,N-1} after if), comparison in then-block, modulo (`x % N` → {0,N-1}), bitwise AND (`x & MASK` → {0,MASK}).

**Expression-derived ranges:** `derive_expr_range(c, expr, &min, &max)` handles `TOK_PERCENT` and `TOK_AMP` with constant RHS (including const global symbol lookup). Used at both var-decl init and NODE_ASSIGN reassignment paths. This eliminates false "index not proven" warnings for hash map patterns like `slot = hash % TABLE_SIZE; arr[slot]`.

**Proven nodes:** `mark_proven(c, node)` adds to `proven_safe` array. `checker_is_proven()` exposed to emitter. Emitter skips `_zer_bounds_check` for proven NODE_INDEX, skips div trap for proven NODE_BINARY.

**Inline call range (2026-04-06):** `arr[func()]` — at NODE_INDEX, if index is NODE_CALL with an ident callee that has `has_return_range`, and `return_range_max < array.size`, the index is proven safe. Enables hash map pattern `table[hash(key)]` with zero overhead.

**find_return_range enhancements (2026-04-06):**
1. Constant returns: `return 0`, `return N+1` via `eval_const_expr_scoped` — unions with `% N` ranges
2. Chained call returns: `return other_func()` inherits callee's `has_return_range` — enables multi-layer `get_slot() → raw_hash() → % N` chains
3. NODE_SWITCH/FOR/WHILE/CRITICAL recursion — finds returns inside all control flow
4. Order-dependent: callee must be checked BEFORE caller (declaration order in ZER). Cross-module: imported functions checked first (topological order) so return ranges are available.

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
- **ABA prevention:** gen counter capped at 0xFFFFFFFF (never wraps). Free: `if (gen < max) gen++`. Alloc: skip slots where `gen == max` (permanently retired, used=0 so get() traps). Prevents silent use-after-free after 2^32 alloc/free cycles per slot. Applied to both Pool and Slab.
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
