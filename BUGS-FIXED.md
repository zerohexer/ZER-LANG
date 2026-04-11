# Bugs Fixed — ZER Compiler

Tracking compiler bugs found and fixed, ordered by discovery date.
Each entry: what broke, root cause, fix, and test that prevents regression.

---

## Session 2026-04-09/10/11 — Move Struct, CFG Zercheck, Barrier Type, Comptime Locals, 690 Tests

### Comptime local variables — eval_comptime_block handles NODE_VAR_DECL
- **Problem:** `comptime u32 F() { u32 x = 4; return x * 3; }` — body evaluator couldn't handle local variable declarations. Only simple expressions/returns worked.
- **Fix (checker.c):** `eval_comptime_block` now evaluates NODE_VAR_DECL init expressions and adds `{name, value}` bindings to a dynamic array (stack-first [8], malloc on overflow). Subsequent statements see all bindings. No fixed limit.
- **Test:** `rust_tests/rt_const_block_eval.zer`.

### Barrier keyword type — eliminates pre-existing UB
- **Problem:** `u32 barrier` with `@barrier_init` — 4-byte variable for ~120-byte struct. UB: `memset` overflows stack. Was masked by old spinlock stack layout, exposed by BUG-473 mutex change.
- **Fix:** Added `Barrier` keyword type (lexer/parser/types/checker/emitter). Checker rejects non-Barrier args to `@barrier_init`/`@barrier_wait`. Compile-time safety instead of silent stack corruption.
- **Test:** `rust_tests/rt_conc_barrier_sync.zer` — now uses `Barrier b;`.

### BUG-473: shared struct auto-lock self-deadlock through cross-module function calls (FIXED)
- **Symptom:** `worker` calls `counter_inc(c)` where c is `*SharedCounter`. `counter_inc` does `c.val += 1` (auto-lock). Non-recursive spinlock deadlocks on re-entrant lock.
- **Root cause:** Spinlock (`__atomic_exchange_n`) is non-recursive. Cross-function auto-lock = same thread locks twice = hang.
- **Fix (emitter.c):** Replaced spinlock with `pthread_mutex_t` (recursive, lazy-init via `_zer_mtx_ensure_init`). All shared structs now use recursive mutex. Added `_XOPEN_SOURCE 500` for `PTHREAD_MUTEX_RECURSIVE`. `_zer_mtx_inited` field added to shared structs for lazy init.
- **Test:** `test_modules/shared_user.zer` (now passes — 2 threads × 10 increments = 20).

### BUG-472: spawn wrapper missing in multi-module builds (FIXED)
- **Symptom:** `spawn worker(&g_counter)` in main module → `/* spawn: wrapper not found */`.
- **Root cause:** `emit_file_no_preamble` didn't prescan for spawn. In topo order, main module is emitted last via `emit_file_no_preamble`, not `emit_file`.
- **Fix (emitter.c):** Added prescan + wrapper emission to `emit_file_no_preamble`.
- **Test:** `test_modules/shared_user.zer` (spawn emits correctly — blocked by BUG-473).

### BUG-471: pool.free()/slab.free() missing Handle element type check
- **Symptom:** `pool_b.free(handle_from_pool_a)` compiled — Handle is u64, no type mismatch at C level.
- **Root cause:** Pool/Slab `.free()` handler didn't validate handle's element type against pool's element type. `.free_ptr()` already had the check.
- **Fix (checker.c):** Added `type_equals(handle.elem, pool.elem)` check to both Pool and Slab `.free()` handlers. ~5 lines each.
- **Test:** `rust_tests/rt_borrowck_free_wrong_pool_uaf.zer` (negative).

### CFG-Aware Zercheck
- **Problem:** Linear AST walk with per-construct merge hacks (block_always_exits, 2-pass+widen, backward goto re-walk). Adding new control flow required new hacks.
- **Fix:** `PathState.terminated` flag + dynamic fixed-point iteration (ceiling 32). if/else/switch merge uses terminated to determine which paths reach post-construct. Loops and backward goto converge naturally via lattice monotonicity.
- **Improvement:** Switch arms that return no longer cause false MAYBE_FREED on post-switch continuation. Nested if/else with returns on all paths correctly marks post-if as unreachable.
- **Tests:** `rt_cfg_*` (9 tests), `tests/zer/cfg_fixedpoint_stress` (stress test).

## Bugs Fixed This Session

### BUG-468: `move struct` conditional transfer not caught — MAYBE_TRANSFERRED not tracked
- **Symptom:** `if (c) { consume(t); } t.field;` — zercheck allowed use after conditional move.
- **Root cause:** Path merge logic checked only `HS_FREED || HS_MAYBE_FREED`. `HS_TRANSFERRED` excluded.
- **Fix (zercheck.c):** 4 sites: if/else merge, if-no-else merge, loop check, NODE_IDENT move check.
- **Test:** `rust_tests/rt_move_struct_cond_then_use.zer`, `rt_move_struct_loop_reuse.zer`.

### BUG-469: Regular struct containing `move struct` field — passing outer doesn't transfer inner
- **Symptom:** `consume_wrapper(w); w.k.val;` where `Wrapper` has `Key` (move struct) field — compiled when it should reject.
- **Root cause:** zercheck `NODE_CALL` only checked `is_move_struct_type()` on direct arg type. Didn't walk struct fields.
- **Fix (zercheck.c):** Added `contains_move_struct_field()` helper, then unified into `should_track_move()`. Applied at NODE_CALL first/second pass + NODE_IDENT + NODE_RETURN.
- **Test:** `rust_tests/rt_move_struct_in_struct_field.zer` (negative).

### BUG-470: `return move_struct;` doesn't mark variable as transferred
- **Symptom:** `return t; u32 k = t.kind;` — dead code after return not flagged as use-after-move.
- **Root cause:** `NODE_RETURN` handler didn't check if returned expression is a move struct ident.
- **Fix (zercheck.c):** NODE_RETURN now marks move struct ident as `HS_TRANSFERRED` using `should_track_move()`.
- **Test:** `rust_tests/rt_move_struct_return_then_use.zer` (negative).

### Systematic Refactoring: Option A unified helpers (zercheck.c)
- **Problem:** 5+ state check sites each had their own list of `HS_FREED || HS_MAYBE_FREED || HS_TRANSFERRED`. Adding a new state required finding all sites.
- **Fix:** 4 helpers: `should_track_move()`, `is_handle_invalid()`, `is_handle_consumed()`, `zc_report_invalid_use()`. 15 scattered sites unified.

### Systematic Refactoring: Escape flag propagation (checker.c)
- **Problem:** 4 of 5 grouped escape flag propagation sites were missing `can_carry_ptr` type guard (BUG-421 class — scalar false positives).
- **Fix:** 2 helpers: `type_can_carry_pointer()`, `propagate_escape_flags()`. All 5 grouped sites replaced.

### Systematic Refactoring: Emitter optional helpers (emitter.c)
- **Problem:** `?void` check (16 sites), optional null literal (12 sites), return-null (6 duplicate blocks).
- **Fix:** 5 helpers: `is_void_opt()`, `emit_opt_null_check()`, `emit_opt_unwrap()`, `emit_opt_null_literal()`, `emit_return_null()`. 4 scattered sites replaced (more can be migrated incrementally).

### Systematic Refactoring: Checker cleanup (checker.c)
- **Problem:** ISR ban check at 4 sites, auto-slab creation duplicated (40 lines × 2), volatile strip check at 5 sites.
- **Fix:** `check_isr_ban()` (4 sites), `find_or_create_auto_slab()` (2 sites, ~80 lines eliminated), `check_volatile_strip()` (5 sites: @ptrcast, @bitcast, @cast, @container, C-style cast).

### Systematic Refactoring: Complete void-optional + null-literal migration (emitter.c)
- **Problem:** 11 remaining `type_unwrap_distinct(...)->kind == TYPE_VOID` checks, 6 manual `{ 0, 0 }` / `{ 0 }` literals.
- **Fix:** All sites migrated to `is_void_opt()` and `emit_opt_null_literal()`. Total: 16 helpers, 39 sites, ~250 lines eliminated.

## Session 2026-04-08 — Zercheck Prefix Walk + Deadlock Model Redesign

### BUG-467: `?*T[N]` parsed as `POINTER(ARRAY(T,N))` instead of `ARRAY(OPTIONAL(POINTER(T)),N)`
- **Symptom:** `?*Device[2] slots;` → GCC error `struct Device[2]* slots`. Parser produced pointer-to-array instead of array-of-optional-pointers.
- **Root cause:** `?` handler calls `parse_type()` → `*` handler calls `parse_type()` → base parser sees `Device` then `[2]` → wraps as `ARRAY(Device,2)`. `*` wraps as `POINTER(ARRAY(Device,2))`. `?` wraps as `OPTIONAL(POINTER(ARRAY(Device,2)))`. The `[N]` gets consumed inside the nested `parse_type` before `?` handler can swap.
- **Fix:** In `?` handler, after getting inner type, check for `POINTER(ARRAY(...))` pattern and restructure to `ARRAY(OPTIONAL(POINTER(...)),N)`. Same swap as BUG-413 but through pointer wrapper. Also handles `?const *T[N]` and `?volatile *T[N]`.
- **Found by:** Auditing `rt_opaque_array_homogeneous` which used struct fields instead of arrays to work around this.
- **Test:** `rust_tests/rt_opaque_array_homogeneous.zer` — now uses native `?*Device[2]` syntax.

### BUG-466: Heterogeneous *opaque array blocked for constant-indexed vtable pattern
- **Symptom:** `Op[2] ops; ops[0].ctx = @ptrcast(*opaque, &adder); ops[1].ctx = @ptrcast(*opaque, &multiplier);` → "heterogeneous *opaque array" error. Pattern is safe — each element is self-contained vtable entry.
- **Root cause:** `prov_map_set` in checker.c forced root-level homogeneous provenance for ALL array keys containing `[`. Didn't distinguish constant indices (compiler knows which element) from variable indices (compiler can't distinguish).
- **Fix:** Check if bracket content is all digits. Constant index → skip root homogeneity check (per-element provenance is fine). Variable index → enforce homogeneity (can't track at compile time).
- **Found by:** Auditing existing tests — `rt_opaque_multi_dispatch.zer` had been rewritten to use separate variables instead of array. The "limitation" was this overly conservative check.
- **Test:** `rust_tests/rt_opaque_multi_dispatch.zer` — now uses `Op[2] ops` array with different *opaque types per element.

### BUG-465: Function pointer as spawn argument — struct field name outside parens
- **Symptom:** `spawn worker(&res, double_it, 21)` where `worker` takes `u32 (*op)(u32)` → GCC error: `uint32_t (*)(uint32_t) a1;` (name outside parens).
- **Root cause:** Spawn wrapper arg struct emission at emitter.c ~line 4572 used `emit_type_and_name(e, at, NULL, 0)` then `emit(e, " a%d; ")` — separate name placement. Function pointers require name inside `(*name)(params)`.
- **Fix:** Pass the field name (`"a0"`, `"a1"`, etc.) directly to `emit_type_and_name` instead of NULL. Same pattern as BUG-412 (funcptr array emission).
- **Found by:** Auditing existing tests for hidden rewrites. `rt_sendfn_spawn_with_fn_arg.zer` had been rewritten to use integer dispatch instead of funcptr args — the "limitation" was actually this bug.
- **Test:** `rust_tests/rt_sendfn_spawn_with_fn_arg.zer` — now uses actual funcptr arg to spawn.

### BUG-462: Constant-indexed handle arrays — orelse unwrap missing in assignment aliasing
- **Symptom:** `Handle(T)[4] ents; ents[0] = m0 orelse return;` → false "handle leaked" on `m0`
- **Root cause:** Assignment aliasing in zercheck (NODE_ASSIGN, ~line 1102) called `handle_key_from_expr(node->assign.value)` directly, but value was `NODE_ORELSE(m0, return)`. `handle_key_from_expr` doesn't handle NODE_ORELSE → returns 0 → alias `"ents[0]"` never created → `m0` appears leaked.
- **Fix:** Unwrap NODE_ORELSE/NODE_INTRINSIC/NODE_TYPECAST before extracting key (8 lines, matching var-decl path at line 811-818 which already did this).
- **Test:** `rust_tests/rt_handle_array_const_idx.zer` — constant-indexed handle array with alloc/use/free cycle.

### BUG-463: Struct field pointer aliasing — UAF through h.inner not caught
- **Symptom:** `h.inner = w; heap.free_ptr(w); h.inner.data` compiled clean — UAF not detected.
- **Root cause:** NODE_FIELD UAF check (zercheck.c ~line 1190) walked expression chain to the ROOT ident (`h`) and only checked that. For `h.inner.data`, root is `h` (untracked stack struct). The tracked key `"h.inner"` (alias of `w`) was never checked.
- **Fix:** Walk EVERY prefix of the field/index chain, not just root. For `h.inner.data`, check `"h.inner.data"`, `"h.inner"`, `"h"` — any tracked prefix that is FREED catches the UAF. Added `free_line < cur_line` guard to avoid false positive when `pool.free(s.h)` marks `s.h` FREED then expression check re-visits same line.
- **Test:** `rust_tests/rt_move_into_struct.zer` — struct field pointer alias, free original, use through struct field.

### BUG-464: Deadlock detection — overly conservative cross-statement ordering
- **Symptom:** `a.x = 10; b.y = 20; if (a.x != 10) { return 1; }` → false "deadlock" error. Pattern is safe — each statement is lock→op→unlock, no nested locks.
- **Root cause:** `check_block_lock_ordering` tracked `last_shared_id` across entire block. Once Beta accessed, any subsequent Alpha access = "descending order" error. But the emitter does lock-per-statement — no two locks held simultaneously.
- **Fix:** Redesigned deadlock detection to match emitter's actual locking model. Only same-statement multi-shared-type access is a real deadlock (emitter can only lock ONE type per statement). New `collect_shared_types_in_expr` finds ALL shared types in one expression. Cross-statement ordering removed entirely — safe by construction.
- **Tests:** `rust_tests/rt_deadlock_order_interleave.zer` (positive — cross-statement safe), `rust_tests/rt_deadlock_order_reject.zer` (negative — same-statement multi-lock).
- **Updated:** 3 existing deadlock tests (`conc_reject_deadlock_abba`, `conc_reject_deadlock_ordering`, `gen_shared_008`) now test same-statement pattern.

---

## Session 2026-04-06 — Dynamic Array UAF Auto-Guard

### NEW FEATURE: Compile-time dynamic array Handle UAF protection
- **What:** `pool.free(handles[k])` with variable `k`, then `handles[j].field` — compiler auto-inserts `if (j == k) { return; }` before the access. Same pattern as bounds auto-guard.
- **Loop free detection:** `for (i = 0; i < N; i += 1) { pool.free(handles[i]); }` marks ALL elements as freed. Any post-loop `handles[j].field` → **compile error** (not auto-guard — provably all freed).
- **Implementation:** `DynFreed` struct on Checker tracks `{array_name, freed_idx, all_freed}`. Set during pool.free/slab.free NODE_CALL handler. Checked during Handle auto-deref NODE_FIELD handler. Auto-guard sentinel `array_size == UINT64_MAX` distinguishes UAF guard from bounds guard in emitter.
- **Tests:** `dyn_array_guard.zer` (positive), `dyn_array_loop_freed.zer` (negative)

---

## Session 2026-04-06 — *opaque Compile-Time Tracking

### NEW FEATURE: Full cross-module *opaque compile-time safety
- **What:** `*opaque` pointers through wrapper functions (any depth) now fully tracked at compile time. Double-free, UAF, and leak detected across module boundaries without runtime checks.
- **Components:**
  1. Signature heuristic: bodyless `void func(*opaque)` auto-detected as free
  2. @ptrcast alias tracking: `*T r = @ptrcast(*T, handle)` links `r` to `handle`
  3. Wrapper allocator recognition: any call returning `?*opaque`/`?*T` registers ALIVE
  4. Cross-module summaries: imported module ASTs scanned for FuncSummary
  5. UAF-at-call-site: passing freed `*opaque` to non-free function = compile error
  6. Qualified call support: `module.func()` summaries resolved via field name
- **Tests:** `test_modules/opaque_wrap.zer` (positive), `test_modules/opaque_wrap_df.zer` (double-free), `test_modules/opaque_wrap_uaf.zer` (UAF)
- **Critical fix:** `import_asts` fed to zercheck in BFS order — dependencies scanned AFTER dependents, breaking summary chain. Fix: use `topo_order` (3 lines in zerc_main.c). Same topo_order already used for emission.

### NEW FEATURE: Dynamic array Handle UAF auto-guard
- **What:** `pool.free(handles[k])` with variable `k` followed by `handles[j].field` — compiler auto-inserts `if (j == k) { return; }` guard. Loop-free-all pattern → compile error.
- **Tests:** `tests/zer/dyn_array_guard.zer` (positive), `tests/zer_fail/dyn_array_loop_freed.zer` (negative)

---

## Session 2026-04-06 — Scale Testing (BUG-432)

### BUG-432: Module-qualified variable access (`config.VERSION`)
- **Symptom:** `import config; if (config.VERSION != 3)` → "undefined identifier 'config'". Qualified function calls (`config.func()`) worked (BUG-416), but qualified variable access didn't.
- **Root cause:** NODE_CALL had pre-`check_expr` interception for module-qualified calls (BUG-416). NODE_FIELD did not — `check_expr(NODE_IDENT)` fired "undefined identifier" for the module name before NODE_FIELD could intercept.
- **Fix:** Added pre-`check_expr` interception in NODE_FIELD (same pattern as NODE_CALL). When object is NODE_IDENT and not found in current scope, try `module__field` mangled lookup in global scope. Rewrite to NODE_IDENT with raw field name (emitter resolves via mangled lookup, avoids double-mangling).
- **Test:** `test_modules/qualified_var.zer`, 10-module scale test

---

## Session 2026-04-05 — track-cptrs Audit (BUG-431)

### BUG-431: `@ptrcast` from `*opaque` with `--track-cptrs` — GCC "cannot convert to pointer type"
- **Symptom:** `*Sensor back = @ptrcast(*Sensor, ctx)` where `ctx` is `*opaque` → GCC error "cannot convert to a pointer type." Only with `--run` (which enables `--track-cptrs`). `--emit-c` without `--track-cptrs` worked fine.
- **Root cause:** `_zer_check_alive((void*)ctx, ...)` cast `_zer_opaque` struct directly to `void*`. With `--track-cptrs`, `*opaque` is a `_zer_opaque` struct `{void *ptr, uint32_t type_id}`, not a raw pointer. `(void*)struct` is invalid C.
- **Fix:** Changed to `_zer_check_alive(ctx.ptr, ...)` — extract the `.ptr` field before passing to alive check.
- **Test:** `opaque_ptrcast_roundtrip.zer`

---

## Session 2026-04-05 — Const in Comptime Args (BUG-430)

### BUG-430: Const variable as comptime function argument rejected
- **Symptom:** `const u32 perms = FLAG_READ() | FLAG_WRITE(); comptime if (HAS_FLAG(perms, FLAG_READ()))` → "requires all arguments to be compile-time constants." Comptime calls with const ident args failed.
- **Root cause:** `eval_const_expr` (ast.h) doesn't resolve `NODE_IDENT` — it has no scope access. Comptime call arg evaluation used `eval_const_expr` directly.
- **Fix:** Added `eval_const_expr_scoped(Checker *c, Node *n)` — tries `eval_const_expr` first, falls back to const symbol lookup via scope chain. Reads `sym->func_node->var_decl.init` and recursively evaluates. Depth-limited to 32 (prevents circular const refs). Also set `sym->func_node = node` for local var-decls (was only set for globals and functions).
- **Test:** `comptime_const_arg.zer`

---

## Session 2026-04-05 — Systematic Audit Round 2 (BUG-429)

### BUG-429: Array variant in union emitted wrong C syntax
- **Symptom:** `union Data { u32 single; u32[4] quad; }` emitted `uint32_t[4] quad;` inside union — invalid C. Should be `uint32_t quad[4];`.
- **Root cause:** Union variant emission used `emit_type()` + manual name printf, which doesn't handle array dimension placement. Struct fields already used `emit_type_and_name()` which handles this correctly.
- **Fix:** Changed union variant emission to use `emit_type_and_name()` (same pattern as struct fields).
- **Test:** `union_array_variant.zer`

---

## Session 2026-04-05 — Systematic Audit (BUG-426/427/428)

### BUG-426: `!` operator rejected integers (only accepted bool)
- **Symptom:** `comptime if (!FEATURE_B())` → "'!' requires bool, got 'u32'". Common C idiom `!integer` rejected.
- **Root cause:** `TOK_BANG` handler required `type_equals(operand, ty_bool)` — only bool accepted.
- **Fix:** Changed to `!type_equals(operand, ty_bool) && !type_is_integer(operand)` — accept bool OR integer. Result is always bool. Updated 2 existing negative tests to positive.
- **Test:** `bang_integer.zer`

### BUG-427: `@atomic_or` rejected as unknown intrinsic
- **Symptom:** `@atomic_or(&flags, 0x0F)` → "unknown intrinsic '@atomic_or'". All other atomics worked.
- **Root cause:** Atomic intrinsic name length check was `nlen >= 10`, but `"atomic_or"` is 9 chars. Minimum should be 9.
- **Fix:** Changed `>= 10` to `>= 9` in checker.c.
- **Test:** `atomic_ops.zer`

### BUG-428: `@atomic_cas` with literal expected value — GCC "lvalue required"
- **Symptom:** `@atomic_cas(&state, 0, 1)` → GCC error "lvalue required as unary '&' operand". `__atomic_compare_exchange_n` needs `&expected` but emitter emitted `&(0)` — taking address of literal.
- **Root cause:** Emitter emitted `&(expected_expr)` directly. Literals are rvalues, can't take their address.
- **Fix:** Hoist expected value into `__typeof__` temp: `({ __typeof__(*ptr) _zer_cas_exp = expected; __atomic_compare_exchange_n(ptr, &_zer_cas_exp, desired, ...); })`.
- **Test:** `atomic_ops.zer`

---

## Session 2026-04-05 — Bug Hunting Round 2 (BUG-425)

### BUG-425: Nested comptime function calls rejected
- **Symptom:** `comptime u32 QUAD(u32 x) { return DOUBLE(DOUBLE(x)); }` → "comptime function 'DOUBLE' requires all arguments to be compile-time constants." Comptime functions calling other comptime functions with their own parameters failed.
- **Root cause:** The checker's NODE_CALL handler validates comptime call args via `eval_const_expr()` during body type-checking. Inside a comptime function body, parameters are `NODE_IDENT` (not yet substituted) — `eval_const_expr` returns `CONST_EVAL_FAIL`. The real evaluation happens at the call site (with concrete values) via `eval_comptime_block` + `eval_const_expr_subst`, which correctly handles parameter substitution and nested calls.
- **Fix:** Added `bool in_comptime_body` to Checker struct. Set to `true` when checking a comptime function body (`check_func_body`). When `in_comptime_body` is true and comptime call args aren't all constant, skip the error — the call site will evaluate with real values.
- **Test:** `comptime_nested_call.zer` (DOUBLE→QUAD→ADD_QUAD chain, MAX→MAX3 chain, QUAD in array size)

---

## Session 2026-04-05 — Late Bug Hunting (BUG-423/424)

### BUG-423: Comptime call in Pool/Ring size argument
- **Symptom:** `Pool(Item, POOL_SIZE())` → "Pool count must be a positive compile-time constant." Comptime function call as Pool/Ring size rejected.
- **Root cause:** `eval_const_expr` ran before `check_expr` resolved the comptime call in the type resolution path (TYNODE_POOL/TYNODE_RING count expression).
- **Fix:** Added `check_expr(c, tn->pool.count_expr)` before `eval_const_expr` in both TYNODE_POOL and TYNODE_RING.
- **Test:** `comptime_pool_size.zer`

### BUG-424: String literal to const slice struct field blocked
- **Symptom:** `struct Log { const [*]u8 msg; }; e.msg = "hello";` → "string literal is read-only." Checker blocked all string→slice assignments without checking target's const qualifier.
- **Root cause:** Assignment string literal check (line 1671) tested `target->kind == TYPE_SLICE` without checking `target->slice.is_const`. Const slice targets are safe for string literals.
- **Fix:** Added `!type_unwrap_distinct(target)->slice.is_const` condition. Also added distinct unwrap.
- **Test:** `const_slice_field.zer`

---

## Session 2026-04-05 — Bugs Found by Hard ZER Programs (BUG-421)

### BUG-421: Scalar field from struct returned by `func(&local)` falsely rejected as local-derived
- **Symptom:** `Token tok = get_tok(&local_state); u32 result = tok.val; return result;` → "cannot return pointer to local 'result'" even though `result` is a plain `u32`.
- **Root cause:** BUG-360/383 conservatively marks struct results of calls with `&local` args as `is_local_derived` (struct might contain pointer field carrying local address). The alias propagation at var-decl init walks field chains to root and propagates the flag without checking the target type. So `u32 val = struct_result.field` inherits `is_local_derived` from the struct, even though `u32` can never carry a pointer.
- **Fix:** In the alias propagation (checker.c ~line 4742), only propagate `is_local_derived`/`is_arena_derived` when the target type can actually carry a pointer (TYPE_POINTER, TYPE_SLICE, TYPE_STRUCT, TYPE_UNION, TYPE_OPAQUE). Scalar types (integers, floats, bools, enums, handles) skip propagation.
- **Test:** `tests/zer/scalar_from_struct_call.zer`, `tests/zer/tokenizer.zer`

### BUG-422: Auto-guard `return 0` for struct/union return type — GCC "incompatible types"
- **Symptom:** Function returning union/struct with auto-guarded array access emits `return 0;` which GCC rejects.
- **Root cause:** `emit_zero_value()` only handled void, optional, pointer, and scalar. TYPE_STRUCT and TYPE_UNION fell through to bare `0`.
- **Fix:** Added struct/union case: emit `(StructType){0}` compound literal.
- **Test:** `tests/zer/tagged_values.zer`

---

## Session 2026-04-05 — Bugs Found by Writing Real ZER Code (BUG-418/419/420)

### BUG-420: `typedef ?u32 (*Handler)(u32)` creates optional funcptr instead of funcptr returning optional
- **Symptom:** `typedef ?u32 (*Handler)(u32)` produces `?(u32 (*)(u32))` (nullable function pointer) instead of `(?u32) (*)(u32)` (function pointer returning `?u32`). Calling `handler(x) orelse fallback` errors "cannot call non-function type '?fn(...)'"
- **Root cause:** All 6 function pointer declaration sites stripped `?` from the return type and re-wrapped the whole funcptr as optional. This was correct for var-decl/param/struct-field/global sites (nullable funcptr is the common use case) but wrong for typedef sites (where `?RetType` should be part of the function signature).
- **Fix:** Only typedef sites (regular + distinct) pass `?RetType` through as the return type. All other 4 sites (local var, global var, struct field, function param) keep the original behavior: `?` wraps the function pointer as optional/nullable.
- **Design:** `?void (*cb)(u32)` at declaration = optional function pointer. `typedef ?u32 (*Handler)(u32)` = funcptr returning optional. `?Handler` via typedef = optional function pointer.
- **Test:** `tests/zer/funcptr_optional_ret.zer`

---

## Session 2026-04-05 — Bugs Found by Writing Real ZER Code (BUG-418/419)

### BUG-418: `else if` chain emits `#line` after `else` — GCC "stray #" error
- **Symptom:** `if (x < 10) { ... } else if (x < 20) { ... }` causes GCC error "stray '#' in program" when source mapping (`--run` or `zerc file.zer`) is enabled.
- **Root cause:** `emit_stmt` emits `#line N "file"` at the start of each statement. When else_body is NODE_IF, the output becomes `else #line 38 ...` on the same line — GCC requires `#line` at the start of a line.
- **Fix:** When else_body is NODE_IF and source_file is set, emit `"else\n"` instead of `"else "` so the `#line` directive starts on its own line. Both regular-if and if-unwrap else paths fixed.
- **Test:** `tests/zer/else_if_chain.zer`

### BUG-419: Array→slice coercion missing in assignment
- **Symptom:** `[*]u8 s; s = array;` and `buf.data = array;` cause GCC error "incompatible types when assigning to type '_zer_slice_u8' from type 'uint8_t*'".
- **Root cause:** Array→slice coercion was handled in var-decl init, call args, and return, but NOT in NODE_ASSIGN. The emitter's assignment handler fell through to plain `emit_expr()` which emits the raw array identifier (decays to pointer in C).
- **Fix:** In NODE_ASSIGN emission, when target is TYPE_SLICE and value is TYPE_ARRAY, call `emit_array_as_slice()` — same function used by var-decl init path.
- **Test:** `tests/zer/array_slice_assign.zer`

---

## Session 2026-04-05 — Late Fixes (BUG-414 through BUG-416)

### BUG-414: Volatile struct array field uses memmove (strips volatile)
- **Symptom:** `struct Hw { volatile u8[4] regs; }; dev.regs = src;` → GCC warns "discards volatile qualifier from pointer target type." GCC can optimize away the write.
- **Root cause:** `expr_is_volatile()` only checked root symbol `is_volatile`. For `dev.regs` where `dev` is non-volatile but `regs` field is volatile, returned false.
- **Fix:** Added `SField.is_volatile` flag in types.h. Checker sets it when field has TYNODE_VOLATILE wrapper. `expr_is_volatile()` now walks field chains and checks SField.is_volatile. Also checks type-level volatile on slice/pointer fields.
- **Test:** `volatile_field_array.zer`

### BUG-415: Comptime negative return values
- **Symptom:** `comptime i32 NEG() { return -1; }; i32 n = NEG();` → "integer literal 18446744073709551615 does not fit in i32." Also `comptime if (MODE() < 0)` failed.
- **Root cause:** In-place NODE_INT_LIT conversion stored `(uint64_t)(-1)` = huge unsigned number. `is_literal_compatible` rejected it.
- **Fix:** (1) Only convert to NODE_INT_LIT for non-negative values. Negative results stay as NODE_CALL with `is_comptime_resolved`. (2) Extended `eval_const_expr` in ast.h to handle NODE_CALL with `is_comptime_resolved` — reads `comptime_value` directly. Works universally in binary expressions and comptime if conditions.
- **Test:** `comptime_negative.zer`

### BUG-416: Cross-module Handle auto-deref — duplicate allocator in global scope
- **Symptom:** Handle auto-deref (`e.id = id`) in imported module function emitted `/* ERROR: no allocator */ 0 = id`.
- **Root cause:** `find_unique_allocator()` returned NULL (ambiguous) because imported module globals are registered TWICE in global scope — once under raw name (`cross_world`) and once under mangled name (`cross_entity__cross_world`, from BUG-233 fix). Both point to the same `Type*` object. The function found two matching Slab entries and returned NULL. The previous session's name-based fallback was a workaround for this, but the true root cause was the duplicate registration, not pointer identity failure.
- **Fix:** In `find_unique_allocator()`, when finding a second match, check `found->type == t` (same Type pointer). If same allocator, skip it. Only return NULL for genuinely different allocators. Removed the name-based fallback in emitter — it was never needed.
- **Test:** `test_modules/cross_handle.zer`, `test_modules/qualified_call.zer`

### BUG-417: popen segfault on 64-bit Linux — missing _POSIX_C_SOURCE
- **Symptom:** `zerc` crashes with SIGSEGV at `fgets()` during GCC auto-detection probe on 64-bit Linux when compiled with `-std=c99`.
- **Root cause:** `popen`/`pclose` are POSIX extensions not declared in strict C99 `<stdio.h>`. Without a declaration, compiler assumes `popen` returns `int` (32-bit), truncating the 64-bit FILE* pointer. The truncated pointer passed to `fgets` causes segfault.
- **Fix:** Added `#define _POSIX_C_SOURCE 200809L` before `<stdio.h>` in `zerc_main.c` (guarded by `#ifndef _WIN32`). Standard practice for POSIX functions.
- **Note:** Did not manifest on Windows (no popen) or Docker `gcc:13` image (defaults to GNU extensions). Only affects strict C99 compilation on 64-bit POSIX systems.

---

## Session 2026-04-05 — Bug Hunting Round 2 (BUG-402/403)

### BUG-413: ?T[N] parsed as OPTIONAL(ARRAY) instead of ARRAY(OPTIONAL)
- **Symptom:** `?Handle(Task)[4] arr; arr[0] = pool.alloc();` → "cannot index type ?Handle(Task)[4]." Indexing failed because type was optional wrapping an array, not array of optionals.
- **Root cause:** Parser `?T` handler calls `parse_type(p)` for inner. For `?Handle(T)[N]`, inner parser sees `Handle(T)` then `[N]` suffix → wraps in TYNODE_ARRAY → returns `ARRAY(HANDLE)`. Optional wraps: `OPTIONAL(ARRAY(HANDLE))`. User wants `ARRAY(OPTIONAL(HANDLE))`.
- **Fix:** After parsing optional inner, check if inner is TYNODE_ARRAY. If so, swap: pull array outside optional → `ARRAY(OPTIONAL(inner_elem))`. Also handle `?T[N]` for named types by checking for `[N]` suffix after optional.
- **Found by:** Writing 170-line task scheduler in ZER — `?Handle(Task)[MAX_TASKS()]` ready queue needed this syntax.
- **Test:** `optional_array.zer` (?Handle[N], ?u32[N], indexing, if-unwrap, == null)

### zercheck: defer free scanning + if-exit MAYBE_FREED fix
- **Defer free:** `defer pool.free(h);` no longer triggers false "never freed" warning. zercheck now scans all top-level defer bodies for free/delete/free_ptr/delete_ptr calls before leak detection. Matched handles are marked FREED.
- **If-exit MAYBE_FREED:** `if (err) { free(h); return; } use(h);` no longer triggers false MAYBE_FREED. When the then-branch always exits (return/break/continue/goto), handles freed inside it stay ALIVE on the continuation path — we only reach post-if if the branch was NOT taken.
- **`block_always_exits()` helper:** Recursively checks NODE_RETURN, NODE_BREAK, NODE_CONTINUE, NODE_GOTO, NODE_BLOCK (last statement), NODE_IF (both branches exit).
- **`defer_scans_free()` helper:** Scans defer body for pool.free, slab.free_ptr, Task.delete, bare free calls. Returns handle key.
- **Test:** `defer_free.zer` (defer free_ptr, defer pool.free, defer Task.delete), `if_exit_free.zer` (multiple if-return guards with alloc_ptr and Handle)

### BUG-410 (cont): Remaining distinct unwrap — all TYPE_ARRAY, TYPE_POINTER, TYPE_SLICE sites
- **Sites fixed in emitter.c (6):** array assign target, @cstr array dest (use buf_eff), array init memcpy, volatile pointer local var-decl (2 sites), volatile pointer global var (2 sites).
- **Sites fixed in checker.c (5):** assign value TYPE_ARRAY check, assign target TYPE_SLICE + value TYPE_ARRAY escape check, const array→mutable slice assign, call-site const array→slice (2 sites).
- **Pattern:** every `->kind == TYPE_X` on a type from `checker_get_type()` or `check_expr()` must use `type_unwrap_distinct()`.

### BUG-410 (cont): Distinct slice/array — indexing, sub-slice, emitter dispatch
- **Symptom:** `distinct typedef const [*]u8 Text; msg[0]` → "cannot index type." `msg[1..]` → "cannot slice type." Various emitter sites produced wrong C for distinct slice/array.
- **Root cause:** Checker NODE_INDEX and NODE_SLICE didn't unwrap distinct on `obj`. Emitter had 10+ sites checking `->kind == TYPE_SLICE` or `->kind == TYPE_ARRAY` without unwrapping: proven index, bounds check, sub-slice, call-site decay, Arena.over(), @cstr dest, var-decl orelse, array→slice coercion, return coercion.
- **Fix:** Checker: unwrap distinct at entry of NODE_INDEX and NODE_SLICE. Emitter: unwrap at 10 sites for TYPE_SLICE dispatch.
- **Test:** `distinct_slice_ops.zer` (indexing, .len, sub-slice on distinct const [*]u8)

### BUG-410: Distinct typedef — deref, field access, pointer auto-deref
- **Symptom:** `distinct typedef *u32 SafePtr; *p` → "cannot dereference non-pointer." `distinct typedef Point Vec2; v.x` → "no field 'x'." `distinct typedef *Motor SM; sm.speed` → GCC "is a pointer, use ->."
- **Root cause:** Checker deref (TOK_STAR) checked `operand->kind != TYPE_POINTER` without unwrapping distinct. NODE_FIELD didn't unwrap distinct on `obj` before struct/slice/pointer dispatch. Emitter NODE_FIELD used `obj_type->kind == TYPE_POINTER` for `->` emission without unwrapping.
- **Fix:** (1) Checker deref unwraps distinct before TYPE_POINTER check. (2) Checker NODE_FIELD unwraps distinct on `obj` at entry. (3) Emitter NODE_FIELD unwraps distinct for enum/pointer dispatch.
- **Test:** `distinct_types.zer` (deref, struct field, pointer auto-deref)

### BUG-409 (cont): Distinct optional — assign null, == null, bare if(), while()
- **Symptom:** `m = null` on distinct ?u32 → GCC "incompatible types." `m == null` → GCC "invalid operands." `if (m)` → GCC "used struct type where scalar required."
- **Root cause:** 5 more emitter sites checked `->kind == TYPE_OPTIONAL` without `type_unwrap_distinct()`: assignment null (line 964/974), `== null` comparison (line 684), bare `if(opt)` condition (line 2711), `while(opt)` condition (line 2761).
- **Fix:** All 5 sites unwrap distinct before TYPE_OPTIONAL dispatch.
- **Test:** `distinct_optional_full.zer` (assign null, == null, != null, bare if, if-unwrap, orelse default, orelse block)

### BUG-409: Distinct typedef wrapping optional types (Gemini finding #1)
- **Symptom:** `distinct typedef ?u32 MaybeId; MaybeId find() { return null; }` → "return type doesn't match." `m orelse 0` → "orelse requires optional type." Distinct wrapping `?T` not recognized as optional.
- **Root cause:** `type_is_optional()` and `type_unwrap_optional()` in types.c didn't call `type_unwrap_distinct()`. Also `can_implicit_coerce()` T→?T path didn't unwrap distinct on target. Emitter orelse paths and return-null/bare-return paths all checked `->kind == TYPE_OPTIONAL` without unwrapping distinct on `current_func_ret`.
- **Fix:** (1) `type_is_optional()` / `type_unwrap_optional()` now unwrap distinct. (2) `can_implicit_coerce` T→?T path unwraps distinct on `to`. (3) Emitter: orelse `is_ptr_optional`/`is_void_optional` use `type_unwrap_distinct(orelse_type)`. (4) Emitter: return null, return value, bare return all unwrap distinct on `current_func_ret`.
- **Test:** `distinct_optional.zer` (distinct ?u32 orelse default, orelse block, return null)
- **Credit:** Gemini 2.5 Pro identified the checker-level issue; emitter fixes found during verification.

### BUG-407: Nested distinct funcptr name placement wrong (Gemini finding)
- **Symptom:** `distinct typedef Fn SafeFn; distinct typedef SafeFn ExtraSafeFn; ?ExtraSafeFn callback` emits `void (*)(uint32_t) callback` — name AFTER type instead of inside `(*callback)`.
- **Root cause:** `emit_type_and_name` only checked one level of TYPE_DISTINCT before TYPE_FUNC_PTR. `t->optional.inner->kind == TYPE_DISTINCT && t->optional.inner->distinct.underlying->kind == TYPE_FUNC_PTR` — misses multi-level distinct chains.
- **Fix:** Changed to `type_unwrap_distinct(t->optional.inner)->kind == TYPE_FUNC_PTR`. Also fixed the non-optional distinct funcptr path (line 480).
- **Test:** `distinct_funcptr_nested.zer` (non-optional, optional, null optional)
- **Credit:** Found by Gemini 2.5 Pro via Repomix codebase audit.

### BUG-408: ?void initialized from void function call (Gemini finding)
- **Symptom:** `?void result = do_work();` emits `_zer_opt_void result = do_work();` — void expression in struct initializer, GCC error.
- **Root cause:** var-decl init for `?T` with NODE_CALL assigns directly (`= call()`). Works for functions returning values, but void functions can't be used as initializer expressions.
- **Fix:** When target is `?void` and call returns `TYPE_VOID`, hoist call to statement: `result; do_work(); result = (_zer_opt_void){ 1 };`. Same pattern as BUG-145 (NODE_RETURN void-as-statement).
- **Test:** `void_optional_init.zer` (void func, ?void func, null)
- **Credit:** Found by Gemini 2.5 Pro via Repomix codebase audit.

### BUG-405: Handle auto-deref scalar store blocked by non-storable check
- **Symptom:** `u32 v = h.value;` where `h` is `Handle(Sensor)` → "cannot store result of get() — use inline." Scalar field reads from Handle auto-deref blocked.
- **Root cause:** `is_non_storable()` check at var-decl init (line 4468) and assignment (line 1635) fired on ALL expressions from Handle auto-deref, regardless of result type. The check should only block `*T` pointer storage (caching pool.get result), not scalar field values.
- **Fix:** Added type check — only error when result is TYPE_POINTER, TYPE_SLICE, TYPE_STRUCT, or TYPE_UNION. Scalar types (u32, bool, etc.) pass through safely.
- **Test:** `handle_scalar_store.zer` (Slab + Pool scalar reads into locals, expressions)

### BUG-406: return string literal from const [*]u8 function blocked
- **Symptom:** `const [*]u8 get() { return "hello"; }` → "cannot return string literal as mutable slice." Checker didn't check if return type was const.
- **Root cause:** NODE_RETURN string literal check (line 5684) tested `ret->kind == TYPE_SLICE` without checking `ret->slice.is_const`. Fired on ALL slice returns including const ones.
- **Fix:** Added `!ret->slice.is_const` condition. Also handles `?const [*]u8` (optional const slice).
- **Test:** `const_slice_return.zer` (const [*]u8 return, sub-slice, function pass)

### BUG-404: comptime call in-place conversion to NODE_INT_LIT
- **Symptom:** `comptime if (FUNC() > 1)` failed — comptime call resolved but `eval_const_expr` in binary expression couldn't read it. Also `u8[BUF_SIZE()]` failed for same reason.
- **Root cause:** Resolved comptime calls set `is_comptime_resolved` + `comptime_value` but kept `NODE_CALL` kind. `eval_const_expr` (ast.h) only handles `NODE_INT_LIT` and binary/unary nodes — doesn't know about comptime resolution.
- **Fix:** After resolving comptime call value, convert node in-place: `node->kind = NODE_INT_LIT; node->int_lit.value = val`. Now `eval_const_expr` sees it universally — in comptime if conditions, array sizes, binary expressions, etc.
- **Side effect:** The emitter's `NODE_CALL` comptime handler (line 994) is now dead code — converted nodes go to `NODE_INT_LIT` instead. Left in place for safety (unreachable).
- **Test:** `comptime_if_call.zer` (direct call, comparison, nested, array size)

### BUG-402: comptime func const not recognized in comptime if
- **Symptom:** `comptime u32 PLATFORM() { return 1; } const u32 P = PLATFORM(); comptime if (P) { ... }` → "comptime if condition must be a compile-time constant."
- **Root cause:** Two issues: (1) `comptime if` ident lookup required `is_comptime` flag (only set on comptime functions, not const vars). (2) Emitter's `eval_const_expr` saw NODE_IDENT for the condition but checker stored result in `int_lit.value` without changing node kind — emitter didn't see it.
- **Fix:** (1) Checker relaxed to check `is_const` (not `is_comptime`), also checks `call.is_comptime_resolved` on init expression. (2) Checker now sets `cond->kind = NODE_INT_LIT` so emitter's `eval_const_expr` picks up the resolved value.
- **Test:** `comptime_const_if.zer` (true/false branch, dead code stripped)

### BUG-403: Optional null init emits `{ {0} }` — GCC warning
- **Symptom:** `return null` from `?u32` function emits `(_zer_opt_u32){ {0} }`. GCC warns: "braces around scalar initializer." Functionally correct but noisy.
- **Root cause:** 6 sites in emitter used `){ {0} }` for non-void optional null. The inner `{0}` wraps a scalar `value` field with unnecessary braces.
- **Fix:** Changed all 6 sites from `){ {0} }` to `){ 0, 0 }` (explicit value=0, has_value=0).
- **Test:** `optional_null_init.zer` (exercises ?u32 and ?bool null returns + orelse)

---

## Session 2026-04-05 — Critical Pattern Audit Fixes (BUG-401)

### BUG-401a: Division guard temps use __auto_type — drops volatile
- **Symptom:** `volatile u32 x; x / divisor` — emitted `__auto_type _zer_dv = divisor` which strips volatile qualifier. GCC may optimize away volatile reads.
- **Root cause:** Division guard (lines 624, 630, 899) predated BUG-289 fix. Never updated to `__typeof__`.
- **Fix:** All 3 sites changed to `__typeof__(expr) _zer_dv = expr` — preserves volatile/const.
- **Test:** `volatile_div.zer` (volatile dividend with const divisor division)

### BUG-401b: orelse { block } uses __auto_type — drops volatile
- **Symptom:** `volatile ?u32 x = expr orelse { block }` — temp strips volatile.
- **Root cause:** orelse block path (line 1874) and orelse default path (line 1890) used `__auto_type` while orelse return paths (1798, 1831) correctly used `__typeof__`.
- **Fix:** Both paths changed to `__typeof__(expr) _zer_tmp = expr`.
- **Test:** `orelse_void_block.zer`, `volatile_orelse.zer`

### BUG-401c: orelse { block } and orelse default access .value on ?void
- **Symptom:** `?void orelse { block }` emits `_zer_tmp.value` — but `_zer_opt_void` has NO `.value` field. GCC error.
- **Root cause:** orelse block path (line 1886) and orelse default path (line 1895) didn't check `is_void_optional`. Only the bare orelse return path (line 1793) had the check.
- **Fix:** Added `is_void_optional` checks — block path emits `(void)0;`, default path emits `_zer_tmp.has_value ? (void)0 : fallback`.
- **Test:** `orelse_void_block.zer` (exercises ?void orelse { return 0; })

### BUG-401d: optional.inner not unwrapped for distinct typedef void
- **Symptom:** `distinct typedef void MyVoid; ?MyVoid x = null;` — emitter checks `optional.inner->kind == TYPE_VOID` without unwrapping distinct. Would emit wrong init for distinct-wrapped void.
- **Root cause:** 13 sites in emitter.c + 1 in checker.c checked `.optional.inner->kind` without `type_unwrap_distinct()`. Same pattern as BUG-279 but for inner types.
- **Fix:** All 14 sites wrapped with `type_unwrap_distinct()`. Also 6 sites checking `.pointer.inner->kind == TYPE_OPAQUE` in @ptrcast/emit_type paths.
- **Test:** Existing tests pass. Edge case too rare for dedicated test (nobody writes `distinct typedef void`).

---

## Session 2026-04-04 — VSIX + Error Messages + Windows Fixes

### BUG: Windows `--run` WinMain undefined reference
- **Symptom:** `zerc Test.zer --run` on Windows with msys64 mingw GCC fails: `undefined reference to 'WinMain'`. GCC links as GUI app instead of console app.
- **Root cause:** zerc's GCC invocation missing `-mconsole` flag on Windows.
- **Fix:** Added `#ifdef _WIN32` → `-mconsole` flag in `zerc_main.c` GCC command construction.
- **Test:** Windows-only, verified manually.

### BUG: `?T` to `T` assignment gives no orelse hint
- **Symptom:** `Handle(Task) t = heap.alloc();` — error says "cannot initialize Handle(Task) with ?Handle(Task)" but doesn't suggest `orelse { return; }`.
- **Root cause:** Generic type mismatch error path at checker.c line ~4275. No special case for `?T` → `T` mismatch.
- **Fix:** Added check: if `init_type->kind == TYPE_OPTIONAL` and `init_type->optional.inner` equals target type, show hint: "add 'orelse { return; }' to unwrap".
- **Test:** Verified manually, existing tests pass.

### FEATURE: VS Code extension auto-PATH setup
- **What:** Extension detects if `zerc` is not on system PATH on first activation. Shows prompt: "Add bundled zerc + gcc to your system PATH?" Clicking Yes uses PowerShell `[Environment]::SetEnvironmentVariable` to add extension's `bin/win32-x64/` and `bin/win32-x64/gcc/bin/` to user PATH permanently.
- **Root cause of prior issues:** Extension's `zer.lspPath` setting was hardcoded to `C:\msys64\mingw64\bin\zer-lsp.exe`, overriding bundled binary detection. Old extension `zerohexer.zer-lang-0.1.0` was also installed alongside new one.
- **Fix:** `findBundled()` works correctly. Auto-PATH prompt added. Check runs BEFORE bundled dir is injected to process PATH (avoids false positive).
- **Key lesson:** `where zerc` check must run BEFORE `process.env.PATH` prepend at line 56, otherwise it finds the bundled binary and thinks zerc is already system-wide.

### FIX: const Handle allows data mutation (like const fd)
- **Symptom:** `const Handle(Task) h; h.id = 42;` → compile error "cannot assign to const variable." Also: `if (maybe) |t| { t.id = 42; }` blocked because if-unwrap capture is const.
- **Root cause:** Assignment checker walked field chain to root ident, found const, blocked ALL writes. Didn't distinguish "modifying the key" from "modifying data through the key."
- **Fix:** In const-assign walker, set `through_pointer = true` when encountering TYPE_HANDLE in field chain. Same logic as const pointer: `const *Task p; p->id = 1;` is valid (const pointer, mutable pointee). Handle is a key (like file descriptor), const key ≠ const data.
- **Side effect:** Removed the earlier Handle-specific const check in auto-deref path (was redundant and wrong).
- **Test:** `const_handle_ok.zer` (positive — const Handle data write), `handle_if_unwrap.zer` (if-unwrap + auto-deref mutation)

### BUG: zercheck doesn't recognize Task.delete() as free
- **Symptom:** `Task.delete(t); Task.delete(t);` — no double-free error. `Task.delete(t); t.id = 99;` — no UAF error.
- **Root cause:** `zc_check_call` only matched `pool.free`/`slab.free` on TYPE_POOL/TYPE_SLAB objects. `Task.delete(t)` has TYPE_STRUCT object — skipped entirely.
- **Fix:** Added TYPE_STRUCT check in `zc_check_call` for `delete`/`delete_ptr` methods. Also added `new`/`new_ptr` recognition in `zc_check_var_init` for alloc tracking.
- **Test:** `task_delete_double.zer`, `task_delete_uaf.zer` (negative)

### BUG: Task.new() not banned in interrupt handler
- **Symptom:** `Task.new()` in `interrupt UART { }` compiles without error. Task.new() uses calloc (via Slab) which may deadlock in ISR context.
- **Root cause:** ISR check only on TYPE_SLAB `alloc`/`alloc_ptr`. Task.new() goes through TYPE_STRUCT path — no `c->in_interrupt` check.
- **Fix:** Added `c->in_interrupt` check to Task.new() and Task.new_ptr() paths.
- **Test:** Verified manually.

### BUG: Auto-slab initializer wrong field order
- **Symptom:** `Task.new()` crashes at runtime. GCC warns: "initialization of 'char **' from 'long unsigned int'". The auto-slab `sizeof(Task)` value goes into `pages` field instead of `slot_size`.
- **Root cause:** Auto-slab emission used positional initializer `{sizeof(Task), 0, 0, ...}` but `_zer_slab` struct has `slot_size` as a later field. Normal Slab emission (line 3422) uses `.slot_size = sizeof(...)` — auto-slab didn't follow the same pattern.
- **Fix:** Changed to designated initializer `{ .slot_size = sizeof(Task) }`. Rest zero-initialized by C default.
- **Found by:** Testing `Task.new()` with both bare and block orelse forms.
- **Test:** `task_new_orelse.zer` (4 variants)

### BUG: orelse block null-sentinel emits 0 instead of _zer_tmp
- **Symptom:** `*Node a = heap.alloc_ptr() orelse { return 1; };` — `a` gets assigned `0` (integer) instead of pointer. GCC warns "initialization of 'struct Node *' from 'int'". Runtime: pointer is 0 → access fault.
- **Root cause:** Emitter's orelse block path (line ~1883) emitted literal `"0;"` as final expression of statement expression for ALL orelse block fallbacks. For null-sentinel `?*T`, should emit `_zer_tmp` (the unwrapped pointer). For struct optionals, should emit `_zer_tmp.value`.
- **Fix:** Changed `emit(e, " 0; })")` to emit `_zer_tmp` for null-sentinel, `_zer_tmp.value` for struct optional — same pattern as the `orelse return` path (line 1863-1867).
- **Affected:** ALL `*T = alloc_ptr() orelse { block }` patterns. Also `?*T` orelse { block } generally.
- **Found by:** Writing HTTP server in ZER — real code exercising `alloc_ptr` + `orelse { return; }` block syntax.
- **Note:** The `orelse return;` bare form (line 1835) was correct — only the block `{ return; }` form was broken. This is why existing tests passed — they all used bare `orelse return`.
- **Test:** Verified via http_server.zer example + manual tests 2/3/5.

### BUG: zercheck false warning: defer free_ptr not recognized as free
- **Symptom:** `defer heap.free_ptr(it);` in loop — zercheck warns "handle 'it' allocated but never freed". But the defer DOES free it — emitted C shows `_zer_slab_free_ptr` correctly.
- **Root cause:** zercheck doesn't look inside defer bodies for free calls. Linear analysis skips deferred statements.
- **Status:** Known limitation. Warning only, not error. Emitted code IS correct.
- **Impact:** Cosmetic — false warning on valid code using defer + free_ptr.

### FEATURE: Task.new() / Task.delete() — auto-Slab
- **What:** `Task.new()` → `?Handle(Task)`, `Task.new_ptr()` → `?*Task`, `Task.delete(h)`, `Task.delete_ptr(p)`. No Slab declaration needed. Compiler auto-creates `_zer_auto_slab_TaskName` per struct type.
- **Implementation:** Checker: auto_slabs array on Checker struct. NODE_FIELD on TYPE_STRUCT intercepts new/new_ptr/delete/delete_ptr. Emitter: two-pass declaration (structs → auto-slabs → functions).
- **Bug found during implementation:** auto-slab declared AFTER functions → "undeclared" GCC error. Fix: two-pass emission.
- **Test:** `task_new.zer`, `task_new_ptr.zer`, `task_new_complex.zer`

## Session 2026-04-04 — Audit Round: 6 bugs fixed in new features

### BUG: goto/label missed NODE_SWITCH, NODE_DEFER, NODE_CRITICAL
- **Symptom:** Label inside switch arm → "goto target not found" false error. Goto inside switch arm not validated.
- **Root cause:** `collect_labels()` and `validate_gotos()` only recursed into NODE_BLOCK, NODE_IF, NODE_FOR, NODE_WHILE. Missing NODE_SWITCH (arm bodies), NODE_DEFER, NODE_CRITICAL.
- **Fix:** Added recursion into switch arms, defer body, critical body in both functions.
- **Test:** `goto_switch_label.zer`

### BUG: goto doesn't fire defers before jumping
- **Symptom:** `defer free(buf); goto skip;` — defer silently skipped. CLAUDE.md claimed "defer fires on all scope exits regardless of goto" but emitter didn't implement it.
- **Root cause:** `NODE_GOTO` in emitter emitted raw `goto label;` without calling `emit_defers()`. Compare with NODE_RETURN, NODE_BREAK, NODE_CONTINUE which all emit defers.
- **Fix:** Added `emit_defers(e)` before `goto` emission.
- **Test:** `goto_defer.zer`

### BUG: free_ptr() doesn't type-check argument
- **Symptom:** `tasks.free_ptr(motor_ptr)` — Motor* passed to Task pool. No error. Runtime UB (wrong slot calculation).
- **Root cause:** Checker only validated `arg_count == 1`, not argument type vs pool/slab element type.
- **Fix:** Added `type_equals` check — arg type must match `type_pointer(elem)`. Error: "pool.free_ptr() expects '*Task', got '*Motor'".
- **Test:** `free_ptr_wrong_type.zer`

### BUG: Handle auto-deref emits 0 when no allocator in scope
- **Symptom:** `u32 get_id(Handle(Task) h) { return h.id; }` — no Pool/Slab in scope. Emitter outputs `/* ERROR */ 0`. Compiles in GCC, returns wrong value silently.
- **Root cause:** Checker accepted Handle auto-deref without verifying an allocator exists. Emitter's fallback was a comment + literal 0.
- **Fix:** Checker now verifies `find_unique_allocator` or `slab_source` exists before accepting auto-deref. Error: "no Pool or Slab found for Handle(Task) — cannot auto-deref."
- **Test:** `handle_no_allocator.zer`

### BUG: const Handle allows mutation through auto-deref
- **Symptom:** `const Handle(Task) h = ...; h.id = 42;` — accepted. Mutates pool slot despite const declaration.
- **Root cause:** Const-assignment check walked field chain looking for TYPE_POINTER with is_const. Handle auto-deref produces TYPE_HANDLE, not TYPE_POINTER, so const check didn't trigger.
- **Fix:** Added const Handle check in Handle auto-deref NODE_FIELD path — if `c->in_assign_target` and handle symbol `is_const`, error.
- **Test:** `const_handle_mutation.zer`

### BUG: Ghost handle check misses alloc_ptr()
- **Symptom:** `tasks.alloc_ptr();` as bare expression — discarded pointer, slot leaked. No warning.
- **Root cause:** Ghost handle check at NODE_EXPR_STMT only matched method name `"alloc"` (5 chars). `"alloc_ptr"` (9 chars) not checked.
- **Fix:** Extended check to match both `alloc` and `alloc_ptr`.
- **Test:** `ghost_alloc_ptr.zer`

### KNOWN: zercheck doesn't track goto backward UAF (Bug #5)
- **What:** `free(h); goto retry;` where retry label is before free — zercheck processes linearly, doesn't see that execution loops back to use freed handle.
- **Status:** Known design limitation. zercheck is linear, not CFG-based. Runtime gen check catches it. Full CFG analysis would be a major refactor (~500+ lines).

---

### FIX: Handle(T)[N] array syntax not parsing
- **Symptom:** `Handle(Task)[4] tasks;` → parse error "expected ';' after variable declaration"
- **Root cause:** Parser returned TYNODE_HANDLE directly without checking for `[N]` array suffix. Array suffix only applied to `parse_base_type()` results, not Handle/Pool/etc.
- **Fix:** After parsing Handle(T), check for `TOK_LBRACKET` → wrap in TYNODE_ARRAY.
- **Test:** `handle_array.zer` (E2E — allocate, auto-deref, free in loop)

### FIX: `_zer_opaque` wrapper conflicts with user-declared malloc/free
- **Symptom:** User declares `*opaque malloc(usize size); void free(*opaque ptr);` — with `--track-cptrs`, emits `_zer_opaque malloc(...)` which conflicts with libc's `void* malloc(...)`.
- **Root cause:** `is_cstdlib` skip list missing `calloc`, `realloc`, `strdup`, `strndup`, `strlen`.
- **Fix:** Added all to skip list in `emit_top_level_decl`.
- **Test:** `opaque_safe_patterns.zer` (uses Slab to avoid the issue, but skip list prevents it for raw malloc users)

### FIX: Cross-function free_ptr not tracked by FuncSummary
- **Symptom:** `destroy(*Task t) { heap.free_ptr(t); }` — caller does `destroy(t); t.id = 99;` → no error. FuncSummary didn't track pointer params.
- **Root cause:** `zc_build_summary()` only checked `TYNODE_HANDLE` params. `TYNODE_POINTER` params skipped.
- **Fix:** Extended `has_handle_param` check and param registration to include `TYNODE_POINTER`. Summary builder now tracks `*T` params same as Handle.
- **Test:** `cross_func_free_ptr.zer` (negative — correctly rejected)

### FIX: free() on untracked key (param field) not registered as FREED
- **Symptom:** `free(c.data)` where `c` is a parameter — zercheck didn't register `c.data` as FREED because it was never tracked as ALIVE.
- **Root cause:** `is_free_call` only updated existing HandleInfo entries. Untracked keys were ignored.
- **Fix:** When `find_handle` returns NULL after `is_free_call`, create a new entry with state=FREED.
- **Test:** `opaque_struct_uaf.zer` (negative — correctly rejected)

### FEATURE: Handle auto-deref (h.field → slab.get(h).field)
- **What:** `h.id = 1` now works where h is Handle(Task). Compiler auto-inserts gen-checked `.get()` call. Same 100% ABA safety.
- **Implementation:** Checker NODE_FIELD on TYPE_HANDLE resolves struct field. `Symbol.slab_source` tracks allocator provenance. `find_unique_allocator()` as fallback. Emitter emits `_zer_slab_get` or `_zer_pool_get` wrapper.
- **Test:** `handle_autoderef.zer`, `handle_autoderef_pool.zer` (E2E)

### FEATURE: alloc_ptr/free_ptr — *Task from Slab/Pool
- **What:** `*Task t = heap.alloc_ptr()` returns a direct pointer instead of Handle. `heap.free_ptr(t)` frees it. zercheck tracks it at compile time (Level 9) — UAF and double-free caught.
- **Safety:** 100% compile-time for pure ZER code. Level 2+3+5 runtime backup for C interop boundary.
- **Implementation:** Checker: new methods on TYPE_SLAB and TYPE_POOL. Emitter: alloc+get combined, `_zer_slab_free_ptr` preamble function. zercheck: `alloc_ptr` recognized as allocation, `free_ptr` as free, NODE_FIELD root ident check for freed status.
- **Test:** `alloc_ptr.zer`, `alloc_ptr_pool.zer`, `alloc_ptr_mixed.zer` (positive), `alloc_ptr_uaf.zer`, `alloc_ptr_double_free.zer` (negative)

### FEATURE: goto + labels (forward + backward)
- **What:** Full goto support with labels. Forward and backward jumps. `NODE_GOTO` + `NODE_LABEL` AST nodes. `TOK_GOTO` keyword + `TOK_COLON` token added to lexer.
- **Safety:** goto inside defer block → compile error. Label validation: target must exist in same function, no duplicate labels. Both forward and backward safe due to auto-zero + defer.
- **Implementation:** ~70 lines across lexer.h, lexer.c, ast.h, parser.c, checker.c, emitter.c.
- **Test:** `tests/zer/goto_label.zer` (forward, backward, nested loop break, error path), `tests/zer_fail/goto_bad_label.zer` (nonexistent target rejected).

### FEATURE: Default compile to exe (temp .c hidden)
- **What:** `zerc main.zer` now compiles to `main.exe` by default. The `.c` intermediate is temp, deleted after GCC. Use `--emit-c` to keep the `.c` file. `-o file.c` also keeps it.
- **Implementation:** `use_temp_c` flag in zerc_main.c. `remove(output_path)` after GCC. `do_run` only triggers execution, not compilation (compilation now always happens by default).
- **Rationale:** Looks like a native compiler. Users see `.zer → exe`. The emit-C-via-GCC architecture is an implementation detail, not a user concern.

### FEATURE: VS Code extension version 0.2.6
- **Changes:** Auto-PATH prompt, `-mconsole` fix in bundled zerc, `?T` orelse hint, `[*]T` + `[]T` deprecation warning.

---

## Session 2026-04-03 — External Audit + Pipeline Integration

## Session 2026-04-03 — [*]T Syntax + []T Deprecation

### FEATURE: [*]T dynamic pointer syntax
- **What:** Added `[*]T` as preferred syntax for slices (dynamic pointer to many). Reads as "pointer to many" — C devs understand `*` = pointer. `[]T` reads as "empty array" which confuses C devs.
- **Implementation:** Parser change only (~10 lines). `TOK_LBRACKET` + `TOK_STAR` + `TOK_RBRACKET` → `TYNODE_SLICE`. Same internal type as `[]T`. Zero checker/emitter changes.
- **Test:** `tests/zer/star_slice.zer` (E2E), 5 checker tests in `test_checker_full.c`.
- **Design doc:** `docs/ZER_STARS.md`

### FEATURE: []T deprecation warning
- **What:** Parser now warns "[]T is deprecated, use [*]T instead" with source line + caret display when `[]T` is used.
- **Implementation:** Added `warn()` function in parser.c. Warning suppressed when `parser.source == NULL` (test harness mode) to avoid noise from 200+ test strings still using `[]T`.
- **Test:** Verified warning fires on real `.zer` files, silent in test harness. Backward compat test in `test_checker_full.c`.

### DESIGN: Handle auto-deref + Task.new() + alloc_ptr() superseded
- **What:** Design for three syntactic sugars: (1) `h.field` auto-inserts `slab.get(h).field` (100% safe, same gen check), (2) `Task.new()` auto-creates module-level Slab (like C's malloc), (3) `alloc_ptr()` superseded by Handle auto-deref (100% > 95%).
- **Design doc:** `docs/ZER_SUGAR.md` (495 lines, full context)
- **Status:** Design only, not implemented yet. ~70 lines for Handle auto-deref, ~50 for Task.new().

---

### BUG-401: Volatile TOCTOU — range propagation unsound for volatile
- **Symptom:** `if (hw_status != 0) { 100 / hw_status; }` where `hw_status` is volatile MMIO — range propagation proves divisor nonzero, skips runtime trap. But volatile can change between check and use.
- **Root cause:** `push_var_range` in if-guard path never checked if the variable's symbol is `is_volatile`.
- **Fix:** Before narrowing range from guard comparison, look up symbol. If `is_volatile`, skip range narrowing entirely. Volatile variables always keep runtime safety checks.
- **Test:** `test_checker_full.c` — volatile TOCTOU rejected, read-once-into-local pattern accepted.

### BUG-402: ISR compound assign field-blind
- **Symptom:** `g_state.flags |= 1` in interrupt handler — ISR safety analysis misses compound assign on struct fields because `track_isr_global` only fires on `NODE_IDENT` targets.
- **Root cause:** Line 1459 checked `node->assign.target->kind == NODE_IDENT`. Struct field targets (`NODE_FIELD`) bypassed the ISR tracking.
- **Fix:** Walk field/index/deref chain to root ident before calling `track_isr_global`. Same walker pattern as scope escape analysis.
- **Test:** `test_checker_full.c` — struct field compound assign tracked.

### BUG-404: Pointer indexing has no bounds check
- **Symptom:** `p[1000000] = 42` where `p` is `*u32` — emits raw C indexing with no `_zer_bounds_check`. Bypasses ALL of ZER's bounds safety.
- **Root cause:** checker.c TYPE_POINTER case in NODE_INDEX returned `pointer.inner` with no validation or warning.
- **Fix:** Added warning for non-volatile pointer indexing: "use slice for bounds-checked access." Not banned (too common in C interop) but made visible. Volatile pointers (MMIO) skip the warning.
- **Test:** Warning emitted on pointer indexing. Volatile pointer indexing is silent (MMIO use case).

### BUG-405: Slab.alloc() allowed in interrupt handler
- **Symptom:** `tasks.alloc()` inside `interrupt UART { }` compiles without error. On real hardware, Slab.alloc() calls calloc which may use a global mutex — deadlock if main thread is also allocating.
- **Root cause:** No `c->in_interrupt` check in Slab alloc method validation.
- **Fix:** Added `c->in_interrupt` check before Slab alloc. Error: "slab.alloc() not allowed in interrupt handler — use Pool(T, N) instead." Pool is safe (static, no malloc).
- **Test:** `tests/zer_fail/isr_slab_alloc.zer` — Slab.alloc in interrupt rejected.

### BUG-406: Ghost Handle — discarded alloc result
- **Symptom:** `pool.alloc();` as bare expression statement — handle returned but never assigned. Allocation leaked silently.
- **Root cause:** NODE_EXPR_STMT didn't check if the expression was a pool/slab alloc() call with discarded result.
- **Fix:** After check_expr in NODE_EXPR_STMT, detect pool.alloc()/slab.alloc() calls and error: "discarded alloc result — handle leaked."
- **Test:** `tests/zer_fail/ghost_handle.zer` — bare pool.alloc() rejected.

### BUG-407: MMIO pointer indexing unchecked
- **Symptom:** `gpio[100]` where `gpio` is `volatile *u32` from `@inttoptr` with `mmio 0x40020000..0x4002001F` — compiles without error even though index 100 is far outside the 32-byte MMIO range.
- **Root cause:** TYPE_POINTER indexing in NODE_INDEX had no bounds information. The `mmio` range declaration existed but was never connected to pointer indexing.
- **Fix:** When `@inttoptr(*T, addr)` is assigned to a variable, calculate `mmio_bound = (range_end - addr + 1) / sizeof(T)` from the matching `mmio` range. Store on Symbol. In NODE_INDEX for TYPE_POINTER, if `mmio_bound > 0` and index is constant, check `idx < mmio_bound`. Compile error on OOB. Both local and global var-decl paths set the bound.
- **Test:** `test_checker_full.c` — MMIO index 7 valid (range = 8), index 8 and 100 rejected. 4 tests.

### BUG-403: zercheck not integrated into zerc pipeline
- **Symptom:** zercheck (UAF, double-free, leak detection) never ran during `zerc` compilation. All 50+ zercheck tests passed because they called `zercheck_run()` directly in the test harness. Users had ZERO zercheck protection.
- **Root cause:** `zerc_main.c` never called `zercheck_run()`. The function existed, was tested, but never invoked in the actual compiler pipeline.
- **Fix:** Added `zercheck_run()` call after `checker_check_bodies()` in `zerc_main.c`. Leaks demoted to warnings (zercheck can't perfectly track handles across function calls or in struct fields). UAF and double-free remain compile errors. Arena allocations excluded from handle tracking.
- **Test:** 7 negative `.zer` tests in `tests/zer_fail/` that exercise every safety system through the full `zerc` pipeline.

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

### BUG-317: Return orelse @ptrcast(&local) escape
- **Symptom:** `return opt orelse @ptrcast(*u8, &x)` compiles — local address escapes through intrinsic in orelse fallback.
- **Root cause:** NODE_RETURN orelse root walk didn't inspect NODE_INTRINSIC or NODE_UNARY(&) in fallback.
- **Fix:** Walk into ptrcast/bitcast intrinsics and & expressions in orelse fallback. Only when return type is pointer (value bitcasts safe).
- **Test:** `test_checker_full.c` — return orelse @ptrcast(&local) rejected.

### BUG-318: Orelse fallback flag propagation missing
- **Symptom:** `*u32 q = opt orelse p` where `p` is local-derived — `q` not marked local-derived, escapes to global.
- **Root cause:** Var-decl init flag propagation only checked `orelse.expr`, not `orelse.fallback`.
- **Fix:** Check both sides — split NODE_ORELSE into two root checks for local/arena-derived.
- **Test:** `test_checker_full.c` — orelse alias local-derived escape rejected.

### BUG-320: @size(distinct void) bypass
- **Symptom:** `distinct typedef void MyVoid; @size(MyVoid)` compiles — void has no size.
- **Root cause:** @size only checked `type_arg` for void/opaque. Named types (distinct typedef) parse as expression args (NODE_IDENT), not type_arg.
- **Fix:** Also check expression arg's resolved type. Call `type_unwrap_distinct` before TYPE_VOID/TYPE_OPAQUE check.
- **Test:** `test_checker_full.c` — @size(distinct void) rejected.

### BUG-314: Orelse assignment escape to global
- **Symptom:** `g_ptr = opt orelse &x` where `x` is local — compiles, creates dangling pointer.
- **Root cause:** NODE_ASSIGN escape check only looked at direct `NODE_UNARY/TOK_AMP`, didn't walk into `NODE_ORELSE` fallback.
- **Fix:** Assignment flag propagation walks into NODE_ORELSE fallback for `&local` and local-derived idents. Direct escape check added for `orelse &local` → global target.
- **Test:** `test_checker_full.c` — orelse &local escape rejected, orelse &global accepted.

### BUG-315: Distinct slice comparison bypass
- **Symptom:** `distinct typedef []u8 Buffer; a == b` passes checker, GCC rejects with "invalid operands."
- **Root cause:** Binary ==/!= check used `left->kind == TYPE_SLICE` without unwrapping distinct.
- **Fix:** Call `type_unwrap_distinct()` on both operands before TYPE_SLICE/TYPE_ARRAY check.
- **Test:** `test_checker_full.c` — distinct slice comparison rejected.

### BUG-316: Bit-set index double evaluation
- **Symptom:** `reg[get_hi()..get_lo()] = val` calls `get_hi()` 2x and `get_lo()` 4x in emitted C.
- **Root cause:** Runtime bit-set path emitted hi/lo expressions inline multiple times (mask calc + shift).
- **Fix:** Hoist into `_zer_bh`/`_zer_bl` uint64_t temps at start of statement expression. Constant path unchanged.
- **Test:** Implicit — existing bit-set tests pass, constant path verified (reg[7..4] = 5 → 80).

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

## Red Team Audit — Session 2026-03-28

### BUG-314: Recursive struct bypass via distinct typedef
- **Symptom:** `distinct typedef S SafeS; struct S { SafeS next; }` bypasses self-reference check.
- **Root cause:** `register_decl` compares `inner == t` (pointer equality) — `TYPE_DISTINCT` wrapping `t` is a different pointer.
- **Fix:** Call `type_unwrap_distinct(inner)` before the self-reference check. Same fix for unions.
- **Test:** `test_checker_full.c` — distinct self-reference rejected, distinct of different struct accepted.

### BUG-315: Array return bypass via distinct typedef
- **Symptom:** `distinct typedef u8[10] Buffer; Buffer get_data()` passes checker — emits invalid C.
- **Root cause:** `check_func_body` checks `ret->kind == TYPE_ARRAY` without unwrapping distinct.
- **Fix:** `type_unwrap_distinct(ret)` before `TYPE_ARRAY` check.
- **Test:** `test_checker_full.c` — distinct array return rejected, distinct non-array return accepted.

### BUG-316: Intrinsic named type resolution for @bitcast/@truncate/@saturate
- **Symptom:** `@bitcast(Meters, x)` where `Meters` is distinct typedef → parse error "expected type".
- **Root cause:** `force_type_arg` only set for `@cast`, not `@bitcast`/`@truncate`/`@saturate`.
- **Fix:** Set `force_type_arg` for all four intrinsics.
- **Test:** `test_checker_full.c` — @bitcast, @truncate, @saturate with distinct types accepted.

### BUG-317: keep validation false positive on imported globals
- **Symptom:** Passing imported global to `keep` param falsely rejected as "local variable".
- **Root cause:** `scope_lookup_local(global_scope, raw_name)` — imported globals stored under mangled key.
- **Fix:** Fallback: try mangled key (`module_name`) when raw lookup fails.
- **Test:** Multi-module pattern (keep with imported global).

### BUG-318: Compiler UB in constant folder shift
- **Symptom:** `1 << 62` on large `int64_t` values → signed overflow UB in the compiler itself.
- **Root cause:** `l << r` with signed `l` — C UB when result exceeds `INT64_MAX`.
- **Fix:** Cast to `(uint64_t)l << r` then back to `int64_t`.
- **Test:** `test_checker_full.c` — large shift expressions don't crash compiler.

### BUG-319: Volatile stripping in var-decl orelse
- **Symptom:** `volatile ?u32 reg; u32 val = reg orelse 0;` — temporary loses volatile.
- **Root cause:** `__auto_type _zer_or = expr` — GCC's `__auto_type` drops volatile.
- **Fix:** Use `__typeof__(expr) _zer_or = expr` — `__typeof__` preserves qualifiers.
- **Test:** Existing E2E tests pass. Volatile orelse now emits `__typeof__`.

### BUG-320: Volatile stripping from source in array copy
- **Symptom:** `dst = volatile_src` uses memmove — strips volatile on read.
- **Root cause:** Only target checked for volatile (`expr_is_volatile(e, target)`), source ignored.
- **Fix:** `arr_volatile = expr_is_volatile(target) || expr_is_volatile(value)`. Source cast uses `const volatile uint8_t*`. Same fix for var-decl init path.
- **Test:** Existing E2E tests pass.

### BUG-321: Volatile stripping in mutable captures
- **Symptom:** `if (volatile_reg) |*v| { ... }` — `v` declared non-volatile.
- **Root cause:** Capture pointer emission checked `is_const` but not `is_volatile`.
- **Fix:** Emit `volatile` prefix on capture pointer when `cond_vol` is true.
- **Test:** Existing E2E tests pass.

### BUG-322: Qualifier loss in __auto_type captures
- **Symptom:** All capture variables (`|v|` and `|*v|`) lose volatile/const via `__auto_type`.
- **Root cause:** `__auto_type` in GCC drops qualifiers from deduced type.
- **Fix:** Replace all 3 capture `__auto_type` sites with `__typeof__()` which preserves qualifiers.
- **Test:** Existing E2E tests pass.

### BUG-325: @bitcast width bypass for structs/unions
- **Symptom:** `@bitcast(Big, small_struct)` accepted — memcpy over-reads.
- **Root cause:** `type_width()` returns 0 for structs. Check `tw > 0 && vw > 0` skipped for 0 vs 0.
- **Fix:** Fall back to `compute_type_size() * 8` when `type_width` returns 0.
- **Test:** `test_checker_full.c` — different-size structs rejected, same-size accepted.

### BUG-326: Const-safety bypass in switch captures
- **Symptom:** `const ?u32 val; switch(val) { default => |*v| { *v = 10; } }` — writes to const.
- **Root cause:** Switch capture `cap_const = false` for mutable `|*v|` without checking source.
- **Fix:** Walk switch expr to root ident, check `is_const`. Apply to both union and optional switch paths.
- **Test:** `test_checker_full.c` — mutable capture on const rejected, on non-const accepted.

### BUG-332: Multi-module symbol collision via underscore separator
- **Symptom:** `mod_a` with symbol `b_c` and `mod_a_b` with symbol `c` both emit `mod_a_b_c` in C.
- **Root cause:** Single underscore `_` separator between module name and symbol name.
- **Fix:** Changed to double underscore `__` separator. `mod_a__b_c` vs `mod_a_b__c` are always distinct. Updated all 8 sites (3 checker registrations, 1 checker lookup, 4 emitter emissions).
- **Test:** All 10 module import tests pass with new separator.

### BUG-334: keep bypass via local array-to-slice coercion
- **Symptom:** `reg(local_buf)` where `reg` takes `keep []u8` — accepted, stack array escapes.
- **Root cause:** keep validation only checked `&local` and `is_local_derived`, not local arrays coerced to slices.
- **Fix:** Check if arg is local `TYPE_ARRAY` (not static/global) when param is `keep`.
- **Test:** `test_checker_full.c` — local array to keep slice rejected.

### BUG-335: zercheck missing handle capture tracking
- **Symptom:** `if (pool.alloc()) |h| { pool.free(h); pool.get(h); }` — no use-after-free error.
- **Root cause:** if-unwrap captures not registered in zercheck's PathState.
- **Fix:** Detect `pool.alloc()` condition in NODE_IF with capture, register capture as HS_ALIVE.
- **Test:** Zercheck captures now tracked for use-after-free detection.

### BUG-336: arena-derived pointer to keep parameter
- **Symptom:** `reg(arena_ptr)` where `reg` takes `keep *T` — accepted, arena memory can be reset.
- **Root cause:** keep validation didn't check `is_arena_derived` flag.
- **Fix:** Reject `is_arena_derived` arguments for keep parameters.
- **Test:** `test_checker_full.c` — arena-derived to keep rejected.

### BUG-337: union variant lock bypass via pointer alias in struct field
- **Symptom:** `s.ptr.b = 10` where `s.ptr` aliases locked union — not caught.
- **Root cause:** Variant lock only checked root ident name, not intermediate pointer types.
- **Fix:** During mutation walk, check if any field's object type is a pointer to the locked union type.
- **Test:** Existing union mutation tests pass, alias through struct pointer blocked.

### BUG-338: is_local_derived escape via intrinsics (@ptrcast/@bitcast)
- **Symptom:** `*opaque p = @ptrcast(*opaque, &x); reg(p);` — local escapes via cast wrapping.
- **Root cause:** Flag propagation walk didn't enter NODE_INTRINSIC or NODE_UNARY(&) to find root.
- **Fix:** Walk into intrinsic args (last arg) and & unary in both var-decl init and &local detection paths.
- **Test:** All existing tests pass. Pattern now blocks cast-wrapped local escapes.

### BUG-339: keep bypass via orelse fallback in function calls
- **Symptom:** `reg(opt orelse &x)` — orelse fallback provides local to keep param.
- **Root cause:** keep validation only checked direct &local, not orelse branches.
- **Fix:** Unwrap orelse — check both expr and fallback. Also walk into intrinsics in both paths.
- **Test:** All existing tests pass.

### BUG-340: Union variant assignment double evaluation
- **Symptom:** `get_msg().sensor = val` — get_msg() called twice (tag update + value assignment).
- **Root cause:** Emitter evaluated union target object twice in comma expression.
- **Fix:** Hoist object into `__typeof__` pointer temp: `_zer_up = &(obj); _zer_up->_tag = N; _zer_up->variant = val`.
- **Test:** All existing E2E tests pass.

### BUG-341: Volatile stripping via @bitcast
- **Symptom:** `*u32 p = @bitcast(*u32, volatile_ptr)` — strips volatile silently.
- **Root cause:** BUG-258 fixed @ptrcast but same check was missing for @bitcast.
- **Fix:** Same volatile check as @ptrcast: source pointer.is_volatile or symbol is_volatile → reject if target not volatile.
- **Test:** All existing tests pass.

### BUG-343: Volatile/const stripping via @cast
- **Symptom:** `distinct typedef *u32 SafePtr; SafePtr p = @cast(SafePtr, volatile_reg)` — strips volatile silently. Same for const.
- **Root cause:** BUG-258 fixed @ptrcast and BUG-341 fixed @bitcast, but @cast (distinct typedef conversion) was missed.
- **Fix:** Added volatile and const checks to @cast handler: unwrap distinct on both sides, check pointer qualifiers. Same pattern as @ptrcast/@bitcast.
- **Test:** test_checker_full.c — 3 new tests (volatile strip rejected, const strip rejected, volatile preserved OK).

### BUG-344: Multiplication overflow in compute_type_size
- **Symptom:** Multi-dimensional arrays with large dimensions could cause `elem_size * count` to wrap via -fwrapv, producing a small positive value.
- **Root cause:** Raw multiplication without overflow guard on line 285 of checker.c.
- **Fix:** Added overflow check: `if (count > 0 && elem_size > INT64_MAX / count) return CONST_EVAL_FAIL`. Falls back to emitter's sizeof() for massive arrays.
- **Test:** test_checker_full.c — @size on large struct OK (no overflow).

### BUG-345: Handle(T) width spec/impl mismatch
- **Symptom:** ZER-LANG.md spec said Handle is platform-width (u32 on 32-bit, u64 on 64-bit). Implementation is always u32.
- **Root cause:** Spec written with future 64-bit targets in mind, but Pool/Slab runtime hardcodes uint32_t and 0xFFFF masks throughout.
- **Fix:** Updated spec (ZER-LANG.md) to match implementation: Handle is always u32 with 16-bit index + 16-bit generation. Changing to platform-width would require runtime rewrite (deferred).
- **Test:** N/A (documentation fix).

### BUG-346: Non-thread-safe non_storable_nodes global (RF12)
- **Symptom:** `non_storable_nodes` was a static global array shared across all Checker instances. LSP concurrent requests could corrupt the list.
- **Root cause:** Legacy design from before RF1 (typemap refactor). Was documented as known technical debt.
- **Fix:** Moved `non_storable_nodes`, `non_storable_count`, `non_storable_capacity` into Checker struct. All call sites now pass Checker pointer. Arena pointer from `c->arena` replaces separate `non_storable_arena` global.
- **Test:** All existing tests pass (no behavior change for single-threaded use).

### BUG-348: Missing memory barriers in Ring push/pop
- **Symptom:** Ring spec (§22) promises internal barriers ("Ring handles barriers INTERNALLY"), but emitted code had no `__atomic_thread_fence` calls.
- **Root cause:** Ring runtime `_zer_ring_push` and inline pop code were implemented without barriers.
- **Fix:** Added `__atomic_thread_fence(__ATOMIC_RELEASE)` in `_zer_ring_push` between data write and head update. Added `__atomic_thread_fence(__ATOMIC_ACQUIRE)` in pop after data read, before tail update. Ensures interrupt/other core sees data before seeing updated head/tail.
- **Test:** All E2E tests pass (barriers are no-ops on single-threaded tests but present in emitted C).

### BUG-349: Module registration order breaks transitive struct deps
- **Symptom:** `main imports mid, mid imports base` — `struct Point` from `base` not in scope when `mid` registers `struct Holder { Point pt; }`. Error: "undefined type 'Point'".
- **Root cause:** Modules registered in BFS discovery order (mid before base), but topological sort only applied to emission, not registration.
- **Fix:** Compute topological order once, reuse for registration, body checking, AND emission. Dependencies registered before dependents.
- **Test:** New module test `transitive.zer` — 3-level import chain with cross-module struct fields. 11 module tests total.

### BUG-350: Array alignment in compute_type_size
- **Symptom:** `struct S { u8 a; u8[10] data; u8 b; }` — `@size(S)` returned 24 but GCC says 12. `u8[10]` got falign=8 (capped size) instead of 1 (element alignment).
- **Root cause:** Generic alignment formula `min(fsize, 8)` doesn't account for arrays whose alignment equals their element type, not their total size.
- **Fix:** Array alignment computed from element type (recursing through multi-dim). Struct alignment computed from max field alignment. Generic types use `min(fsize, 8)` as before.
- **Test:** test_checker_full.c — @size struct with array uses element alignment. 429 checker tests total.

## Safe Intrinsics (Features, not bugs)

### FEAT: mmio Range Registry (@inttoptr validation)
- **What:** New `mmio 0xSTART..0xEND;` top-level declaration. Constant `@inttoptr` addresses validated against ranges at compile time. Variable addresses get runtime range check + trap. No mmio declarations = all allowed (backward compat).
- **Scope:** lexer (TOK_MMIO), parser (NODE_MMIO), ast (mmio_decl), checker (mmio_ranges array + @inttoptr validation), emitter (runtime range check + comment).
- **Test:** 6 new checker tests (valid range, outside range, backward compat, multiple ranges, start>end rejected, variable address).

### FEAT: @ptrcast Type Provenance Tracking
- **What:** Tracks original type through `*opaque` round-trips. `*opaque ctx = @ptrcast(*opaque, &sensor)` records provenance = `*Sensor`. Later `@ptrcast(*Motor, ctx)` → compile error (provenance mismatch). Unknown provenance (params, cinclude) allowed.
- **Scope:** types.h (Symbol.provenance_type), checker.c (set in var-decl init + assignment, check in @ptrcast handler). Propagates through aliases, clears+re-derives on assignment.
- **Test:** 4 new checker tests (round-trip OK, wrong type rejected, unknown allowed, alias propagation).

### FEAT: @container Field Validation + Provenance Tracking
- **What:** (1) Validates field exists in struct — was unchecked, GCC caught it. (2) Tracks which struct+field a pointer was derived from via `&struct.field`. Wrong struct or field in `@container` → compile error when provenance known.
- **Scope:** types.h (Symbol.container_struct/field), checker.c (set in var-decl/assign on &struct.field, check in @container handler). Propagates through aliases.
- **Test:** 6 new checker tests (field exists, field missing, proven containment, wrong struct, wrong field, unknown allowed).

### FEAT: comptime Functions (compile-time evaluated, type-checked macros)
- **What:** New `comptime` keyword prefix for functions. Body evaluated at compile time with argument substitution. All args must be compile-time constants. Results inlined as literals in emitted C. Comptime function bodies not emitted. Replaces C `#define` function-like macros.
- **Scope:** lexer (TOK_COMPTIME), ast (func_decl.is_comptime, call.comptime_value/is_comptime_resolved), parser (comptime prefix), types.h (Symbol.is_comptime), checker (eval_comptime_block/eval_const_expr_subst + call-site interception), emitter (skip comptime funcs + emit constant for calls). Extended eval_const_expr with comparisons, logical, XOR, NOT.
- **Test:** 8 checker tests + 2 E2E tests. 452 checker, 215 E2E.

### FEAT: comptime if (conditional compilation, #ifdef replacement)
- **What:** `comptime if (CONST) { ... } else { ... }` — condition evaluated at compile time, only taken branch type-checked and emitted. Dead branch stripped entirely. Replaces C `#ifdef/#ifndef/#endif`.
- **Scope:** ast (if_stmt.is_comptime), parser (comptime + if at statement level), checker (eval condition, check only taken branch), emitter (emit only taken branch).
- **Test:** 3 checker tests + 1 E2E test. 455 checker, 216 E2E.

### BUG-351: @cast escape bypass — local address via distinct typedef
- **Symptom:** `return @cast(SafePtr, &x)` where SafePtr is `distinct typedef *u32` — compiles fine, returns dangling stack pointer.
- **Root cause:** Escape check sites (return + orelse fallback) only walked into @ptrcast and @bitcast, not @cast.
- **Fix:** Added `(ilen == 4 && memcmp(iname, "cast", 4) == 0)` to both escape check sites.
- **Test:** test_checker_full.c — @cast local escape rejected.

### BUG-352: Volatile stripping in union switch rvalue temp
- **Symptom:** `switch(get_volatile_union())` — rvalue temp uses `__auto_type` which strips volatile.
- **Root cause:** Line 2455 in emitter.c used `__auto_type` for rvalue union switch temps.
- **Fix:** Changed to `__typeof__(expr)` which preserves volatile qualifier.
- **Test:** Existing tests pass (narrow edge case — rvalue volatile union switch).

### BUG-354: comptime if breaks all_paths_return
- **Symptom:** `u32 f() { comptime if (1) { return 1; } }` — rejected with "not all paths return" even though the taken branch always returns.
- **Root cause:** `all_paths_return` didn't check `is_comptime` — treated comptime if like regular if, requiring both branches.
- **Fix:** In `all_paths_return(NODE_IF)`, if `is_comptime`, evaluate condition and only check the taken branch.
- **Test:** test_checker_full.c — comptime if true without else OK, comptime if false with else OK.

### BUG-355: Assignment escape through intrinsic wrapper
- **Symptom:** `g_ptr = @ptrcast(*u32, p)` where p is local-derived — compiles fine, stores dangling pointer in global.
- **Root cause:** Assignment escape check (BUG-205) only checked `NODE_IDENT` values, not `NODE_INTRINSIC`-wrapped values.
- **Fix:** Walk through intrinsics before checking root ident: `while (vnode->kind == NODE_INTRINSIC) vnode = vnode->intrinsic.args[last]`.
- **Test:** test_checker_full.c — @ptrcast and @cast assignment escape rejected.

### BUG-356: is_local_derived lost through pointer dereference
- **Symptom:** `*u32 p2 = *pp` where `pp` is `**u32` pointing to local-derived `p` — `p2` not marked local-derived, `return p2` accepted (dangling pointer).
- **Root cause:** Flag propagation walk handled field, index, intrinsic, and & — but not deref (*). The walk stopped at NODE_UNARY(STAR).
- **Fix:** Added `NODE_UNARY(TOK_STAR)` to the walk — deref walks into operand to check its flags.
- **Test:** test_checker_full.c — deref flag propagation catches escape.

### BUG-358: Provenance lost through @bitcast/@cast
- **Symptom:** `*opaque q = @bitcast(*opaque, ctx)` where `ctx` has provenance `*Sensor` — `q` loses provenance, allowing wrong-type `@ptrcast(*Motor, q)`.
- **Root cause:** Provenance alias propagation only checked direct NODE_IDENT. @bitcast and @cast wrapping an ident was not walked through.
- **Fix:** Walk through all intrinsics before checking root ident for provenance: `while (prov_root->kind == NODE_INTRINSIC) prov_root = prov_root->intrinsic.args[last]`.
- **Test:** test_checker_full.c — provenance preserved through @bitcast.

### BUG-360: Identity washing — local address escape through function return
- **Symptom:** `return identity(&x)` where `identity(*u32 p) { return p; }` — compiles fine, returns dangling stack pointer.
- **Root cause:** NODE_CALL return values never marked as local-derived, even when pointer arguments were local-derived.
- **Fix:** In NODE_RETURN, if return expr is NODE_CALL returning pointer type, check all args for &local and local-derived idents. Also added same check in NODE_VAR_DECL init for call results.
- **Test:** Existing tests pass. Identity washing caught.

### BUG-361: zercheck global handle blindspot
- **Symptom:** `g_h = pool.alloc(); pool.free(g_h); pool.get(g_h);` — zercheck didn't track handles assigned via NODE_ASSIGN (only NODE_VAR_DECL).
- **Root cause:** zc_check_expr(NODE_ASSIGN) handled aliasing but not pool.alloc() assignment.
- **Fix:** Added pool.alloc() detection in NODE_ASSIGN handler — registers handle in PathState same as var-decl init.
- **Test:** Existing tests pass.

### BUG-362: Struct field summation overflow
- **Symptom:** compute_type_size could overflow when summing multiple massive array fields.
- **Root cause:** `total += fsize` without overflow guard (BUG-344 only guarded array multiplication).
- **Fix:** Added `if (fsize > 0 && total > INT64_MAX - fsize) return CONST_EVAL_FAIL` before field summation.
- **Test:** Existing tests pass.

### BUG-363: usize width tied to host instead of target
- **Symptom:** `type_width(TYPE_USIZE)` returned `sizeof(size_t) * 8` from the host machine. Cross-compiling from 64-bit host for 32-bit target → checker uses wrong width for coercion/truncation validation.
- **Root cause:** `types.c` used `sizeof(size_t)` which reflects the compiler's own platform, not the target.
- **Fix:** Added `zer_target_ptr_bits` global (default 32 for embedded targets). `type_width(TYPE_USIZE)` returns this value. `--target-bits N` flag to override. Emitted C uses `size_t` which GCC resolves per target — always correct. `can_implicit_coerce` updated: same-width u32↔usize coercion allowed when involving TYPE_USIZE.
- **Test:** usize widening test updated for 32-bit default.

### BUG-364: Union alignment uses size not element alignment
- **Symptom:** Union containing `u8[10]` variant gets `data_align = 8` instead of 1. Same bug as BUG-350 but in the union path.
- **Root cause:** `compute_type_size(TYPE_UNION)` used `max_variant` size for alignment instead of computing per-variant element alignment.
- **Fix:** Same pattern as BUG-350: array variants use element alignment, struct variants use max field alignment. Iterate variants to find max alignment independently from max size.
- **Test:** Existing tests pass.

### BUG-370: keep validation bypass via nested orelse
- **Symptom:** `reg(o_local orelse opt orelse &x)` where `o_local` is local-derived — bypasses keep check. Nested orelse chains hide local-derived pointers.
- **Root cause:** Keep validation only unwrapped one level of orelse (BUG-339). For `a orelse b orelse c` (AST: ORELSE(a, ORELSE(b, c))), only `a` and `ORELSE(b,c)` were checked, not `b` and `c` individually. Also BUG-221 local-derived ident check only fired on direct NODE_IDENT, not through orelse expr chain.
- **Fix:** Recursive orelse collection into keep_checks array (up to 8 branches). Added separate walk through orelse expr chain to check local-derived idents for keep params.
- **Test:** Existing tests pass. Nested orelse keep bypass now caught.

### BUG-371: MMIO range bypass for constant expressions
- **Symptom:** `@inttoptr(*u32, 0x50000000 + 0)` with mmio ranges — checker skipped validation because address arg was NODE_BINARY, not NODE_INT_LIT.
- **Root cause:** MMIO range check only triggered for `node->intrinsic.args[0]->kind == NODE_INT_LIT`. Constant expressions (binary ops on literals) were treated as variable addresses → runtime check instead of compile-time error.
- **Fix:** Use `eval_const_expr()` on the address arg. If it returns a constant (not CONST_EVAL_FAIL), validate against mmio ranges at compile time.
- **Test:** Existing tests pass. Constant expression addresses now validated.

### BUG-372: void as slice/pointer inner type allowed
- **Symptom:** `[]void x` and `*void p` compiled without error. Void is for return types only per spec — slices/pointers to void have no semantic meaning.
- **Root cause:** `resolve_type` for TYNODE_POINTER and TYNODE_SLICE didn't check inner type for TYPE_VOID.
- **Fix:** Added void rejection in both TYNODE_POINTER and TYNODE_SLICE resolution. `*void` → "use *opaque for type-erased pointers". `[]void` → "void has no size".
- **Test:** Existing tests pass.

### BUG-393: *opaque provenance — compile-time compound keys + runtime type tags
- **Symptom:** Provenance tracking only worked for simple variables. Struct fields (`h.p`), array elements (`arr[0]`), function returns all lost provenance.
- **Root cause:** `provenance_type` on Symbol only tracked simple variable assignments. No compound path tracking, no runtime fallback.
- **Fix (3 layers):**
  1. **Compile-time Symbol-level** (existing): `ctx = @ptrcast(*opaque, &s)` → `ctx.provenance_type = *Sensor`. Catches simple ident mismatches.
  2. **Compile-time compound keys** (new): `h.p = @ptrcast(*opaque, &s)` → stores provenance under key `"h.p"` in `prov_map`. @ptrcast check tries `build_expr_key` + `prov_map_get` when source isn't simple ident. Catches struct fields and constant array indices.
  3. **Runtime type tags** (new): `*opaque` emitted as `_zer_opaque{void *ptr, uint32_t type_id}`. Each struct/enum/union gets unique `type_id`. @ptrcast checks type_id at runtime — traps on mismatch. Catches everything including variable indices and function returns.
- **Coverage:** Simple idents → compile-time. `h.p`, `arr[0]` → compile-time. `arr[i]`, `get_ctx()` → runtime. 100% total.
- **Test:** 2 checker tests (struct field mismatch/match), 2 E2E tests (round-trip, struct field round-trip).

### BUG-393 runtime implementation: *opaque runtime provenance via type tags
- **Symptom:** Provenance tracking for `@ptrcast` was compile-time only, stored on Symbol. Struct fields (`h.p`), array elements (`arr[i]`), function returns, and cross-function flows all lost provenance.
- **Root cause:** `provenance_type` on Symbol only tracked simple variable assignments. No runtime fallback for cases the compiler couldn't prove.
- **Fix:** `*opaque` in emitted C becomes `_zer_opaque` struct: `{ void *ptr; uint32_t type_id; }`. Each struct/enum/union gets a unique `type_id` assigned during `register_decl`. `@ptrcast(*opaque, sensor_ptr)` wraps with `(_zer_opaque){(void*)ptr, TYPE_ID}`. `@ptrcast(*Sensor, ctx)` unwraps with runtime check: `if (ctx.type_id != expected && ctx.type_id != 0) _zer_trap(...)`. Type ID 0 = unknown (params, cinclude) → always allowed.
- **Breaking changes:** `?*opaque` is no longer a null sentinel — becomes struct optional `_zer_opt_opaque`. Symbol-level `provenance_type` removed (was compile-time only). Old provenance checker tests changed from `err` to `ok` (runtime catches instead).
- **Coverage:** 100% of `*opaque` round-trips — local vars, struct fields, arrays, function returns, cross-function. The type_id travels with the data, not compiler metadata.
- **Test:** 2 E2E tests: simple round-trip (42), struct field round-trip (99). 3 checker tests updated from `err` to `ok`.

### BUG-391: comptime function calls as array sizes
- **Symptom:** `u8[BIT(3)] buf` failed — "array size must be a compile-time constant". comptime couldn't replace C macros for buffer sizing.
- **Root cause:** `eval_const_expr` in ast.h has no Checker access, can't resolve NODE_CALL. Array size resolution in TYNODE_ARRAY only tried `eval_const_expr`, never attempted comptime evaluation.
- **Fix:** In `resolve_type_inner(TYNODE_ARRAY)`, when `eval_const_expr` returns CONST_EVAL_FAIL and size expr is NODE_CALL with comptime callee, evaluate via `eval_comptime_block`. Forward-declared `ComptimeParam` and `eval_comptime_block` above `resolve_type_inner`.
- **Limitation:** Nested comptime calls in array sizes (`BUF_SIZE()` calling `BIT()`) don't work yet — `eval_comptime_block` can't resolve nested comptime calls. Direct calls with literal args work.
- **Test:** 2 tests added: `BIT(3)` and `SLOTS(2)` as array sizes.

### BUG-392: Union array lock blocks all elements
- **Symptom:** `switch(msgs[0]) { .data => |*v| { msgs[1].data = 20; } }` — rejected even though msgs[1] is independent.
- **Root cause:** Union switch lock stored only the root ident name (`"msgs"`). Any assignment to any element of `msgs` triggered the lock.
- **Fix:** Added `union_switch_key` to Checker — full expression key built via `build_expr_key()` (e.g., `"msgs[0]"`). Mutation check compares target's object key against switch key. Different keys (e.g., `"msgs[1]" != "msgs[0]"`) are allowed. Same element and pointer aliases still blocked.
- **Test:** 2 tests added: different element allowed, same element blocked.

### BUG-389: eval_const_expr stack overflow on deep expressions
- **Symptom:** Pathological input with deeply nested arithmetic (e.g., `1+1+1+...` 2000 levels) crashes compiler with stack overflow.
- **Root cause:** `eval_const_expr` in ast.h was purely recursive with no depth limit. Unlike `check_expr` (which has `expr_depth` guard), the constant folder had no protection.
- **Fix:** Renamed to `eval_const_expr_d(Node *n, int depth)` with `depth > 256 → CONST_EVAL_FAIL` guard. Added `eval_const_expr(Node *n)` wrapper that calls with depth 0. All recursive calls pass `depth + 1`.
- **Test:** Existing tests pass. Parser depth limit (64) prevents most pathological input from reaching this code.

### BUG-390: Handle generation counter wraps at 65536 (ABA problem)
- **Symptom:** After 65,536 alloc/free cycles on a single Pool/Slab slot, `uint16_t` gen wraps to 0. A stale handle from gen 0 passes the safety check — use-after-free undetected.
- **Root cause:** Handle was `uint32_t` with `gen(16) << 16 | idx(16)`. Gen counter was `uint16_t`.
- **Fix:** Handle is now `uint64_t` with `gen(32) << 32 | idx(32)`. Gen counter is `uint32_t`. 4 billion cycles per slot before wrap — practically infinite. Updated: Pool struct (gen array), Slab struct (gen pointer), all alloc/get/free functions, optional Handle type (`_zer_opt_u64`), alloc call emission.
- **Test:** All existing Pool/Slab E2E tests pass with u64 handles.

### BUG-386: Pool/Ring/Slab allowed in union variants
- **Symptom:** `union Oops { Pool(u32, 4) p; u32 other; }` compiled — produces invalid C (macro inside union).
- **Root cause:** BUG-287 added the check for struct fields but not union variants.
- **Fix:** Added same TYPE_POOL/TYPE_RING/TYPE_SLAB check to union variant registration in NODE_UNION_DECL.
- **Test:** 2 tests added: Pool and Ring in union rejected.

### BUG-387: orelse keep fallback local-derived bypass
- **Symptom:** `reg(opt orelse local_ptr)` where `local_ptr` is local-derived — passes keep validation.
- **Root cause:** BUG-370 orelse walk only followed `orelse.expr` (the expression side), never checked `orelse.fallback` for local/arena-derived idents.
- **Fix:** Rewrote keep orelse walk to collect ALL terminal nodes from orelse chain (both expr and fallback sides, up to 8 branches). Each checked for is_local_derived and is_arena_derived.
- **Test:** 1 test added: orelse fallback local-derived pointer rejected.

### BUG-388: comptime optional emission wrong
- **Symptom:** `comptime ?u32 maybe(u32 x) { ... }` — call emitted as `10` instead of `(_zer_opt_u32){10, 1}`. GCC error or wrong struct initialization.
- **Root cause:** Emitter comptime path emitted raw `%lld` without checking if return type is TYPE_OPTIONAL.
- **Fix:** Check `checker_get_type` on comptime call node. If TYPE_OPTIONAL, wrap in `(type){value, 1}`.
- **Test:** Verified emitted C shows correct optional struct literal.

### BUG-383: Identity washing via struct wrappers
- **Symptom:** `return wrap(&x).p` — wraps local address in struct, extracts pointer field. BUG-360 only checked direct call with pointer return type, not struct-returning calls with field extraction.
- **Root cause:** BUG-360/374 check required `node->ret.expr->kind == NODE_CALL && ret_type->kind == TYPE_POINTER`. When return expr is NODE_FIELD on NODE_CALL (struct wrapper), the call was never inspected.
- **Fix:** In NODE_RETURN, walk return expression through field/index chains. If root is NODE_CALL with local-derived args and final return type is pointer, reject. Same walk added to NODE_VAR_DECL init path.
- **Test:** 2 tests added: `wrap(&local).p` rejected, `wrap(&global).p` allowed.

### BUG-384: @cstr volatile source stripping
- **Symptom:** `@cstr(local_buf, mmio_buf[0..4])` emitted `memcpy` — GCC may optimize away volatile reads from hardware buffer.
- **Root cause:** @cstr emitter only checked destination volatility (BUG-223), not source. Also `expr_root_symbol` didn't walk through NODE_SLICE.
- **Fix:** (1) Added `src_volatile` check via `expr_is_volatile` on source arg. `any_volatile = dest_volatile || src_volatile` triggers byte loop. Source pointer cast uses `volatile const uint8_t*` when source is volatile. (2) Added NODE_SLICE to `expr_root_symbol` walk — `mmio_buf[0..4]` now correctly resolves to `mmio_buf` symbol.
- **Test:** Verified emitted C uses byte loop with volatile source cast.

### BUG-385: zercheck doesn't scan struct params for Handle fields
- **Symptom:** `void f(State s) { pool.free(s.h); pool.get(s.h); }` — UAF not detected. zercheck only registered direct `Handle(T)` params, not handles nested in struct params.
- **Root cause:** `zc_check_function` param scan only checked `TYNODE_HANDLE`. Struct params (`TYNODE_NAMED`) were ignored.
- **Fix:** For TYNODE_NAMED params, resolve via checker's global scope, walk struct fields for TYPE_HANDLE. Build compound keys `"param.field"` and register as HS_ALIVE.
- **Test:** 3 tests added: struct param UAF, double free, valid lifecycle.

### BUG-381: @container strips volatile qualifier
- **Symptom:** `volatile *u32 ptr = ...; *Device d = @container(*Device, ptr, list)` — emitted C casts volatile pointer to non-volatile `Device*`. GCC optimizes away subsequent hardware register accesses.
- **Root cause:** @container emitter emitted `(T*)((char*)(ptr) - offsetof(T, field))` without checking source volatility. Checker also didn't validate volatile like @ptrcast (BUG-258).
- **Fix:** Checker: if source pointer is volatile (type-level or symbol-level) and target is non-volatile pointer, error. Same pattern as @ptrcast BUG-258. Emitter: `expr_is_volatile()` check on source arg, prepends `volatile` to cast type.
- **Test:** 2 tests added: volatile stripping rejected, volatile preserved accepted.

### BUG-357: zercheck cannot track handles in arrays or struct fields
- **Symptom:** `pool.free(arr[0]); pool.get(arr[0])` — use-after-free invisible to zercheck. Same for `pool.free(s.h); pool.get(s.h)`.
- **Root cause:** `find_handle` matched by flat name string. `pool.free(arr[0])` arg is NODE_INDEX, not NODE_IDENT — `find_handle` never matched.
- **Fix:** Added `handle_key_from_expr()` helper that builds string keys from NODE_IDENT (`"h"`), NODE_FIELD (`"s.h"`), NODE_INDEX with constant index (`"arr[0]"`). Updated all handle lookup/registration sites in `zc_check_call` (free/get), `zc_check_var_init` (aliasing), and assignment tracking to use compound keys. Arena-allocated keys for stored HandleInfo pointers. Variable indices (`arr[i]`) remain untrackable (runtime traps only).
- **Test:** 5 tests added: array UAF, array double-free, struct field UAF, valid array lifecycle, independent array indices.

### BUG-373: is_literal_compatible uses host sizeof(size_t) for usize range
- **Symptom:** `usize x = 5000000000` accepted on 64-bit host compiling for 32-bit target (--target-bits 32). Value truncated silently by target GCC.
- **Root cause:** Two issues: (1) `is_literal_compatible` used `sizeof(size_t) == 8` (host) instead of `zer_target_ptr_bits == 64` (target). (2) Integer literals default to `ty_u32`, so `can_implicit_coerce(u32, usize)` succeeded before `is_literal_compatible` was ever checked — oversized values bypassed range validation.
- **Fix:** Changed `is_literal_compatible` to use `zer_target_ptr_bits`. Added explicit literal range checks in NODE_VAR_DECL, NODE_ASSIGN, and NODE_GLOBAL_VAR that fire AFTER coercion passes — always validates integer literal values against target type range.
- **Test:** 3 tests added: 5B rejected on 32-bit, accepted on 64-bit, u32_max accepted on 32-bit.

### BUG-374: Nested identity washing bypass via nested calls
- **Symptom:** `return identity(identity(&x))` bypassed BUG-360 escape check. Outer call saw NODE_CALL arg, not NODE_IDENT or NODE_UNARY(&).
- **Root cause:** BUG-360 only checked direct args of the call for &local/local-derived. Nested calls hid the local pointer one level deep.
- **Fix:** Added `call_has_local_derived_arg()` recursive helper. Checks args for &local, local-derived idents, AND recurses into pointer-returning NODE_CALL args (max depth 8). Used in both NODE_RETURN and NODE_VAR_DECL BUG-360 paths.
- **Test:** 2 tests added: triple-nested identity caught, identity(&global) allowed.

### BUG-375: Missing type validation for pointer intrinsics
- **Symptom:** `@inttoptr(u32, addr)`, `@ptrcast(u32, ptr)`, `@container(*S, non_ptr, f)` compiled — produced invalid C that GCC rejected.
- **Root cause:** @inttoptr and @ptrcast validated source types but not target type (must be pointer). @container didn't validate that first arg is a pointer.
- **Fix:** Added target type checks: @inttoptr → `result` must be TYPE_POINTER. @ptrcast → `result` must be TYPE_POINTER or TYPE_FUNC_PTR. @container → first arg must be TYPE_POINTER.
- **Test:** 3 tests added: each intrinsic with non-pointer target/source rejected.

### BUG-377: Local array escape via orelse fallback
- **Symptom:** `g_slice = opt orelse local_buf` — global slice got pointer to stack memory. Orelse fallback provided a local array as slice, unchecked.
- **Root cause:** BUG-240 assignment check and BUG-203 var_decl check only inspected direct value, not orelse fallback branches.
- **Fix:** Both NODE_ASSIGN (BUG-240) and NODE_VAR_DECL (BUG-203) now also check orelse fallback for local array roots. Assignment path collects both direct value and orelse fallback into `arr_checks[]`. Var_decl path iterates over init + fallback in `arr_roots[]`.
- **Test:** 2 tests added: orelse array assign to global, orelse array var_decl then assign to global.

### zercheck change 1: MAYBE_FREED state
- **Symptom:** `if (cond) { pool.free(h); } pool.get(h)` — not caught. Conditional frees on one branch left handle as ALIVE (under-approximation).
- **Root cause:** if/else merge only marked FREED when BOTH branches freed. If-without-else kept handle ALIVE. Switch only marked FREED when ALL arms freed.
- **Fix:** Added `HS_MAYBE_FREED` state. if/else: one branch frees → MAYBE_FREED. if-without-else: then-branch frees → MAYBE_FREED. Switch: some arms free → MAYBE_FREED. pool.get/pool.free on MAYBE_FREED handle → error.
- **Test:** 9 new tests: if-no-else use/free after, both-branch-free OK, one-branch leak, partial switch caught.

### zercheck change 2: Handle leak detection
- **Symptom:** `h = pool.alloc(); /* never freed */` — silently compiled. `h = pool.alloc(); h = pool.alloc()` — first handle silently leaked.
- **Root cause:** zercheck only tracked use-after-free and double-free. No check for handles that were never freed or overwritten.
- **Fix:** At function exit, scan PathState for HS_ALIVE or HS_MAYBE_FREED handles allocated inside the function → error. At alloc assignment, check if target handle already HS_ALIVE → error "overwritten while alive". Parameter handles excluded (caller responsible).
- **Test:** 4 new tests: alloc without free, overwrite alive handle, clean lifecycle OK, param not freed OK.

### zercheck change 3: Loop second pass
- **Symptom:** Conditional free patterns spanning loop iterations weren't caught by the single-pass analysis.
- **Root cause:** zercheck ran loop body once. If a conditional free changed handle state, the second iteration wasn't analyzed.
- **Fix:** After first loop pass, if any handle state changed from pre-loop, run body once more. If state still unstable after second pass → widen to MAYBE_FREED.
- **Test:** 1 new test: free-then-realloc in loop (valid cycling pattern) stays OK.

### zercheck change 4: Cross-function analysis
- **Symptom:** `void free_handle(Handle(T) h) { pool.free(h); }` — calling `free_handle(h)` then `pool.get(h)` was not caught. zercheck didn't follow non-builtin calls.
- **Root cause:** `zc_check_call` only handled `pool.method()` calls (NODE_FIELD callee). Regular function calls (NODE_IDENT callee) were invisible to zercheck.
- **Fix:** Pre-scan phase builds `FuncSummary` for each function with Handle params. `zc_build_summary()` runs existing `zc_check_stmt` walker with `building_summary=true` (suppresses error reporting). After walking, checks each Handle param's final state → `frees_param[]` (FREED) or `maybe_frees_param[]` (MAYBE_FREED). At call sites, `zc_apply_summary()` looks up callee summary and applies effects to caller's PathState. Also propagates to aliases.
- **Test:** 6 new tests: wrapper frees → UAF, wrapper frees → double free, wrapper uses (no free) → OK, conditional free wrapper → MAYBE_FREED, non-handle param → no effect, process-and-free wrapper → caller clean.

### Value range propagation
- **Feature:** Compiler tracks `{min_val, max_val, known_nonzero}` per variable through control flow. Eliminates redundant runtime bounds and division checks.
- **Implementation:** `VarRange` stack on Checker struct. `push_var_range()` adds shadowing entries. Save/restore via count for scoped narrowing. `proven_safe` array tracks nodes proven safe. `checker_is_proven()` exposed to emitter.
- **Patterns covered:**
  - Literal array index (`arr[3]` where 3 < arr size) → bounds check skipped
  - For-loop variable (`for (i = 0; i < N; ...)`) → `arr[i]` bounds check skipped inside body
  - Guard pattern (`if (i >= N) { return; }`) → `arr[i]` bounds check skipped after guard
  - Literal divisor (`x / 4`) → division check skipped
  - Nonzero guard (`if (d == 0) { return; }`) → `x / d` division check skipped
- **Emitter changes:** NODE_INDEX checks `checker_is_proven()` → emits plain `arr[idx]` without `_zer_bounds_check`. NODE_BINARY (TOK_SLASH/TOK_PERCENT) checks → emits plain `(a / b)` without `({ if (_d == 0) trap; })`.
- **Test:** 5 new E2E tests: literal index, for-loop index, literal divisor, division after guard, bounds after guard.

### Forced division guard
- **Feature:** Division by zero is C undefined behavior. ZER now requires proof that the divisor is nonzero for simple variable divisors. Complex expressions (struct fields, function calls) keep runtime check.
- **Implementation:** ~10 lines in checker.c NODE_BINARY handler, after range propagation check. If divisor is NODE_IDENT and not in `proven_safe`, emit compile error with fix suggestion.
- **Error message:** `divisor 'd' not proven nonzero — add 'if (d == 0) { return; }' before division`
- **Proof methods:** literal nonzero, `u32 d = 5` init, `if (d == 0) return` guard, for loop `i = 1..N`
- **Test:** 7 new checker tests: literal OK, var init OK, guard OK, unguarded error, modulo error, modulo+guard OK, for-loop var OK.

### Bounds auto-guard
- **Feature:** When an array index cannot be proven safe by range propagation, the compiler inserts an invisible bounds guard (if-return) before the containing statement instead of trapping at runtime.
- **Design decision:** An earlier "forced bounds guard" design was rejected — it required the programmer to add explicit guards everywhere, breaking hundreds of tests and being too invasive. The final design makes the compiler responsible: prove it OR auto-guard it, always with a warning.
- **Implementation:** `AutoGuard` struct + `auto_guard_count/capacity` on Checker. `checker_mark_auto_guard()` called in checker when index is unproven. `emit_auto_guards(e, stmt)` walks statement tree, emits `if (idx >= size) { return <zero>; }` before the statement. `emit_zero_value()` helper emits correct zero for any return type (void/int/bool/pointer/optional). `_zer_bounds_check` still present after guard (belt and suspenders). Warning emitted to programmer.
- **API:** `checker_auto_guard_size(c, node)` — emitter queries guard size for a given node.
- **Test:** 5 new E2E tests: param index, global array, volatile index, computed index, guard suppresses warning.

### Auto-keep on function pointer pointer-params
- **Feature:** When a call goes through a function pointer (not a direct named function call), ALL pointer arguments are automatically treated as `keep` parameters. The compiler cannot inspect the callee's body to know if the pointer escapes.
- **Implementation:** In NODE_CALL keep validation: if callee is not NODE_IDENT, or the resolved ident symbol has `is_function == false` (it's a fn-ptr variable), all pointer args are assumed kept. Invisible to programmer — no annotation needed.
- **Test:** 2 new checker tests: fn-ptr call with local ptr (blocked), direct call with no-keep param (allowed).

### @cstr overflow auto-return
- **Feature:** @cstr buffer overflow previously called `_zer_trap()` (crash). Now emits the same auto-return pattern as bounds auto-guard — `if (src.len + 1 > dest_size) { emit_defers(); return <zero_value>; }`.
- **Root cause of change:** A crash on buffer overflow is unrecoverable and not useful for embedded systems. A silent return-zero is consistent with the bounds auto-guard design.
- **Implementation:** Emitter @cstr handler replaced `_zer_trap(...)` with `emit_zero_value()` + `emit_defers()` call. Applied to both array-dest and slice-dest overflow paths.
- **Test:** 2 new E2E tests: @cstr overflow in void function (returns), @cstr overflow in u32 function (returns 0).

### *opaque array homogeneous provenance
- **Feature:** All elements of a `*opaque` array must have the same concrete type. `arr[0] = @ptrcast(*opaque, &sensor)` and `arr[1] = @ptrcast(*opaque, &motor)` in the same array is a compile error.
- **Implementation:** `prov_map_set()` — when key contains `[`, also sets root key. If root key already has DIFFERENT provenance → error: `"heterogeneous *opaque array — all elements must have the same type"`.
- **Test:** 2 new checker tests: homogeneous OK, mixed types error.

### Cross-function *opaque provenance summaries
- **Feature:** If a function always returns a specific concrete type cast to `*opaque`, callers automatically get that provenance when assigning to `*opaque` variables — no `@ptrcast` annotation needed at call sites.
- **Implementation:** `find_return_provenance(c, func_node)` walks body for NODE_RETURN with @ptrcast source or provenance-carrying ident. `add_prov_summary()` / `lookup_prov_summary()` on Checker. Built after checking each `*opaque`-returning function. Applied in NODE_VAR_DECL when init is a call to a function with known summary.
- **Test:** 3 new checker tests: propagation to local var, mismatch detected, unknown function (no summary) passes.

### Struct field range propagation
- **Feature:** Range propagation + forced division guard extended to struct fields via `build_expr_key()`.
- **Bug:** compound key `cmp_key_buf` was stack-local, pointer dangled after if-block scope. Fix: arena-allocate before pushing to var_ranges.
- **Test:** 2 new checker tests.

### Whole-program *opaque param provenance
- **Feature:** Post-check pass validates call-site argument provenance against callee's @ptrcast expected type.
- **Implementation:** `find_param_cast_type()` + `ParamExpect` on Checker + `check_call_provenance()` Pass 3.
- **Test:** 3 new checker tests.

### @probe intrinsic
- **Feature:** `@probe(addr)` → `?u32`. Safe MMIO read — catches hardware faults, returns null.
- **Bug fixed:** `NODE_INTRINSIC` returning `?T` not handled in var-decl optional init — double-wrapped. Fix: added to direct-assign check.
- **Test:** 7 checker + 3 E2E tests.

### MMIO auto-discovery
- **Feature:** 5-phase boot scan when `--no-strict-mmio` + `@inttoptr` + no `mmio`. Discovers all hardware via brute-force RCC/power controller + rescan.
- **Implementation:** `has_inttoptr()` AST scan, `__attribute__((constructor))` discovery, `_zer_mmio_valid()` validation.
- **Test:** 1 E2E test.

### Forward declaration emission fix
- **Symptom:** Functions forward-declared without body (extern, mutual recursion) produced GCC errors: "conflicting types" or "implicit declaration" when the return type was non-int (bool, struct, opaque).
- **Root cause:** Emitter skipped forward declarations without body — no C prototype emitted. GCC assumed `int` return, conflicting with actual return type.
- **Fix:** Emit C prototypes for ALL bodyless forward declarations. For forward decls WITH later definition in same file, still emit prototype (needed for mutual recursion). Skip well-known C stdlib names (puts, printf, etc.) to avoid conflicting with `<stdio.h>`.
- **Test:** Existing mutual recursion test (is_even/is_odd) now passes. Pre-existing bug.

### Nested comptime function calls
- **Symptom:** `comptime u32 BUF_SIZE() { return BIT(3) * 4; }` failed with "body could not be evaluated" when BIT is another comptime function.
- **Root cause:** `eval_const_expr_subst` handled NODE_IDENT, NODE_INT_LIT, NODE_BINARY but NOT NODE_CALL. Nested comptime calls within comptime bodies returned CONST_EVAL_FAIL.
- **Fix:** Added `eval_comptime_call_subst()` — when NODE_CALL is encountered during comptime evaluation, looks up callee via `_comptime_global_scope`, evaluates args with current substitution, recursively evaluates callee body. `_comptime_global_scope` set before each comptime evaluation site (array size resolution + call-site evaluation).
- **Test:** 2 new checker tests + 1 E2E test.

### NODE_INTRINSIC optional init fix
- **Symptom:** `?u32 val = @probe(addr);` emitted `(_zer_opt_u32){ _zer_probe(addr), 1 }` — double-wrapping because @probe already returns `_zer_opt_u32`.
- **Root cause:** Var-decl optional init path only checked NODE_CALL and NODE_ORELSE for direct assignment. NODE_INTRINSIC fell to the `else` branch that wrapped in `{ val, 1 }`.
- **Fix:** Added `NODE_INTRINSIC` to the `NODE_CALL || NODE_ORELSE` direct-assign check in emitter NODE_VAR_DECL.

### Auto-discovery removal + mmio startup validation (design decision, not bug)
- **Decision (2026-04-01):** Removed 5-phase brute-force auto-discovery boot scan and `_zer_mmio_valid()` runtime gate.
- **Why:** Auto-discovery couldn't find locked/gated/write-only peripherals (~80% coverage presented as 100%). `_zer_mmio_valid()` false-blocked legitimate MMIO accesses. STM32-centric RCC brute-forcing didn't work on other chip families.
- **Removed:** `has_inttoptr()` AST scanner, `_zer_disc_scan`, `_zer_disc_brute_enable`, `_zer_mmio_discover` constructor, `_zer_mmio_valid`, `_zer_in_disc`, `_zer_disc_add`, `need_discovery_check` path in @inttoptr emission.
- **Added:** mmio declaration startup validation — `_zer_mmio_validate()` as `__attribute__((constructor))` probes start address of each declared `mmio` range via `@probe()`. Wrong datasheet address caught at first power-on. Skips wildcard ranges and x86 hosted.
- **@probe kept:** Standalone intrinsic for safe MMIO reads. Fault handler preamble unchanged.

### Comptime mutual recursion segfault
- **Symptom:** `comptime u32 crash(u32 n) { return crash(n); }` with `u32[crash(1)] arr;` caused compiler segfault (stack overflow from infinite recursion in comptime evaluator).
- **Root cause:** `eval_comptime_block` and `eval_comptime_call_subst` had no recursion depth limit. `eval_const_expr_d` had depth limit (BUG-389) but the comptime function evaluator path bypassed it.
- **Fix:** Added `static int depth` counter in `eval_comptime_block` (limit 32) and `_comptime_call_depth` in `eval_comptime_call_subst` (limit 16). Also added `_subst_depth` guard in `eval_const_expr_subst` NODE_CALL handler. Three-layer guard prevents stack overflow from any recursion path.
- **Test:** 1 new checker test (comptime mutual recursion → error, not crash).
- **Note:** Previous Docker builds were caching old code, masking the fix. Required `docker build --no-cache` to verify. Also found leftover `_comptime_block_depth` reference from partial edit — caused compile failure in Docker.

### Pool/Slab generation counter ABA prevention
- **Symptom:** After 2^32 alloc/free cycles on a single slot, generation counter wraps to 0. Stale handles from first cycle match — silent use-after-free on long-running servers.
- **Root cause:** `gen[idx]++` in free has no overflow guard. u32 wraps at 4,294,967,296.
- **Fix:** Cap gen at 0xFFFFFFFF (never wrap). In free: `if (gen[idx] < 0xFFFFFFFFu) gen[idx]++`. In alloc: skip slots where `gen[idx] == 0xFFFFFFFFu` (permanently retired). Retired slots stay used=0 so get() always traps. One slot lost per 2^32 cycles — negligible.
- **Applied to:** Both Pool (`_zer_pool_alloc`/`_zer_pool_free`) and Slab (`_zer_slab_alloc`/`_zer_slab_free`).
- **Note:** First proposed fix (set used=1 on wrap) was WRONG — would let stale handles with gen=0 pass get() check. Correct fix keeps slot free (used=0) so get() always rejects.

### Pool/Slab zero-handle collision
- **Symptom:** `Handle(T) h;` (zero-initialized) passes `get()` check when slot 0 was allocated with gen=0. Silent memory access via uninitialized handle.
- **Root cause:** Pool/Slab gen arrays start at 0 (C static init / calloc). First alloc returns handle `(gen=0 << 32 | idx=0)` = 0. Zero-initialized Handle also = 0. Match → silent UAF.
- **Fix:** In alloc, before returning handle: `if (gen[i] == 0) gen[i] = 1`. First alloc returns gen=1. Zero handle (gen=0) never matches any valid allocation. Applied to Pool alloc, Slab alloc (scan path), and Slab alloc (grow path).

### Struct wrapper launders local-derived pointers — FIXED
- **Symptom:** `return identity(wrap(&x).p)` and `Box b = wrap(&x); return b.p` — local pointer escapes via struct field wrapping. Checker missed it.
- **Root cause:** Two gaps: (1) `call_has_local_derived_arg` didn't check NODE_FIELD on NODE_CALL args (missed `wrap(&x).p` as arg to `identity`). (2) NODE_VAR_DECL only marked local-derived for pointer results, not struct results containing pointers.
- **Fix:** (1) Added field-to-call-root walk in `call_has_local_derived_arg` — if arg is NODE_FIELD chain leading to NODE_CALL with local-derived args, return true. (2) Extended var-decl local-derived check from `TYPE_POINTER` only to `TYPE_POINTER || TYPE_STRUCT`. Struct result from call with local-derived args marks the variable.
- **Test:** 3 new checker tests: identity(wrap(&x).p), Box b = wrap(&x) return b.p, wrap(global).p OK.

### Slab metadata calloc overflow on 64-bit
- **Symptom:** Heap corruption when Slab grows to many pages on 64-bit systems.
- **Root cause:** `calloc(nc * _ZER_SLAB_PAGE_SLOTS, sizeof(uint32_t))` — `nc` is uint32_t, multiplication overflows at 2^26 pages (2^32 items). calloc gets tiny value, memcpy overwrites heap.
- **Fix:** Cast to size_t: `calloc((size_t)nc * _ZER_SLAB_PAGE_SLOTS, ...)`. Applied to both gen and used arrays.

### Range propagation stale guard bypass
- **Symptom:** `if (i < 10) { i = get_input(); arr[i] = 1; }` — `arr[i]` skipped bounds check because `i` had stale proven range [0,9] from before reassignment.
- **Root cause:** `push_var_range` intersects with existing ranges (only narrows). Reassignment `i = get_input()` didn't invalidate the existing range — intersection of [0,9] with [INT64_MIN,INT64_MAX] = [0,9].
- **Fix:** In NODE_ASSIGN (TOK_EQ), directly overwrite existing VarRange entry via `find_var_range()`. Non-literal assignment → wipe to full range. Literal → set exact value. No intersection.
- **Test:** 1 new checker test.

### Compound /= and %= bypass forced division guard
- **Symptom:** `x /= d` compiled without error even when `d` not proven nonzero. `x / d` correctly errored.
- **Root cause:** Forced division guard only checked NODE_BINARY (TOK_SLASH/TOK_PERCENT). Compound assignments (TOK_SLASHEQ/TOK_PERCENTEQ) are handled in NODE_ASSIGN, which had no division guard.
- **Fix:** Added forced division guard in NODE_ASSIGN compound path: check divisor is literal nonzero or range-proven nonzero, else error.
- **Test:** 2 new checker tests (/= error, /= literal OK).

### Identity washing via orelse fallback
- **Symptom:** `return identity(opt orelse &x)` — local pointer `&x` in orelse fallback escapes through function call. Checker didn't catch it.
- **Root cause:** `call_has_local_derived_arg` checked NODE_UNARY(&), NODE_IDENT, NODE_CALL, NODE_FIELD — but not NODE_ORELSE. Orelse fallback `&x` was invisible to the escape walker.
- **Fix:** Added NODE_ORELSE case in `call_has_local_derived_arg`: checks fallback for direct `&local` and local-derived idents.
- **Test:** 1 new checker test.

### @cstr local-derived not propagated
- **Symptom:** `*u8 p = @cstr(local_buf, "hi"); return identity(p);` — pointer to local buffer escapes. `p` not marked local-derived from @cstr.
- **Root cause:** NODE_VAR_DECL only checked `&local` and NODE_CALL for local-derived init. `@cstr(local, ...)` is NODE_INTRINSIC — not checked.
- **Fix:** Added NODE_INTRINSIC("cstr") case in var-decl: walks first arg (buffer) to root ident, marks local-derived if local.
- **Test:** 1 new checker test.

### Slice escape via struct wrapper not caught
- **Symptom:** `return wrap(local_array).data` — slice pointing to stack escapes via struct field. BUG-383 only checked TYPE_POINTER returns, not TYPE_SLICE.
- **Root cause:** Two gaps: (1) BUG-360/383 return checks used `ret_type->kind == TYPE_POINTER` — slices excluded. (2) `call_has_local_derived_arg` didn't detect local arrays passed as slice args (array→slice coercion).
- **Fix:** (1) Extended return checks to `TYPE_POINTER || TYPE_SLICE`. (2) Added TYPE_ARRAY check in `call_has_local_derived_arg` — local array passed to function treated as local-derived. (3) Var-decl local-derived marking extended to TYPE_SLICE results.
- **Test:** 1 new checker test.

### identity(@cstr(local,...)) direct arg escape
- **Symptom:** `return identity(@cstr(local, "hi"))` — @cstr result (pointer to local buffer) passed directly as call argument. `call_has_local_derived_arg` didn't check NODE_INTRINSIC args.
- **Root cause:** Previous fix only handled `p = @cstr(local,...); return identity(p)` (intermediate variable). Direct `identity(@cstr(local,...))` has the @cstr as a NODE_INTRINSIC arg to the call — no intermediate symbol to mark.
- **Fix:** Added NODE_INTRINSIC("cstr") case in `call_has_local_derived_arg`: walks first arg (buffer) to root ident, returns true if local.
- **Test:** 1 new checker test.

---

## Session 2026-04-06 — @critical Safety + Auto-Guard Walker Gaps

### BUG-433: emit_auto_guards missing NODE_INTRINSIC, NODE_SLICE, NODE_ORELSE fallback
- **Symptom:** `u32 x = @bitcast(u32, arr[i])` where `i` is unproven — auto-guard silently skipped. The checker marked `arr[i]` for auto-guard, but the emitter's expression walker didn't recurse into intrinsic args. Instead of a graceful return, the hard `_zer_bounds_check` trap fired.
- **Root cause:** `emit_auto_guards` only handled NODE_INDEX, NODE_FIELD, NODE_ASSIGN, NODE_BINARY, NODE_UNARY, NODE_CALL, NODE_ORELSE. Missing: NODE_INTRINSIC (intrinsic args), NODE_SLICE (sub-slice expressions), NODE_ORELSE fallback (value fallback path).
- **Fix:** Added 3 cases to `emit_auto_guards`: NODE_INTRINSIC (recurse all args), NODE_SLICE (object/start/end), NODE_ORELSE (recurse fallback when not return/break/continue).
- **Test:** `autoguard_intrinsic.zer` — array index inside @bitcast, auto-guard fires correctly.

### BUG-434: contains_break missing NODE_CRITICAL
- **Symptom:** `while(true) { @critical { break; } }` — `contains_break` didn't recurse into `@critical` body, so `all_paths_return` falsely considered the while(true) loop a terminator (infinite, no break). Functions could appear to "always return" when they actually exit via break from a critical block.
- **Root cause:** `contains_break` switch had no NODE_CRITICAL case — fell to `default: return false`.
- **Fix:** Added `case NODE_CRITICAL: return contains_break(node->critical.body);`.
- **Test:** `critical_break.zer` (negative — break banned inside @critical, see BUG-436).

### BUG-435: all_paths_return missing NODE_CRITICAL
- **Symptom:** `@critical { return 42; }` as last statement in non-void function — rejected with "not all control flow paths return a value" even though it always returns.
- **Root cause:** `all_paths_return` switch had no NODE_CRITICAL case — fell to `default: return false`.
- **Fix:** Added `case NODE_CRITICAL: return all_paths_return(node->critical.body);`.
- **Test:** `critical_return.zer` (negative — return banned inside @critical, see BUG-436).

### BUG-436: return/break/continue/goto inside @critical not banned — leaves interrupts disabled
- **Symptom:** `@critical { return; }` compiled without error. The interrupt re-enable code (emitted after the body) became dead code — interrupts permanently disabled. Same class as `defer` control flow ban (BUG-192).
- **Root cause:** `@critical` had no `critical_depth` tracking. `defer` uses `defer_depth` to ban return/break/continue/goto, but `@critical` had no equivalent.
- **Fix:** Added `int critical_depth` to Checker struct. Incremented/decremented in NODE_CRITICAL handler. All 4 control flow nodes (return, break, continue, goto) check `critical_depth > 0` → compile error with message "interrupts would not be re-enabled".
- **Test:** `critical_return.zer` (negative), `critical_break.zer` (negative).

### BUG-437: zercheck zc_check_stmt missing NODE_CRITICAL
- **Symptom:** `@critical { pool.free(h); pool.get(h); }` — use-after-free invisible to zercheck. Handle tracking didn't recurse into @critical block bodies.
- **Root cause:** `zc_check_stmt` switch had no NODE_CRITICAL case. Also `block_always_exits` didn't handle NODE_CRITICAL.
- **Fix:** Added `case NODE_CRITICAL: zc_check_stmt(zc, ps, node->critical.body);` in zc_check_stmt. Added `if (node->kind == NODE_CRITICAL) return block_always_exits(node->critical.body);` in block_always_exits.
- **Test:** `critical_handle.zer` (positive — handle ops inside @critical tracked correctly).

### BUG-438: distinct union variant assignment skips tag update
- **Symptom:** `distinct typedef Msg SafeMsg; SafeMsg sm; sm.sensor = 42;` — emitted C was plain `sm.sensor = 42;` without `_tag = 0` update. Switch on the union would read stale tag, matching wrong variant.
- **Root cause:** Emitter line 860 checked `obj_type->kind == TYPE_UNION` without calling `type_unwrap_distinct()`. `checker_get_type` returns TYPE_DISTINCT for distinct typedef, so the check failed and tag update was skipped.
- **Fix:** Added `type_unwrap_distinct()` before TYPE_UNION check in NODE_ASSIGN variant assignment path.
- **Test:** `distinct_union_assign.zer` — verifies tag update for both variants, verified by emitting C and checking `_tag = 0` / `_tag = 1` presence.

### NEW FEATURE: Backward goto UAF detection in zercheck
- **What:** `goto retry;` where `retry:` is before a `pool.free(h)` — zercheck now detects use-after-free across backward jumps. Previously documented as a known limitation ("zercheck is linear, not CFG-based").
- **Mechanism:** In NODE_BLOCK, scan for labels and track statement indices. When NODE_GOTO targets a label at an earlier index (backward jump), re-walk statements from label to goto with current PathState (same 2-pass + widen-to-MAYBE_FREED pattern as for/while loops). ~30 lines.
- **Tests:** `goto_backward_uaf.zer` (negative — UAF after backward goto caught), `goto_backward_safe.zer` (positive — safe use across backward goto).

### BUG-439: emit_auto_guards not called for if conditions
- **Symptom:** `if (arr[i] > 0)` — auto-guard not emitted before the condition. Inline `_zer_bounds_check` trap still caught OOB, but graceful auto-guard return was skipped.
- **Root cause:** `emit_auto_guards` only called for NODE_EXPR_STMT, NODE_VAR_DECL, NODE_RETURN — not for NODE_IF condition.
- **Fix:** Added `emit_auto_guards(e, node->if_stmt.cond)` before both regular-if and if-unwrap condition emission.
- **Note:** NOT added for while/for conditions — those are re-evaluated each iteration, auto-guard before the loop would only check the initial value. Inline `_zer_bounds_check` is the correct behavior for loop conditions (trap on OOB rather than silent return, since OOB condition data would cause wrong-branch execution).

### Enhanced range propagation: inline call + constant return + chained call ranges
- **What:** Three improvements to value range propagation that eliminate more auto-guards/bounds checks at compile time:
  1. **Inline call range:** `arr[func()]` — if `func` has proven return range and fits array size, index proven safe. Zero overhead.
  2. **Constant return range:** `return 0;` in `find_return_range` — handles constant expressions via `eval_const_expr_scoped`. Multi-path functions like `if (x==0) { return 0; } return x % 8;` → union `[0, 7]`.
  3. **Chained call range:** `return other_func();` in `find_return_range` — inherits callee's return range through call chain.
  4. **NODE_SWITCH/FOR/WHILE/CRITICAL:** `find_return_range` recurses into switch arms, loop bodies, @critical blocks.
- **Effect:** Hash map patterns `table[hash(key)]` now zero overhead — no bounds check, no auto-guard, proven at compile time through call chain.
- **Tests:** `inline_call_range.zer` (basic), `inline_range_deep.zer` (3-layer deep, 7 accesses, all proven).

### Guard-clamped return range: if (idx >= N) { return C; } return idx;
- **What:** `find_return_range` now handles `return ident` when the ident has a known VarRange from a preceding guard. Pattern: `if (idx >= 8) { return 0; } return idx;` — the guard narrows `idx` to `[0, 7]`, this range is used for the return expression.
- **Mechanism:** After derive_expr_range, constant, and chained-call checks all fail, try `find_var_range()` on the return ident. If a range is found (set by the guard narrowing in check_stmt), use it.
- **Limitation:** VarRange is only available because `find_return_range` runs immediately after `check_stmt(body)` — the ranges haven't been cleaned up yet. This is intentional coupling.
- **Note:** Auto-guard for NODE_CALL indices was attempted but reverted — it double-evaluates side-effect expressions (function called twice: once in guard, once in access). Inline `_zer_bounds_check` with statement expression is the correct mechanism for call indices (single-eval).
- **Tests:** `guard_clamp_range.zer` — clamp/safe_get patterns, both variable and inline call, all proven zero overhead.

### BUG-440: non-keep pointer parameter stored in global — uncaught
- **Symptom:** `void store(*u32 p) { g_ptr = p; }` compiled without error. Caller could pass `&local`, function stores it in global → dangling pointer after function returns.
- **Root cause:** `keep` enforcement was only on the CALLER side (caller can't pass locals to `keep`). The FUNCTION side — storing a non-keep param to global — was never checked. Spec clearly says non-keep `*T` is "non-storable: use it, read it, write through it."
- **Fix:** In NODE_ASSIGN, when target is global/static and value is a non-keep pointer ident that is local-scoped (not global, not static, not local-derived, not arena-derived), error: "add 'keep' qualifier to parameter."
- **Tests:** `nonkeep_store_global.zer` (negative), `keep_store_global.zer` (positive with keep).
- **BUG-440 fix correction:** Initial heuristic falsely flagged local variables as "non-keep parameters" (used is_local_derived/is_arena_derived check). Fixed: use `func_node == NULL` — parameters never get func_node set, local var-decls always do.

### BUG-441: keep validation `arg_node` vs `karg` variable mismatch — compiler crash
- **Symptom:** `store(@ptrcast(*opaque, &global_val))` where `store` has `keep *opaque` param → compiler segfault. ASAN: `checker.c:3148 in check_expr`.
- **Root cause:** In keep parameter validation (NODE_CALL), the orelse-unwrap loop produces `karg` (walked through intrinsics to find `&local` patterns). But line 3147 used `arg_node` (original unwrapped argument) instead of `karg`. When the original arg is `@ptrcast(...)` (NODE_INTRINSIC), `arg_node->unary.operand` dereferences garbage — segfault.
- **Fix:** Changed `arg_node` to `karg` at line 3147-3148.
- **Lesson:** When loop variables shadow outer variables (`karg` vs `arg_node`), always use the loop variable inside the loop body. ASan pinpointed the exact line in one command.
- **Tests:** `driver_registry.zer` — PCI-style driver registration with funcptr + keep *opaque context, 2 drivers, probe + read dispatch.

### BUG-442: defer fires before return expression evaluation — UAF
- **Symptom:** `defer pool.free(h); ... return pool.get(h).val;` → handle freed BEFORE return value computed. Runtime gen check traps with "use-after-free."
- **Root cause:** Emitter's NODE_RETURN handler calls `emit_defers(e)` BEFORE `emit_expr(node->ret.expr)`. Emitted C: `free(h); return get(h).val;` — wrong order.
- **Fix:** When return has an expression AND pending defers, hoist expression into typed temp: `RetType _ret = expr; defers; return _ret;`. Handles ?T wrapping (checks if return type is optional and expression type differs). Skips for trivial returns (null/int/bool/float literals — no side effects).
- **Impact:** Every `return expr` with `defer` was broken if `expr` accessed deferred resources. This is a very common pattern: `defer free(h); return h.field;`.
- **Tests:** `defer_return_order.zer` — slab handle with defer free, return field access, both u32 and ?u32 return types.

### BUG-443: block defer with multiple frees only tracked FIRST free
- **Symptom:** `defer { pool_a.free(ha); pool_b.free(hb); }` — only `ha` marked as freed, `hb` showed false "handle leaked" warning.
- **Root cause:** `defer_scans_free` returned on FIRST match (`if (klen > 0) return klen;` in block scan loop). Second and subsequent frees in the same block were never visited.
- **Fix:** Replaced `defer_scans_free` (returns one key) with `defer_scan_all_frees` (walks ALL statements, marks each found handle as FREED directly). Split into `defer_stmt_is_free` (single statement check) + `defer_scan_all_frees` (recursive block walker).
- **Tests:** `defer_multi_free.zer` (3 handles in one block defer, all tracked). `defer_user.zer` (cross-module, 3 assets, block defer, return accessing deferred data).

### REDESIGN: Handle leaks upgraded from WARNING to compile ERROR (2026-04-06)
- **What:** Handle leaks are now compile errors (MISRA C:2012 Rule 22.1). Every Pool/Slab/Task allocation MUST be freed via explicit `pool.free(h)`, `defer pool.free(h)`, or returned to the caller. Unfixed leaks → compile fails.
- **Key design:** `alloc_id` field on HandleInfo. Each allocation gets a unique ID. Aliases (orelse unwrap, struct copy, assignment) share the same ID. At leak check, if ANY handle in the group is FREED or escaped → allocation covered → no error. This naturally handles `?Handle mh` / `Handle h` pairs.
- **Previous approach failed:** Name-based tracking treated `mh` and `h` as independent variables. Required `is_optional` hack, escape arrays, consumed scans — each patch created new false positives. The alloc_id redesign eliminated ALL false positives.
- **Escape detection:** `escaped` flag on HandleInfo, set when: returned, stored in global, stored in pointer param field, assigned to untrackable target (variable-index array).
- **Recursive defer scan:** Finds defers inside loops, if-bodies, blocks — not just top-level. `defer_scan_all_frees()` walks ALL statements (BUG-443 fix for first-match-only).
- **if-unwrap propagation:** `if (mh) |t| { free(t); }` — freed alloc_id from then_state propagated to mark `mh` as covered in main state.
- **Tests:** `super_defer_complex.zer` (10 patterns), `defer_deep.zer` + `defer_deep_user.zer` (cross-module 3-layer).

### BUG-444: Interior pointer UAF not caught — `&b.field` after `free_ptr(b)`
- **Symptom:** `*u32 p = &b.c; heap.free_ptr(b); u32 val = p[0];` — compiles without error. `p` is a dangling interior pointer to freed memory.
- **Root cause (1):** zercheck's `zc_check_var_init` and NODE_ASSIGN alias tracking didn't recognize `NODE_UNARY(TOK_AMP)` on a field expression as deriving from a tracked allocation. `p` was untracked, so freeing `b` didn't affect `p`.
- **Root cause (2):** zercheck's `zc_check_expr` had no `case NODE_INDEX:`. Pointer indexing `p[0]` fell through without checking if `p` was freed. NODE_FIELD (`b.x`) and NODE_UNARY/TOK_STAR (`*p`) had UAF checks, but NODE_INDEX was missing.
- **Fix (1):** Added interior pointer alias tracking: when init/value is `&expr`, walk through field/index/deref chains to root ident, look up in handle table, copy alloc_id to new variable. Same alloc_id mechanism as handle aliasing.
- **Fix (2):** Added `case NODE_INDEX:` to `zc_check_expr` — checks if indexed object is a freed handle, same pattern as NODE_FIELD UAF check. Also recurses into object and index sub-expressions.
- **Tests:** `interior_ptr_safe.zer` (field ptr used before free — compiles), `interior_ptr_uaf.zer` (field ptr after free — rejected), `interior_ptr_func.zer` (field ptr passed to function after free — rejected).
- **Remaining gap:** `@ptrtoint` + math + `@inttoptr` creates pointer with no allocation link. Guarded by `mmio` declaration requirement (not pointer tracking).

### BUG-445: C-style cast in comptime function body fails
- **Symptom:** `comptime u32 TO_U32(u8 x) { return (u32)x; }` — "body could not be evaluated at compile time."
- **Root cause:** `eval_const_expr` doesn't handle NODE_TYPECAST. Comptime body evaluator can't fold casts.
- **Status:** Known, deferred. Workaround: use implicit widening (`return x;`) or `@truncate`.

### BUG-446: C-style cast skips provenance check through *opaque
- **Symptom:** `*opaque raw = (*opaque)a; *B wrong = (*B)raw;` compiles — wrong type through *opaque not caught.
- **Root cause:** NODE_TYPECAST handler only validated cast direction (ptr↔ptr OK) but didn't check provenance. Also, var-decl provenance propagation only walked NODE_INTRINSIC, not NODE_TYPECAST.
- **Fix:** (1) Added provenance check to NODE_TYPECAST — same logic as @ptrcast. (2) Added NODE_TYPECAST to var-decl provenance walker (line ~5236) so `*opaque raw = (*opaque)a` propagates `a`'s provenance to `raw`.
- **Tests:** `typecast_provenance.zer` (wrong type through *opaque — rejected), `typecast_safe_complex.zer` (same-type round-trip — works).

### BUG-447: C-style cast strips volatile qualifier silently
- **Symptom:** `volatile *u32 reg = ...; *u32 bad = (*u32)reg;` compiles — volatile stripped.
- **Root cause:** NODE_TYPECAST handler didn't check volatile qualifier on pointer casts.
- **Fix:** Added volatile check — same logic as @ptrcast BUG-258. Checks both type-level and symbol-level volatile.
- **Tests:** `typecast_volatile_strip.zer` (volatile strip — rejected).

### BUG-448: C-style cast strips const qualifier silently
- **Symptom:** `const *u32 cp = &x; *u32 bad = (*u32)cp;` compiles — const stripped.
- **Root cause:** NODE_TYPECAST handler didn't check const qualifier on pointer casts.
- **Fix:** Added const check — same logic as @ptrcast BUG-304.
- **Tests:** `typecast_const_strip.zer` (const strip — rejected).

### BUG-449: C-style cast allows *A to *B directly (no *opaque)
- **Symptom:** `*A pa = &a; *B pb = (*B)pa;` compiles — type-punning without *opaque round-trip.
- **Root cause:** NODE_TYPECAST handler only checked valid=true for ptr↔ptr, without verifying inner types match.
- **Fix:** Added check: when both source and target are pointers (neither *opaque), inner types must match. Error message: "use *opaque round-trip for type-punning."
- **Tests:** `typecast_direct_ptr.zer` (*A to *B — rejected).

### BUG-450: C-style cast allows integer → pointer (bypasses @inttoptr)
- **Symptom:** `*u32 p = (*u32)addr;` compiles — bypasses mmio range validation that @inttoptr enforces.
- **Root cause:** NODE_TYPECAST handler had `valid = true` for int→ptr casts.
- **Fix:** Reject int→ptr casts with error message directing to @inttoptr.
- **Tests:** `typecast_int_to_ptr.zer` (int→ptr — rejected).

### BUG-451: C-style cast allows pointer → integer (bypasses @ptrtoint)
- **Symptom:** `u32 addr = (u32)p;` compiles — bypasses portability warnings that @ptrtoint provides.
- **Root cause:** NODE_TYPECAST handler had `valid = true` for ptr→int casts.
- **Fix:** Reject ptr→int casts with error message directing to @ptrtoint.
- **Tests:** `typecast_ptr_to_int.zer` (ptr→int — rejected).

### BUG-452: scan_frame missing NODE_BINARY — recursion not detected in expressions
- **Symptom:** `return fibonacci(n-1) + fibonacci(n-2);` — no recursion warning. `fibonacci` not found as callee.
- **Root cause:** `scan_frame` (Pass 5 stack depth) only handled NODE_CALL, NODE_BLOCK, NODE_IF, etc. Missed NODE_BINARY, NODE_UNARY, NODE_ORELSE — calls inside expressions invisible.
- **Fix:** Added NODE_BINARY (recurse left+right), NODE_UNARY (recurse operand), NODE_ORELSE (recurse expr) to scan_frame.
- **Tests:** Verified with `fibonacci` and mutual recursion (`is_even`/`is_odd`).

### BUG-453: checker_post_passes not called from zerc_main.c
- **Symptom:** Recursion warning, interrupt safety, and whole-program provenance never ran in actual compiler — only in unit tests via `checker_check`.
- **Root cause:** `zerc_main.c` calls `checker_check_bodies` (Pass 2 only), not `checker_check` (all passes). Pass 3/4/5 never executed in the real pipeline.
- **Fix:** Added `checker_post_passes()` function (runs Pass 3+4+5), called from `zerc_main.c` after `checker_check_bodies`.
- **Impact:** Same class as zercheck integration bug (2026-04-03). Post-passes existed and passed unit tests but were never called from the actual compiler.

### BUG-454: C-style cast `(*opaque)&a` doesn't propagate provenance
- **Symptom:** `*opaque raw = (*opaque)&a; *B wrong = (*B)raw;` compiles — wrong type not caught through C-style cast with address-of expression.
- **Root cause (1):** NODE_TYPECAST provenance-setting code only handled NODE_IDENT expressions, not NODE_UNARY(TOK_AMP). `(*opaque)&a` → `&a` is NODE_UNARY, not NODE_IDENT.
- **Root cause (2):** Var-decl provenance propagation recognized @ptrcast (NODE_INTRINSIC) but not NODE_TYPECAST for source type extraction. `*opaque raw = (*opaque)&a` didn't copy source type to `raw`'s provenance.
- **Fix (1):** Walk through `&`, field, index chains to find root ident in NODE_TYPECAST provenance setter.
- **Fix (2):** Added NODE_TYPECAST case to var-decl provenance handler — extracts source type from `init->typecast.expr` via typemap, same as @ptrcast does with `init->intrinsic.args[0]`.
- **Tests:** Existing `typecast_provenance.zer` covers named-ptr path. `(*opaque)&a` path verified manually.

### BUG-455: Global arena pointer stored in global variable not caught
- **Symptom:** `Arena scratch; ?*Cfg g = null; ... g = scratch.alloc(Cfg) orelse return;` compiles — arena pointer stored in global, dangles after `scratch.reset()`.
- **Root cause:** `is_arena_derived` flag only set for LOCAL arena allocs (had `!arena_is_global` guard). Global arena allocs were considered "safe" — wrong, because global arenas can still be reset().
- **Fix:** Added `is_from_arena` flag on Symbol (set for ALL arena allocs, global or local). Assignment-to-global check uses `is_from_arena`. Return/keep/call checks still use `is_arena_derived` (only local arenas — global arena pointers CAN be returned from functions safely).
- **Tests:** `arena_global_escape.zer` (global arena ptr to global — rejected).

### Enhancement: func_returns_arena — arena wrapper functions excluded from handle tracking
- **Problem:** Wrapper functions returning `?*T` from arena triggered false "handle never freed" errors. Chained wrappers (app → driver → hal → arena.alloc) and freelist type-punning through `*opaque` adapter functions also affected.
- **Fix:** Three-layer allocation coloring system:
  1. `source_color` on HandleInfo — set at alloc, propagated through all aliases
  2. `func_returns_color_by_name()` — recursive transitive color resolution through call chains, cached on Symbol
  3. Param color inference (`returns_param_color`) — when function returns cast of param, caller's arg color flows to result. Covers `*opaque → *T` adapter functions.
- **Coverage:** Direct arena wrapper (100%), chained wrappers up to 8 levels (100%), freelist type-punning through `*opaque` adapters (100%), pool/malloc leaks still caught.
- **Alias walker updated:** NODE_TYPECAST added to alias source walker for alloc_id + source_color propagation through C-style casts.

### BUG-456: *opaque adapter function return treated as new allocation (found by semantic fuzzer)
- **Symptom:** `*Src back = unwrap(raw);` where `unwrap(*opaque r) { return (*Src)r; }` — zercheck reports "back never freed" even though `back` is the same memory as the already-deferred original pointer.
- **Root cause:** Param color inference copied `source_color` but not `alloc_id`. `back` was tracked as a separate allocation from `s`, so defer on `s` didn't cover `back`.
- **Fix:** Param color inference now creates full ALIAS — copies alloc_id, state, pool_id, free_line, source_color from arg's HandleInfo. Result shares allocation identity with the arg.
- **Found by:** Semantic fuzzer (`test_semantic_fuzz.c`) — `safe_opaque_*` pattern with Slab + adapter function + defer.
- **Tests:** 1000 semantic fuzz tests across 5 seeds — zero failures after fix.

### BUG-457: scan_frame NODE_SPAWN fallthrough crash (segfault)
- **Symptom:** Compiler segfaults on any file with `continue;` or `break;` when NODE_SPAWN exists in enum. Exit code 139.
- **Root cause:** NODE_SPAWN case placed in exhaustive switch after NODE_CONTINUE without a `break;`. NODE_CONTINUE fell through to NODE_SPAWN handler, which accessed `node->spawn_stmt.arg_count` on a NODE_CONTINUE node — wrong union member → NULL dereference.
- **Fix:** Added `break;` between NODE_SIZEOF leaf group and NODE_SPAWN active case. NODE_SPAWN now has its own isolated case with proper break.
- **Lesson:** When adding active cases (with logic) to exhaustive switches, NEVER place them adjacent to leaf case groups without explicit breaks. The exhaustive switch pattern prevents MISSING cases but doesn't prevent FALLTHROUGH within existing cases.
- **Found by:** `comptime_const_arg.zer` and `comptime_if_call.zer` crashing with exit 139. ASan pinpointed exact line.

### BUG-458: `shared` keyword conflicts with variable/method name `shared`
- **Symptom:** `u32 shared;` in atomic_ops.zer → parse error. `ecs_world.spawn()` method → parse error.
- **Root cause:** `shared` and `spawn` added as reserved keywords in lexer. Any use as variable/method name broke.
- **Fix:** `spawn` made contextual (detected by ident match in parser, not lexer keyword). `shared` kept as keyword but `.shared` accepted as field name in field access parser. Renamed `shared` variable in atomic_ops.zer to `shared_val`.
- **Lesson:** New keywords can break existing code. Prefer contextual keywords when the syntax is unambiguous at the call site.

### BUG-459: shared struct pointer uses -> not . for lock access
- **Symptom:** `void worker(*State s) { s.count += 1; }` where State is shared → GCC error "c is a pointer; did you mean to use ->?"
- **Root cause:** `emit_shared_lock/unlock` emitted `c._zer_lock` for pointer parameters. Should be `c->_zer_lock`.
- **Fix:** Check `checker_get_type(root)` — if TYPE_POINTER, use `->`, else use `.`. Same fix for both lock and unlock.
- **Found by:** Rust concurrency test `conc_shared_counter.zer` — first test to pass *shared struct through a function parameter.

### BUG-460: spawn emitter uses UB function pointer cast for multi-arg functions
- **Symptom:** `spawn worker(42, &shared_state)` — the function expects `(u32, *State)` but `pthread_create` casts it to `void*(*)(void*)`. UB for multi-arg functions on some platforms.
- **Root cause:** Old emitter used `(void*(*)(void*))func_name` — casts the function directly. Works on x86 by accident but UB per C standard.
- **Fix:** Proper file-scope wrapper functions. Pre-scan phase assigns unique IDs to all NODE_SPAWN nodes. Wrapper emitted at file scope: `static void *_zer_spawn_wrap_N(void *_raw) { struct args *a = _raw; func(a->a0, a->a1); free(a); return NULL; }`. Forward declarations emitted for target functions.
- **Lesson:** Never cast function pointers to incompatible types. Always use a wrapper with the correct signature.

### BUG-461: const global with shift operator fails (GCC statement expression in global scope)
- **Symptom:** `const u32 X = 1 << 2;` at global scope → GCC error "braced-group within expression allowed only inside a function."
- **Root cause:** `_zer_shl` safety macro uses GCC statement expression `({...})` which is invalid in global initializer context. The emitter used `emit_expr()` for const global inits, which always emits the safety macro.
- **Fix:** In `emit_global_var`, if `node->var_decl.is_const`, try `eval_const_expr()` first. If evaluation succeeds (both operands are compile-time constants), emit the pre-computed numeric result directly instead of the expression. Falls back to `emit_expr()` for non-const expressions.
- **Found by:** `rt_const_binops.zer` translated from Rust's `tests/ui/consts/const-binops.rs`.
- **Lesson:** Const global initializers must not use GCC extensions (statement expressions, typeof in expression position). Pre-evaluate when possible — it's both safer (compile-time verified) and more portable.

### Designated Initializers + Container Keyword (2026-04-11, new features)

**Designated initializers:**
- `Point p = { .x = 10, .y = 20 };` — NODE_STRUCT_INIT parsed in `parse_primary` when `{` followed by `.`.
- Checker validates field names against target struct, emitter produces C99 compound literal `(Type){ .field = val }`.
- Works in both var-decl init and assignment contexts.

**Container keyword (monomorphization):**
- `container Stack(T) { T[64] data; u32 top; }` defines parameterized struct template.
- `Stack(u32)` stamps concrete `struct Stack_u32` with T→u32.
- No methods, no `this` — functions take `*Container(T)` explicitly.
- Template stored on Checker, stamped on TYNODE_CONTAINER resolution, instances cached per (name, concrete_type).
- T substitution handles: direct T, *T, ?T, []T, T[N] field types.

**@container intrinsic conflict resolved:**
- `TOK_CONTAINER` keyword conflicted with `@container` intrinsic (container_of). Fix: parser's `@` handler accepts both `TOK_IDENT` and `TOK_CONTAINER` as intrinsic name.

**Statement lookahead for container types:**
- `Stack(u32) s;` — parser statement heuristic for `IDENT(` case extended: after consuming `(`, checks if type token follows, speculatively parses `Type)` and checks for trailing IDENT. This detects container var-decls vs function calls.

**Stale forward declaration removed:**
- `eval_comptime_stmt` forward declaration was leftover from pre-ComptimeCtx refactor. Removed to eliminate -Wunused-function warning.

### do-while, comptime array indexing, comptime struct return (2026-04-11)

**do-while:** `do { body } while (cond);` — NODE_DO_WHILE reuses while_stmt data. All walkers updated. Merged with NODE_WHILE case in checker/emitter/zercheck.

**Comptime array indexing:** ComptimeParam extended with array_values/array_size. ct_ctx_set_array creates binding, ct_eval_assign handles arr[i]=val, eval_const_expr_subst handles arr[i] read. Memory managed: arrays freed on scope pop and ctx_free. CRITICAL: all ComptimeParam stack arrays must be memset-zeroed to prevent freeing garbage pointers (caused munmap_chunk crash before fix).

**Comptime struct return:** comptime functions can return `{ .x = a, .y = b }`. When eval_comptime_block returns CONST_EVAL_FAIL, checker tries find_comptime_struct_return + eval_comptime_struct_return as fallback. Creates constant NODE_STRUCT_INIT stored as call.comptime_struct_init. Emitter delegates to emit_expr (existing compound literal path). Also required adding NODE_STRUCT_INIT validation in NODE_RETURN (4th value-flow site — was missing, caused "return type 'void' doesn't match" error).

### Comptime enum values + comptime float (2026-04-11)

**Comptime enum values:** `Color.red` resolves to integer at compile time. `resolve_enum_field` helper + `eval_const_expr_scoped` extended to recurse through binary expressions containing enum fields. Works in `static_assert`, array indices, comptime function args.

**Comptime float:** `comptime f64 PI_HALF() { return 3.14 / 2.0; }` — parallel eval path at call site. `eval_comptime_float_expr` handles float literal/param/arithmetic. Float params stored as bits in int64 via memcpy. Emitter outputs `%.17g`.

**Design decision: no array literal syntax.** `u32[4] t = {1,2,3,4}` not added because: (1) C doesn't have array literals in expression position either, (2) element-by-element is clearer for large arrays, (3) would create NODE_ARRAY_INIT with parser ambiguity vs NODE_STRUCT_INIT, (4) ~100 lines for convenience-only feature.

### Spawn global data race detection + --stack-limit (2026-04-11)

**Spawn global data race:** `scan_unsafe_global_access` scans spawned function body (+ transitive callees, 8 levels) for non-shared global access. No sync → error. Has @atomic/@barrier → warning. Escape: volatile, shared, threadlocal, @atomic_*, const. `has_atomic_or_barrier` needed full expression tree recursion (was missing binary/unary/call/assign initially — caused false error on `@atomic_load` in while condition).

**--stack-limit N:** Per-function frame size + entry-point call chain depth checked against limit. Two separate checks catch different failure modes (big local array vs deep call chain).

**Audit fixes:** `find_comptime_struct_return` was duplicate of `find_comptime_return_expr` — removed (-19 lines). `#include <math.h>` moved from mid-file to top. Missing `-Wswitch` cases for TYNODE_CONTAINER, NODE_CONTAINER_DECL, NODE_DO_WHILE in ast_print fixed.

### @ptrtoint(&local) escape + funcptr indirect recursion (2026-04-12)

**@ptrtoint escape:** Two checks needed — direct `return @ptrtoint(&x)` (no intermediate var, caught in NODE_RETURN) and indirect `usize a = @ptrtoint(&x); return a` (is_local_derived propagation at var-decl, caught by existing return escape check). Initial attempt removed direct check thinking indirect covered both — it doesn't (no var-decl = no symbol to flag). Both checks are complementary, not redundant.

**Funcptr call graph:** scan_frame NODE_CALL now checks TYPE_FUNC_PTR variables. If init was a function ident, adds that function as callee. Enables indirect recursion detection through function pointers.

### Local funcptr init required + division by zero call divisors (2026-04-12)

**Local funcptr init:** Local `void (*cb)(u32)` without init auto-zeros to NULL — calling segfaults. Now requires init or `?` nullable prefix. Global funcptrs exempt (C convention: assigned in init functions). Matches existing *T pointer init requirement.

**Division by zero call divisors:** `x / func()` where func() has no proven nonzero return range → compile error. Extended forced division guard from NODE_IDENT/NODE_FIELD to also cover NODE_CALL. Functions with `has_return_range && return_range_min > 0` pass.

**Stack overflow indirect calls:** `has_indirect_call` flag on StackFrame propagated through call chain. Without --stack-limit: warning. With --stack-limit: error on entry points with unresolvable funcptr calls.

### Red Team audit fixes: transitive deadlock, comptime budget, naked validation (2026-04-12)

**V1 — Transitive deadlock (Gemini red team):** `a.x = sneaky_helper()` where `sneaky_helper()` accesses different shared struct `b.y`. The `collect_shared_types_in_expr` was statement-local — didn't scan callee function bodies. Fix: transitive scanning of called function bodies (depth 4) for shared type accesses. Also added NODE_RETURN/NODE_EXPR_STMT/NODE_VAR_DECL handling in the expression scanner (were missing, caused callee body statements to be skipped).

**V3 — Comptime nested loop DoS:** Nested `for (10000) { for (10000) }` = 100M iterations. Per-loop limit was 10000 but didn't account for nesting. Fix: global `_comptime_ops` counter incremented per loop iteration, cap at 1M total operations. Resets on top-level eval_comptime_block call.

**V4 — Naked function with non-asm:** `naked void f() { u32[16] buf; }` compiled — GCC skips prologue but emitted code uses stack. Fix: checker scans naked function body, errors on any non-NODE_ASM, non-NODE_RETURN statement.

**V2 — Union mutation via *opaque (NOT a bug):** Already caught — "cannot take address of union inside its switch arm — pointer alias would bypass variant lock." Gemini's test was invalid.

### Red Team V5-V6 (2026-04-12, Gemini round 2)

**V5 — Thread-unsafe Slab/Pool/Ring from spawn:** `scan_unsafe_global_access` skipped TYPE_POOL/TYPE_SLAB/TYPE_RING. These allocators have non-atomic metadata — alloc/free from multiple threads is a data race. Fix: only skip TYPE_ARENA and TYPE_BARRIER. Also fixed two scanner gaps: NODE_FIELD not in recursion switch (callee `global_slab.alloc()` has slab as NODE_FIELD object), and NODE_CALL not scanning callee expression. 6 tests that used Ring/Pool from spawn correctly reclassified as negative tests.

**V6 — Container infinite recursion:** `container Node(T) { ?*Node(T) next; }` caused infinite type resolution. Fix: `_container_depth` limit (32) + `subst_typenode()` recursive TypeNode substitution replacing 5 one-level pattern matches with single recursive function. Handles T at any nesting depth.

### Red Team V9-V12 (2026-04-12, Gemini round 3)

**V9 — Async defer bypass:** NOT a bug. Defer fires correctly on async completion — Duff's device state machine handles defers properly. Verified with test (EXIT 0, g_cleaned == 1).

**V10 — Move struct in shared struct:** CONFIRMED + FIXED. `move struct Token` as field of `shared struct Vault` allows ownership breach across threads. Fix: checker rejects move struct fields in shared struct declarations at register_decl time.

**V11 — Same-type instance deadlock:** NOT a real deadlock. ZER's per-statement locking means locks never overlap — each statement acquires/releases before the next. Atomicity concern (partial transfer visible) is a design limitation, same as Rust (use single shared struct for atomic multi-field ops).

**V12 — Container type-id collision:** Already caught. Each container stamp gets unique type_id (c->next_type_id++). @ptrcast provenance check catches Wrapper(u32) → Wrapper(i32) mismatch.

### Red Team V13-V16 (2026-04-12, Gemini round 4)

**V13 — Move struct value capture:** CONFIRMED + FIXED. `if (opt) |k|` copies move struct, creating two owners. Fix: checker errors on value capture of move struct — must use `|*k|` pointer capture.

**V14 — Async shared struct lock-hold:** CONFIRMED + FIXED. Shared struct access in async function → lock held across yield/await = deadlock. Fix: `c->in_async` flag, checker errors on shared struct field access in async body. Same approach as Rust (MutexGuard not Send across await).

**V15 — Comptime @ptrtoint:** NOT a bug. `@ptrtoint` in comptime returns CONST_EVAL_FAIL — comptime evaluator can't resolve pointer addresses.

**V16 — Move struct partial field access:** NOT a bug. zercheck marks entire struct as HS_TRANSFERRED, any field access errors.

### Red Team V17-V20 (2026-04-12, Gemini round 5)

**V17 — Async return + defer:** NOT a bug. Emitter handles async return correctly — defers fire on completion.

**V18 — Shared pointer in async:** CONFIRMED + FIXED. `*Bus b` parameter in async function bypassed V14 check because NODE_FIELD checked TYPE_POINTER (the object type), not the pointed-to shared struct. Fix: unwrap pointer before checking is_shared. Also revealed emitter bug: async transformer doesn't carry function parameters into poll function (GCC error 'b' undeclared) — the safety check prevents reaching that code generation bug.

**V19 — Spawn move bypass:** NOT a bug. zercheck already tracks move struct args to spawn as HS_TRANSFERRED.

**V20 — Container pointer-to-array decay:** NOT a bug. Container *T substitution produces correct *concrete type.

### Red Team V21-V24 (2026-04-12, Gemini round 6)

**V21 — Async cancellation leak:** Design limitation (same as Rust Future drop). Dropping async state struct without completing leaks resources. Not fixable at compile time — would need cancel protocol. Documented.

**V22 — Move-union bypass:** NOT exploitable. Union variant read requires switch — direct `w1.k.id` blocked by "cannot read union variant directly." `contains_move_struct_field` extended to check TYPE_UNION variants as defense-in-depth.

**V23 — Spawn non-void return:** CONFIRMED + FIXED. `spawn produce()` where produce returns non-void → return value lost. Fix: checker errors if spawn target has non-void return type.

**V24 — Comptime const bypass:** NOT applicable. Comptime evaluator is pure computation — no const concept. Re-assignment is valid for building values.
