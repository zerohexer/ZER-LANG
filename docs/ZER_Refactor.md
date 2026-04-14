# ZER Compiler Refactor Plan — Complete Context Dump

**Generated:** 2026-04-14 (updated after execution)
**Session:** Full codebase audit — 25,757 compiler lines, ~15,000 lines read directly, 100% pattern-searched
**Status:** Phases 1-5 COMPLETE. Only emitter duplication cleanup remaining (B3, B5-B8, B11).

## COMPLETED (this session)
- B1: `track_dyn_freed_index()` — checker.c
- B2: `check_union_switch_mutation()` — checker.c
- B4: `emit_opt_wrap_value()` — emitter.c
- B10: `handle_key_arena()` — zercheck.c (27 sites converted)
- A1: 5 snprintf clamp fixes — checker.c
- A2-A6: 6 emitter distinct optional unwrap — emitter.c (BUG-506)
- A7: spawn string literal check — checker.c
- A8-A14: 7 checker distinct unwrap — checker.c (BUG-506)
- A15: spawn validation gaps (is_literal_compatible + validate_struct_init) — checker.c
- A16: labels[128] → dynamic — checker.c
- A17: container fields[128] → dynamic — parser.c
- A18: volatile bounds temps — emitter.c
- A19: emit_type_and_name distinct-optional-funcptr — emitter.c
- A20: module-qualified call distinct unwrap — checker.c
- C1-C2: zig test runner + Makefile

## COMPLETED (second batch)
- B3: Orelse emission → use emit_opt_null_check/emit_opt_unwrap helpers (net -64 lines)
- B7: Return optional wrapping → use emit_opt_wrap_value helper
- B8: Union typedef emission → EMIT_UNAME() local macro (12× → 12 calls)

## DEFERRED TO v0.4 (intentional differences, not pure duplication)
- B5-B6: Pool/Slab/Task alloc emission — middle logic differs (pool inline vs slab function). Wrapping is identical but extracting needs `is_pool` flag = no simplification. Better addressed by v0.4 table-driven architecture.
- B11: Pool vs Slab method dispatch in checker — same structure but different types/ISR rules. Better addressed by v0.4 table-driven architecture.

**MANDATORY:** Read CLAUDE.md and docs/compiler-internals.md FIRST. This document supplements those — it does NOT replace them. CLAUDE.md has the language spec, compiler-internals.md has the emission patterns.

---

## How To Use This Document

1. Read CLAUDE.md (language spec + rules + workflow)
2. Read docs/compiler-internals.md (emission patterns + bug history)
3. Read THIS document (refactor plan)
4. For each fix: read ~20 lines around the cited line number to verify context
5. Apply fix, run `make docker-check`, commit

**DO NOT start implementing without reading the 3 documents above.** The no-debt rule applies: understand the code flow before changing it.

---

## Codebase Architecture Summary

```
source.zer → Lexer (lexer.c, 655 lines)
           → Parser (parser.c, 2625 lines)
           → AST (ast.h, 679 lines)
           → Checker (checker.c, 10654 lines)  ← BIGGEST file, most fixes here
           → ZER-CHECK (zercheck.c, 2763 lines) ← path-sensitive handle tracking
           → Emitter (emitter.c, 6005 lines)   ← C code generation
           → output.c → GCC

Driver: zerc_main.c (634 lines)
Headers: lexer.h, ast.h, types.h, checker.h, emitter.h, zercheck.h, parser.h
```

### Key Type System Facts (needed for EVERY fix)

**TYPE_DISTINCT** wraps any type via `distinct typedef u32 MyId;`. The Type* from `checker_get_type()` or `check_expr()` CAN be TYPE_DISTINCT. Every `->kind == TYPE_X` check on user-facing types MUST call `type_unwrap_distinct()` first. This was the #1 historical bug class (35+ sites fixed previously, 15 more found in this audit).

```c
// WRONG — fails for distinct typedef:
if (type->kind == TYPE_OPTIONAL) { ... }

// CORRECT:
Type *eff = type_unwrap_distinct(type);
if (eff->kind == TYPE_OPTIONAL) { ... }
```

**Helpers that unwrap internally (safe to call directly):**
- `type_is_optional(t)` — checks if optional (unwraps distinct)
- `type_is_integer(t)` — checks if any integer type (unwraps)
- `type_is_float(t)` — checks if f32/f64 (unwraps)
- `type_width(t)` — bit width (unwraps)
- `is_null_sentinel(t)` — checks if TYPE_POINTER or TYPE_FUNC_PTR (unwraps)
- `is_void_opt(t)` — checks if ?void (unwraps) — emitter helper

**Helpers that do NOT unwrap (caller must unwrap):**
- `type_equals(a, b)` — intentionally nominal for distinct
- `can_implicit_coerce(from, to)` — intentionally blocks distinct→non-distinct

### Key Optional Patterns (needed for emitter fixes)

```c
// ?*Task — pointer optional (null sentinel, zero overhead)
// C representation: Task* (NULL = none, non-NULL = some)
// Null check: if (ptr)
// Unwrap: just use ptr directly

// ?u32 — value optional (struct wrapper)
// C representation: typedef struct { uint32_t value; uint8_t has_value; } _zer_opt_u32;
// Null check: .has_value
// Unwrap: .value
// Init null: { 0, 0 }
// Init value: { val, 1 }

// ?void — void optional (NO value field!)
// C representation: typedef struct { uint8_t has_value; } _zer_opt_void;
// ⚠️ Accessing .value on ?void = GCC error (field doesn't exist)
// Init null: { 0 }
// Init value: { 1 }
```

### Checker Structure (check_expr is the main function)

`check_expr()` in checker.c is a massive switch on `node->kind`. It starts around line 1750 and runs through line 5955. Key handlers:
- NODE_BINARY (line ~2050): arithmetic, comparison, logical, bitwise
- NODE_UNARY (line ~2232): minus, bang, tilde, deref, address-of
- NODE_ASSIGN (line ~2350): assignment validation, VRP, escape checking
- NODE_CALL (line ~3343): module-qualified rewrite, builtin methods (Pool/Ring/Slab/Arena/Task), normal call, keep params, comptime, VRP invalidation
- NODE_FIELD (line ~4294): module-qualified var, struct field, Handle auto-deref, enum variant, union variant, pointer auto-deref
- NODE_INDEX (line ~4762): array/slice/pointer indexing, bounds checking, MMIO bounds
- NODE_ORELSE (line ~4996): optional unwrap validation
- NODE_TYPECAST (line ~5047): C-style cast validation with provenance
- NODE_INTRINSIC (line ~5194): all @builtin validation

`check_stmt()` starts at line 6165. Key handlers:
- NODE_VAR_DECL (line ~6187): type resolution, init validation, escape flags, provenance
- NODE_IF (line ~6960): condition check, comptime if, if-unwrap captures
- NODE_SWITCH (line ~7200): exhaustive check, capture, union lock
- NODE_RETURN (line ~7544): scope escape, string return, const/volatile laundering
- NODE_SPAWN (line ~8059): thread safety, qualifier checks, global data race scan

### Emitter Structure (emit_expr and emit_stmt)

`emit_expr()` starts at line 923. Key handlers:
- NODE_NULL_LIT (line 960): emits `0`
- NODE_IDENT (line ~970): module-mangled name resolution
- NODE_BINARY (line ~1025): arithmetic with division guard, shift safety
- NODE_ASSIGN (line ~1181): optional wrapping, array copy, null assignment
- NODE_CALL (line ~1415): comptime inline, builtin methods (Pool/Ring/Slab/Arena/Task), spawn wrapper lookup
- NODE_INDEX (line ~1960): bounds check emission (comma operator vs GCC stmt expr)
- NODE_ORELSE (line ~2280): 3 paths (bare return/break, block, default value)
- NODE_TYPECAST (line ~2400): C cast with *opaque wrap/unwrap
- NODE_INTRINSIC (line ~2484): all @builtin C emission

`emit_stmt()` starts at line 3000. Key handlers:
- NODE_BLOCK (line ~3005): shared struct auto-lock grouping
- NODE_VAR_DECL (line ~3115): async local promotion, optional init, auto-zero
- NODE_IF (line ~3345): if-unwrap capture emission
- NODE_RETURN (line ~3575): defer-before-return hoisting, optional wrap
- NODE_SWITCH (line ~3920): enum/union/optional switch emission

### Zercheck Structure

`zc_check_expr()` starts at line 1000. Checks NODE_IDENT for move struct use-after-transfer, NODE_FIELD/NODE_INDEX for UAF on freed handles, NODE_CALL for freed args.

`zc_check_stmt()` starts at line 1598. Handles:
- NODE_BLOCK: scope_depth push/pop, shadow cleanup
- NODE_IF: 4-way path merge (both-exit, then-exit, else-exit, both-fallthrough)
- NODE_SWITCH: per-arm path merge
- NODE_FOR/WHILE/DO_WHILE: dynamic fixed-point iteration (ceiling 32)
- NODE_RETURN: 9c freed pointer return check
- NODE_VAR_DECL: alloc tracking, alias propagation, move struct registration

Leak detection at function exit: `zc_check_func()` around line 2597. Uses `covered_ids` (dynamic, stack-first [64]).

---

## Confirmed NOT Bugs (DO NOT re-investigate)

These were investigated and confirmed correct during the audit:

1. **`types.c can_implicit_coerce` missing distinct unwrap** — BY DESIGN. Distinct types intentionally block implicit conversion. Only T→?T wrapping (line 367) unwraps, which is correct.
2. **`type_equals` nominal for distinct** — BY DESIGN. Two distinct types are only equal if same definition (pointer identity).
3. **Const/volatile laundering checks without unwrap** (checker.c lines 3179, 3190, 3888, 3896, 6278, 6286, 7577, 7588) — Belt-and-suspenders. `can_implicit_coerce` already catches at type level. Missing unwrap just gives generic error instead of specific message. NOT a safety hole.
4. **`pathstate_equal` asymmetric check** (zercheck.c:62) — Correct for loop convergence. Only checks a→b because new handles from loop body are expected.
5. **`import_asts[64]`** (zerc_main.c:460) — Graceful degradation for >64 modules. Not a crash.
6. **Emitter `asname[128]`** (emitter.c:1746) — Used with `%.*s` which stops at null. Safe (unlike checker's `memcpy` which doesn't stop).
7. **`@offset` struct type check** (checker.c:5254) — `struct_type->kind == TYPE_STRUCT` without unwrap. Cosmetic: wrong error message for distinct struct, not a safety gap.

---

## A. Bug Fixes — Wrong Behavior

### A1: Buffer over-read in checker.c — P0 (UB, 5 sites)

**What:** `snprintf(buf, sizeof(buf), fmt, ...)` returns the would-be length if buffer was infinite. When formatted string exceeds buffer, `snprintf` truncates output but returns the FULL length. Subsequent `memcpy(dst, buf, sn_len + 1)` reads `sn_len + 1` bytes from a buffer that only has `sizeof(buf)` bytes — reading garbage from the stack.

**Why it matters:** Undefined behavior. In practice, reads random stack bytes into the arena-allocated copy. Could corrupt symbol names silently.

**Where:**
| Line | Buffer | Format | Trigger |
|---|---|---|---|
| 746 | `slab_name[128]` | `_zer_auto_slab_%s` | struct name > ~110 chars |
| 1320 | `mangled[256]` | `%s_%s` (container+type) | container+type name > 255 |
| 8521 | `aname[256]` | `_zer_async_%s` | async func name > ~240 |
| 8538 | `iname[256]` | `_zer_async_%s_init` | async func name > ~235 |
| 8554 | `pname[256]` | `_zer_async_%s_poll` | async func name > ~235 |

**Fix for each site (one line after `snprintf`):**
```c
if (sn_len >= (int)sizeof(BUFFER_NAME)) sn_len = (int)sizeof(BUFFER_NAME) - 1;
```

**Surrounding code pattern (line 746 as example):**
```c
char slab_name[128];
int sn_len = snprintf(slab_name, sizeof(slab_name),
    "_zer_auto_slab_%.*s",
    (int)struct_type->struct_type.name_len, struct_type->struct_type.name);
// ADD FIX HERE: if (sn_len >= (int)sizeof(slab_name)) sn_len = (int)sizeof(slab_name) - 1;
char *name_copy = (char *)arena_alloc(c->arena, sn_len + 1);
memcpy(name_copy, slab_name, sn_len + 1);  // NOW SAFE: sn_len <= 127
```

---

### A2: Emitter var-decl distinct optional null init — P1

**What:** `distinct typedef ?u32 MaybeId; MaybeId x = null;` — emitter checks `type->kind == TYPE_OPTIONAL` but type is `TYPE_DISTINCT`. Falls through to general init path which emits `= 0` instead of `= { 0, 0 }`. GCC error: "incompatible types in initialization."

**Where:** emitter.c:3232. Inside `emit_stmt` → `case NODE_VAR_DECL` → local variable init path.

**Surrounding code (lines 3224-3240):**
```c
emit_type_and_name(e, type, node->var_decl.name, node->var_decl.name_len);
if (node->var_decl.init) {
    /* Optional init: null → {0, 0}, value → {val, 1} */
    if (type && type->kind == TYPE_OPTIONAL &&          // ← LINE 3232: BROKEN for distinct
        !is_null_sentinel(type->optional.inner)) {
        if (node->var_decl.init->kind == NODE_NULL_LIT) {
            emit(e, " = ");
            emit_opt_null_literal(e, type);             // emits { 0, 0 } for ?T, { 0 } for ?void
        } else if (node->var_decl.init->kind == NODE_CALL || ...
```

**Fix:**
```c
Type *type_eff = type ? type_unwrap_distinct(type) : NULL;
if (type_eff && type_eff->kind == TYPE_OPTIONAL &&
    !is_null_sentinel(type_eff->optional.inner)) {
```
Inside the block, use `type_eff->optional.inner` for inner type access, but keep `type` for `emit_type(e, type)` calls (preserves distinct name).

**Test:** `tests/zer/distinct_optional_null_init.zer`:
```zer
distinct typedef ?u32 MaybeId;
u32 main() {
    MaybeId x = null;
    u32 v = x orelse 42;
    if (v != 42) { return 1; }
    return 0;
}
```

---

### A3: Emitter init_type distinct optional check — P1

**What:** When var-decl init is an ident or expression, emitter checks `init_type->kind == TYPE_OPTIONAL` to decide whether to wrap in `{val, 1}` or assign directly. If init_type is `distinct ?T`, the check fails → wraps an already-optional value in `{val, 1}` → double-wrapping → GCC error.

**Where:** emitter.c:3259 and 3272.

**Surrounding code (lines 3256-3275):**
```c
} else if (node->var_decl.init->kind == NODE_IDENT) {
    Type *init_type = checker_get_type(e->checker, node->var_decl.init);
    if (init_type && init_type->kind == TYPE_OPTIONAL) {  // ← LINE 3259: BROKEN
        emit(e, " = ");
        emit_expr(e, node->var_decl.init);                // assign directly (already ?T)
    } else {
        emit(e, " = ("); emit_type(e, type);
        emit(e, "){ "); emit_expr(e, node->var_decl.init);
        emit(e, ", 1 }");                                  // wrap in {val, 1}
    }
} else {
    Type *init_type = checker_get_type(e->checker, node->var_decl.init);
    if (init_type && init_type->kind == TYPE_OPTIONAL) {  // ← LINE 3272: BROKEN (same)
```

**Fix for both:** `type_unwrap_distinct(init_type)->kind == TYPE_OPTIONAL`

**NOTE:** This fix will be BUILT INTO the B4 helper (`emit_opt_wrap_value`). If doing B4 first, these lines get replaced by the helper call and the fix is automatic.

---

### A4: Emitter comptime call distinct optional — P1

**What:** Comptime function returning `distinct ?u32` — emitter checks `ct->kind == TYPE_OPTIONAL` to decide wrapping. Distinct type fails, emits as plain integer.

**Where:** emitter.c:1438.

**Surrounding code (lines 1437-1444):**
```c
Type *ct = checker_get_type(e->checker, node);
if (ct && ct->kind == TYPE_OPTIONAL) {       // ← LINE 1438: BROKEN
    emit(e, "("); emit_type(e, ct);
    emit(e, "){%lld, 1}", (long long)node->call.comptime_value);
} else {
    emit(e, "%lld", (long long)node->call.comptime_value);
}
```

**Fix:** `Type *ct_eff = ct ? type_unwrap_distinct(ct) : NULL; if (ct_eff && ct_eff->kind == TYPE_OPTIONAL)`

---

### A5: Emitter global var distinct optional null init — P1

**What:** Same as A2 but for global variables. `emit_top_level_decl` → `emit_global_var` path.

**Where:** emitter.c:4925.

**Surrounding code (lines 4923-4930):**
```c
if (node->var_decl.init) {
    if (type && type->kind == TYPE_OPTIONAL &&     // ← LINE 4925: BROKEN
        !is_null_sentinel(type->optional.inner) &&
        node->var_decl.init->kind == NODE_NULL_LIT) {
        emit(e, " = ");
        emit_opt_null_literal(e, type);
```

**Fix:** Same pattern as A2: `type_unwrap_distinct(type)->kind == TYPE_OPTIONAL`

---

### A6: Emitter if-unwrap distinct optional condition — P1

**What:** If-unwrap `if (opt) |val| { }` — emitter checks condition type for null-sentinel optimization. Distinct `?*T` misses the optimization, takes struct optional path.

**Where:** emitter.c:3362.

**Surrounding code (lines 3359-3363):**
```c
int tmp = e->temp_count++;
Type *cond_type = checker_get_type(e->checker, node->if_stmt.cond);
bool is_ptr_opt = cond_type &&
    cond_type->kind == TYPE_OPTIONAL &&            // ← LINE 3362: BROKEN
    is_null_sentinel(cond_type->optional.inner);
```

**Fix:** `Type *cond_eff = cond_type ? type_unwrap_distinct(cond_type) : NULL;` then use `cond_eff`.

---

### A7: Spawn missing string-literal-to-mutable-slice — P1

**What:** `spawn process("hello")` where `process([]u8 data)` has mutable slice param. Regular call at line 3871 catches this. Spawn at line 8096 doesn't.

**Where:** checker.c, NODE_SPAWN handler (~line 8096). The arg loop validates pointer safety (shared vs non-shared) and type equality but NOT string literal safety.

**Fix:** Add before the pointer safety check (after line 8098):
```c
// String literal safety — same check as regular call (line 3871)
if (func_sym->func_node && func_sym->func_node->kind == NODE_FUNC_DECL &&
    i < func_sym->func_node->func_decl.param_count) {
    Type *param_type = resolve_type(c, func_sym->func_node->func_decl.params[i].type);
    if (node->spawn_stmt.args[i]->kind == NODE_STRING_LIT &&
        param_type && param_type->kind == TYPE_SLICE && !param_type->slice.is_const) {
        checker_error(c, node->loc.line,
            "spawn argument %d: cannot pass string literal to mutable []u8 parameter — "
            "string data is read-only, use const []u8", i + 1);
    }
}
```

---

### A8-A14: Checker distinct unwrap gaps — P2

All follow the same pattern: add `type_unwrap_distinct()` before `->kind` check. See original section for each line number and exact fix. These are one-liners — read 5 lines around each to verify context.

### A15-A20: See original section — P2/P3

---

## B. Duplication Elimination — Full Context

### B1: Pool/Slab DynFreed tracking — HIGH PRIORITY

**What:** When `pool.free(handles[k])` is called with a variable index, the checker records which array and index were freed, so it can auto-guard later `handles[j].field` access. This tracking code is IDENTICAL between pool.free (lines 3478-3498) and slab.free (lines 3639-3659). BUG-471 was caused by adding handle type validation to pool.free but forgetting slab.free.

**Surrounding code (pool.free, simplified):**
```c
if (mlen == 4 && memcmp(mname, "free", 4) == 0) {
    // ... arg count check, BUG-471 type check ...
    // Track dynamic-index free for auto-guard:
    if (node->call.arg_count == 1) {
        Node *arg = node->call.args[0];
        if (arg->kind == NODE_INDEX && arg->index_expr.object->kind == NODE_IDENT &&
            arg->index_expr.index->kind != NODE_INT_LIT) {
            // EXACT SAME 20 lines of grow+add to c->dyn_freed array
        }
    }
}
```

**Helper to extract:**
```c
static void track_dyn_freed_index(Checker *c, Node *arg) {
    if (arg->kind != NODE_INDEX || arg->index_expr.object->kind != NODE_IDENT ||
        arg->index_expr.index->kind == NODE_INT_LIT) return;
    // grow c->dyn_freed if needed
    // add entry: array_name, freed_idx, all_freed = c->in_loop
}
```

**Call sites:** Replace 20 lines at 3478 and 3639 with `track_dyn_freed_index(c, node->call.args[0]);`.

---

### B2: Union switch lock check — MEDIUM

**What:** When modifying a union during its own switch arm, the checker blocks the mutation (prevents type confusion). This check is duplicated between:
- Pointer auto-deref union (line 4577-4631): `ptr.variant = ...` inside `switch(*ptr)`
- Direct union field (line 4683-4736): `u.variant = ...` inside `switch(u)`

Both blocks do the same thing: walk to root ident, check name match, check type alias through pointer, check precise array key, emit error.

**Helper to extract:**
```c
static bool check_union_switch_mutation(Checker *c, Node *field_object, int line) {
    if (!c->union_switch_var) return false;
    Node *mut_root = field_object;
    while (mut_root) {
        if (mut_root->kind == NODE_UNARY && mut_root->unary.op == TOK_STAR)
            mut_root = mut_root->unary.operand;
        else if (mut_root->kind == NODE_FIELD) mut_root = mut_root->field.object;
        else if (mut_root->kind == NODE_INDEX) mut_root = mut_root->index_expr.object;
        else break;
    }
    if (!mut_root || mut_root->kind != NODE_IDENT) return false;
    // name match + type alias + precise key check
    // return true if blocked, false if allowed
}
```

---

### B3: Emitter orelse emission — use existing helpers

**What:** 4 near-identical blocks for orelse emission. Each manually checks `.has_value`, emits `.value`, handles `?void`. Helpers `emit_opt_null_check()` and `emit_opt_unwrap()` exist (defined at emitter.c lines 84-101) but are NOT used in these paths.

**The 4 blocks:**
1. Line 2296: `orelse return/break/continue` on `?void` — emits `(void)0` after check
2. Line 2317: `orelse return/break/continue` on `?T` — emits `_zer_tmp.value`
3. Line 2355: `orelse { block }` — full null/ptr/void dispatch
4. Line 2378: `orelse default_value` — ternary dispatch

**Common pattern in all 4:**
```c
emit(e, "({__typeof__("); emit_expr(e, expr);
emit(e, ") _zer_tmp%d = ", tmp); emit_expr(e, expr);
emit(e, "; if (!_zer_tmp%d.has_value) { ... } _zer_tmp%d.value; })", tmp, tmp);
//        ^^^^^^^^^^^^^^^^^^^^^^              ^^^^^^^^^^^^^^^^
//        could use emit_opt_null_check       could use emit_opt_unwrap
```

---

### B4: Optional {val, 1} wrapping — extract helper

**What:** 4 sites emit `(Type){ val, 1 }` for wrapping a value into a struct optional.

**Sites:**
- emitter.c:1401 — assignment optional wrapping
- emitter.c:1439 — comptime call optional wrapping
- emitter.c:3263 — var-decl ident init wrapping
- emitter.c:3276 — var-decl other-expr init wrapping

**Helper:**
```c
static void emit_opt_wrap_value(Emitter *e, Type *opt_type, Node *value_expr) {
    // Build in A3 fix: unwrap distinct before checking
    Type *eff = opt_type ? type_unwrap_distinct(opt_type) : NULL;
    if (eff && is_void_opt(opt_type)) {
        // ?void: call must be statement, then assign { 1 }
        emit_expr(e, value_expr);
        // caller handles the void special case
    } else {
        emit(e, "("); emit_type(e, opt_type);
        emit(e, "){ "); emit_expr(e, value_expr);
        emit(e, ", 1 }");
    }
}
```

---

### B5-B11: See original detailed descriptions above.

---

## Execution Order (detailed)

### Phase 1: Extract helpers (prevents fixing duplicated code twice)

**Step 1.1 — B1:** `track_dyn_freed_index()` in checker.c
- Read lines 3476-3500 and 3637-3661 to confirm they're identical
- Write helper at ~line 400 (near other checker helpers)
- Replace both 20-line blocks with one-line call
- `make docker-check` — expect 0 failures

**Step 1.2 — B4:** `emit_opt_wrap_value()` in emitter.c
- Build `type_unwrap_distinct` into the helper (fixes A3 automatically)
- Place helper near other opt helpers (~line 100)
- Replace 4 sites: 1401, 1439, 3263, 3276
- `make docker-check` — expect 0 failures

**Step 1.3 — B2:** `check_union_switch_mutation()` in checker.c
- Read lines 4577-4631 and 4683-4736 to confirm pattern
- Write helper, replace both ~50-line blocks
- `make docker-check`

### Phase 2: Bug fixes in unique code

**Step 2.1 — A1:** Add 5 `snprintf` clamps (lines 746, 1320, 8521, 8538, 8554)
**Step 2.2 — A2,A4,A5,A6:** Add `type_unwrap_distinct` at 4 remaining emitter sites
**Step 2.3 — A7:** Add spawn string literal check (~10 lines)
**Step 2.4 — Write test:** `tests/zer/distinct_optional_null_init.zer`
- `make docker-check`

### Phase 3: Remaining checker fixes

**Step 3.1 — A8-A14:** 7 one-liner distinct unwraps in checker.c
**Step 3.2 — A15:** Spawn validation gaps (5 checks)
**Step 3.3 — A16,A17:** Fixed arrays → stack-first dynamic
- `make docker-check`

### Phase 4: Emitter cleanup

**Step 4.1 — B3:** Orelse emission → use helpers
**Step 4.2 — B7:** Return optional wrapping consolidation
**Step 4.3 — B5,B6,B8:** Alloc emission, union typedef
- `make docker-check`

### Phase 5: Low priority

**Step 5.1 — B10:** zercheck arena keys
**Step 5.2 — A18-A20:** Volatile temps, distinct edge cases
**Step 5.3 — C1,C2:** Zig test runner + Makefile
**Step 5.4 — B11:** Pool/Slab method unification (biggest, save for last)
- `make docker-check`

---

## After All Phases — Documentation Updates

1. **BUGS-FIXED.md** — Add all bug numbers with symptom/cause/fix/test
2. **docs/compiler-internals.md** — Document all new helpers (B1-B4), update VRP architecture if changed
3. **CLAUDE.md** — Update:
   - Implementation status table (refactoring complete)
   - Roadmap (v0.3.0 → v0.3.1 with refactors)
   - Add new helpers to "Unified Helpers" sections
   - Update test counts
4. **README.md** — Update test counts if changed

---

## Files Modified Summary

| Phase | Files Modified | Helpers Added | Lines Removed | Lines Added |
|---|---|---|---|---|
| 1 | checker.c, emitter.c | `track_dyn_freed_index`, `emit_opt_wrap_value`, `check_union_switch_mutation` | ~140 | ~60 |
| 2 | checker.c, emitter.c, tests/zer/ | — | 0 | ~30 |
| 3 | checker.c, parser.c | — | 0 | ~50 |
| 4 | emitter.c | helper calls | ~100 | ~30 |
| 5 | zercheck.c, zig_tests/, Makefile | `handle_key_from_expr_a` | 0 | ~70 |
| **Total** | | 4 new helpers | **~240** | **~240** |

Net: ~0 lines change. Code stays same size but with fewer duplicated paths and more bug fixes.

---

## Quick Reference: All Fix Locations

```
CHECKER.C:
  182-184  A8   cross-module collision unwrap
  746      A1   snprintf clamp (slab_name)
  1108     A9   *void check unwrap
  1117     A10  ??T nesting unwrap
  1320     A1   snprintf clamp (mangled)
  1405     A11  const propagation unwrap
  1408     A11  const propagation unwrap (slice)
  1418     A11  volatile propagation unwrap
  1424     A11  volatile propagation unwrap (slice)
  1553     A12  comptime enum resolve unwrap
  2614     A13  resource assignment unwrap
  3367     A20  module-qualified call unwrap
  3478     B1   DynFreed tracking (pool) → helper
  3639     B1   DynFreed tracking (slab) → helper
  4577     B2   union switch lock (ptr) → helper
  4683     B2   union switch lock (direct) → helper
  7566     A14  string return mutable slice unwrap
  8096     A7   spawn string literal check (ADD)
  8127     A15  spawn validation gaps
  8521     A1   snprintf clamp (aname)
  8538     A1   snprintf clamp (iname)
  8554     A1   snprintf clamp (pname)
  8807     A16  labels[128] → dynamic

EMITTER.C:
  1438     A4   comptime call optional unwrap
  1401     B4   optional wrapping → helper
  1439     B4   optional wrapping → helper
  2026     A18  __auto_type → __typeof__ (volatile)
  2199     A18  __auto_type → __typeof__ (volatile)
  2296     B3   orelse → use helpers
  2317     B3   orelse → use helpers
  2355     B3   orelse → use helpers
  2378     B3   orelse → use helpers
  3232     A2   var-decl optional init unwrap
  3259     A3/B4 init_type optional unwrap → helper
  3272     A3/B4 init_type optional unwrap → helper
  3362     A6   if-unwrap cond unwrap
  3600     B7   return optional wrapping → consolidate
  3648     B7   return optional wrapping → consolidate
  4925     A5   global var optional init unwrap
  5246     B8   union typedef emission → helper

ZERCHECK.C:
  28 sites B10  char key[128] → arena-allocated

PARSER.C:
  2296     A17  container fields[128] → dynamic
```
