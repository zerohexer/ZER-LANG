# Bugs Fixed — ZER Compiler

Tracking compiler bugs found and fixed, ordered by discovery date.
Each entry: what broke, root cause, fix, and test that prevents regression.

---

## Round 9 — Agent-Driven Audit (2026-03-23)

Three parallel audit agents (checker, emitter, interaction edge cases) plus code quality review. Found 12 bugs across parser, checker, emitter, AST, and main.

### BUG-084: Parser stack buffer overflow in switch arm values
- **Symptom:** Switch arm with 17+ comma-separated values overflows `Node *values[16]` stack buffer. Stack corruption, potential crash.
- **Root cause:** `parser.c:925` — fixed-size array `values[16]` with no bounds check before `values[val_count++]`.
- **Fix:** Added `if (val_count >= 16) { error(p, "too many values in switch arm (max 16)"); break; }`.
- **Test:** 18 switch values → clean error, no crash.

### BUG-085: Slice expression uses anonymous struct for most primitive types
- **Symptom:** `u16[8] arr; []u16 s = arr[0..4];` — GCC error: anonymous `struct { uint16_t* ptr; size_t len; }` incompatible with named `_zer_slice_u16`. Only u8 and u32 used named typedefs.
- **Root cause:** `emitter.c` NODE_SLICE emission had `if (is_u8_slice)` and `else if (is_u32_slice)` with named typedefs, all others fell to anonymous struct.
- **Fix:** Switch on elem_type->kind for ALL primitives (u8-u64, i8-i64, usize, f32, f64, bool) mapping to named `_zer_slice_T`.
- **Test:** `[]u16`, `[]i32` slicing works end-to-end.

### BUG-086: `emit_file_no_preamble` missing NODE_TYPEDEF handler
- **Symptom:** Typedefs (including function pointer typedefs) in imported modules silently dropped. GCC error: undeclared typedef name.
- **Root cause:** `emit_file_no_preamble` switch had no `case NODE_TYPEDEF:` — fell to `default: break;`.
- **Fix:** Added NODE_TYPEDEF case mirroring `emit_file`'s handler.

### BUG-087: `emit_file_no_preamble` missing NODE_INTERRUPT handler
- **Symptom:** Interrupt handlers in imported modules silently dropped. Missing `__attribute__((interrupt))` function in emitted C.
- **Root cause:** Same as BUG-086 — no `case NODE_INTERRUPT:` in `emit_file_no_preamble`.
- **Fix:** Added NODE_INTERRUPT case mirroring `emit_file`'s handler.

### BUG-088: `?DistinctFuncPtr` not treated as null sentinel
- **Symptom:** `?Handler` (where Handler is `distinct typedef u32 (*)(u32)`) emitted as anonymous struct wrapper `{ value, has_value }` instead of null-sentinel pointer. GCC error on name placement.
- **Root cause:** `IS_NULL_SENTINEL` macro only checks `TYPE_POINTER || TYPE_FUNC_PTR`, doesn't unwrap `TYPE_DISTINCT`. Also `emit_type_and_name` had no case for `TYPE_OPTIONAL + TYPE_DISTINCT(TYPE_FUNC_PTR)`.
- **Fix:** Added `is_null_sentinel()` function that unwraps TYPE_DISTINCT before checking. Replaced all `IS_NULL_SENTINEL(t->optional.inner->kind)` with `is_null_sentinel(t->optional.inner)`. Added `?Distinct(FuncPtr)` case to `emit_type_and_name` for correct name-inside-parens.
- **Test:** `?Op maybe` emits `uint32_t (*maybe)(uint32_t)` — compiles and runs.

### BUG-089: Array-to-slice coercion uses wrong type for TYPE_DISTINCT callees
- **Symptom:** Calling a distinct function pointer with array argument that needs slice coercion accesses `callee_type->func_ptr.params[i]` on a TYPE_DISTINCT node — undefined behavior (wrong union member).
- **Root cause:** `emitter.c:679` used `callee_type` instead of `eff_callee` (the unwrapped version).
- **Fix:** Changed to `eff_callee->func_ptr.params[i]`.

### BUG-090: Missing error for unknown struct field access
- **Symptom:** `p.nonexistent` on a struct silently returns `ty_void` with no error. Confusing downstream type errors.
- **Root cause:** `checker.c:977-981` — after struct field loop finds no match, returns `ty_void` without `checker_error()`. Comment says "UFCS fallback" but UFCS was dropped.
- **Fix:** Added `checker_error("struct 'X' has no field 'Y'")`. Updated UFCS tests to expect error (UFCS was dropped from spec).

### BUG-091: `@cast` validation issues — can't unwrap, cross-distinct allowed
- **Symptom:** Two bugs: (1) `@cast(u32, celsius_val)` fails — "target must be distinct typedef" even though unwrapping is valid. (2) `@cast(Fahrenheit, celsius_val)` succeeds — cross-distinct cast allowed even though types are unrelated.
- **Root cause:** Line 1310 required target to be TYPE_DISTINCT (blocks unwrapping). Line 1316-1322 only validated when BOTH are distinct with different underlying types, missing the cross-distinct same-underlying case.
- **Fix:** Rewrote validation: (1) allow if target is distinct and source matches underlying (wrap). (2) allow if source is distinct and target matches underlying (unwrap). (3) reject cross-distinct unless one directly wraps the other.
- **Test:** wrap u32→Celsius works, unwrap Celsius→u32 works, Celsius→Fahrenheit errors.

### BUG-092: No argument count validation for Pool/Ring/Arena builtin methods
- **Symptom:** `pool.alloc(42)`, `pool.free()`, `ring.push()` — wrong arg counts pass checker, produce broken C.
- **Root cause:** Builtin method handlers set return type without checking `node->call.arg_count`.
- **Fix:** Added arg count checks for all 10 builtin methods: pool.alloc(0), pool.get(1), pool.free(1), ring.push(1), ring.push_checked(1), ring.pop(0), arena.over(1), arena.alloc(1), arena.alloc_slice(2), arena.reset(0), arena.unsafe_reset(0).

### BUG-093: Fallback to void with no error on field access of non-struct types
- **Symptom:** `u32 y = x.something` — field access on integer silently returns `ty_void` with no error.
- **Root cause:** `checker.c:1095-1096` — fallback `result = ty_void; break;` with no `checker_error()`.
- **Fix:** Added `checker_error("cannot access field 'Y' on type 'T'")`.

### BUG-094: NODE_CINCLUDE missing from AST debug functions
- **Symptom:** `node_kind_name(NODE_CINCLUDE)` returns "UNKNOWN" in diagnostics/debugging.
- **Root cause:** `ast.c` `node_kind_name()` and `ast_print()` had no case for NODE_CINCLUDE.
- **Fix:** Added `case NODE_CINCLUDE: return "CINCLUDE";` and corresponding ast_print handler.

### BUG-095: Unchecked fread return value in zerc_main.c
- **Symptom:** If file read fails or is short, compiler processes garbage/partial input silently.
- **Root cause:** `zerc_main.c:52` — `fread(buf, 1, size, f);` return value ignored.
- **Fix:** Check `bytes_read != (size_t)size` → free buffer, close file, return NULL.

### BUG-241: @cstr to const pointer not rejected
- **Symptom:** `void bad(const *u8 p) { @cstr(p, "hi"); }` compiles — writes through const pointer.
- **Fix:** In @cstr handler, check if destination type is const pointer (`pointer.is_const`).
- **Test:** `test_checker_full.c` — @cstr to const pointer rejected.

### BUG-240: Nested array assign escape to global/static
- **Symptom:** `global_s = s.arr` where `s` is local struct — dangling slice in global.
- **Root cause:** Array→slice escape check in NODE_ASSIGN only matched direct NODE_IDENT values.
- **Fix:** Walk value's field/index chains to root, check if local and target is global/static.
- **Test:** `test_checker_full.c` — nested array assign to global rejected.

### BUG-239: Non-null pointer (*T) allowed without initializer
- **Symptom:** `*u32 p;` compiles — auto-zeroes to NULL, violating *T non-null guarantee.
- **Fix:** NODE_VAR_DECL rejects TYPE_POINTER without init (local vars only, globals need init elsewhere).
- **Test:** `test_checker_full.c` — non-null pointer without init rejected.

### BUG-238: @cstr to const destination not rejected
- **Symptom:** `const u8[16] buf; @cstr(buf, "hello");` compiles — writes to const buffer.
- **Fix:** In @cstr checker handler, look up destination symbol and reject if `is_const`.
- **Test:** `test_checker_full.c` — @cstr to const array rejected.

### BUG-237: Nested array return escape (struct field → slice)
- **Symptom:** `struct S { u8[10] arr; } []u8 bad() { S s; return s.arr; }` — returns dangling slice.
- **Root cause:** NODE_RETURN array→slice check only matched NODE_IDENT, missed NODE_FIELD chains.
- **Fix:** Walk field/index chains to find root ident before checking if local.
- **Test:** `test_checker_full.c` — nested array return escape rejected.

### BUG-236: Mutating methods on const builtins allowed
- **Symptom:** `const Pool(Task, 4) tasks; tasks.alloc()` compiles — modifies const resource.
- **Fix:** In NODE_CALL builtin handlers, walk object to root symbol, check `is_const`. All mutating methods (Pool: alloc/free, Ring: push/push_checked/pop, Arena: alloc/alloc_slice/unsafe_reset) rejected on const.
- **Test:** `test_checker_full.c` — const Pool alloc rejected.

### BUG-234: @cstr compile-time overflow not caught
- **Symptom:** `u8[4] buf; @cstr(buf, "hello world");` compiles — runtime trap catches it but compile-time is better.
- **Fix:** In @cstr checker handler, if dest is TYPE_ARRAY and src is NODE_STRING_LIT, compare `string.length + 1 > array.size`.
- **Test:** `test_checker_full.c` — @cstr constant overflow rejected.

### BUG-233: Global symbol collision across modules
- **Symptom:** `mod_a` and `mod_b` both define `u32 val` and `get_val()`. Inside `ga_get_val()`, `val` resolves to `gb_val` (wrong module).
- **Root cause:** Raw key `val` in global scope holds last-registered module's symbol. Emitter inside module body finds wrong module's symbol.
- **Fix:** (1) `checker_register_file` registers imported non-static functions/globals under mangled key (`module_name`) in addition to raw key. (2) Emitter NODE_IDENT prefers mangled lookup for current module before raw lookup.
- **Test:** `test_modules/gcoll` — `ga_read() + gb_read()` = 30 (10+20, each reads own `val`).

### BUG-232: Recursive struct via array not caught
- **Symptom:** `struct S { S[1] next; }` → GCC "array type has incomplete element type".
- **Root cause:** BUG-227 check only tested `sf->type == t` but `S[1]` is TYPE_ARRAY wrapping S.
- **Fix:** Unwrap TYPE_ARRAY chain before comparing element type to struct being defined.
- **Test:** `test_checker_full.c` — recursive struct via array rejected.

### BUG-231: @size(void) and @size(opaque) not rejected
- **Symptom:** `@size(opaque)` emits `sizeof(void)` — GCC extension returns 1 (meaningless).
- **Fix:** In @size handler, resolve type_arg and reject TYPE_VOID and TYPE_OPAQUE.
- **Test:** `test_checker_full.c` — @size(opaque) and @size(void) rejected.

### BUG-230: Pointer parameter escape — &local through param field
- **Symptom:** `void leak(*Holder h) { u32 x = 5; h.p = &x; }` allowed. Caller may pass &global, creating dangling pointer.
- **Fix:** NODE_ASSIGN escape check treats pointer parameters with field access as potential escape targets.
- **Test:** `test_checker_full.c` — local escape through pointer param rejected.

### BUG-229: Static symbol collision across modules
- **Symptom:** `mod_a` and `mod_b` both have `static u32 x` — second one silently dropped, `get_a()` returns wrong value.
- **Root cause:** `scope_add` used unmangled name as key in global scope — collision returns NULL.
- **Fix:** Register statics under mangled key (`module_name`) in global scope. Emitter NODE_IDENT tries mangled lookup when raw lookup fails.
- **Test:** `test_modules/static_coll` — `get_a() + get_b()` = 30 (10+20).

### BUG-228: &const_var yields mutable pointer (const leak)
- **Symptom:** `const u32 x = 42; *u32 p = &x; *p = 99;` — writes to .rodata, segfault.
- **Root cause:** TOK_AMP handler propagated `is_volatile` but not `is_const`.
- **Fix:** Propagate `sym->is_const` to `result->pointer.is_const` in TOK_AMP handler.
- **Test:** `test_checker_full.c` — mutable pointer from &const rejected.

### BUG-227: Recursive struct by value not rejected
- **Symptom:** `struct S { S next; }` → GCC "field has incomplete type".
- **Fix:** After resolving field type, check if `sf->type == t` (struct being defined) → error.
- **Test:** `test_checker_full.c` — recursive struct by value rejected.

### BUG-226: Float switch allowed (spec violation)
- **Symptom:** `switch (f32_val) { default => { ... } }` compiles. ZER spec says "switch on float: NOT ALLOWED."
- **Fix:** Added float check at top of NODE_SWITCH handler.
- **Test:** `test_checker_full.c` — float switch rejected.

### BUG-225: Pool/Ring assignment produces broken C
- **Symptom:** `Pool p; Pool q; p = q;` — GCC "incompatible types" (anonymous structs).
- **Fix:** Reject Pool/Ring assignment in checker — hardware resources are not copyable.
- **Test:** `test_checker_full.c` — Pool assignment rejected.

### BUG-224: void struct fields and union variants not rejected
- **Symptom:** `struct S { void x; }` → GCC "field declared void".
- **Fix:** Check field/variant type after resolve_type — error if TYPE_VOID.
- **Test:** `test_checker_full.c` — void struct field and void union variant rejected.

### BUG-223: @cstr loses volatile qualifier on destination
- **Symptom:** `volatile u8[64] buf; @cstr(buf, slice);` — memcpy discards volatile, GCC may optimize away writes.
- **Root cause:** Destination always cast to plain `uint8_t*`.
- **Fix:** Check if destination ident is `is_volatile`. If so, cast to `volatile uint8_t*` and use byte-by-byte copy loop instead of memcpy.
- **Test:** `test_emit.c` — volatile @cstr preserves writes.

### BUG-222: Static variable collision across imported modules
- **Symptom:** Two modules with `static u32 x` → GCC "redefinition" error.
- **Root cause:** BUG-213 registered statics in global scope, causing collisions.
- **Fix:** Statics from imported modules registered only in module scope (not global). Module scope registers statics during `push_module_scope`. Global scope registration adds module_prefix for emitter. Statics also mangled in emitter output.
- **Known limitation:** Cross-module static name collision in global scope may resolve to wrong symbol. Per-module symbol tables needed for full fix (v2.0).

### BUG-221: keep parameter bypass with local-derived pointers
- **Symptom:** `*u32 p = &x; store(p)` where `store(keep *u32 p)` — no error. Dangling pointer stored via keep.
- **Root cause:** keep check only looked for direct `&local`, not `is_local_derived` aliases.
- **Fix:** In function call keep param check, also reject idents with `is_local_derived` flag.
- **Test:** `test_checker_full.c` — local-derived ptr to keep param rejected.

### BUG-220: @size recursive computation for nested structs
- **Symptom:** `struct Outer { Inner inner; u8 flag; }` — @size computed 8 (wrong), should be 16.
- **Root cause:** `type_width` returns 0 for TYPE_STRUCT. Constant-eval fell back to 4 bytes.
- **Fix:** Extracted `compute_type_size()` helper — recursively computes struct, array, pointer, slice sizes with natural alignment. Used for all @size constant evaluation.
- **Test:** Manual — `@size(Outer)` now matches GCC sizeof(Outer) = 16.

### BUG-219: @size struct calculation ignores C alignment padding
- **Symptom:** `struct S { u8 a; u32 b; }` — checker computes @size = 5 (field sum), GCC sizeof = 8 (with alignment).
- **Root cause:** Constant @size resolution summed field sizes without alignment.
- **Fix:** Natural alignment: each field aligned to its own size, struct padded to multiple of largest field. Packed structs skip padding.
- **Test:** Manual — `@size(S)` now matches GCC's sizeof(S).

### BUG-218: Multi-module function/global name collision
- **Symptom:** Two modules with same function name → GCC "redefinition" error.
- **Root cause:** Functions and globals emitted with raw names, not mangled with module prefix (types were already mangled).
- **Fix:** Added `module_prefix` to Symbol struct. Emitter uses `EMIT_MANGLED_NAME` for function declarations. `NODE_IDENT` emission looks up symbol prefix. `emit_global_var` uses mangled name for imported module globals.
- **Test:** Module test — two modules with `init()` now compile as `mod_a_init` and `mod_b_init`.

### BUG-217: Compile-time slice bounds check for arrays
- **Symptom:** `u8[10] arr; []u8 s = arr[0..15];` passes checker. Should be caught at compile time.
- **Root cause:** BUG-196 added compile-time OOB for indexing but not slicing.
- **Fix:** In NODE_SLICE, if object is TYPE_ARRAY and end/start is a constant, check against `array.size`.
- **Test:** `test_checker_full.c` — slice end 15 on array[10] rejected.

### BUG-216: Bit-set assignment double-evaluates target
- **Symptom:** `regs[next_idx()][3..0] = 5` calls `next_idx()` twice — once for read, once for write.
- **Root cause:** Bit-set emission called `emit_expr(obj)` multiple times.
- **Fix:** Hoist target address via `__typeof__(obj) *_p = &(obj)`, then use `*_p` for both read and write. `__typeof__` doesn't evaluate in GCC.
- **Test:** `test_emit.c` — bit-set with side-effecting index, counter = 1.

### BUG-215: Unary `~` on narrow types (u8/u16) not cast — C integer promotion
- **Symptom:** `u8 a = 0xAA; if (~a == 0x55)` evaluates to false. C promotes `~(uint8_t)0xAA` to `0xFFFFFF55`.
- **Root cause:** Emitter wrapped binary operations (BUG-186) but not unary `~` and `-`.
- **Fix:** In `NODE_UNARY` for `TOK_TILDE`/`TOK_MINUS`, if result type is u8/u16/i8/i16, wrap in cast: `(uint8_t)(~a)`.
- **Test:** `test_emit.c` — `~u8(0xAA) == 85` returns true.

### BUG-214: Slice-to-slice sub-slicing doesn't propagate is_local_derived
- **Symptom:** `[]u8 s = local_arr; []u8 s2 = s[0..2]; return s2;` — dangling slice via sub-slice.
- **Root cause:** BUG-207 check only looked for TYPE_ARRAY root. A TYPE_SLICE root already marked local-derived wasn't checked.
- **Fix:** Check `src->is_local_derived` first (before TYPE_ARRAY check) — propagate flag from source symbol.
- **Test:** `test_checker_full.c` — sub-slice of local-derived slice blocked.

### BUG-213: Static variables invisible to own module's functions
- **Symptom:** `static u32 count = 0; void inc() { count += 1; }` → "undefined identifier 'count'".
- **Root cause:** `checker_register_file` skipped static declarations to prevent cross-module visibility. But this also hid them from the module's own functions.
- **Fix:** Register ALL declarations including statics. Cross-module visibility is handled by the module scope system.
- **Test:** `test_checker_full.c` + `test_emit.c` — static variable visible, inc() x3 returns 3.

### BUG-212: If-unwrap capture loses is_local_derived from condition
- **Symptom:** `?*u32 opt = &x; if (opt) |p| { return p; }` — returns dangling pointer via capture.
- **Root cause:** Capture symbol creation didn't propagate safety flags from the condition expression.
- **Fix:** Walk condition to root ident, propagate `is_local_derived`/`is_arena_derived` to capture symbol.
- **Test:** `test_checker_full.c` — if-unwrap capture inherits local-derived.

### BUG-211: Union switch lock bypassed via field-based access
- **Symptom:** `switch (s.msg) { .a => |*v| { s.msg.b.y = 20; } }` — type confusion through struct field.
- **Root cause:** Lock only set for NODE_IDENT expressions. NODE_FIELD (`s.msg`) fell through with no lock. Mutation check also only matched NODE_IDENT objects.
- **Fix:** Walk through NODE_FIELD/NODE_INDEX/NODE_UNARY(STAR) to find root ident for both lock setup and mutation check.
- **Test:** `test_checker_full.c` — field-based union mutation blocked.

### BUG-210: Bit-set assignment (`reg[7..0] = val`) produces broken C
- **Symptom:** `reg[7..0] = 0xFF` emits a struct literal on LHS — GCC "lvalue required" error.
- **Root cause:** Emitter's NODE_ASSIGN didn't handle NODE_SLICE target on integer type. NODE_SLICE emits an rvalue (bit extraction struct), which can't be assigned to.
- **Fix:** In NODE_ASSIGN, detect NODE_SLICE on integer → emit `target = (target & ~mask) | ((value << low) & mask)`. Safe mask generation for all widths.
- **Test:** `test_emit.c` — bit-set `reg[3..0] = 5; reg[7..4] = 10` → 165.

### BUG-209: @cstr slice destination has no bounds check
- **Symptom:** `@cstr(slice_dest, src)` emits raw memcpy with no overflow check when dest is a slice.
- **Root cause:** BUG-152 added bounds check for TYPE_ARRAY destinations but skipped TYPE_SLICE.
- **Fix:** For slice destinations, hoist slice into temp `_zer_cd`, use `.ptr` for memcpy and `.len` for bounds check: `if (src.len + 1 > dest.len) _zer_trap(...)`.
- **Test:** Manual test — @cstr overflow on slice traps (exit 3), valid @cstr works.

### BUG-208: Union switch lock bypassed via pointer alias (&union_var)
- **Symptom:** `switch(msg) { .a => |*v| { *Msg alias = &msg; alias.b.y = 20; } }` — type confusion.
- **Root cause:** Union lock only checked field access on the exact variable name. `&msg` created a pointer alias that bypassed the name check.
- **Fix:** In `check_expr(NODE_UNARY/TOK_AMP)`, block `&union_var` when `union_switch_var` is active.
- **Test:** `test_checker_full.c` — address-of union in switch arm rejected.

### BUG-207: Sub-slice from local array escapes (BUG-203 bypass)
- **Symptom:** `[]u8 s = local_arr[1..4]; return s;` — dangling slice. BUG-203 only checked `NODE_IDENT` init, not `NODE_SLICE`.
- **Root cause:** Slice-from-local detection only matched `init->kind == NODE_IDENT`, missed `init->kind == NODE_SLICE`.
- **Fix:** Walk through `NODE_SLICE` to find the object, then walk field/index chains to find root. If root is local array, mark `is_local_derived`.
- **Test:** `test_checker_full.c` — sub-slice from local array blocked.

### BUG-206: orelse unwrap loses is_local_derived from expression
- **Symptom:** `?*u32 maybe = &x; *u32 p = maybe orelse return; return p;` — returns dangling pointer. No error.
- **Root cause:** Var-decl init flag propagation walked NODE_FIELD/NODE_INDEX but not NODE_ORELSE. The orelse expression's root symbol was never checked.
- **Fix:** Walk through NODE_ORELSE to reach the expression root before checking `is_local_derived`/`is_arena_derived`.
- **Test:** `test_checker_full.c` — orelse unwrap preserves local-derived.

### BUG-205: Local-derived pointer escape via assignment to global
- **Symptom:** `*u32 p = &x; global_p = p;` — stores dangling pointer in global. No error.
- **Root cause:** Assignment check only caught direct `&local` in value, not `is_local_derived` aliases.
- **Fix:** After flag propagation in NODE_ASSIGN, check if value ident has `is_local_derived` and target root is global/static → error.
- **Test:** `test_checker_full.c` — local-derived assigned to global rejected.

### BUG-204: `orelse break` bypasses `contains_break` in return analysis
- **Symptom:** `while(true) { u32 x = mg() orelse break; return x; }` — function falls off end. No error.
- **Root cause:** `contains_break` didn't walk NODE_ORELSE, NODE_VAR_DECL, or NODE_EXPR_STMT.
- **Fix:** Added NODE_ORELSE (check `fallback_is_break`), NODE_VAR_DECL (check init), NODE_EXPR_STMT (check expr) to `contains_break`.
- **Test:** `test_checker_full.c` — orelse break in while(true) rejected.

### BUG-203: Slice from local array escapes via variable
- **Symptom:** `[]u8 s = local_arr; return s;` — returns slice pointing to stack memory. No error.
- **Root cause:** `is_local_derived` only tracked for pointers (`*T`), not slices (`[]T`). Array→slice coercion creates a slice pointing to local memory but didn't mark the symbol.
- **Fix:** In var-decl init, when type is `TYPE_SLICE` and init is `NODE_IDENT` with `TYPE_ARRAY`, check if the array is local. If so, set `sym->is_local_derived = true`.
- **Test:** `test_checker_full.c` — slice from local array blocked, slice from global array safe.

### BUG-202: orelse &local in var-decl init doesn't mark is_local_derived
- **Symptom:** `*u32 p = maybe orelse &local_x; return p;` — returns dangling pointer. No error.
- **Root cause:** `&local` detection in var-decl only checked direct `NODE_UNARY/TOK_AMP`, not `NODE_ORELSE` wrapping `&local`.
- **Fix:** Check both direct `&local` AND orelse fallback `&local` in a loop over address expressions.
- **Test:** `test_checker_full.c` — orelse &local marks local-derived, orelse &global is safe.

### BUG-201: `type_width`/`type_is_integer`/etc. don't unwrap TYPE_DISTINCT
- **Symptom:** `type_width(Meters)` returns 0 for `distinct typedef u32 Meters`. Breaks `@size(Distinct)` (returns 0 → rejected), and could confuse intrinsic validation.
- **Root cause:** Type query functions in `types.c` dispatch on `a->kind` without unwrapping distinct first.
- **Fix:** Added `a = type_unwrap_distinct(a)` at the top of `type_width`, `type_is_integer`, `type_is_signed`, `type_is_unsigned`, `type_is_float`. Also unwrap in `@size` constant resolution path.
- **Test:** `test_checker_full.c` — `@size(distinct u32)` = 4 accepted as array size.

### BUG-200: `while(true)` with `break` falsely treated as terminator
- **Symptom:** `u32 f(bool c) { while(true) { if (c) { break; } return 1; } }` — function falls off end after break. GCC warns "control reaches end of non-void function."
- **Root cause:** BUG-195 made `while(true)` return `true` in `all_paths_return` unconditionally. But `break` exits the loop, so the function CAN fall through.
- **Fix:** Added `contains_break(body)` helper that checks for `NODE_BREAK` targeting the current loop (stops at nested loops). `while(true)` is only a terminator when `!contains_break(body)`. Same for `for(;;)`.
- **Test:** `test_checker_full.c` — while(true)+break rejected, while(true) without break still accepted.

### BUG-199: `@size(T)` not recognized as compile-time constant in array sizes
- **Symptom:** `u8[@size(Task)] buffer;` errors "array size must be a compile-time constant."
- **Root cause:** `eval_const_expr` in `ast.h` handles literals and binary ops but not `NODE_INTRINSIC`. No way to resolve type sizes without checker context.
- **Fix:** In checker's TYNODE_ARRAY resolution, detect `NODE_INTRINSIC` with name "size" when `eval_const_expr` returns -1. Resolve the type via `type_arg` or named lookup, compute byte size from `type_width / 8` (primitives) or field sum (structs) or 4 (pointers).
- **Test:** `test_checker_full.c` — `@size(T)` accepted as array size. `test_emit.c` — E2E `@size(Task)` = 8 bytes.

### BUG-198: Duplicate enum variant names not caught
- **Symptom:** `enum Color { red, green, red }` passes checker. Emitter outputs duplicate `#define` — GCC warns about redefinition.
- **Root cause:** BUG-191 fixed duplicate struct/union fields but missed enum variants.
- **Fix:** Added collision check in `NODE_ENUM_DECL` registration loop (same pattern as struct fields).
- **Test:** `test_checker_full.c` — duplicate enum variant rejected, distinct variants accepted.

### BUG-197: Volatile decay on address-of — `&volatile_var` loses volatile
- **Symptom:** `volatile u32 x; *u32 p = &x; *p = 1;` — write through `p` can be optimized away because `p` is not volatile.
- **Root cause:** `TOK_AMP` in checker didn't propagate volatile from Symbol to the resulting pointer type. The pointer lost the volatile qualifier.
- **Fix:** In `check_expr(NODE_UNARY/TOK_AMP)`, look up operand symbol — if `is_volatile`, set `result->pointer.is_volatile = true`. In var-decl init, block volatile→non-volatile pointer assignment.
- **Test:** `test_checker_full.c` — non-volatile ptr from volatile rejected, volatile ptr accepted.

### BUG-196b: Switch on struct-optional emits struct==int — GCC error
- **Symptom:** `switch (?u32 val) { 5 => { ... } }` emits `if (_zer_sw0 == 5)` where `_zer_sw0` is a struct. GCC rejects "invalid operands to binary ==."
- **Root cause:** Emitter switch fallback compared the full optional struct against integer values. No special handling for struct-based optionals.
- **Fix:** Detect `is_opt_switch` when expression type is `TYPE_OPTIONAL` with non-null-sentinel inner. Compare `.has_value && .value == X`. Handle captures by extracting `.value`.
- **Test:** `test_emit.c` — switch on ?u32 matches value, null hits default, capture works.

### BUG-196: Constant array OOB not caught at compile time
- **Symptom:** `u8[10] arr; arr[100] = 1;` passes checker, traps at runtime. Should be caught at compile time.
- **Root cause:** Checker `NODE_INDEX` had no constant bounds check — relied entirely on runtime bounds checking in emitted C.
- **Fix:** In `NODE_INDEX`, if index is `NODE_INT_LIT` and object is `TYPE_ARRAY`, compare `idx_val >= array.size` → error.
- **Test:** `test_checker_full.c` — index 10 on [10] rejected, index 9 on [10] accepted. `test_emit.c` — compile-time OOB + runtime OOB tests.

### BUG-195: `while(true)` rejected by all_paths_return — false positive
- **Symptom:** `u32 f() { while (true) { return 1; } }` errors "not all control flow paths return."
- **Root cause:** `all_paths_return` had no `NODE_WHILE` case — fell to `default: return false`. Infinite loops are terminators (never exit normally), so they satisfy return analysis.
- **Fix:** Added `NODE_WHILE` case: if condition is literal `true`, return `true`. Same for `NODE_FOR` with no condition.
- **Test:** `test_checker_full.c` — while(true) with return accepted, with conditional return accepted. `test_emit.c` — E2E while(true) return.

### BUG-194: Sticky `is_local_derived` / `is_arena_derived` — false positives and negatives
- **Symptom:** `*u32 p = &x; p = &g; return p` → false positive ("cannot return local pointer"). `*u32 p = &g; p = &x; return p` → false negative (unsafe return not caught).
- **Root cause:** Safety flags only set during `NODE_VAR_DECL` init, never updated or cleared during `NODE_ASSIGN`. Reassignment didn't clear old flags or set new ones.
- **Fix:** In `NODE_ASSIGN` with `op == TOK_EQ`, clear both flags on target root, then re-derive: `&local` → set `is_local_derived`, alias of local/arena-derived → propagate flag.
- **Test:** `test_checker_full.c` — reassign clears flag (positive), assign &local sets flag (negative). `test_emit.c` — E2E reassign local-derived to global.

### BUG-193: Multi-module type name collision — unhelpful error
- **Symptom:** Two imported modules with same type name → "redefinition" error with no module info.
- **Fix:** Checker detects cross-module collision and reports: "name collision: 'X' in module 'a' conflicts with 'X' in module 'b' — rename one." Emitter has module-prefix infrastructure ready for future per-module scoping.

### BUG-191: Duplicate struct/union field/variant names not caught
- **Symptom:** `struct S { u32 x; u32 x; }` passes checker, GCC rejects "duplicate member."
- **Fix:** Field/variant registration loops check previous names for collision.

### BUG-192: Return/break/continue inside defer — control flow corruption
- **Symptom:** `defer { return 5; }` crashes compiler or produces invalid control flow.
- **Fix:** NODE_RETURN, NODE_BREAK, NODE_CONTINUE check `defer_depth > 0` → error.

### BUG-190: Missing return in non-void function — undefined behavior
- **Symptom:** `u32 f(bool c) { if (c) { return 1; } }` — falls off end without returning.
- **Fix:** `all_paths_return()` recursive check after function body type-checking. Handles NODE_BLOCK, NODE_IF (requires else), NODE_SWITCH (exhaustive), NODE_RETURN.

### BUG-187: Volatile index double-read in bounds check
- **Symptom:** `arr[*volatile_ptr]` reads volatile register twice (bounds check + access).
- **Fix:** Broadened side-effect detection: NODE_UNARY (deref) now triggers single-eval path.

### BUG-188: @saturate negative → unsigned returns wrong value
- **Symptom:** `@saturate(u8, -5)` returns 251 instead of 0. Only checked upper bound.
- **Fix:** Unsigned saturation checks both bounds: `val < 0 ? 0 : val > max ? max : (T)val`.

### BUG-189: Runtime slice start > end — buffer overflow
- **Symptom:** `arr[i..j]` with i > j produces massive `size_t` length. No runtime check.
- **Fix:** Emitter inserts `if (start > end) _zer_trap(...)` for variable indices.

### BUG-182: Const array → mutable slice coercion at call site
- **Symptom:** `const u32[4] arr; mutate(arr)` where `mutate([]u32)` passes. Const array data written through mutable slice.
- **Fix:** Call site checks if arg is const NODE_IDENT with TYPE_ARRAY coerced to mutable TYPE_SLICE param.

### BUG-183: Signed division overflow (INT_MIN / -1) — hardware exception
- **Symptom:** `i32(-2147483648) / -1` triggers x86 SIGFPE / ARM HardFault. Result overflows signed type.
- **Fix:** Division trap checks `divisor == -1 && dividend == TYPE_MIN` for each signed type width.

### BUG-184: Slice `arr[5..2]` — negative length → buffer overflow
- **Symptom:** `arr[5..2]` produces `len = 2 - 5` = massive unsigned. Already fixed in BUG-179 but separate from bit extraction.
- **Fix:** Compile-time check start > end (excludes bit extraction `[high..low]`).

### BUG-185: Volatile lost on struct fields
- **Symptom:** `struct S { volatile u32 x; }` emits `uint32_t x` — no volatile keyword. GCC optimizes away MMIO reads.
- **Fix:** Struct field emission checks TYNODE_VOLATILE wrapper on field type node, emits `volatile` keyword.

### BUG-186: Integer promotion breaks narrow type wrapping
- **Symptom:** `u8 a = 255; u8 b = 1; if (a + b == 0)` is false — C promotes to int, 256 != 0.
- **Fix:** Emitter casts narrow type arithmetic to result type: `(uint8_t)(a + b)`.

### BUG-177: Write through `const *T` pointer not blocked
- **Symptom:** `*p = 5` where `p` is `const *u32` passes checker. Segfault on .rodata/Flash.
- **Fix:** Assignment target walk detects const pointer (deref or auto-deref) → error.

### BUG-178: Mutation of struct fields through `const *S` pointer
- **Symptom:** `p.val = 10` where `p` is `const *S` passes. Same issue as BUG-177 via auto-deref.
- **Fix:** Same fix — walk detects `through_const_pointer` via field auto-deref path.

### BUG-179: Slice `arr[5..2]` produces corrupt negative length
- **Symptom:** `arr[5..2]` → len = `2 - 5` = massive unsigned. Buffer overflow on use.
- **Fix:** Compile-time check for constant start > end (excludes bit extraction `[high..low]`).

### BUG-180: Integer promotion breaks narrow type wrapping semantics
- **Symptom:** `u8 a = 255; u8 b = 1; if (a + b == 0)` is false — C promotes to int, 256 != 0.
- **Fix:** Emitter casts arithmetic result to narrow type: `(uint8_t)(a + b)` for u8/u16/i8/i16.

### BUG-181: Runtime helpers use `uint32_t` for capacity — truncates >32-bit sizes
- **Symptom:** Pool/Ring with >4B capacity silently truncated in preamble functions.
- **Fix:** Changed `uint32_t capacity` → `size_t capacity` in all preamble runtime helpers.

### BUG-174: Global array init from variable — invalid C
- **Symptom:** `u32[4] b = a;` at global scope emits `uint32_t b[4] = a;` — GCC rejects.
- **Fix:** Checker rejects NODE_IDENT init for TYPE_ARRAY globals.

### BUG-175: `void` variable declaration — invalid C
- **Symptom:** `void x;` passes checker, GCC rejects "variable declared void."
- **Fix:** NODE_VAR_DECL/NODE_GLOBAL_VAR rejects TYPE_VOID.

### BUG-176: Deep const leak via `type_equals` ignoring `is_const`
- **Symptom:** `**u32 mp = cp;` where `cp` is `const **u32` passes because `type_equals` ignored const.
- **Fix:** `type_equals` now checks `is_const` for TYPE_POINTER and TYPE_SLICE. Recursive — works at any depth.

### BUG-171: Global variable with non-constant initializer — invalid C
- **Symptom:** `u32 g = f()` passes checker. GCC rejects: "initializer element is not constant."
- **Fix:** NODE_GLOBAL_VAR init rejects NODE_CALL expressions.

### BUG-172: NODE_SLICE double-evaluates side-effect base object
- **Symptom:** `get_slice()[1..]` calls `get_slice()` twice (ptr + len).
- **Fix:** Detect side effects in object chain, hoist into `__auto_type _zer_so` temp, build slice from temp.

### BUG-168: Pointer escape via orelse fallback — `return opt orelse &local`
- **Symptom:** `return opt orelse &x` where `x` is local passes checker. If `opt` is null, returns dangling pointer.
- **Fix:** NODE_RETURN checks orelse fallback for `&local` pattern (walk field/index chains).

### BUG-169: Division by literal zero not caught at compile time
- **Symptom:** `u32 x = 10 / 0` passes checker, traps at runtime. Should be compile error.
- **Fix:** NODE_BINARY checks `/` and `%` with NODE_INT_LIT right operand == 0.

### BUG-170: Slice/array comparison produces invalid C
- **Symptom:** `sa == sb` where both are slices emits struct `==` in C. GCC rejects.
- **Fix:** Checker rejects `==`/`!=` on TYPE_SLICE and TYPE_ARRAY.

### BUG-165: Const laundering via assignment — `m = const_ptr` passes
- **Symptom:** `*u32 m; m = const_ptr;` passes because `type_equals` ignores `is_const`.
- **Fix:** NODE_ASSIGN checks const-to-mutable mismatch for pointers and slices.

### BUG-166: Const laundering via orelse init — `*u32 m = ?const_ptr orelse return`
- **Symptom:** `*u32 m = opt orelse return` where `opt` is `?const *u32` strips const during unwrap.
- **Fix:** Var-decl init checks const-to-mutable mismatch for pointers and slices.

### BUG-167: Signed bit extraction uses implementation-defined right-shift
- **Symptom:** `i8 val = -1; val[7..0]` emits `val >> 0` — right-shifting negative signed is impl-defined.
- **Fix:** Cast to unsigned equivalent before shifting: `(uint8_t)val >> 0`.

### BUG-162: Slice-to-pointer implicit coercion allows NULL — non-null guarantee broken
- **Symptom:** `[]u8 empty; clear(empty)` passes, `empty.ptr` is NULL but `*u8` is non-null type.
- **Fix:** Remove `[]T → *T` implicit coercion from `can_implicit_coerce`. Use `.ptr` explicitly.

### BUG-163: Pointer escape via local variable — `p = &x; return p`
- **Symptom:** `*u32 p = &x; return p` passes because return check only handles direct `&x`.
- **Fix:** Add `is_local_derived` flag on Symbol. Set when `p = &local`. Propagate through aliases. Block on return.

### BUG-164: Base-object double-evaluation in slice indexing
- **Symptom:** `get_slice()[0]` calls `get_slice()` twice (bounds check + access).
- **Fix:** Detect side effects in base object chain. Hoist slice into `__auto_type _zer_obj` temp.

### BUG-157: Const laundering via return — const ptr returned as mutable
- **Symptom:** `*u32 wash(const *u32 p) { return p; }` passes because `type_equals` ignores `is_const`.
- **Fix:** NODE_RETURN checks const mismatch between return type and function return type for pointers/slices.

### BUG-158: Arena-derived flag lost through field/index read
- **Symptom:** `*Val p = w.p;` where `w` is arena-derived — `p` not marked, escapes via return.
- **Fix:** Var-decl init walks field/index chains to find root, propagates `is_arena_derived`.

### BUG-159: Return `&local[i]` — dangling pointer via index
- **Symptom:** `return &arr[0]` passes because `&` operand check only handled NODE_IDENT.
- **Fix:** Walk field/index chains from `&` operand to find root ident, check if local.

### BUG-160: Compound shift double-eval on field access chains
- **Symptom:** `get_obj().field <<= 1` calls `get_obj()` twice. Side-effect detection only checked NODE_INDEX.
- **Fix:** Walk entire target chain checking for NODE_CALL/NODE_ASSIGN at any level.

### BUG-161: Local Pool/Ring on stack — silent stack overflow risk
- **Symptom:** `Pool(Task, 8) p;` in function body compiles, but large pools overflow the stack.
- **Fix:** Checker rejects Pool/Ring in NODE_VAR_DECL unless `is_static`.

### BUG-155: Arena return escape via struct field
- **Symptom:** `h.ptr = a.alloc(Val) orelse return; return h.ptr;` — arena-derived pointer escapes through struct field. NODE_IDENT-only check missed NODE_FIELD.
- **Fix:** 1) Assignment `h.ptr = arena.alloc()` propagates `is_arena_derived` to root `h`. 2) Return check walks field/index chains to find root.

### BUG-156: Division/modulo by zero — undefined behavior in C
- **Symptom:** `a / b` where `b=0` → raw C division, UB (SIGFPE on x86, undefined on ARM).
- **Fix:** Wrap `/` and `%` in `({ auto _d = divisor; if (_d == 0) _zer_trap(...); (a / _d); })`. Same for `/=` and `%=`.

### BUG-153: Integer literal overflow not caught by checker
- **Symptom:** `u8 x = 256` passes checker, GCC silently truncates to 0.
- **Fix:** `is_literal_compatible` validates literal value fits target type's range (0-255 for u8, etc.). Negative literals checked against signed ranges.

### BUG-154: Bit extraction index out of range for type width
- **Symptom:** `u8 val; val[15..0]` passes checker, reads junk bits beyond the 8-bit type.
- **Fix:** NODE_SLICE in checker validates constant `high` index < `type_width(obj)`.

### BUG-150: Array init/assignment produces invalid C
- **Symptom:** `u32[4] b = a;` emits `uint32_t b[4] = a;` — GCC rejects (arrays aren't initializers in C).
- **Fix:** Detect array=array in var-decl init and NODE_ASSIGN → emit `memcpy(dst, src, sizeof(dst))`.

### BUG-151: Const pointer not emitted in C output
- **Symptom:** `const *u32 p` emits as `uint32_t* p` — no `const` keyword. C libraries may write through it.
- **Fix:** `emit_type(TYPE_POINTER)` checks `is_const` and emits `const` before the inner type.

### BUG-152: @cstr has no bounds check — buffer overflow possible
- **Symptom:** `@cstr(small_buf, long_slice)` does raw memcpy with no size check.
- **Fix:** If destination is TYPE_ARRAY, emit `if (slice.len + 1 > array_size) _zer_trap(...)` before memcpy.

### BUG-143: Arena return escape — pointer to dead stack memory
- **Symptom:** `*Task bad() { Arena a = Arena.over(buf); return a.alloc(Task) orelse return; }` — returns pointer to stack-allocated arena memory.
- **Fix:** NODE_RETURN checks `is_arena_derived` on returned symbol. Only blocks local arenas (global arenas outlive functions).

### BUG-144: String literal leak to `?[]u8` return type
- **Symptom:** `?[]u8 get() { return "hello"; }` bypasses the TYPE_SLICE check.
- **Fix:** NODE_RETURN string literal check covers both TYPE_SLICE and TYPE_OPTIONAL(TYPE_SLICE).

### BUG-145: `?void` return void expression — invalid C compound literal
- **Symptom:** `?void f() { return do_stuff(); }` emits `return (_zer_opt_void){ do_stuff(), 1 };` — GCC rejects (void in initializer + excess elements).
- **Fix:** Emit void expression as statement, then `return (_zer_opt_void){ 1 };` separately.

### BUG-146: Volatile qualifier lost on scalar types
- **Symptom:** `volatile u32 status` emits as `uint32_t status` — GCC optimizer may eliminate reads.
- **Fix:** Emit `volatile` keyword for non-pointer types in both global and local var-decl paths.

### BUG-147: Compound shift `<<=`/`>>=` double-evaluates side-effect targets
- **Symptom:** `arr[next()] <<= 1` calls `next()` twice (read from one index, write to another).
- **Fix:** Detect side-effect targets (NODE_CALL/NODE_ASSIGN in index), hoist via pointer: `*({ auto *_p = &target; *_p = _zer_shl(*_p, n); })`.

### BUG-148: Enum/union exhaustiveness bitmask limited to 64 variants
- **Symptom:** Enum with >64 variants shows "handles 64 of N" even when all arms covered.
- **Fix:** Replace `uint64_t` bitmask with `uint8_t[]` byte array (stack-allocated up to 256, arena for larger).

### BUG-149: `@cstr` double-evaluates buf argument
- **Symptom:** `@cstr(buf, slice)` emits `buf` 3 times — side-effecting buf expressions execute thrice.
- **Fix:** Hoist buf into `uint8_t *_zer_cb` temp for single evaluation.

### BUG-141: Bit extraction with negative width — shift by negative is UB
- **Symptom:** `val[2..4]` (hi < lo) → `_zer_w = -1` → `1ull << -1` is undefined behavior.
- **Fix:** Add `<= 0` check to runtime ternary: `(_zer_w >= 64) ? ~0ULL : (_zer_w <= 0) ? 0ULL : ((1ull << _zer_w) - 1)`.

### BUG-142: Topological sort silently skips modules on stall
- **Symptom:** If topo sort stalls (no progress but `emit_count < module_count`), modules are silently skipped → confusing "undefined symbol" from GCC.
- **Fix:** After topo loop, check `emit_count < module_count` → error "circular dependency or unresolved imports".

### BUG-139: `if (optional)` emits struct as C boolean — GCC rejects
- **Symptom:** `if (val)` where `val` is `?u32` emits `if (val)` in C — but val is a struct. GCC: "used struct type value where scalar is required."
- **Fix:** Emitter regular-if and while paths check if condition is non-null-sentinel optional → emit `.has_value`.

### BUG-140: Const type not propagated from `const []u8` var to Type
- **Symptom:** `const []u8 msg = "hello"; mutate(msg)` passes checker because `is_const` is only on Symbol, not on the slice Type.
- **Fix:** In NODE_VAR_DECL and NODE_GLOBAL_VAR, when `is_const` is true and type is slice/pointer, create a const-qualified Type.

### BUG-137: Ring buffer overwrite doesn't advance tail pointer
- **Symptom:** After overwriting a full ring, `pop()` returns newest item (40) instead of oldest (20).
- **Fix:** `_zer_ring_push` now takes `tail` param, advances it when buffer is full.

### BUG-138: Return string literal as mutable `[]u8` — .rodata write risk
- **Symptom:** `[]u8 get() { return "hello"; }` passes checker. Caller can write through returned slice.
- **Fix:** NODE_RETURN checks NODE_STRING_LIT + TYPE_SLICE target → error.

### BUG-132: Side-effect index as lvalue fails — GCC rejects statement expression
- **Symptom:** `arr[func()] = 42` — GCC error "lvalue required." Statement expression is rvalue.
- **Fix:** Pointer dereference pattern: `*({ size_t _i = func(); check(_i); &arr[_i]; })`.

### BUG-133: Strict aliasing — GCC optimizer reorders through @ptrcast
- **Symptom:** `@ptrcast(*f32, &u32_val)` — GCC `-O2` may reorder reads/writes via TBAA.
- **Fix:** Added `-fno-strict-aliasing` to GCC invocation and preamble comment.

### BUG-128: Runtime bit extraction [63..0] still has UB when indices are variables
- **Symptom:** `val[hi..lo]` where `hi=63, lo=0` are runtime variables returns 0 instead of full value. BUG-125 only fixed the constant-folded path.
- **Root cause:** When `eval_const_expr` returns -1 (non-constant), the `else` branch emits raw `1ull << (high - low + 1)` which is UB when width=64.
- **Fix:** Three paths: (1) constant width >= 64 → `~(uint64_t)0`, (2) constant width < 64 → `(1ull << width) - 1` (precomputed), (3) runtime width → safe ternary `(width >= 64) ? ~0ULL : ((1ull << width) - 1)`.

### BUG-127: Shift by >= width is UB in emitted C — spec promises 0
- **Symptom:** `u32 x = 1 << 32;` emits raw `1 << 32` which is UB in C. GCC warns. Spec says "shift by >= width returns 0."
- **Root cause:** Emitter emitted raw `<<` and `>>` for NODE_BINARY shifts, passing C's UB through.
- **Fix:** Added `_zer_shl`/`_zer_shr` macros to preamble using GCC statement expressions (single-eval for shift amount). NODE_BINARY and compound shift assignments (`<<=`, `>>=`) now use these macros. `(b >= sizeof(a)*8) ? 0 : (a << b)`.
- **Test:** `1 << 32` = 0, `1 << 4` = 16, `x <<= 32` = 0. No GCC warnings.

### BUG-126: Bounds check double-eval for assignment-in-index expressions
- **Symptom:** `arr[i = func()] = 42` — the assignment `i = func()` is evaluated twice (once for bounds check, once for access). Double side effects.
- **Root cause:** Side-effect detection in NODE_INDEX only checked `NODE_CALL`, not `NODE_ASSIGN`.
- **Fix:** Extended check: `idx_has_side_effects = (kind == NODE_CALL || kind == NODE_ASSIGN)`.
- **Test:** Existing tests pass; verified manually that assignment-in-index uses single-eval path.

### BUG-124: String literal assigned to mutable `[]u8` — segfault on write
- **Symptom:** `[]u8 msg = "hello"; msg[0] = 'H';` — compiles, segfaults at runtime. String literal is in `.rodata` (read-only memory), but mutable slice allows writes.
- **Root cause:** Checker returned `const []u8` for string literals but `type_equals` ignores const flag on slices, so `[]u8 = const []u8` matched.
- **Fix:** Added check in var-decl and assignment: if value is NODE_STRING_LIT and target is mutable slice, error. `const []u8 msg = "hello"` still works. String literals as function arguments still work (parameter receives a copy of the slice struct).
- **Test:** `test_checker_full.c` — mutable slice from string → error, const slice → OK.

### BUG-125: Bit extraction `[63..0]` undefined behavior — `1ull << 64`
- **Symptom:** `u64_val[63..0]` emits `(1ull << 64) - 1` — shifting by type width is UB in C. GCC warns. Result may be wrong on some platforms.
- **Root cause:** Bit mask generation `(1ull << (high - low + 1)) - 1` doesn't handle full-width case.
- **Fix:** Check if width >= 64 at compile time (using `eval_const_expr`). If so, emit `~(uint64_t)0` instead of the shift expression.
- **Test:** Verified: `val[63..0]` compiles without GCC warning, returns correct value.

### BUG-121: Array/Pool/Ring size expressions silently evaluate to 0
- **Symptom:** `u8[4 * 256] buf` emits `uint8_t buf[0]`. `Pool(T, 4 + 4)` creates pool with 0 slots. Any size expression that isn't a bare int literal silently becomes 0.
- **Root cause:** Both checker and emitter only accepted `NODE_INT_LIT` for size expressions. Binary expressions (`4 * 256`, `512 + 512`) fell through with size=0.
- **Fix:** Added `eval_const_expr()` in `ast.h` (shared between checker and emitter). Recursively evaluates `+`, `-`, `*`, `/`, `%`, `<<`, `>>`, `&`, `|` on integer literals. Fixed in checker's `resolve_type` AND emitter's `resolve_type_for_emit` (the emitter had its own duplicate type resolver with the same bug).
- **Test:** `test_emit.c` — `u8[4*256]` and `u32[512+512]` both work correctly.

### BUG-122: Dangling slice via assignment — local array to global slice
- **Symptom:** `[]u8 g; void f() { u8[64] b; g = b; }` — implicit array-to-slice coercion in assignment to global variable. Slice dangles after function returns. No compiler error.
- **Root cause:** Scope escape check in NODE_ASSIGN only caught `&local` (NODE_UNARY+TOK_AMP). Implicit array-to-slice coercion (NODE_IDENT with TYPE_ARRAY) bypassed the check.
- **Fix:** Added check: if target is global/static TYPE_SLICE and value is local TYPE_ARRAY, error. Mirrors BUG-120 logic (return path) but for assignment path.
- **Test:** `test_checker_full.c` — local array to global slice → error.

### BUG-123: zer-check-design.md claims bounded loop unrolling (not implemented)
- **Symptom:** Design doc describes "Bounded loop unrolling: Unroll to pool capacity" but actual implementation does single-pass must-analysis.
- **Fix:** Updated zer-check-design.md to reflect actual implementation: single-pass loop analysis, not bounded unrolling.

### BUG-119: Bounds check double-evaluates index with side effects
- **Symptom:** `arr[next_idx()] = 42` — `next_idx()` called twice (once for bounds check, once for access). Side effects execute twice, and bounds check validates a different index than the one written to.
- **Root cause:** Inline comma operator pattern `(_zer_bounds_check(idx, ...), arr)[idx]` evaluates `idx` expression twice.
- **Fix:** Detect if index is a function call (NODE_CALL). If so, use GCC statement expression with temp variable for single evaluation. Simple indices (ident, literal) keep the comma operator for lvalue compatibility.
- **Test:** `test_emit.c` — func-call index evaluated once, counter=1.

### BUG-120: Return local array as slice — dangling pointer via implicit coercion
- **Symptom:** `[]u8 f() { u8[64] buf; return buf; }` — local array implicitly coerces to slice on return. Slice points to dead stack memory. No compiler error.
- **Root cause:** Scope escape check only caught `return &local` (NODE_UNARY + TOK_AMP), not `return local_array` with implicit array-to-slice coercion.
- **Fix:** Added check in NODE_RETURN: if return type is TYPE_SLICE and expression is TYPE_ARRAY from a local variable, error. Global/static arrays allowed.
- **Test:** `test_checker_full.c` — local array return → error, global array return → OK.

### BUG-118: Arena-derived flag not propagated to if-unwrap capture variables
- **Symptom:** `if (arena.alloc(Task)) |t| { global = t; }` — escape not caught because capture `t` never gets `is_arena_derived = true`.
- **Root cause:** If-unwrap creates capture symbol but never checks if the condition expression is an arena.alloc() call.
- **Fix:** After creating capture symbol, check if `node->if_stmt.cond` is a `arena.alloc()` or `arena.alloc_slice()` call. If so, set `cap->is_arena_derived = true`.
- **Test:** `test_checker_full.c` — arena if-unwrap capture escape to global → error.

### BUG-117: ZER-CHECK misses use-after-free on Handle parameters
- **Symptom:** `void f(Handle(T) h) { pool.free(h); pool.get(h).x = 5; }` — use-after-free on parameter handle not detected by zercheck.
- **Root cause:** `zc_check_function` created a fresh PathState but never registered Handle parameters as alive handles. Only `pool.alloc()` results were tracked.
- **Fix:** Scan function parameters for TYNODE_HANDLE and register them as HS_ALIVE in the PathState before checking the function body. Pool ID set to -1 (unknown — can't validate wrong-pool for params).
- **Test:** `test_zercheck.c` — handle param free+use → error, handle param use+free → OK.

### BUG-113: `[]bool` type emission uses anonymous struct instead of `_zer_slice_u8`
- **Symptom:** `[]bool` parameter emits anonymous `struct { uint8_t* ptr; size_t len; }` but slice expression uses `_zer_slice_u8`. GCC type mismatch.
- **Root cause:** `emit_type(TYPE_SLICE)` and `emit_type(TYPE_OPTIONAL > TYPE_SLICE)` inner switches missing `case TYPE_BOOL:`.
- **Fix:** Added `case TYPE_BOOL:` mapping to `_zer_slice_u8` / `_zer_opt_slice_u8` (bool = uint8_t in C).
- **Test:** `test_emit.c` — `[]bool` param + slice expression, count true values.

### BUG-114: Switch exhaustiveness skipped for distinct typedef over enum/bool/union
- **Symptom:** `switch (shade) { .red => {} }` where `Shade` is `distinct typedef Color` — non-exhaustive switch passes without error.
- **Root cause:** Exhaustiveness check dispatches on `expr->kind` without unwrapping TYPE_DISTINCT. Distinct enums/bools/unions skip all exhaustiveness logic.
- **Fix:** Added `Type *sw_type = type_unwrap_distinct(expr)` before the exhaustiveness dispatch. All checks use `sw_type`.
- **Test:** `test_checker_full.c` — distinct enum non-exhaustive → error.

### BUG-115: `arena.alloc_slice()` result not tracked as arena-derived
- **Symptom:** `[]D s = arena.alloc_slice(D, 4) orelse return; global = s;` — alloc_slice result escapes to global without error.
- **Root cause:** Arena-derived detection only checked `mlen == 5 && "alloc"`, missing `mlen == 11 && "alloc_slice"`.
- **Fix:** Added `|| (mlen == 11 && memcmp(mname, "alloc_slice", 11) == 0)` to the detection condition.
- **Test:** `test_checker_full.c` — arena.alloc_slice escape to global → error.

### BUG-116: ZER-CHECK misses handle use-after-free in if/while/for conditions
- **Symptom:** `pool.free(h); if (pool.get(h).x == 5) {}` — use-after-free in condition not detected by zercheck.
- **Root cause:** `zc_check_stmt` for NODE_IF never called `zc_check_expr` on condition. NODE_FOR/NODE_WHILE never checked init/cond/step.
- **Fix:** Added `zc_check_expr` calls for: if condition, while condition, for init/cond/step.
- **Test:** `test_zercheck.c` — use-after-free in if condition and while condition caught.

### BUG-111: Field access on distinct struct types fails — checker doesn't unwrap distinct
- **Symptom:** `Job j; j.id` where `Job` is `distinct typedef Task` — "cannot access field 'id' on type 'Job'". Both direct access and pointer auto-deref (`*Job` → field) affected.
- **Root cause:** Checker NODE_FIELD handler dispatches on `obj->kind` for struct/enum/union/pointer without unwrapping TYPE_DISTINCT first. Distinct structs fall through to "cannot access field" error.
- **Fix:** Added `obj = type_unwrap_distinct(obj)` before the struct/enum/union/pointer dispatch. Pointer auto-deref inner types also unwrapped with `type_unwrap_distinct(obj->pointer.inner)`.
- **Test:** `test_emit.c` — distinct struct field access + pointer deref + global auto-zero.

### BUG-112: Global/local auto-zero for distinct compound types emits `= 0` instead of `= {0}`
- **Symptom:** `Job global_job;` (distinct struct) emits `struct Task global_job = 0;` — GCC error "invalid initializer". Same for local distinct arrays/slices/optionals.
- **Root cause:** Auto-zero paths check `type->kind == TYPE_STRUCT || TYPE_ARRAY || ...` without unwrapping TYPE_DISTINCT. Distinct wrapping a struct gets `= 0` (scalar) instead of `= {0}` (compound).
- **Fix:** Added `type_unwrap_distinct()` before the compound-type check in both global and local auto-zero paths.

### BUG-106: `@ptrcast` accepts non-pointer source
- **Symptom:** `@ptrcast(*u32, 42)` — integer source passes checker, emits cast that GCC silently accepts. Creates pointer from integer with no diagnostic.
- **Root cause:** No source type validation in checker's @ptrcast handler.
- **Fix:** Validate source is TYPE_POINTER or TYPE_FUNC_PTR (unwrap distinct first).

### BUG-107: `@inttoptr` accepts non-integer source
- **Symptom:** `@inttoptr(*u32, some_struct)` — struct source passes checker. GCC rejects.
- **Root cause:** No source type validation in checker's @inttoptr handler.
- **Fix:** Validate source `type_is_integer()` (unwrap distinct first).

### BUG-108: `@ptrtoint` accepts non-pointer source
- **Symptom:** `@ptrtoint(u32_var)` — integer source passes checker, GCC accepts, produces meaningless "address".
- **Root cause:** No source type validation in checker's @ptrtoint handler.
- **Fix:** Validate source is TYPE_POINTER or TYPE_FUNC_PTR (unwrap distinct first).

### BUG-109: `@offset` accepts non-existent field
- **Symptom:** `@offset(S, bogus)` passes checker. GCC rejects with "no member named 'bogus'".
- **Root cause:** No field existence validation in checker's @offset handler.
- **Fix:** Resolve struct type, iterate fields, error if field name not found.

### BUG-110: `?[]DistinctType` emits anonymous struct for optional slice
- **Symptom:** `?[]Score` (where Score is `distinct typedef u32`) emits anonymous struct wrapper instead of `_zer_opt_slice_u32`.
- **Root cause:** `emit_type(TYPE_OPTIONAL)` TYPE_SLICE case extracts `elem = opt_inner->slice.inner` but doesn't unwrap TYPE_DISTINCT on elem before the switch.
- **Fix:** Added `if (elem->kind == TYPE_DISTINCT) elem = elem->distinct.underlying;` before switch.

### BUG-104: `?DistinctType` emits anonymous struct instead of named typedef
- **Symptom:** `?Vec2` (where Vec2 is `distinct typedef Point`) emits anonymous `struct { struct Point value; uint8_t has_value; }` instead of `_zer_opt_Point`. GCC type mismatch between function return and variable declaration.
- **Root cause:** `emit_type(TYPE_OPTIONAL)` inner switch dispatches on `t->optional.inner->kind`. When inner is TYPE_DISTINCT, it falls to the anonymous struct default because TYPE_DISTINCT isn't in the switch.
- **Fix:** Unwrap TYPE_DISTINCT before the switch: `opt_inner = t->optional.inner; if (opt_inner->kind == TYPE_DISTINCT) opt_inner = opt_inner->distinct.underlying;`. All references within the switch use `opt_inner`.
- **Test:** `test_emit.c` — `?DistinctStruct` returns null, if-unwrap skipped. `?Distinct(u32)` with orelse.

### BUG-105: `[]DistinctType` emits anonymous struct in both emit_type and NODE_SLICE
- **Symptom:** `[]Meters` (where Meters is `distinct typedef u32`) emits anonymous `struct { uint32_t* ptr; size_t len; }` instead of `_zer_slice_u32`. Same mismatch pattern as BUG-104.
- **Root cause:** Both `emit_type(TYPE_SLICE)` and NODE_SLICE expression emission dispatch on inner->kind without unwrapping TYPE_DISTINCT.
- **Fix:** Unwrap TYPE_DISTINCT in both paths: `sl_inner`/`eff_elem` variables unwrap before the switch.
- **Test:** `test_emit.c` — `[]Distinct` slice expression compiles and runs.

### BUG-099: `\x` hex escape in char literals stores wrong value
- **Symptom:** `u8 c = '\x0A';` stores 120 ('x') instead of 10 (0x0A).
- **Root cause:** `parser.c:444` — escape sequence switch had no `case 'x':` handler. `\xNN` fell to `default:` which stored `text[2]` literally (the character 'x').
- **Fix:** Added `case 'x':` that parses two hex digits from `text[3..4]`.
- **Test:** `test_emit.c` — `\x0A` = 10, `\xFF` = 255, `\x00` = 0.

### BUG-100: `orelse break` / `orelse continue` outside loop passes checker
- **Symptom:** `u32 x = get() orelse break;` at function scope passes checker. GCC rejects: "break statement not within loop or switch".
- **Root cause:** `checker.c:1228-1230` — orelse fallback_is_break/continue not validated against `c->in_loop`. Standalone `break`/`continue` were validated but orelse variants were not.
- **Fix:** Added `if (!c->in_loop) checker_error(...)` for both orelse break and orelse continue.
- **Test:** `test_checker_full.c` — orelse break/continue outside loop → error, inside loop → OK.

### BUG-101: Bare `return;` in `?*T` function emits invalid compound literal
- **Symptom:** Bare `return;` in `?*Task get_task()` emits `return (struct Task*){ 0, 1 };` — excess elements in scalar initializer.
- **Root cause:** `emitter.c:1579` — bare return path checked `TYPE_OPTIONAL` without excluding null-sentinel types. `?*T` is a raw pointer (null = none), not a struct.
- **Fix:** Check `is_null_sentinel()` first: null-sentinel → `return (T*)0;` (none). Struct-wrapper → existing `{ 0, 1 }` path.
- **Test:** `test_emit.c` — bare return in ?*T = none, if-unwrap skipped.

### BUG-102: Defer inside if-unwrap body fires at wrong scope
- **Symptom:** `if (maybe()) |val| { defer inc(); counter += 10; }` — defer fires at function exit, not at if-unwrap block exit. `counter` reads 10 instead of 11 after the if block.
- **Root cause:** `emitter.c:1452-1459` — if-unwrap unwraps the block to inject capture variable, but doesn't save/restore defer stack. Defers accumulate on function-level stack instead of block-level.
- **Fix:** Save `defer_stack.count` before emitting block, call `emit_defers_from()` after, restore count. Same fix applied to union switch capture arms.
- **Test:** `test_emit.c` — defer fires inside if-unwrap, counter=11 before after_if.

### BUG-103: Calling non-callable type produces no checker error
- **Symptom:** `u32 x = 5; x(10);` passes checker silently, emits invalid C.
- **Root cause:** `checker.c:938-944` — else branch for non-TYPE_FUNC_PTR callee set `result = ty_void` without error. UFCS comment block masked the missing error.
- **Fix:** Added `checker_error("cannot call non-function type '%s'")`.
- **Test:** `test_checker_full.c` — call u32, call bool → error.

### BUG-096: Unknown builtin method names silently return void
- **Symptom:** `pool.bogus()`, `ring.clear()`, `arena.destroy()` — unrecognized method names on Pool/Ring/Arena types fall through with no error, returning ty_void.
- **Root cause:** After all known method `if` checks, code fell through to `/* not a builtin */` without an error for builtin types.
- **Fix:** Added fallback `checker_error("Pool/Ring/Arena has no method 'X' (available: ...)")` after each builtin type's method checks.
- **Test:** `test_checker_full.c` — Pool/Ring/Arena unknown methods → error.

### BUG-097: Arena-derived flag not propagated through aliases
- **Symptom:** `*D d = arena.alloc(D) orelse return; *D q = d; global = q;` — `q` not marked arena-derived, escape to global not caught.
- **Root cause:** `is_arena_derived` flag only set on direct `arena.alloc()` init, not propagated to aliases (var-decl or assignment).
- **Fix:** Propagate `is_arena_derived` on var-decl init from simple identifier (`*D q = d`) and on assignment (`q = d`).
- **Test:** `test_checker_full.c` — alias escape via var-decl and assignment both caught; chain `d→q→r→global` caught.

### BUG-098: Union switch lock not applied through pointer auto-deref
- **Symptom:** `switch (*ptr) { .a => |*v| { ptr.b = 99; } }` — mutation allowed because union switch lock only checked direct union field access path, not pointer auto-deref path.
- **Root cause:** Union mutation check existed in `TYPE_UNION` field handler but not in `TYPE_POINTER(TYPE_UNION)` auto-deref handler.
- **Fix:** Added union switch lock check to pointer auto-deref union path. Lock now set for both `switch (d)` and `switch (*ptr)`.
- **Test:** `test_checker_full.c` — mutation via `*ptr` in switch arm caught.

---

## Round 8 — External Security Review (2026-03-23)

Gemini-prompted deep review of compiler safety guarantees. Found 6 structural bugs in bounds checking, scope escape, union safety, handle tracking, and arena lifetimes.

### BUG-078: Bounds checks missing in if/while/for conditions
- **Symptom:** `if (arr[10] == 42)` on `u32[4]` — no bounds check, reads garbage memory. `while (arr[i] < 50)` loops past array end unchecked.
- **Root cause:** `emit_bounds_checks()` was a statement-level hoisting function called only from NODE_VAR_DECL, NODE_RETURN, and NODE_EXPR_STMT. NODE_IF, NODE_WHILE, and NODE_FOR never called it, so conditions had zero bounds checking.
- **Fix:** Replaced statement-level hoisting with inline bounds checks in `emit_expr(NODE_INDEX)` using the comma operator: `(_zer_bounds_check(idx, len, ...), arr)[idx]`. Comma operator preserves lvalue semantics (assignments still work). Inline checks naturally work everywhere expressions appear — conditions, loops, var-decl, return, arguments.
- **Test:** All 141 E2E tests pass. Verified: `if (arr[10]==42)` traps, `while (arr[i]<50)` traps at OOB.

### BUG-079: Bounds check hoisting breaks short-circuit evaluation (`&&`/`||`)
- **Symptom:** `bool x = (i < 4) && (arr[i] == 42)` with `i=10` — hoisted bounds check runs unconditionally before the statement, trapping even though `i < 4` is false and `arr[i]` would never execute.
- **Root cause:** `emit_bounds_checks()` recursed into both sides of `&&`/`||` (`NODE_BINARY`) and emitted all checks before the statement, ignoring C's short-circuit evaluation.
- **Fix:** Same as BUG-078 — inline bounds checks in `emit_expr(NODE_INDEX)`. The bounds check for `arr[i]` is now inside the right operand of `&&`, so C's short-circuit naturally skips it when the left side is false.
- **Test:** `(i < 4) && (arr[i] == 42)` with i=10 exits 0 (no trap). Verified correct.

### BUG-080: Scope escape via struct field — `global.ptr = &local` not caught
- **Symptom:** `global_holder.ptr = &local` compiles without error. Dangling pointer created silently.
- **Root cause:** Scope escape check at checker.c:609 required `node->assign.target->kind == NODE_IDENT`. Struct field targets (`NODE_FIELD`) and array index targets (`NODE_INDEX`) bypassed the check entirely. Also only checked `is_static` targets, not global-scoped variables.
- **Fix:** Walk the assignment target chain (NODE_FIELD/NODE_INDEX) to find the root identifier. Check if root is static OR global (via `scope_lookup_local(global_scope)`). Catches `global.ptr = &local`, `arr[0] = &local`, and nested chains.
- **Test:** `test_checker_full.c` — `global.ptr = &local` error, `global.ptr = &global_val` allowed.

### BUG-081: Union type confusion — variant mutation during mutable switch capture
- **Symptom:** Inside a `switch (d) { .integer => |*ptr| { d.other = 999; *ptr = 42; } }`, the compiler allows `d.other = 999` which changes the active variant while `ptr` still points to the old variant's memory. Silent type confusion / memory corruption.
- **Root cause:** The `in_assign_target` flag allowed union variant assignment anywhere (checker.c:1018). No tracking of whether a switch arm was currently holding a mutable capture pointer to the same union.
- **Fix:** Added `union_switch_var` / `union_switch_var_len` fields to `Checker` struct. Set when entering a union switch arm with capture. In the union field assignment check, if the field object matches the currently-switched-on variable, emit error. Per-variable (mutating a different union is allowed). Saved/restored for nesting.
- **Test:** `test_checker_full.c` — same-union mutation error, different-union mutation allowed, non-capture arm allowed.

### BUG-082: ZER-CHECK aliasing blindspot — handle copies not tracked
- **Symptom:** `Handle(T) alias = h1; pool.free(h1); pool.get(alias).x = 5;` — ZER-CHECK produces zero warnings. Static analyzer only tracks handles by variable name string, has no concept of aliasing.
- **Root cause:** `find_handle()` in zercheck.c does pure string matching. When `alias = h1`, no entry is created for `alias`. Only `pool.alloc()` registers new handles.
- **Fix:** 1) In `zc_check_var_init`, when init is a simple identifier matching a tracked handle, register the new variable with the same state/pool/alloc_line. 2) In `zc_check_expr(NODE_ASSIGN)`, same for assignment aliasing. 3) When `pool.free(h)` is called, propagate HS_FREED to all handles with the same pool_id + alloc_line (aliases of the same allocation). Independent handles from the same pool are unaffected.
- **Test:** `test_zercheck.c` — alias use-after-free caught, assignment alias caught, valid alias use allowed, independent handles no false positive.

### BUG-083: Arena pointer lifetime escape — arena-derived pointers stored in globals
- **Symptom:** `*Data d = arena.alloc(Data) orelse return; global_holder.ptr = d;` compiles cleanly. When the function returns, `d` points to dead stack memory (the arena's buffer). Silent dangling pointer with no compile-time or runtime protection.
- **Root cause:** `arena.alloc(T)` returns bare `?*T` with no lifetime metadata. The type system does not track that the pointer originated from an arena.
- **Fix:** Added `is_arena_derived` flag to `Symbol` struct. In the checker's var-decl handler, detect `arena.alloc(T)` / `arena.alloc(T) orelse ...` patterns and mark the resulting variable. In the assignment handler, if an arena-derived variable is being stored in a global/static target (walking field/index chain to root), emit error.
- **Test:** `test_checker_full.c` — arena ptr to global error, arena ptr local use allowed, arena ptr in local struct allowed.

---

## Round 1 — Firmware Pattern Stress Tests (2026-03-19)

### BUG-001: Enum value access `State.idle` fails type-check
- **Symptom:** `State.idle` type-checks as `void`, all enum value usage broken
- **Root cause:** Checker `NODE_FIELD` had no handler for `TYPE_ENUM`. Enum dot access fell through to "unresolved field" fallback returning `ty_void`
- **Fix:** Added TYPE_ENUM handler in checker.c that validates variant name and returns the enum type
- **Test:** `test_firmware_patterns.c` — enum state machine tests

### BUG-002: Enum values emit invalid C (`State.idle` instead of `_ZER_State_idle`)
- **Symptom:** GCC rejects emitted C — `State.idle` not valid in C
- **Root cause:** Emitter `NODE_FIELD` emitted `obj.field` for all types, didn't check for enum
- **Fix:** Added enum type check at top of NODE_FIELD in emitter — emits `_ZER_EnumName_variant`
- **Test:** `test_firmware_patterns.c` — all enum E2E tests

### BUG-003: Enum switch arms emit bare identifier
- **Symptom:** `.idle =>` in switch emits `if (_sw == idle)` — GCC error, `idle` undeclared
- **Root cause:** Non-union enum switch arms hit generic `emit_expr` path, not the _ZER_ prefixed path
- **Fix:** Added enum switch arm branch in emitter that emits `_ZER_EnumName_variant`
- **Test:** `test_firmware_patterns.c` — enum state machine + switch tests

### BUG-004: Defer not firing on return inside nested blocks
- **Symptom:** `defer cleanup(); if (cond) { return 1; }` — cleanup never runs
- **Root cause:** `NODE_BLOCK` saved/restored the ENTIRE defer stack, so inner blocks couldn't see outer defers. Return inside inner block found empty stack.
- **Fix:** Changed to base-offset approach: blocks track where their defers start, return emits ALL defers from top to 0, block exit emits only that block's defers
- **Test:** `test_firmware_patterns.c` — defer + early return, defer + orelse return

### BUG-005: Orelse-return path skipped defers
- **Symptom:** `defer mark(); u32 val = nothing() orelse return;` — mark() never called when orelse triggers return
- **Root cause:** The orelse-return expansion (`if (!has_value) return 0;`) didn't call `emit_defers()` before the return. The break/continue paths already had it.
- **Fix:** Added `emit_defers()` call in orelse-return expansion
- **Test:** `test_firmware_patterns.c` — defer + orelse return combo

### BUG-006: `&x.field` parsed as `(&x).field` instead of `&(x.field)`
- **Symptom:** `&sys.primary` returns `*System` then field access gives `Sensor` instead of `*Sensor`
- **Root cause:** `parse_unary` recursively called itself for the operand but returned directly to the primary parser — postfix (. [] ()) wasn't applied. So `&sys` was the unary, `.primary` was postfix on the result.
- **Fix:** Changed `parse_unary` to call `parse_postfix(parse_primary())` for non-prefix case, matching C precedence (postfix > prefix)
- **Test:** `test_firmware_patterns.c` — nested struct pointer chains, address-of nested fields

### BUG-007: Ring push wrote wrong size (`sizeof(int)` instead of `sizeof(u8)`)
- **Symptom:** `Ring(u8, 16)` push/pop FIFO returned wrong values — only first element correct
- **Root cause:** Emitter used `__auto_type` for push temp variable, which deduced `int` (4 bytes). `memcpy` then wrote 4 bytes per element into 1-byte slots, corrupting adjacent data.
- **Fix:** Emit the actual ring element type for the push temp variable
- **Test:** `test_firmware_patterns.c` — ring push/pop FIFO order

---

## Round 2 — Firmware Pattern Stress Tests (2026-03-19)

### BUG-008: Pointer indexing `(*u32)[i]` rejected
- **Symptom:** `data[0]` on `*u32` pointer fails with "cannot index type '*u32'"
- **Root cause:** Checker `NODE_INDEX` only handled TYPE_ARRAY and TYPE_SLICE, not TYPE_POINTER
- **Fix:** Added TYPE_POINTER case returning `pointer.inner`
- **Test:** `test_firmware_patterns2.c` — array passed via &arr[0]

### BUG-009: `@size(StructName)` emitted empty `sizeof()`
- **Symptom:** GCC error: `sizeof()` with no argument
- **Root cause:** Parser excluded `TOK_IDENT` from type_arg detection (line: `p->current.type != TOK_IDENT`). Named types like `Header` were parsed as expression args, not type_arg. Emitter only checked type_arg.
- **Fix:** Emitter falls back to looking up args[0] as a type name when type_arg is NULL
- **Test:** `test_firmware_patterns2.c` — @size(Header)

### BUG-010: Forward function declarations not supported
- **Symptom:** `u32 func(u32 n);` (with semicolon, no body) fails to parse — "expected '{'"
- **Root cause:** Parser unconditionally called `parse_block()` after parameter list
- **Fix:** Check for semicolon before `parse_block()`. If found, set body to NULL (forward decl)
- **Test:** `test_firmware_patterns2.c` — mutual recursion with forward decl

### BUG-011: Forward decl followed by definition = "redefinition"
- **Symptom:** Forward declare then define same function → checker error
- **Root cause:** `add_symbol` rejects duplicate names unconditionally
- **Fix:** Before adding, check if existing symbol is a forward-declared function (no body). If so, update it with the new body instead of erroring.
- **Test:** `test_firmware_patterns2.c` — mutual recursion

### BUG-012: break/continue emitted ALL defers (including outer scope)
- **Symptom:** `for { defer f(); for { break; } }` — inner break fires outer defer
- **Root cause:** `emit_defers()` emitted from index 0 (all defers). Break should only emit defers within the loop scope.
- **Fix:** Added `loop_defer_base` to Emitter. Loops save/restore it. Break/continue use `emit_defers_from(e, e->loop_defer_base)` instead of `emit_defers(e)`. Return still emits all.
- **Test:** `test_firmware_patterns2.c` — inner break + outer defer

---

## Round 3 — Firmware Pattern Stress Tests (2026-03-19)

### BUG-013: `return ring.pop()` from `?u8` function double-wraps optional
- **Symptom:** `?u8 uart_recv() { return rx_buf.pop(); }` emits `return (_zer_opt_u8){ <already_opt>, 1 }` — GCC error
- **Root cause:** Emitter always wraps return value in `{expr, 1}` for `?T` functions, even when expr is already `?T`
- **Fix:** Check if return expr's type already matches function return type via `checker_get_type` + `type_equals`. If so, return directly without wrapping.
- **Test:** `test_firmware_patterns3.c` — UART loopback with ring.pop() return

---

## Linked List Session (2026-03-19)

### BUG-014: Self-referential structs fail — "undefined type 'Node'"
- **Symptom:** `struct Node { ?*Node next; }` — "undefined type 'Node'" on the `?*Node` field
- **Root cause:** `register_decl` resolved field types BEFORE registering the struct name in scope. So `Node` wasn't in scope when its own field `?*Node` was resolved.
- **Fix:** Move `add_symbol` BEFORE field type resolution for both structs and unions.
- **Test:** `ZER-Test/linked_list.zer` — doubly linked list with ?*Node prev/next

### BUG-015: `orelse` precedence lower than `=` — assignment eats the orelse
- **Symptom:** `current = current.next orelse return` parsed as `(current = current.next) orelse return` instead of `current = (current.next orelse return)`
- **Root cause:** Precedence table had PREC_ORELSE below PREC_ASSIGN. Assignment consumed `current.next` as its RHS, leaving `orelse return` outside.
- **Debugging:** Confirmed via targeted debug: auto-deref returned kind=14 (TYPE_OPTIONAL) for `current.next`, but orelse handler received kind=13 (TYPE_POINTER). Typemap overwrite debug showed NO overwrites. This proved the orelse was receiving a different expression (`current` not `current.next`).
- **Fix:** Swap PREC_ASSIGN and PREC_ORELSE in the precedence enum. Update `parse_expression` to start at PREC_ASSIGN.
- **Test:** `ZER-Test/test_walk.zer` — linked list traversal with `current = current.next orelse return`

### BUG-016: Slice-to-pointer decay missing for C interop
- **Symptom:** `void puts(*u8 s); puts("Hello World");` — "expected '*u8', got '*u8'" (string literal is []u8, not *u8)
- **Root cause:** No implicit coercion from []T to *T. String literals are const []u8.
- **Fix:** Added []T → *T coercion in `can_implicit_coerce`. Emitter appends `.ptr` at call site when passing slice to pointer param. Pure extern forward declarations (no body) skipped in emission to avoid <stdio.h> conflicts.
- **Test:** Hello World: `void puts(*u8 s); puts("Hello World");` compiles and runs

---

## OS/Kernel Pattern Session (2026-03-19)

### BUG-017: `orelse return` in `?T` function emitted `return 0` instead of `return (?T){0,0}`
- **Symptom:** `?u32 task_create() { Handle h = pool.alloc() orelse return; ... }` — GCC error, `return 0` incompatible with `_zer_opt_u32`
- **Root cause:** Orelse-return emission only checked for void vs non-void. Didn't distinguish `?T` return type needing `{0, 0}`.
- **Fix:** Added TYPE_OPTIONAL check in orelse-return emission path.
- **Test:** `ZER-Test/scheduler.zer` — Pool-based task scheduler

### BUG-018: `Ring(Struct).pop()` return causes GCC anonymous struct mismatch
- **Symptom:** `?Event poll_event() { return event_queue.pop(); }` — GCC error, two anonymous structs with same layout but different types
- **Root cause:** `?StructName` emitted as anonymous `struct { ... }` everywhere, creating incompatible types for same layout.
- **Fix:** Named typedef `_zer_opt_StructName` emitted after every struct declaration. `emit_type` for TYPE_OPTIONAL(TYPE_STRUCT) uses the named typedef.
- **Test:** `ZER-Test/event_queue.zer` — Ring(Event) with enum dispatch

### BUG-019: Assigning `u32` to `?u32` emitted bare value (no optional wrapping)
- **Symptom:** `?u32 best = null; best = some_value;` — GCC error, assigning `uint32_t` to `_zer_opt_u32`
- **Root cause:** NODE_ASSIGN emission had no T→?T wrapping logic.
- **Fix:** Added optional wrapping in NODE_ASSIGN: if target is `?T` and value is `T`, emit `(type){value, 1}`. For null, emit `{0, 0}`.
- **Test:** `ZER-Test/net_stack.zer` — routing table with `?u32 best_gateway`

---

## Multi-Module Session (2026-03-19)

### BUG-020: Imported module enums/unions not emitted in C output
- **Symptom:** `DeviceStatus.offline` in imported module → GCC error `'DeviceStatus' undeclared`
- **Root cause:** `emit_file_no_preamble` only handled NODE_STRUCT_DECL, NODE_FUNC_DECL, NODE_GLOBAL_VAR. Missing NODE_ENUM_DECL (#define constants) and NODE_UNION_DECL.
- **Fix:** Added enum #define emission, union struct emission, and extern forward-decl skipping to `emit_file_no_preamble`.
- **Test:** `ZER-Test/multi/driver.zer` — imports device.zer with enum DeviceStatus

### BUG-020.1: Emitter enum value fallback for imported modules
- **Symptom:** `DeviceStatus.offline` emitted as `DeviceStatus.offline` (invalid C) instead of `_ZER_DeviceStatus_offline` in imported module functions
- **Root cause:** `checker_get_type(node->field.object)` returned NULL for imported module nodes — typemap had no entries. Enum value detection in NODE_FIELD failed.
- **Fix:** Added scope_lookup fallback in NODE_FIELD: if checker_get_type returns NULL and object is NODE_IDENT, look up the identifier in global scope.
- **Test:** `ZER-Test/multi/driver.zer` — enum values in imported module functions

### BUG-021: Imported module function bodies never type-checked
- **Symptom:** `gpio.mode = mode` in imported function emitted `gpio.mode` (dot) instead of `gpio->mode` (arrow) — pointer auto-deref failed
- **Root cause:** Only `checker_check` was called on the main file. Imported modules only had `checker_register_file` (declarations only, no function bodies). Typemap had no entries for imported module expressions.
- **Fix:** Added `checker_check_bodies()` — checks function bodies without re-registering declarations. Called on all imported modules before main.
- **Test:** `ZER-Test/multi/firmware.zer` — imported HAL functions with pointer params

### BUG-022: Main module registered before imports → types undefined
- **Symptom:** `ErrCode init_system()` in main file → "undefined type 'ErrCode'" even though error.zer is imported
- **Root cause:** `checker_register_file` processed modules in order [main, imports...]. Main's function signatures resolved before imported types were in scope.
- **Fix:** Register imported modules first (loop from index 1), then main module (index 0).
- **Test:** `ZER-Test/multi/firmware.zer` — uses ErrCode from error.zer in function signature

---

## Edge Case Session (2026-03-19)

### BUG-023: Enum value rejected as array index
- **Symptom:** `arr[Color.red]` → "array index must be integer, got 'Color'"
- **Root cause:** `type_is_integer()` didn't include TYPE_ENUM. Enums are i32 internally but weren't recognized as integers.
- **Fix:** Added TYPE_ENUM to `type_is_integer`, `type_is_signed`, and `type_width` (32-bit signed).
- **Test:** `ZER-Test/edge_cases.zer` — enum as array index

### BUG-024: `??u32` (nested optional) accepted but emits invalid C
- **Symptom:** `??u32` compiles but emits anonymous struct wrapping another anonymous struct — GCC rejects
- **Root cause:** Checker's `resolve_type` for TYNODE_OPTIONAL didn't reject optional-of-optional
- **Fix:** Added check in resolve_type: if inner type is already TYPE_OPTIONAL, emit error "nested optional '??T' is not supported"
- **Test:** `ZER-Test/test_opt_opt.zer` — rejected at compile time

---

## Spec Audit — Missing Features (2026-03-20)

### BUG-025: Function pointer declarations not parseable
- **Symptom:** `void (*callback)(u32 event);` fails to parse — "expected expression" error. Spec §13 vtable pattern impossible to write.
- **Root cause:** Parser had `/* TODO: function pointer declarations */` at line 1121. AST node `TYNODE_FUNC_PTR`, type system, checker, and emitter all supported function pointers, but the parser never created the node. No call site (struct fields, var decls, parameters, top-level) handled `type (*name)(params...)` syntax.
- **Fix:** Added `parse_func_ptr_after_ret()` helper. Added function pointer detection at 4 sites: `parse_func_or_var` (global), `parse_var_decl` (local), struct field parsing, and function parameter parsing. Fixed `emit_type_and_name` to emit correct C syntax `ret (*name)(params)`. Added lookahead in statement parser to detect `type (* ...` as var decl.
- **Test:** `test_emit.c` — 6 E2E tests (local var, reassign, parameter, struct field vtable, global, callback registration). `test_parser_edge.c` — 5 parser tests.

### BUG-026: `arena.alloc(T)` returns `void` instead of `?*T`
- **Symptom:** `Arena(1024) a; ?*Task t = a.alloc(Task);` — type checker accepts but emitter produces invalid C. `alloc()` resolved to `void` return type, so the optional wrapping was wrong.
- **Root cause:** Checker's builtin method handler for `alloc` on Arena types returned `ty_void` unconditionally. It didn't resolve the type argument from the call's `NODE_IDENT` arg via `scope_lookup`.
- **Fix:** Added type resolution in the `alloc` method handler: look up the type name argument via `scope_lookup`, then return `type_optional(type_pointer(sym->type))` — i.e., `?*T`.
- **Test:** `test_checker_full.c` — arena alloc type resolution

---

## Comprehensive Audit — Bugs 027-035 (2026-03-21)

### BUG-027: `arena.alloc_slice(T, n)` returns `void` instead of `?[]T`
- **Symptom:** Same class as BUG-026. `alloc_slice` placeholder in NODE_FIELD returned `ty_void`, but no NODE_CALL handler existed to resolve the actual type.
- **Root cause:** Missing `alloc_slice` handler in checker.c NODE_CALL Arena methods section.
- **Fix:** Added `alloc_slice` handler: look up type arg via `scope_lookup`, return `type_optional(type_slice(sym->type))`.
- **Test:** `test_checker_full.c` — arena alloc_slice type resolution

### BUG-028: `type_name()` single static buffer corrupts error messages
- **Symptom:** `"expected %s, got %s", type_name(a), type_name(b)` prints the same type for both — second call overwrites first buffer.
- **Root cause:** Single `type_name_buf[256]` used by all calls.
- **Fix:** Two alternating buffers (`type_name_buf0`, `type_name_buf1`) with a toggle counter.
- **Test:** Implicit — all checker error messages with two types now display correctly.

### BUG-029: `?void` bare return emits `{ 0, 1 }` for single-field struct
- **Symptom:** `_zer_opt_void` has only `has_value` field, but `return;` in `?void` function emitted `{ 0, 1 }` (2 initializers). GCC: "excess elements in struct initializer".
- **Root cause:** Return emission didn't distinguish `?void` from other `?T` types.
- **Fix:** Check if inner type is `TYPE_VOID` — emit `{ 1 }` for bare return, `{ 0 }` for return null. Also fixed if-unwrap to not access `.value` on `?void`.
- **Test:** `test_emit.c` — ?void bare return and return null E2E tests

### BUG-030: `?bool` has no named typedef
- **Symptom:** `?bool` fell to anonymous struct fallback in `emit_type`, causing type mismatch when mixing `?bool` values.
- **Root cause:** Missing `TYPE_BOOL` case in optional typedef switch.
- **Fix:** Added `_zer_opt_bool` typedef in preamble and `TYPE_BOOL` case in `emit_type`.
- **Test:** `test_emit.c` — ?bool function returning and unwrapping

### BUG-031: `@saturate` for signed types was just a C cast (UB)
- **Symptom:** `@saturate(i8, 200)` emitted `(int8_t)_zer_sat0` — undefined behavior if value out of range.
- **Root cause:** Signed path had "just cast for now" placeholder.
- **Fix:** Proper min/max clamping ternaries per signed width (i8: -128..127, i16: -32768..32767, i32: full range). Also fixed unsigned u32/u64 path that had broken control flow.
- **Test:** `test_emit.c` — @saturate(i8, 200)=127, @saturate(u8, 300)=255

### BUG-032: Optional var init with NODE_IDENT skips wrapping
- **Symptom:** `?u32 x = some_u32_var;` emitted without `{val, 1}` wrapper — GCC type mismatch.
- **Root cause:** Emitter assumed NODE_IDENT init "might already be ?T" and skipped wrapping unconditionally.
- **Fix:** Use `checker_get_type` to check if ident is already optional. If not, wrap it.
- **Test:** `test_emit.c` — ?u32 from plain u32 var and from optional var

### BUG-033: Float literal `%f` loses precision
- **Symptom:** `f64 pi = 3.141592653589793;` emitted as `3.141593` (6 decimal places).
- **Root cause:** `emit(e, "%f", ...)` default precision.
- **Fix:** Changed to `"%.17g"` for full double round-trip precision.
- **Test:** `test_emit.c` — f64 precision check

### BUG-034: `emit_type` for TYPE_FUNC_PTR produces incomplete C
- **Symptom:** Direct `emit_type` call for func ptr emitted `ret (*` with no parameter list or closing paren.
- **Root cause:** `emit_type` left name and params to caller, but not all callers use `emit_type_and_name`.
- **Fix:** `emit_type` now emits complete anonymous func ptr type: `ret (*)(params...)`.
- **Test:** `test_emit.c` — func ptr as parameter compiles correctly

### BUG-035: ZER-CHECK if/else merge false positives
- **Symptom:** Handle freed on only ONE branch of if/else was marked as FREED — false positive for subsequent use.
- **Root cause:** Merge condition used `||` (either branch) instead of `&&` (both branches).
- **Fix:** Only mark freed if freed on BOTH branches (under-approximation per design doc). Also added switch arm merge with ALL-arms-must-free logic. Added NODE_INTERRUPT body checking.
- **Test:** `test_zercheck.c` — one-branch free OK, both-branch use-after-free detected, switch merge tests

### Pool/Ring scope fix
- **Symptom:** Pool/Ring builtin method emission only looked up `global_scope`, breaking for local variables.
- **Root cause:** Emitter and zercheck used `scope_lookup(global_scope, ...)` only.
- **Fix:** Try `checker_get_type` first (works for any scope), fall back to global_scope.
- **Test:** Implicit — all existing Pool/Ring tests pass with new lookup path

## Arena E2E + Gap Fixes (2026-03-21)

### Arena E2E emission (feature)
- **Symptom:** Arena methods (alloc, alloc_slice, over, reset) type-checked but emitter output literal method calls → GCC rejected.
- **Root cause:** Emitter had no Arena method interception — Pool and Ring had it, Arena didn't.
- **Fix:** Added `_zer_arena` typedef + `_zer_arena_alloc()` runtime helper in preamble. Added method emission for `Arena.over(buf)`, `arena.alloc(T)`, `arena.alloc_slice(T, n)`, `arena.reset()`, `arena.unsafe_reset()`. Added `TOK_ARENA` in parser expression context. Added "Arena" symbol in checker global scope.
- **Test:** `test_emit.c` — 5 Arena E2E tests (alloc, alloc_slice, reset, exhaustion, multiple allocs)

### BUG-036: Slice indexing emits `slice[i]` instead of `slice.ptr[i]`
- **Symptom:** Indexing a `[]T` slice variable emitted `items[0]` — GCC rejected because `items` is a struct, not an array.
- **Root cause:** `NODE_INDEX` emission in `emit_expr` didn't check if object was a slice type.
- **Fix:** Added `TYPE_SLICE` check in NODE_INDEX: emit `.ptr` suffix when indexing a slice.
- **Test:** `test_emit.c` — arena.alloc_slice exercises slice indexing

### BUG-037: Slice `orelse return` unwrap uses anonymous struct incompatible types
- **Symptom:** `[]Elem items = expr orelse return;` → GCC error: "invalid initializer" — two distinct anonymous structs treated as incompatible.
- **Root cause:** Var decl orelse unwrap emitted `struct { T* ptr; size_t len; } items = _zer_or0.value;` — GCC treats the anonymous struct in the optional and the declared type as different types.
- **Fix:** Use `__auto_type` for slice type unwrap to inherit the exact type from the optional's `.value`.
- **Test:** `test_emit.c` — arena.alloc_slice with orelse return

### BUG-038: `?void orelse return` accesses non-existent `.value` field
- **Symptom:** `push_checked(x) orelse return;` → GCC error: `_zer_opt_void has no member named 'value'`.
- **Root cause:** Expression-level NODE_ORELSE handler emitted `_zer_tmp.value` for all non-pointer optionals, but `_zer_opt_void` is `{ has_value }` only — no value field.
- **Fix:** Added `is_void_optional` check in NODE_ORELSE expression handler. For `?void orelse return/break/continue`, emit inline `if (!has_value) { return; }` instead of extracting `.value`.
- **Test:** `test_emit.c` — ring.push_checked orelse return

### ring.push_checked() emission (feature)
- **Symptom:** `ring.push_checked(val)` type-checked as `?void` but emitter had no handler → fell through to generic call emission → GCC rejected.
- **Root cause:** Missing emitter case for push_checked alongside push and pop.
- **Fix:** Added `push_checked` handler in Ring method emission block. Checks `count < capacity` before pushing; returns `_zer_opt_void` with `has_value=1` on success, `{0}` on full.
- **Test:** `test_emit.c` — push_checked success + push_checked full ring returns null

### @container E2E test (test coverage)
- **Symptom:** `@container(*T, ptr, field)` had emitter implementation but no E2E test.
- **Fix:** Added E2E test: recover `*Node` from `&n.y` using @container, verify field access.
- **Test:** `test_emit.c` — @container recover Node from field pointer

### BUG-039: Arena alignment uses fixed `sizeof(void*)` instead of type alignment
- **Symptom:** `arena.alloc(T)` always aligned to pointer width. Types requiring stricter alignment (or smaller types wasting space on lax alignment) not handled.
- **Root cause:** `_zer_arena_alloc` hardcoded `sizeof(void*)` as alignment.
- **Fix:** Added `align` parameter to `_zer_arena_alloc()`. Call sites now pass `_Alignof(T)` for natural type alignment. ARM Cortex-M0 unaligned access faults are prevented.
- **Test:** `test_emit.c` — alloc Byte(u8) then Word(u32), verify Word is accessible (would fault on strict-alignment targets without fix)

### BUG-040: Signed integer overflow is undefined behavior in emitted C
- **Symptom:** ZER spec says `i32` overflow wraps. But emitted C uses raw `int32_t + int32_t` which is UB in C99. GCC at `-O2` can optimize assuming no signed overflow, breaking ZER's wrapping guarantee.
- **Root cause:** Emitter outputs plain arithmetic operators without wrapping protection.
- **Fix:** Added `-fwrapv` to GCC invocation in `zerc --run` and test harness. Added compile hint in emitted C preamble. This makes GCC treat signed overflow as two's complement wrapping, matching ZER semantics.
- **Test:** `test_emit.c` — `i8 x = 127; x = x + 1;` wraps to -128, bitcast to u8 = 128

### BUG-077: Mutable union capture `|*v|` modifies copy, not original
- **Symptom:** `switch (msg) { .command => |*cmd| { cmd.code = 99; } }` — mutation doesn't persist because switch copies the union value.
- **Root cause:** Union switch emitted `__auto_type _zer_sw = expr` (value copy). Mutable capture's pointer pointed to the copy.
- **Fix:** Union switch now emits `__auto_type *_zer_swp = &(expr)` (pointer to original). Captures read/write through `_zer_swp->variant`.

### BUG-076: Union switch mutable capture `|*v|` emitted `__auto_type *v` — GCC rejects
- **Symptom:** `switch (m) { .sensor => |*v| { v.temp = 99; } }` — GCC error: `__auto_type *v` is not valid in this context.
- **Root cause:** Mutable capture emitted `__auto_type *v = &union.field` — GCC rejects `__auto_type` with pointer declarator in some contexts.
- **Fix:** Look up actual variant type from union definition, emit `SensorReading *v = &_zer_swp->sensor` instead.

### BUG-075: `?Handle(T)` optional emits anonymous struct — GCC type mismatch
- **Symptom:** `?Handle(Task) h = pool.alloc() orelse return;` — `?Handle(T)` emits anonymous struct instead of named `_zer_opt_u32`. GCC type mismatch between function return and variable.
- **Root cause:** `emit_type(TYPE_OPTIONAL)` inner switch had no `case TYPE_HANDLE:`. Handle is u32 internally, fell to anonymous struct default.
- **Fix:** Added `case TYPE_HANDLE: emit("_zer_opt_u32");`.

### BUG-074: `TYPE_DISTINCT` not unwrapped for function call dispatch
- **Symptom:** Calling through a distinct typedef function pointer: `SafeOp op = @cast(SafeOp, add); op(20, 22);` — checker returns `ty_void`, emitter emits wrong C variable declaration syntax.
- **Root cause:** Checker's NODE_CALL handler and emitter's `emit_type_and_name` + call arg coercion only checked `TYPE_FUNC_PTR`, not `TYPE_DISTINCT` wrapping it.
- **Fix:** Checker unwraps distinct before checking `TYPE_FUNC_PTR` for call dispatch. Emitter unwraps in `emit_type_and_name` for name placement and in call arg emission for decay/coercion checks.

### BUG-073: `distinct typedef` does not support function pointer syntax
- **Symptom:** `distinct typedef u32 (*Callback)(u32);` fails to parse — distinct path expects ident immediately after type.
- **Root cause:** The `distinct` typedef path didn't have the `(*` function pointer detection that the non-distinct path had.
- **Fix:** Added function pointer detection to distinct typedef path (same pattern as non-distinct).

### BUG-072: Missing `_zer_opt_slice_` typedef for unions in `emit_file_no_preamble`
- **Symptom:** Imported module defines a union, main module uses `?[]UnionName` — GCC error: undefined `_zer_opt_slice_UnionName`.
- **Root cause:** `emit_file_no_preamble` emitted `_zer_opt_` and `_zer_slice_` for unions but not `_zer_opt_slice_`. The main `emit_file` path had all three.
- **Fix:** Added `_zer_opt_slice_UnionName` emission after `_zer_slice_UnionName` in `emit_file_no_preamble`.

### BUG-071: Function pointer typedef not supported
- **Symptom:** `typedef u32 (*Callback)(u32);` fails to parse — parser's typedef path only calls `parse_type()` which doesn't handle function pointer syntax.
- **Root cause:** typedef declaration parsed return type then expected an ident name, but func ptr names go inside `(*)`.
- **Fix:** Added `(*` detection in typedef path (same pattern as var-decl/param/field). Emitter uses `emit_type_and_name` for typedef emission.

### BUG-070: `?FuncPtr` not supported — function pointers always nullable
- **Symptom:** `?void (*cb)(u32)` parsed `?` as wrapping `void` (return type), not the whole function pointer.
- **Root cause:** Parser's `?` attaches to the next type token, but function pointer declarations have the type split around the name.
- **Fix:** All 4 func-ptr parse sites (local, global, struct field, param) detect `?T` prefix, unwrap it, parse func ptr with inner return type, then wrap result in TYNODE_OPTIONAL. Emitter uses `IS_NULL_SENTINEL` macro (TYPE_POINTER || TYPE_FUNC_PTR) at every null-sentinel check.

### BUG-069: All `[]T` slice types use anonymous structs — type mismatch across functions
- **Symptom:** `[]Task` emitted as anonymous `struct { Task* ptr; size_t len; }` — each use creates a different C type, GCC rejects assignments/parameters between them.
- **Root cause:** Only `[]u8` and `[]u32` had named typedefs. All other slice types used anonymous structs.
- **Fix:** Added `_zer_slice_T` typedefs for all primitives in preamble. Struct/union declarations emit `_zer_slice_StructName`. `?[]T` also gets `_zer_opt_slice_T` typedefs. `emit_type(TYPE_SLICE)` uses named typedefs for all types.

### BUG-068: Explicit enum values (`enum { a = 5 }`) silently emit wrong constants
- **Symptom:** `enum Prio { low = 1, med = 5, high = 10 }` emits `#define _ZER_Prio_low 0`, `_ZER_Prio_med 1`, `_ZER_Prio_high 2` — uses loop index instead of declared value.
- **Root cause:** Emitter's enum `#define` loop uses `j` (loop counter) as the value, ignoring `v->value` from the AST. Parser and checker already handled explicit values correctly.
- **Fix:** Emitter now reads `v->value->int_lit.value` when present, with auto-increment for implicit values after explicit ones. Fixed in both `emit_file` and `emit_file_no_preamble`.
- **Test:** `test_emit.c` — explicit values (1,5,10) and gaps with auto-increment (0,100,101,102)

### BUG-067: `*Union` pointer auto-deref returns `ty_void` in checker
- **Symptom:** `*Msg p = &msg; p.sensor = s;` fails with "cannot assign 'S' to 'void'" — checker doesn't auto-deref pointers to unions.
- **Root cause:** Pointer auto-deref path (line 982) only handled `TYPE_POINTER` where inner is `TYPE_STRUCT`, not `TYPE_UNION`.
- **Fix:** Added parallel auto-deref block for `TYPE_UNION` inner — looks up variant by name, returns variant type.

### BUG-066: Var-decl `orelse return` in `?void` function emits `{ 0, 0 }`
- **Symptom:** `u32 val = get() orelse return;` inside a `?void` function emits `return (_zer_opt_void){ 0, 0 };` — excess initializer for 1-field struct.
- **Root cause:** Var-decl orelse-return path had no `TYPE_VOID` check (the other 3 paths had it).
- **Fix:** Added `inner->kind == TYPE_VOID` → `{ 0 }` instead of `{ 0, 0 }`.

### BUG-065: Union switch `|*v|` mutable capture emits value copy
- **Symptom:** `switch (m) { .sensor => |*v| { v.temp = 99; } }` — mutation silently dropped. Emitted C copies the variant value, mutations go to the copy.
- **Root cause:** Capture always emitted `__auto_type v = union.field` regardless of `capture_is_ptr`.
- **Fix:** When `capture_is_ptr`, emit `__auto_type *v = &union.field` instead.

### BUG-064: `volatile` qualifier completely stripped from emitted C
- **Symptom:** `volatile *u32 reg = @inttoptr(...)` emits as `uint32_t* reg` — no volatile keyword. GCC optimizes away MMIO reads/writes.
- **Root cause:** Parser consumes `volatile` as a var-decl flag (`is_volatile`), not as part of the type node. Emitter never checked `is_volatile` to emit the keyword.
- **Fix:** `emit_global_var` and `emit_stmt(NODE_VAR_DECL)` propagate `is_volatile` to pointer type. `emit_type(TYPE_POINTER)` emits `volatile` prefix when `is_volatile` is set.

### BUG-063: Expression-level `orelse return/break/continue` skips defers
- **Symptom:** `defer cleanup(); get_val() orelse return;` — cleanup never called because the expression-level orelse handler emits `return` without `emit_defers()`.
- **Root cause:** Var-decl orelse path had `emit_defers()` but expression-level path in `emit_expr(NODE_ORELSE)` did not.
- **Fix:** Added `emit_defers()` before return and `emit_defers_from()` before break/continue in both void and non-void expression orelse paths.

### BUG-062: `?UnionType` optional emits anonymous struct — GCC type mismatch
- **Symptom:** `?Msg` (optional union) emits anonymous `struct { ... }` at each use — incompatible types.
- **Root cause:** `emit_type(TYPE_OPTIONAL)` had no `case TYPE_UNION:`. Union declarations didn't emit `_zer_opt_UnionName` typedef.
- **Fix:** Added `case TYPE_UNION:` → `_zer_opt_UnionName`. Added typedef emission after union declarations.

### BUG-061: Compound `u8 += u64` accepted — silent narrowing
- **Symptom:** Compound assignment didn't check type width compatibility. `u8 += u64` silently truncated.
- **Root cause:** Compound assignment only checked `type_is_numeric()`, not width compatibility.
- **Fix:** Added narrowing check (reject when value wider than target), with literal exemption (`u8 += 1` is fine).

### BUG-060: Const capture field mutation bypasses const check
- **Symptom:** `if (opt) |pt| { pt.x = 99; }` accepted — const-captured struct field modified.
- **Root cause:** Const check only examined `NODE_IDENT` targets, not field/index chains.
- **Fix:** Walk field/index chain to root ident, check const. Allow mutation through pointers (auto-deref).

### BUG-059: `@truncate`/`@saturate` accept non-numeric source
- **Symptom:** `@truncate(u8, some_struct)` accepted — struct passed to truncate.
- **Root cause:** No source type validation in intrinsic handlers.
- **Fix:** Validate source is numeric (unwrap distinct types before checking).

### BUG-058: Union switch arm variant names never validated
- **Symptom:** `.doesnt_exist =>` in union switch accepted — nonexistent variant.
- **Root cause:** Union switch arms skipped name validation entirely.
- **Fix:** Validate each arm's variant name against the union's variant list.

### BUG-057: Union switch exhaustiveness counts duplicates
- **Symptom:** `.sensor, .sensor =>` counts as 2 handled, hiding missing `.command`.
- **Root cause:** Raw `value_count` sum instead of unique variant tracking.
- **Fix:** Bitmask-based deduplication (same approach as enum fix BUG-048).

### BUG-056: Bitwise compound `&= |= ^= <<= >>=` accepted on floats
- **Symptom:** `f32 x = 1.0; x &= 2;` compiles — GCC rejects the emitted C.
- **Root cause:** Compound assignment only checked `type_is_numeric()`, which includes floats.
- **Fix:** Added explicit check: bitwise compound ops require integer types.

### BUG-055: `@cast` — parser excluded TOK_IDENT from type_arg
- **Symptom:** `@cast(Fahrenheit, c)` fails — checker returns ty_void because type_arg is NULL.
- **Root cause:** Parser's `is_type_token && type != TOK_IDENT` guard excluded all named types from being parsed as type_arg.
- **Fix:** Added `force_type_arg` for `@cast` intrinsic, allowing TOK_IDENT to be parsed as type.

### BUG-054: Array-to-slice coercion missing at call sites, var-decl, and return
- **Symptom:** `process(buf)` where buf is `u8[N]` and param is `[]u8` — GCC type mismatch.
- **Root cause:** Emitter passed raw array pointer instead of wrapping in slice compound literal.
- **Fix:** Added `emit_array_as_slice()` helper. Applied at 3 sites: call args, var-decl init, return.

### BUG-053: Slice-of-slice missing `.ptr` + open-end slice on slices
- **Symptom:** `data[1..3]` on a `[]u8` parameter emits `&(data)[1]` — subscript on struct.
- **Root cause:** Slice emission didn't add `.ptr` for slice-type objects. Open-end `slice[start..]` emitted length `0`.
- **Fix:** Added `.ptr` when object is TYPE_SLICE. Added `slice.len - start` for open-end on slices.

### BUG-052: `?T orelse return` as expression — guard completely missing
- **Symptom:** `get_val() orelse return;` emits `({ auto t = expr; t.value; })` — no guard, no return.
- **Root cause:** Non-void, non-pointer path in expression-level orelse handler extracted `.value` unconditionally.
- **Fix:** Added `if (!has_value) { return; }` guard with correct return type wrapping.

### BUG-051: `?void` var-decl null init emits wrong initializer
- **Symptom:** `?void x = null;` (global) emits `= 0` (scalar for struct). Local emits `{ 0, 0 }` (2 fields for 1-field struct).
- **Root cause:** Global path called `emit_expr(NULL_LIT)` which emits scalar 0. Local path didn't check for TYPE_VOID.
- **Fix:** Both paths now check `inner == TYPE_VOID` → emit `{ 0 }`.

### BUG-050: `@bitcast` accepts mismatched widths
- **Symptom:** `@bitcast(i64, u32_val)` accepted — spec requires same width.
- **Root cause:** No width validation in checker's @bitcast handler.
- **Fix:** Compare `type_width(target)` vs `type_width(source)`, error if different.

### BUG-049: Bool switch checks arm count, not actual coverage
- **Symptom:** `switch (x) { true => {} true => {} }` accepted — false never handled.
- **Root cause:** Checked `arm_count < 2` instead of tracking which values are covered.
- **Fix:** Track `has_true`/`has_false` flags from actual arm values.

### BUG-048: Enum switch exhaustiveness tricked by duplicate variants
- **Symptom:** `.idle, .idle =>` counts as 2, masking missing variants.
- **Root cause:** Raw `value_count` sum instead of unique variant tracking.
- **Fix:** Bitmask-based deduplication — each variant index tracked as a bit.

### BUG-047: `bool x = 42` accepted — int literal coerces to bool
- **Symptom:** Integer literal assigned to bool variable without error.
- **Root cause:** `is_literal_compatible` had `NODE_INT_LIT && TYPE_BOOL → true`.
- **Fix:** Removed that rule. Only `true`/`false` literals can initialize bool.

### BUG-046: `@trap()` rejected as unknown intrinsic
- **Symptom:** `@trap()` fails with "unknown intrinsic '@trap'" at checker.
- **Root cause:** Checker had no handler for `@trap` — fell to the `else` branch that reports unknown intrinsics.
- **Fix:** Added `@trap` handler in checker (returns `ty_void`) and emitter (emits `_zer_trap("explicit trap", __FILE__, __LINE__)`).
- **Test:** `test_emit.c` — conditional @trap skipped = 42

### BUG-045: Non-u8/u32 array slicing emits `void*` pointer — type mismatch
- **Symptom:** `u32[8]` sliced with `arr[0..3]` emits `struct { void* ptr; size_t len; }`, incompatible with `_zer_slice_u32`.
- **Root cause:** Slice emission only checked for u8, everything else got `void*` and an anonymous struct.
- **Fix:** Added u32 check → `_zer_slice_u32`. Other types use typed pointer instead of `void*`.
- **Test:** `test_emit.c` — u32 array slicing arr[0..3] → []u32, sum = 60

### BUG-044: Slice variables auto-zero emits `= 0` instead of `= {0}`
- **Symptom:** `[]u8 s;` (global or local) emits `_zer_slice_u8 s = 0;` — invalid initializer for struct.
- **Root cause:** `TYPE_SLICE` missing from the compound-type condition in both local and global auto-zero paths.
- **Fix:** Added `TYPE_SLICE` to both conditions.
- **Test:** `test_emit.c` — global+local slice auto-zero, len=0

### BUG-043: `?void` assign null emits `{ 0, 0 }` — excess initializer
- **Symptom:** `status = null;` where status is `?void` emits `(_zer_opt_void){ 0, 0 }` — 2 initializers for 1-field struct.
- **Root cause:** Assign-null path didn't special-case `?void` (which has only `has_value`, no `value` field).
- **Fix:** Check `inner->kind == TYPE_VOID` → emit `{ 0 }` instead of `{ 0, 0 }`.
- **Test:** `test_emit.c` — ?void assign null, has_value=0

### BUG-042: `?Enum` optional emits anonymous struct — GCC type mismatch
- **Symptom:** `?Status` (optional enum) emits `struct { int32_t value; uint8_t has_value; }` everywhere. Each anonymous struct is a different C type, causing "incompatible types" errors on return and assignment.
- **Root cause:** `emit_type` TYPE_OPTIONAL handler had no `case TYPE_ENUM:` — fell to `default` anonymous struct fallback.
- **Fix:** Added `case TYPE_ENUM: emit("_zer_opt_i32");` since enums are int32_t underneath.
- **Test:** `test_emit.c` — enum switch inside if-unwrap: `?Status` returned from function, unwrapped, switched on

### BUG-041: Bit extraction `[31..0]` emits `1u << 32` — undefined behavior
- **Symptom:** Full-width bit extraction `x[31..0]` on u32 emits `(1u << 32)` which is UB (shift equals type width).
- **Root cause:** Mask formula used `1u` (32-bit) which overflows when shift count reaches 32.
- **Fix:** Changed `1u` to `1ull` (64-bit) so shifts up to 63 are safe.
- **Test:** `test_emit.c` — `[0..0]` single bit, `[7..0]` low byte, `[15..8]` mid-range

---

## Session 3 — Red Team Audit (2026-03-26)

266 bugs fixed total. 10 structural refactors (RF1-RF10). 1,685 tests.

### BUG-248: Union assignment during switch capture
- **Symptom:** `msg = other` inside `|*v|` arm compiles — invalidates capture pointer.
- **Root cause:** NODE_ASSIGN didn't check if target matches `union_switch_var`.
- **Fix:** Walk target to root ident, compare against locked variable name.
- **Test:** `test_checker_full.c` — union assign in capture rejected, field mutation accepted.

### BUG-249: Switch capture doesn't propagate safety flags
- **Symptom:** `switch(opt) { default => |p| { return p; } }` — p doesn't inherit `is_local_derived`.
- **Root cause:** Capture symbol creation didn't walk switch expression to find source symbol flags.
- **Fix:** Walk switch expr through deref/field/index/orelse to root, propagate flags to capture.
- **Test:** `test_checker_full.c` — switch capture local-derived return rejected.

### BUG-250: `@size(union)` returns -1
- **Symptom:** `u8[@size(Msg)] buffer` fails — "array size must be a compile-time constant".
- **Root cause:** `compute_type_size` had no TYPE_UNION case.
- **Fix:** Added: tag(4) + align padding + max(variant_sizes), padded to struct alignment.
- **Test:** `test_emit.c` — @size(union) = 16 for `{ u32 a; u64 b; }`, `test_checker_full.c` — accepted as array size.

### BUG-251: `return opt orelse local_ptr` unchecked
- **Symptom:** `return opt orelse p` where p is local-derived compiles.
- **Root cause:** NODE_RETURN walk stopped at NODE_ORELSE — never checked fallback.
- **Fix:** Split: if ret.expr is NODE_ORELSE, check both `.orelse.expr` and `.orelse.fallback`.
- **Test:** `test_checker_full.c` — orelse local/arena fallback rejected, global accepted.

### BUG-252: Array assignment double-eval
- **Symptom:** `get_s().arr = local` calls `get_s()` twice in emitted memcpy.
- **Root cause:** `memcpy(target, src, sizeof(target))` evaluates target twice.
- **Fix:** Pointer hoist: `({ __typeof__(t) *_p = &(t); memcpy(_p, src, sizeof(*_p)); })`.
- **Test:** Existing E2E tests pass (array assignment still works).

### BUG-253: Global non-null `*T` without initializer
- **Symptom:** `*u32 g_ptr;` at global scope compiles — auto-zeros to NULL.
- **Root cause:** Non-null pointer init check only applied to NODE_VAR_DECL, not NODE_GLOBAL_VAR.
- **Fix:** Added check in `register_decl(NODE_GLOBAL_VAR)` path.
- **Test:** `test_checker_full.c` — global `*T` without init rejected, `?*T` accepted.

### BUG-254: `&const_arr[i]` yields mutable pointer
- **Symptom:** `const u32[4] arr; *u32 p = &arr[0];` compiles — const leak.
- **Root cause:** TOK_AMP handler only checked NODE_IDENT operand for const, not field/index chains.
- **Fix:** Walk operand through field/index chains to root, propagate is_const/is_volatile.
- **Test:** `test_checker_full.c` — &const_arr[idx] to mutable rejected, to const accepted.

### BUG-255: Orelse index double-eval
- **Symptom:** `arr[get() orelse 0]` calls get() twice (bounds check + access).
- **Root cause:** NODE_ORELSE not in `idx_has_side_effects` detection.
- **Fix:** Added NODE_ORELSE to side-effect check — triggers single-eval temp path.
- **Test:** `test_emit.c` — orelse index counter=1 (not 2).

### BUG-256: `@ptrcast` local-derived ident bypass in return
- **Symptom:** `return @ptrcast(*u8, p)` where p is local-derived compiles.
- **Root cause:** BUG-246 only checked `&local` inside intrinsic, not local-derived idents.
- **Fix:** Check `is_local_derived`/`is_arena_derived` on arg ident (only when result is pointer type).
- **Test:** `test_checker_full.c` — ptrcast local-derived rejected, global accepted.

### BUG-257: Optional `== null` emits broken C
- **Symptom:** `?u32 x; if (x == null)` emits `if (x == 0)` — struct == int is GCC error.
- **Root cause:** NODE_BINARY emitter didn't special-case struct optionals with null.
- **Fix:** Detect NULL_LIT side + struct optional → emit `(!x.has_value)` / `(x.has_value)`.
- **Test:** `test_emit.c` — optional == null / != null returns correct values.

### BUG-258: `@ptrcast` strips volatile
- **Symptom:** `@ptrcast(*u32, volatile_reg)` allowed — GCC optimizes away writes.
- **Root cause:** No volatile check in @ptrcast handler.
- **Fix:** Check both type-level `pointer.is_volatile` and symbol-level `sym->is_volatile`.
- **Test:** `test_checker_full.c` — volatile to non-volatile rejected, volatile to volatile accepted.

### BUG-259: `return @cstr(local_buf)` dangling pointer
- **Symptom:** `return @cstr(buf, "hi")` where buf is local compiles — dangling pointer.
- **Root cause:** NODE_RETURN didn't check @cstr intrinsic for local buffer args.
- **Fix:** Detect NODE_INTRINSIC "cstr", walk buffer arg to root, reject if local.
- **Test:** `test_checker_full.c` — @cstr local rejected, global accepted.

### BUG-260: `*pool.get(h) = &local` escape
- **Symptom:** Storing local address through dereferenced function call compiles.
- **Root cause:** Assignment escape check didn't recognize NODE_CALL as potential global target.
- **Fix:** Walk target through deref/field/index; if root is NODE_CALL, reject &local and local-derived.
- **Test:** `test_checker_full.c` — store &local through *pool.get() rejected.

### BUG-261: Union alias bypass via same-type pointer
- **Symptom:** `alias.b.y = 99` inside `switch(g_msg)` capture — alias is `*Msg` pointing to g_msg.
- **Root cause:** Union switch lock only checked variable name, not pointer type aliases.
- **Fix:** Store `union_switch_type` on Checker. Check if mutation root's type is `*UnionType` matching locked type. Only applies to pointers (not local values).
- **Test:** `test_checker_full.c` — same-type pointer mutation rejected, different-type accepted.

### BUG-262: Slice start/end double-eval
- **Symptom:** `arr[get_start()..get_end()]` calls get_start() 3x and get_end() 2x.
- **Root cause:** Emitter's runtime check path evaluated start/end directly multiple times.
- **Fix:** Hoist into `_zer_ss`/`_zer_se` temps inside GCC statement expression.
- **Test:** `test_emit.c` — counter=2 (not 4+) after slice with side-effecting bounds.

### BUG-263: Volatile pointer to non-volatile param
- **Symptom:** `write_reg(volatile_ptr)` where param is `*u32` compiles — volatile stripped.
- **Root cause:** No volatile check at function call arg sites.
- **Fix:** Check arg pointer is_volatile (type-level OR symbol-level) vs param non-volatile.
- **Test:** `test_checker_full.c` — volatile to non-volatile rejected, volatile to volatile accepted.

### BUG-264: If-unwrap `|*v|` on rvalue — GCC error
- **Symptom:** `if (get_opt()) |*v|` emits `&(get_opt())` — rvalue address illegal in C.
- **Root cause:** Emitter took address of condition directly, didn't check for rvalue.
- **Fix:** Detect NODE_CALL condition, hoist into typed temp first. Lvalues still use direct `&`.
- **Test:** `test_emit.c` — if-unwrap |*v| on rvalue compiles and runs correctly.

### BUG-265: Recursive union by value not caught
- **Symptom:** `union U { A a; U recursive; }` compiles — incomplete type in C.
- **Root cause:** BUG-227 recursive check only applied to NODE_STRUCT_DECL, not NODE_UNION_DECL.
- **Fix:** Same self-reference check in union variant registration. Walks through arrays.
- **Test:** `test_checker_full.c` — recursive union rejected, pointer self-reference accepted.

### BUG-266: Arena `alloc_slice` multiplication overflow
- **Symptom:** `a.alloc_slice(Task, huge_n)` — `sizeof(T) * n` overflows to small value, creates tiny buffer with huge `.len`.
- **Root cause:** No overflow check on size multiplication in emitted C.
- **Fix:** Use `__builtin_mul_overflow(sizeof(T), n, &total)` — overflow returns null.
- **Test:** `test_emit.c` — overflow alloc returns null (not corrupted slice).

### BUG-267: Volatile stripping via `__auto_type` in if-unwrap
- **Symptom:** `volatile ?u32 reg; if (reg) |v|` — capture `v` loses volatile qualifier.
- **Root cause:** `__auto_type` drops volatile in GCC.
- **Fix:** Use `emit_type_and_name` for explicit typed copy. Handles func ptr name placement.
- **Test:** Existing tests pass (volatile preserved in emitted type).

### BUG-268: Union switch mutable capture modifies copy
- **Symptom:** `switch(g_msg) { .a => |*v| { v.x = 99; } }` — modification lost, g_msg unchanged.
- **Root cause:** Always hoisted into `__auto_type _zer_swt` temp, then `&_zer_swt`. Mutations go to copy.
- **Fix:** Detect lvalue expressions (not NODE_CALL), use direct `&(expr)`. Rvalue still uses temp.
- **Test:** `test_emit.c` — union switch |*v| on global modifies original (returns 99).

### BUG-269: Constant expression div-by-zero not caught
- **Symptom:** `10 / (2 - 2)` passes checker — traps at runtime instead of compile time.
- **Root cause:** Zero check only tested `NODE_INT_LIT == 0`, not computed expressions.
- **Fix:** Use `eval_const_expr` on divisor. `val == 0` → compile-time error.
- **Test:** `test_checker_full.c` — const expr div-by-zero rejected.

### BUG-270: Array return type produces invalid C
- **Symptom:** `u8[10] get_buf()` emits `uint8_t get_buf()` — dimension lost, type mismatch.
- **Root cause:** `emit_type(TYPE_ARRAY)` recurses to base type. C forbids array returns.
- **Fix:** Checker rejects TYPE_ARRAY return types in `check_func_body`.
- **Test:** `test_checker_full.c` — array return type rejected.

### BUG-271: Distinct typedef union/enum in switch broken
- **Symptom:** `switch(distinct_event)` — captures fail, treated as integer switch.
- **Root cause:** Switch dispatch checked `sw_type->kind == TYPE_UNION` without unwrapping distinct.
- **Fix:** `type_unwrap_distinct` before dispatch in both checker (`expr_eff`) and emitter (`sw_eff`).
- **Test:** `test_emit.c` — distinct typedef union switch works (returns 77).

### BUG-272: Volatile stripped in if-unwrap capture copy
- **Symptom:** `volatile ?u32 reg; if(reg) |v|` — initial copy loses volatile qualifier.
- **Root cause:** `emit_type_and_name` doesn't carry symbol-level volatile to emitted type.
- **Fix:** Check source ident's `is_volatile` flag, emit `volatile` prefix on typed copy.
- **Test:** Verified emitted C shows `volatile _zer_opt_u32 _zer_uw0`.

### BUG-273: Volatile array assignment uses memcpy
- **Symptom:** `volatile u8[16] hw; hw = src` emits `memcpy` which doesn't respect volatile.
- **Root cause:** Array assign handler always used memcpy regardless of volatile.
- **Fix:** Walk target to root, check `is_volatile`. If volatile, emit byte-by-byte loop.
- **Test:** Verified emitted C uses volatile byte loop.

### BUG-304: @ptrcast const stripping bypass
- **Symptom:** `@ptrcast(*u32, &const_val)` strips const — allows writing to ROM.
- **Root cause:** @ptrcast checked volatile (BUG-258) but not const.
- **Fix:** Check `eff->pointer.is_const && !result->pointer.is_const` → error.
- **Test:** `test_checker_full.c` — ptrcast const strip rejected, const-to-const accepted.

### BUG-305: Mutable capture |*v| on const source
- **Symptom:** `const ?u32 val; if(val) |*v| { *v = 99; }` — writes through const.
- **Root cause:** Capture always set `cap_const = false` for |*v|.
- **Fix:** Walk to root symbol, if `is_const`, force const on capture pointer.
- **Test:** `test_checker_full.c` — write through const capture rejected.

### BUG-306: Array self-assignment UB (memcpy overlap)
- **Symptom:** `arr = arr` emits `memcpy(arr, arr, size)` — UB for overlapping memory.
- **Root cause:** Used `memcpy` which doesn't handle overlap.
- **Fix:** Changed to `memmove` in both assign and var-decl paths.
- **Test:** Implicit — all existing tests pass with memmove.

### BUG-308: @saturate(u64, f64) overflow UB
- **Symptom:** `@saturate(u64, huge_f64)` — cast of f64 > UINT64_MAX to u64 is UB.
- **Root cause:** u64 path had no upper bound check (only `< 0`).
- **Fix:** Added `> 18446744073709551615.0 ? UINT64_MAX` clamp.
- **Test:** Implicit — correct saturation behavior.

### BUG-310: Volatile slice qualifier — `volatile []T`
- **Symptom:** `volatile u8[16] hw_regs; poll(hw_regs)` where `poll([]u8)` — slice `.ptr` is non-volatile, GCC optimizes away MMIO reads in loops.
- **Root cause:** TYPE_SLICE had no `is_volatile` flag.
- **Fix:** Added `bool is_volatile` to TYPE_SLICE. Full type system change: `type_volatile_slice()` constructor, `type_equals` checks volatile, `can_implicit_coerce` blocks volatile stripping (allows non-volatile→volatile widening). Parser `TYNODE_VOLATILE` propagates to TYPE_SLICE. Emitter uses `_zer_vslice_T` typedefs with `volatile T *ptr`. Volatile array → non-volatile slice rejected at call/var-decl/assign.
- **Test:** `test_checker_full.c` — 6 tests (rejection + acceptance). `test_emit.c` — E2E volatile slice param.

### BUG-302: Rvalue struct field assignment
- **Symptom:** `get_s().x = 5` passes checker but GCC rejects — "lvalue required."
- **Root cause:** BUG-294 lvalue check only caught direct NODE_CALL, not field chains on calls.
- **Fix:** Walk field/index chains to base. NODE_CALL with non-pointer return → reject. Pointer return → valid lvalue.
- **Test:** `test_checker_full.c` — rvalue field assign rejected, lvalue field assign accepted.

### BUG-295: `type_unwrap_distinct` not recursive
- **Symptom:** `distinct typedef P1 P2; P2 x + y` — rejected as "not numeric."
- **Root cause:** Single `if` unwrap, not `while` loop. P2 → P1 (still distinct).
- **Fix:** Changed to `while (t && t->kind == TYPE_DISTINCT) t = t->distinct.underlying;`.
- **Test:** `test_checker_full.c` — nested distinct arithmetic accepted.

### BUG-296: INT_MIN / -1 in constant folder
- **Symptom:** Potential signed overflow UB in the compiler itself.
- **Root cause:** Division path had no INT_MIN / -1 guard.
- **Fix:** Check `l == INT64_MIN && r == -1` → CONST_EVAL_FAIL for both / and %.
- **Test:** Implicit in existing tests (no crash).

### BUG-297: @size(array) loses dimensions
- **Symptom:** `@size(u32[10])` returns 4 instead of 40.
- **Root cause:** `emit_type(TYPE_ARRAY)` recursed to base type, dropping [10].
- **Fix:** Walk array chain, emit all `[N]` dimensions after base type.
- **Test:** `test_emit.c` — @size(u32[10]) = 40.

### BUG-292: Volatile stripping in |*v| mutable capture
- **Symptom:** `if (volatile_reg) |*v|` — `_zer_uwp` declared without volatile.
- **Root cause:** BUG-272 fixed immutable captures but mutable `|*v|` path was separate.
- **Fix:** `expr_is_volatile` check added to mutable capture branch; emits `volatile T *_zer_uwp`.
- **Test:** Verified emitted C shows `volatile _zer_opt_u32 *_zer_uwp0`.

### BUG-294: Assignment to non-lvalue (function call)
- **Symptom:** `get_val() = 5` passes checker but produces GCC error.
- **Root cause:** No lvalue validation in NODE_ASSIGN.
- **Fix:** Check target kind — NODE_CALL, NODE_INT_LIT, NODE_STRING_LIT, NODE_NULL_LIT, NODE_BOOL_LIT → error.
- **Test:** `test_checker_full.c` — assign to call rejected, assign to variable accepted.

### BUG-289: Volatile stripping in orelse temp
- **Symptom:** `volatile ?u32 reg; u32 val = reg orelse 0` — orelse temp copies as non-volatile.
- **Root cause:** `__auto_type` strips qualifiers in GCC.
- **Fix:** All 3 orelse temp sites use `__typeof__(expr) _zer_tmp` instead. `__typeof__` preserves volatile.
- **Test:** All existing tests pass; volatile is preserved in emitted C.

### BUG-290: Local address escape via *param
- **Symptom:** `void leak(**u32 p) { u32 x; *p = &x; }` — compiles, creates dangling pointer in caller.
- **Root cause:** Target walk in &local escape check didn't handle NODE_UNARY(STAR).
- **Fix:** Walk extended: NODE_UNARY(TOK_STAR) added. `target_is_param_ptr` broadened to any deref/field/index.
- **Test:** `test_checker_full.c` — *param = &local rejected, *param = &global accepted.

### BUG-286: Arena.over double evaluation
- **Symptom:** `Arena.over(next_buf())` calls `next_buf()` twice — counter=2 instead of 1.
- **Root cause:** Emitter accesses `.ptr` and `.len` separately from the expression.
- **Fix:** Hoist slice arg into `__auto_type _zer_ao` temp. Array path unchanged (sizeof doesn't eval).
- **Test:** `test_emit.c` — Arena.over single-eval (counter=1).

### BUG-287: Pool/Ring as struct fields (architectural)
- **Symptom:** `struct M { Pool(u32, 4) tasks; }` → GCC error "incomplete type."
- **Root cause:** Pool/Ring emitted as C macros at global scope, can't be inside struct definitions.
- **Fix:** Checker rejects Pool/Ring as struct fields. v0.2 will support this.
- **Test:** `test_checker_full.c` — Pool/Ring struct fields rejected.

### BUG-288: Bit extraction hi < lo silent no-op
- **Symptom:** `reg[0..7]` compiles but produces garbage (negative width).
- **Root cause:** No compile-time check that hi >= lo for constant bit ranges.
- **Fix:** In NODE_SLICE integer path, check constant hi < lo → error.
- **Test:** `test_checker_full.c` — hi < lo rejected, hi >= lo accepted.

### BUG-281: Volatile pointer stripping on return
- **Symptom:** `*u32 wash(volatile *u32 p) { return p; }` compiles — volatile stripped silently.
- **Root cause:** NODE_RETURN had const check but no volatile check.
- **Fix:** After const check, check if return expr is volatile (type-level or symbol-level) and func return type is non-volatile.
- **Test:** `test_checker_full.c` — volatile return as non-volatile rejected.

### BUG-282: Volatile pointer stripping on init/assign
- **Symptom:** `*u32 p = vp` where vp is `volatile *u32` compiles — volatile stripped.
- **Root cause:** Var-decl init checked `pointer.is_volatile` on Type but missed symbol-level `is_volatile`. Assignment had no volatile check.
- **Fix:** Both init and assign paths check source ident's `sym->is_volatile`. Assignment also checks target symbol.
- **Test:** `test_checker_full.c` — init and assign volatile-to-non-volatile rejected.

### BUG-278: Volatile array var-decl init uses memcpy
- **Symptom:** `volatile u8[4] hw = src` emits `memcpy(hw, src, sizeof(hw))` — volatile stripped.
- **Root cause:** Var-decl array init path always used memcpy regardless of volatile.
- **Fix:** Check `var_decl.is_volatile`, emit byte-by-byte loop when volatile.
- **Test:** `test_emit.c` — volatile array init via byte loop works.

### BUG-279: `is_null_sentinel` only unwraps one distinct level
- **Symptom:** `?Ptr2` where Ptr2 is `distinct typedef (distinct typedef *u32)` treated as struct optional.
- **Root cause:** `is_null_sentinel` had single `if (TYPE_DISTINCT)`, not recursive.
- **Fix:** Changed to `while (TYPE_DISTINCT)` loop.
- **Test:** `test_checker_full.c` — nested distinct optional uses null-sentinel.

### BUG-280: `@size(usize)` returns 4 on 64-bit targets
- **Symptom:** `u8[@size(usize)] buf` creates 4-byte buffer on 64-bit where sizeof(size_t) is 8.
- **Root cause:** `compute_type_size` reached `type_width(TYPE_USIZE)` = 32 before target-dependent check.
- **Fix:** Check TYPE_USIZE before type_width, return CONST_EVAL_FAIL. Emitter uses sizeof(size_t).
- **Test:** `test_checker_full.c` — @size(usize) as array size accepted.

### BUG-277: `keep` bypass via function pointers
- **Symptom:** Assigning `store` (with `keep *u32 p`) to `void (*fn)(*u32)` erases keep — `fn(&local)` bypasses check.
- **Root cause:** `keep` not stored in TYPE_FUNC_PTR. Call-site check only worked for direct function calls via `func_node`.
- **Fix:** Added `bool *param_keeps` to TYPE_FUNC_PTR. Parser parses `keep` in func ptr params. `type_equals` checks keep mismatch. Call-site validation uses Type's `param_keeps` for both direct and func ptr calls.
- **Test:** `test_checker_full.c` — keep func to non-keep ptr rejected, keep ptr call with local rejected, keep ptr call with global accepted.

### BUG-275: `@size` pointer width mismatch on 64-bit targets
- **Symptom:** `u8[@size(*u32)] buf` creates 4-byte buffer, but `sizeof(*u32)` is 8 on 64-bit.
- **Root cause:** `compute_type_size` hardcoded pointer=4, slice=8. Constant folder disagrees with GCC.
- **Fix:** `compute_type_size` returns `CONST_EVAL_FAIL` for pointer/slice. Array stores `sizeof_type` — emitter emits `sizeof(T)`. GCC resolves per target.
- **Test:** `test_emit.c` — @size(*u32) matches target width. `test_checker_full.c` — @size(*u32) as array size accepted.

### BUG-276: `_zer_` prefix not reserved
- **Symptom:** `u32 _zer_tmp0 = 100` compiles — could shadow compiler temporaries.
- **Root cause:** No prefix reservation in `add_symbol`.
- **Fix:** Check `name_len >= 5 && memcmp(name, "_zer_", 5) == 0` → error.
- **Test:** `test_checker_full.c` — `_zer_foo` rejected, `zer_foo` accepted.

### BUG-274: Union switch mutable capture drops volatile on pointer
- **Symptom:** `switch(volatile_msg) { .a => |*v| }` — `v` declared as `struct A *`, not `volatile struct A *`.
- **Root cause:** Variant pointer type emitted without checking if switch expression is volatile.
- **Fix:** `sw_volatile` flag detected from switch expression root symbol. Mutable capture emits `volatile T *v`.
- **Test:** Verified emitted C shows `volatile struct A *v`.
