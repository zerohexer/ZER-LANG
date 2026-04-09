# ZER Compiler — Systematic Refactoring Gaps (Full Context Dump)

Generated 2026-04-09 after 100% codebase read (22,592 lines across 9 source files).
This document captures every scattered pattern, exact line numbers, past bugs caused,
and the helper functions needed to prevent future bugs systematically.

**Purpose:** A fresh session reads this BEFORE making changes. It replaces hours of
grep + read cycles. Every pattern here was verified by reading the actual source code.

---

## Codebase File Map

| File | Lines | Role |
|---|---|---|
| lexer.c | 643 | Scanner, tokens, keywords. No type dispatch. Clean. |
| parser.c | 2341 | Pratt parser, AST construction. Works with TypeNode, not Type. Clean. |
| ast.c/h | 466 | Node kinds (39 total), TypeNode kinds (22), `eval_const_expr`. Clean. |
| types.c/h | 532 | Semantic types (22 TypeKind), scope chain, Symbol struct, `type_unwrap_distinct`. Clean. |
| checker.c | 8815 | Type checking, builtin methods, escape analysis, range propagation, provenance, ISR safety. **Main refactoring target.** |
| emitter.c | 5336 | C code generation, optional handling, builtin emission, coercion. **Second refactoring target.** |
| zercheck.c | 2461 | Path-sensitive handle/move tracking. **Already partially refactored (Option A done 2026-04-09).** |
| zerc_main.c | 627 | Compiler entry, multi-module pipeline. Clean. |
| zer_lsp.c | 1371 | LSP server. 3 trivial type checks for IDE display. Clean. |
| tools/zer-convert.c | 2405 | C→ZER syntax converter. No type system code. N/A. |
| tools/zer-upgrade.c | 1525 | ZER compat→safe upgrader. No type system code. N/A. |

---

## Gap 1: Escape Flag Propagation (checker.c) — CRITICAL

### The Problem
Three boolean flags on Symbol track pointer provenance:
- `is_local_derived` — pointer to stack variable (cannot return/store in global)
- `is_arena_derived` — pointer from LOCAL arena (cannot return, but global arena is ok)
- `is_from_arena` — pointer from ANY arena (cannot store in global/static)

These flags are propagated at ~20 sites in checker.c. Each site must:
1. Check if source symbol has the flag
2. Check if target type can carry a pointer (`can_carry_ptr`)
3. Set the flag on the target symbol

**The bug:** Only 1 of 5 grouped propagation sites uses the `can_carry_ptr` type guard.
The other 4 propagate flags to ANY type including scalars, causing false positives
(BUG-421: `u32 val = struct_result.field` falsely marked local-derived).

### Exact Sites — Grouped propagation (all 3 flags set together)

**Line 1894-1896 (NODE_ASSIGN orelse fallback ident):**
```c
if (src && src->is_local_derived) tsym->is_local_derived = true;
if (src && src->is_arena_derived) tsym->is_arena_derived = true;
if (src && src->is_from_arena) tsym->is_from_arena = true;
```
**MISSING `can_carry_ptr` guard.** Scalar types falsely inherit flags.

**Line 1919-1921 (NODE_ASSIGN value ident alias):**
```c
if (src && src->is_local_derived) tsym->is_local_derived = true;
if (src && src->is_arena_derived) tsym->is_arena_derived = true;
    if (src && src->is_from_arena) tsym->is_from_arena = true;
```
**MISSING `can_carry_ptr` guard.** Note: indentation inconsistency (extra indent on from_arena).

**Line 5302-5307 (NODE_VAR_DECL init root ident) — THE ONLY CORRECT SITE:**
```c
Type *sym_eff = type ? type_unwrap_distinct(type) : NULL;
bool can_carry_ptr = sym_eff && (
    sym_eff->kind == TYPE_POINTER || sym_eff->kind == TYPE_SLICE ||
    sym_eff->kind == TYPE_STRUCT || sym_eff->kind == TYPE_UNION ||
    sym_eff->kind == TYPE_OPAQUE);
if (src && src->is_arena_derived && can_carry_ptr) sym->is_arena_derived = true;
if (src && src->is_from_arena && can_carry_ptr) sym->is_from_arena = true;
if (src && src->is_local_derived && can_carry_ptr) sym->is_local_derived = true;
```
This is the reference implementation. All other grouped sites should match.

**Line 5695-5697 (if-unwrap capture propagation):**
```c
if (csym && csym->is_local_derived) cap->is_local_derived = true;
if (csym && csym->is_arena_derived) cap->is_arena_derived = true;
if (csym && csym->is_from_arena) cap->is_from_arena = true;
```
**MISSING `can_carry_ptr` guard.**

**Line 6104-6106 (switch capture propagation):**
```c
if (src && src->is_local_derived) cap->is_local_derived = true;
if (src && src->is_arena_derived) cap->is_arena_derived = true;
if (src && src->is_from_arena) cap->is_from_arena = true;
```
**MISSING `can_carry_ptr` guard.**

### Exact Sites — Partial propagation (only SOME flags set)

| Line | Flags Set | Context | Correct? |
|---|---|---|---|
| 1889 | `local_derived` only | NODE_ASSIGN &local in orelse | Yes — `&local` is always local, arena flags N/A |
| 1909 | `local_derived` only | NODE_ASSIGN &local direct | Yes — same reason |
| 2233-2234 | `arena_derived` + `from_arena` | NODE_ASSIGN arena.alloc() | Yes — arena alloc, not local |
| 2270-2271 | `arena_derived` + `from_arena` | NODE_ASSIGN arena target | Yes |
| 5243-5246 | `arena_derived` + `from_arena` | NODE_VAR_DECL arena.alloc() | Yes |
| 5350 | `local_derived` only | NODE_VAR_DECL &local.field chain | Yes — only local flag needed |
| 5387-5392 | `local_derived`, conditional `from_arena` | NODE_VAR_DECL alias | **Needs audit** — is `from_arena` always propagated? |
| 5488 | `local_derived` only | Array→slice coercion | Yes — array is on stack |
| 5530 | `local_derived` only | Call result with local arg | Yes |
| 5714-5715 | `arena_derived` + `from_arena` | if-unwrap arena.alloc | Yes |

### Flag clear sites (NODE_ASSIGN target=NODE_IDENT, line 1866-1873)

```c
if (node->assign.target->kind == NODE_IDENT) {
    tsym->is_local_derived = false;
    tsym->is_arena_derived = false;
    tsym->provenance_type = NULL;
    // NOTE: is_from_arena is NOT cleared here. Bug or intentional?
}
```
**`is_from_arena` is NOT cleared on reassignment.** This may cause false positives:
`p = arena.alloc(); p = &global;` — `p` still has `is_from_arena=true` after reassign.
Needs investigation.

### Suggested Helpers

```c
// In types.h or checker.c:
static bool type_can_carry_pointer(Type *t) {
    if (!t) return false;
    Type *eff = type_unwrap_distinct(t);
    return eff && (eff->kind == TYPE_POINTER || eff->kind == TYPE_SLICE ||
                   eff->kind == TYPE_STRUCT || eff->kind == TYPE_UNION ||
                   eff->kind == TYPE_OPAQUE);
}

static void propagate_escape_flags(Symbol *dst, Symbol *src, Type *dst_type) {
    if (!src || !type_can_carry_pointer(dst_type)) return;
    if (src->is_local_derived) dst->is_local_derived = true;
    if (src->is_arena_derived) dst->is_arena_derived = true;
    if (src->is_from_arena) dst->is_from_arena = true;
}
```

### Bug Class Prevented
- BUG-421 (scalar false positive)
- Any future 4th escape flag (e.g., `is_thread_derived`) added at some sites but not all
- False positives from scalar captures in if-unwrap/switch arms

---

## Gap 2: Void Optional Check (emitter.c) — HIGH

### The Problem
`?void` has ONE field (`has_value`). All other `?T` have TWO fields (`value`, `has_value`).
Emitting `.value` on `?void` → GCC error. The check `type_unwrap_distinct(t->optional.inner)->kind == TYPE_VOID`
is repeated manually at **16 sites** in emitter.c.

### Exact Sites

| Line | Context |
|---|---|
| 155 | `emit_type` slice optional inner void check |
| 1280 | NODE_ASSIGN null to ?void |
| 2141 | Orelse `is_void_optional` flag |
| 2162 | Orelse return path — ?void return null |
| 2199 | Orelse return path (duplicate for non-void orelse) |
| 2981 | Var-decl orelse return — ?void null |
| 3000 | Var-decl ?void init from void call |
| 3027 | Var-decl ?void orelse block |
| 3039 | Var-decl ?void from void function call |
| 3231 | if-unwrap ?void condition |
| 3382 | NODE_RETURN defer hoist — ?void wrap |
| 3414 | NODE_RETURN null literal — ?void |
| 3431 | NODE_RETURN value from ?void func |
| 3472 | NODE_RETURN bare — ?void |
| 4348 | emit_top_level_decl ?void global init |

Plus 1 in checker.c (line ~4275 — ?T→T orelse hint).

### Suggested Helper

```c
// In emitter.c:
static bool is_void_optional(Type *t) {
    if (!t) return false;
    Type *eff = type_unwrap_distinct(t);
    if (eff->kind != TYPE_OPTIONAL) return false;
    Type *inner = type_unwrap_distinct(eff->optional.inner);
    return inner && inner->kind == TYPE_VOID;
}
```

### Bug Class Prevented
- BUG-042: `?Enum` fell to anonymous struct (`.value` on void-sized type)
- BUG-145: `?void` return emitted `.value`
- BUG-408: `?void` init from void function emitted compound literal with void expression
- Any new optional emission path that forgets the void check

---

## Gap 3: Optional Null-Sentinel Branching (emitter.c) — HIGH

### The Problem
`?*T` and `?FuncPtr` use null sentinel (pointer is NULL = none, non-NULL = some).
All other `?T` use struct wrapper (`{ T value; uint8_t has_value; }`).
Every optional emission site branches on `is_null_sentinel(inner)`:
- Check: `if (!expr)` vs `if (!expr.has_value)`
- Unwrap: `expr` vs `expr.value`
- Null: `NULL`/`0` vs `{ 0, 0 }`
- Wrap: bare value vs `{ val, 1 }`

This branching is at **20 sites** in emitter.c. `is_null_sentinel()` exists as a helper
(line 54), which is good. But each site manually implements the branch logic.

### is_null_sentinel implementation (emitter.c:54-62)

```c
static inline bool is_null_sentinel(Type *inner) {
    if (!inner) return false;
    inner = type_unwrap_distinct(inner);
    return inner->kind == TYPE_POINTER || inner->kind == TYPE_FUNC_PTR;
}
```

### Key emission sites using is_null_sentinel

| Line | Context | Pattern |
|---|---|---|
| 152-160 | emit_type optional | emit opt typedef vs pointer |
| 510 | emit_type TYPE_OPTIONAL | null sentinel = raw pointer |
| 983 | NODE_BINARY == null | `.has_value` vs `!ptr` |
| 1266-1283 | NODE_ASSIGN optional | wrap/null emit |
| 2137-2219 | Orelse 3 paths | check + unwrap + null return |
| 2957-3045 | Var-decl orelse/init | check + unwrap + null |
| 3155-3160 | if bare `if(optional)` | `.has_value` vs `!ptr` |
| 3281 | if-unwrap condition | `.has_value` vs ptr check |
| 3342 | while(optional) | `.has_value` vs ptr check |
| 3380-3475 | NODE_RETURN all paths | null/value/bare return |

### Suggested Helpers

```c
static void emit_opt_null_check(Emitter *e, const char *tmp, Type *opt_type) {
    Type *eff = type_unwrap_distinct(opt_type);
    if (is_null_sentinel(eff->optional.inner))
        emit(e, "!%s", tmp);
    else
        emit(e, "!%s.has_value", tmp);
}

static void emit_opt_unwrap(Emitter *e, const char *tmp, Type *opt_type) {
    Type *eff = type_unwrap_distinct(opt_type);
    if (is_null_sentinel(eff->optional.inner))
        emit(e, "%s", tmp);
    else if (is_void_optional(opt_type))
        emit(e, "(void)0");  // ?void has no .value
    else
        emit(e, "%s.value", tmp);
}

static void emit_opt_null_literal(Emitter *e, Type *opt_type) {
    Type *eff = type_unwrap_distinct(opt_type);
    if (is_null_sentinel(eff->optional.inner)) {
        emit(e, "(");
        emit_type(e, eff->optional.inner);
        emit(e, ")0");
    } else if (is_void_optional(opt_type)) {
        emit(e, "("); emit_type(e, opt_type); emit(e, "){ 0 }");
    } else {
        emit(e, "("); emit_type(e, opt_type); emit(e, "){ 0, 0 }");
    }
}

static void emit_opt_wrap_value(Emitter *e, Type *opt_type, const char *val_expr) {
    Type *eff = type_unwrap_distinct(opt_type);
    if (is_null_sentinel(eff->optional.inner)) {
        emit(e, "%s", val_expr);
    } else if (is_void_optional(opt_type)) {
        emit(e, "("); emit_type(e, opt_type); emit(e, "){ 1 }");
    } else {
        emit(e, "("); emit_type(e, opt_type); emit(e, "){ %s, 1 }", val_expr);
    }
}
```

### Bug Class Prevented
- BUG-042: `?Enum` anonymous struct mismatch
- Every orelse path bug in history
- Every return-null/return-value path bug
- New optional-like types (e.g., `?Handle` becoming null-sentinel in future)

---

## Gap 4: ISR Ban Check (checker.c) — LOW

### The Problem
`c->in_interrupt` is checked at 4 allocation method sites to ban heap allocation in ISR.
Each check emits its own error message. Adding a new allocation method means adding another.

### Exact Sites

| Line | Method |
|---|---|
| 2711 | `slab.alloc()` |
| 2766 | `slab.alloc_ptr()` |
| 2900 | `Task.new()` |
| 2950 | `Task.new_ptr()` |

Pool methods (lines 2562-2648) do NOT check `in_interrupt` — correct, Pool is fixed-size and ISR-safe.

### Suggested Helper

```c
static bool check_isr_ban(Checker *c, int line, const char *method) {
    if (!c->in_interrupt) return false;
    checker_error(c, line,
        "%s not allowed in interrupt handler — "
        "malloc/calloc may deadlock. Use Pool(T, N) instead", method);
    return true;
}
```

### Bug Class Prevented
- New Slab-like allocator (e.g., `GrowablePool`) allowed in ISR silently

---

## Gap 5: Optional Null/Wrap Literal Emission (emitter.c) — MEDIUM

### The Problem
The patterns `{ 0, 0 }` (null ?T), `{ 0 }` (null ?void), `{ val, 1 }` (wrap ?T),
`{ 1 }` (wrap ?void) are emitted manually at ~12 sites. Getting the field count wrong
for ?void was the root cause of BUG-042.

### Exact `{ 0, 0 }` Sites (null optional struct)

| Line | Context |
|---|---|
| 158 | emit_type optional null |
| 1283 | NODE_ASSIGN null to ?T |
| 2165 | Orelse void return null |
| 2202 | Orelse non-void return null |
| 2984 | Var-decl orelse return null |
| 3417 | NODE_RETURN null literal ?T |
| 5013 | Preamble pool alloc return |

### Exact `{ 0 }` Sites (null ?void)

| Line | Context |
|---|---|
| 155 | emit_type optional void null |
| 1280 | NODE_ASSIGN null to ?void |
| 2163 | Orelse void return null |
| 2200 | Orelse non-void return (void variant) |
| 3415 | NODE_RETURN null literal ?void |

### Covered by Gap 3 helpers
`emit_opt_null_literal()` and `emit_opt_wrap_value()` from Gap 3 would eliminate all of these.

---

## Gap 6: Array→Slice Coercion Dispatch (emitter.c) — LOW

### The Problem
`emit_array_as_slice()` must be called at all 4 value-flow sites. BUG-419 was caused
by the assignment site being missing.

### Exact Sites

| Line | Context | Added when |
|---|---|---|
| 1288 | NODE_ASSIGN | BUG-419 fix |
| 1694 | NODE_CALL arg | Original |
| 3084 | NODE_VAR_DECL init | Original |
| 3450 | NODE_RETURN | Original |

### Not a helper issue — it's a dispatch issue
A `emit_value_coercion(e, from_type, to_type, expr)` dispatcher called from all 4 sites
would prevent missing any. But the current explicit calls work and are documented in
CLAUDE.md ("any new coercion must work in ALL 4 value-flow sites").

---

## Gap 7: Return-Null Emission (emitter.c) — MEDIUM

### The Problem
The pattern "emit return with zero/null value for current function's return type" is
duplicated at ~6 sites. Each must handle: `?void` → `{ 0 }`, `?T` struct → `{ 0, 0 }`,
`?*T` null sentinel → `(T*)0`, void → `return;`, other → `return 0;`.

### Exact Duplicate Code Blocks

**Block A: Lines 2156-2170 (orelse ?void return path)**
```c
if (e->current_func_ret && e->current_func_ret->kind == TYPE_OPTIONAL &&
    !is_null_sentinel(e->current_func_ret->optional.inner)) {
    emit(e, "return ("); emit_type(e, e->current_func_ret);
    if (type_unwrap_distinct(e->current_func_ret->optional.inner)->kind == TYPE_VOID)
        emit(e, "){ 0 }; ");
    else emit(e, "){ 0, 0 }; ");
} else if (e->current_func_ret && e->current_func_ret->kind != TYPE_VOID) {
    emit(e, "return 0; ");
} else { emit(e, "return; "); }
```

**Block B: Lines 2193-2207 — IDENTICAL to Block A**

**Block C: Lines 2978-2985 (var-decl orelse return) — same pattern**

**Block D: Lines 3407-3418 (NODE_RETURN null literal) — same pattern with semicolons**

**Block E: Lines 3459-3479 (NODE_RETURN bare) — similar but for bare return (has_value=1)**

### Suggested Helper

```c
static void emit_return_null(Emitter *e) {
    // Emits "return <zero_value_for_current_func_ret>;"
    // Handles ?void, ?T struct, ?*T null sentinel, void, other
    Type *ret = e->current_func_ret;
    if (!ret || ret->kind == TYPE_VOID) { emit(e, "return; "); return; }
    Type *eff = type_unwrap_distinct(ret);
    if (eff->kind == TYPE_OPTIONAL && !is_null_sentinel(eff->optional.inner)) {
        emit(e, "return ("); emit_type(e, ret);
        if (is_void_optional(ret)) emit(e, "){ 0 }; ");
        else emit(e, "){ 0, 0 }; ");
    } else { emit(e, "return 0; "); }
}
```

### Bug Class Prevented
- BUG-409 (distinct ?T return null)
- Wrong field count in return null literal
- Inconsistent return path for new optional variants

---

## Gap 8: Auto-Slab Creation Duplication (checker.c) — LOW

### The Problem
`Task.new()` (lines 2908-2941) and `Task.new_ptr()` (lines 2958-2990) have **identical**
40-line blocks for finding or creating an auto-Slab. Copy-paste duplication.

### Suggested Helper

```c
static Symbol *find_or_create_auto_slab(Checker *c, Type *struct_type) {
    // Check existing auto_slabs
    for (int i = 0; i < c->auto_slab_count; i++) {
        if (type_equals(c->auto_slabs[i].elem_type, struct_type))
            return c->auto_slabs[i].slab_sym;
    }
    // Create new auto-slab symbol
    char name[128];
    int len = snprintf(name, sizeof(name), "_zer_auto_slab_%.*s",
        (int)struct_type->struct_type.name_len, struct_type->struct_type.name);
    // ... (arena alloc, scope_add, register in auto_slabs array)
    return new_sym;
}
```

### Bug Class Prevented
- Divergent auto-slab creation logic between .new() and .new_ptr()
- If a third method (e.g., `.new_slice()`) is added, third copy of 40 lines

---

## Gap 9: Volatile Stripping Check (checker.c) — LOW

### The Problem
The volatile qualifier stripping check (walk source ident to find `sym->is_volatile`,
reject if target drops volatile) is repeated at 3 sites with identical logic.

### Exact Sites

| Line | Context |
|---|---|
| 4146-4158 | NODE_TYPECAST (C-style cast) |
| 4374-4390 | @ptrcast intrinsic |
| 4455-4472 | @bitcast intrinsic |

Each site does:
```c
if (src pointer volatile && target pointer !volatile) {
    // also check sym->is_volatile for symbol-level volatile
    if (src_sym && src_sym->is_volatile) src_volatile = true;
    if (src_volatile) error("cannot strip volatile");
}
```

### Suggested Helper

```c
static void check_volatile_strip(Checker *c, Node *src_expr, Type *src_type,
                                  Type *tgt_type, int line, const char *context) {
    Type *seff = type_unwrap_distinct(src_type);
    Type *teff = type_unwrap_distinct(tgt_type);
    if (seff->kind != TYPE_POINTER || teff->kind != TYPE_POINTER) return;
    if (teff->pointer.is_volatile) return;  // target keeps volatile — ok
    bool src_vol = seff->pointer.is_volatile;
    if (!src_vol && src_expr->kind == NODE_IDENT) {
        Symbol *s = scope_lookup(c->current_scope, ...);
        if (s && s->is_volatile) src_vol = true;
    }
    if (src_vol) checker_error(c, line, "%s cannot strip volatile qualifier", context);
}
```

---

## Already Refactored (zercheck.c, done 2026-04-09)

### Unified Helpers Added

| Helper | Purpose | Sites Updated |
|---|---|---|
| `should_track_move(Type *t)` | Move struct OR struct containing move field | 5 sites |
| `is_handle_invalid(HandleInfo *h)` | FREED / MAYBE_FREED / TRANSFERRED | 5 use-check sites |
| `is_handle_consumed(HandleInfo *h)` | Same states, for merge logic | 4 merge sites |
| `zc_report_invalid_use(...)` | Correct error message by state | 5 error sites |
| `contains_move_struct_field(Type *t)` | Struct with move field (1 level) | Used by should_track_move |

### Bugs These Prevented
- BUG-468: `HS_TRANSFERRED` missing from if/else merge (conditional move struct)
- BUG-469: Regular struct containing move struct field not tracked
- BUG-470: NODE_RETURN not marking move struct as transferred

---

## Priority Implementation Order

| Priority | Gap | Impact | Effort | Lines Changed |
|---|---|---|---|---|
| **1** | Gap 1: Escape flags + `can_carry_ptr` | **Active bug risk** — 4 sites missing guard | ~20 lines | checker.c: 5 sites |
| **2** | Gap 2: `is_void_optional()` | Prevents #1 historical emitter bug class | ~10 lines | emitter.c: 16 sites |
| **3** | Gap 3: Optional emission helpers | Prevents all orelse/return path bugs | ~40 lines | emitter.c: 20 sites |
| **4** | Gap 7: `emit_return_null()` | Eliminates 5 duplicate code blocks | ~15 lines | emitter.c: 6 sites |
| **5** | Gap 8: Auto-slab dedup | Eliminates 40-line copy-paste | ~30 lines | checker.c: 2 sites |
| **6** | Gap 4: ISR ban helper | Clean but low risk | ~10 lines | checker.c: 4 sites |
| **7** | Gap 9: Volatile strip helper | Clean but only 3 sites | ~15 lines | checker.c: 3 sites |
| **8** | Gap 6: Coercion dispatch | Already documented, low risk | ~40 lines | emitter.c: 4 sites |

### Estimated Total
~180 lines of new helpers, replacing ~300 lines of scattered checks.
Net reduction: ~120 lines. Zero new features — pure structural improvement.

---

## How to Verify After Refactoring

1. `make docker-check` — all 3400+ C tests must pass
2. `rust_tests/run_tests.sh` — all 567 Rust-equivalent tests must pass
3. Grep for the OLD pattern to confirm it's fully replaced:
   - Gap 1: `grep "is_local_derived = true" checker.c` — should only appear in the helper
   - Gap 2: `grep "optional.inner.*TYPE_VOID" emitter.c` — should only appear in `is_void_optional`
   - Gap 3: `grep "has_value" emitter.c` — count should drop significantly
4. Write a NEW test that exercises the edge case the scattered pattern missed
   (e.g., scalar capture in if-unwrap for Gap 1)

---

## Cross-Reference: Past Bugs Caused by These Patterns

| Bug | Gap | Root Cause |
|---|---|---|
| BUG-042 | 2,3,5 | `?Enum` fell to anonymous struct — `.value` on void-sized type |
| BUG-145 | 2 | `?void` return emitted `.value` |
| BUG-408 | 2 | `?void` init from void function in compound literal |
| BUG-409 | 2,3,7 | Distinct `?T` — 5 sites missed distinct unwrap before void check |
| BUG-419 | 6 | Array→slice coercion missing in NODE_ASSIGN |
| BUG-421 | 1 | Scalar field falsely marked `is_local_derived` |
| BUG-468 | (fixed) | `HS_TRANSFERRED` missing from if/else path merge |
| BUG-469 | (fixed) | Struct containing move struct field not tracked |
| BUG-470 | (fixed) | NODE_RETURN not marking move struct as transferred |
