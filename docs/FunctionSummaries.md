# Function Summaries — Design Document

## Problem Statement

ZER's compiler has 28 independent tracking systems for safety. Five of them track
function-level properties — "what does this function DO?" — but each uses a different
pattern, different caching, different cycle detection, and different scan functions:

| # | System | Location | What it answers | Caching | Cycle detection |
|---|---|---|---|---|---|
| 9 | `FuncSummary` | zercheck.c | "does this function free param[i]?" | Pre-built per function | Multi-pass (4 iterations) |
| 18 | `StackFrame` | checker.c | "what functions does this call? frame size?" | Built in post-pass | DFS with `is_recursive` |
| 4 | `ProvSummary` | checker.c | "what provenance does return carry?" | Array on Checker | None |
| 28 | `FuncSharedTypes` | checker.c | "what shared types does this touch?" | `computed` + `in_progress` | Proper DFS |
| — | `has_atomic_or_barrier()` | checker.c | "does this body use atomics?" | None (re-scans every time) | None |
| — | `scan_unsafe_global_access()` | checker.c | "does this body touch unsafe globals?" | None (re-scans every time) | Static depth counter (limit 8) |

Additionally, there are context flags on the Checker struct (`in_loop`, `defer_depth`,
`critical_depth`, `in_async`, `in_interrupt`, `in_naked`) that track "where am I right
now?" — but these are LOCAL state, not function PROPERTIES. They can't answer "does
calling helper() from inside @critical eventually yield?"

### The Bugs This Solves

The flag-handler matrix audit (`tools/audit_matrix.sh`) found 5 missing checks:

| Bug | What happens | Why flags can't fix it properly |
|---|---|---|
| BUG-507: yield in @critical | Interrupts stay disabled across yield → system hang | Flag checks only catch DIRECT yield, not yield through function calls |
| BUG-508: spawn in async | Spawned thread outlives coroutine → dangling thread | Same — can't catch spawn inside called helper |
| yield in defer | Duff's device state machine corrupted | Same — transitive case missed |
| await in @critical | Same as yield — suspends with interrupts disabled | Same |
| await in defer | Same as yield in defer | Same |
| spawn in interrupt | pthread_create in ISR — unsafe | Same |

Example of the transitive case flags can NEVER catch:

```zer
void helper() {
    yield;  // yields inside helper
}

@critical {
    helper();  // flags see a function call, not a yield
              // BUG: yield happens transitively but @critical doesn't know
}
```

Flags check "is there a yield statement HERE?" — they can't follow function calls.
Function summaries check "can this function (or anything it calls) yield?" — transitive.

### Why Not Add More Flags?

Adding `if (c->critical_depth > 0)` to NODE_YIELD fixes the DIRECT case (3 lines).
But:
1. Doesn't catch transitive case (helper() that yields)
2. Must be added to EVERY new control-flow node — forgettable
3. The audit script catches forgetting, but after the fact
4. Each new context (future) × each new operation = N×M manual checks

Function summaries fix the direct AND transitive case with ONE mechanism.

## Design Decision History

### Approaches Considered

**1. More flags (rejected)**
Add `critical_depth` check to NODE_YIELD, `in_interrupt` check to NODE_SPAWN, etc.
- Pro: 3 lines each, trivial
- Con: Only catches direct cases, not transitive. Forgettable. N×M scaling.
- Verdict: REJECTED — patches the symptom, doesn't fix the model

**2. Table-driven dispatch (rejected for this problem)**
Define a table mapping NODE_ types to banned context flags. One dispatch function
checks all flags before calling the handler.
- Pro: Can't forget flag checks — table forces you to declare constraints
- Con: Still flag-based — still can't follow function calls transitively
- Con: Table is just organized flags, same model, same transitive limitation
- Verdict: REJECTED for context safety. Useful for v0.4 code organization, but
  orthogonal to the transitive analysis problem.

**3. Effect system — language feature (rejected)**
Add effect annotations to function signatures: `void worker() yields { ... }`
Compiler checks that callers respect effect restrictions.
- Pro: Formal, user-visible, standard PL theory
- Con: Requires new syntax — language feature addition
- Con: ZER sees ALL function bodies (no opaque boundaries) — inference is complete
- Con: User annotations would be redundant since compiler can figure it out
- Verdict: REJECTED — adds syntax complexity for zero safety benefit. Effects are
  needed when the compiler CAN'T see bodies (Rust traits, Java interfaces). ZER's
  `import` gives full AST visibility. Inference covers everything.

**4. Function summaries — inferred properties (CHOSEN)**
Scan function bodies, compute properties (can_yield, can_spawn, can_alloc), cache
on Symbol, check at context boundaries. Same approach as GCC IPA and LLVM function
attributes.
- Pro: No new syntax — fully inferred from existing code
- Pro: Catches both direct AND transitive cases
- Pro: Cached — each function scanned once, O(1) lookup after
- Pro: Proper DFS cycle detection — no depth limit hack
- Pro: Unifies 3 existing ad-hoc scanners into one framework
- Pro: Production-proven pattern (GCC, LLVM, every serious compiler)
- Verdict: CHOSEN — correct, minimal, proven

### Why Not Effects?

Effects are a language-level concept where functions DECLARE what they do:
```
void worker() yields { yield; }        // user writes "yields"
@critical { worker(); }                // compiler checks: "yields" forbidden
```

Function summaries are a compiler-internal concept where the compiler INFERS what
functions do:
```
void worker() { yield; }              // user writes nothing special
@critical { worker(); }               // compiler scanned worker, knows it yields
```

For effects to be needed, you need **opaque boundaries** — places where the compiler
can't see the function body:
- Rust: trait objects (`dyn Trait`) — don't know which impl at compile time
- Java: interfaces — implementation is in a different JAR
- C with headers: `.h` file has declaration but body is in separate `.c`

ZER has NONE of these:
- No traits, no dynamic dispatch
- No opaque interfaces
- `import` gives full AST (parser + checker see everything)
- `cinclude` functions have no body, but C functions can't yield/spawn anyway

So inference is COMPLETE. The compiler sees every body, infers every property, and
user annotations would be redundant. Function summaries give identical correctness
to effects without any syntax addition.

ZER will never need effects because ZER will never have opaque module boundaries.
That's a design decision, not a limitation. ZER is memory-safe C — full visibility
is the architecture.

## What Function Summaries Are

Standard compiler engineering term. Every production compiler has them:

- **GCC IPA (Inter-Procedural Analysis):** Computes function summaries in a separate
  pass. Properties: pure, const, noreturn, malloc-like, etc. Used for optimization
  AND correctness. `__attribute__((pure))` is a user override for when inference
  isn't possible (assembly, external libs).

- **LLVM Function Attributes:** `readonly`, `noreturn`, `nounwind`, `willreturn`.
  Inferred by optimization passes, stored on Function objects. Used by later passes
  for correctness checking and optimization.

- **ZER (existing, fragmented):** 4 separate summary structures + 2 standalone scan
  functions. Each answers one question about one function. No unified framework.

Function summaries are NOT a new concept. ZER already does them 6 times. This design
unifies the pattern into one consistent framework.

## Architecture

### Data Structure

```c
// On Symbol struct in types.h
typedef struct {
    bool computed;        // true = cached, don't re-scan
    bool in_progress;     // true = currently scanning (DFS cycle detection)

    // Context safety effects (NEW — fixes the 5 bugs)
    bool can_yield;       // body contains yield/await, directly or transitively
    bool can_spawn;       // body contains spawn, directly or transitively
    bool can_alloc;       // body contains slab.alloc/Task.new, directly or transitively

    // Absorbs existing standalone scanners (UNIFICATION)
    bool has_sync;                  // replaces has_atomic_or_barrier()
    bool accesses_unsafe_global;    // replaces scan_unsafe_global_access() result
    // Note: accesses_unsafe_global needs the global name for error messages,
    // so scan_unsafe_global_access() stays as a separate detailed scanner.
    // has_sync is a pure bool — can be fully absorbed.
} FuncProps;
```

Added to Symbol:
```c
struct Symbol {
    // ... existing fields ...
    FuncProps props;   // computed function properties (lazy, cached)
};
```

### Why On Symbol, Not On Checker

Properties are per-FUNCTION, not per-checker-position. A function's `can_yield`
doesn't change based on where the checker is currently looking. It's an intrinsic
property of the function body. Symbol is the right place — same as `return_range`,
`provenance_type`, and other per-function data already on Symbol.

### Lazy Computation

Properties are computed ON FIRST ACCESS, not eagerly. This matters because:

1. **Forward calls:** If `main()` calls `helper()` and both are in the same file
   with `main` declared first, `helper`'s body hasn't been type-checked when `main`
   is processed. But the RAW AST is available (from register_decl pass 1).

2. **Cross-module:** Imported module functions are registered in global scope during
   pass 1. Their `func_node` points to the AST. Lazy scan follows them.

3. **Performance:** Most functions are never called from restricted contexts. Lazy
   means we only scan functions that actually need it.

```c
static void ensure_func_props(Checker *c, Symbol *sym) {
    if (!sym || !sym->is_function) return;
    if (sym->props.computed) return;          // already done
    if (sym->props.in_progress) return;       // cycle — conservative default (false)
    if (!sym->func_node) return;              // no body (cinclude, forward decl)

    sym->props.in_progress = true;

    Node *body = NULL;
    if (sym->func_node->kind == NODE_FUNC_DECL)
        body = sym->func_node->func_decl.body;
    else if (sym->func_node->kind == NODE_INTERRUPT)
        body = sym->func_node->interrupt.body;

    if (body) {
        scan_func_props(c, body, &sym->props);
    }

    sym->props.in_progress = false;
    sym->props.computed = true;
}
```

### DFS Cycle Detection

Uses the same pattern as `FuncSharedTypes` (deadlock DFS) already in the codebase:

- `computed = false, in_progress = false` → not yet scanned
- `computed = false, in_progress = true` → currently being scanned (we're inside this function's DFS)
- `computed = true` → done, use cached result

If we encounter a function with `in_progress = true`, we've found a cycle (recursive
call chain). We stop — the cycle contributes no NEW effects (if function A calls B
calls A, A's effects are already being collected on the first visit). This is
conservative and correct.

No depth limit needed. No static counter. Handles any call graph depth and any
cycle shape. Same pattern proven by the deadlock DFS that's been in production.

### The Scanner

One recursive AST walker that collects ALL properties in a single pass:

```c
static void scan_func_props(Checker *c, Node *node, FuncProps *props) {
    if (!node) return;

    switch (node->kind) {
    // === Direct effect detection ===
    case NODE_YIELD:
    case NODE_AWAIT:
        props->can_yield = true;
        return;

    case NODE_SPAWN:
        props->can_spawn = true;
        return;

    // === Allocation detection (ISR safety) ===
    // slab.alloc(), slab.alloc_ptr(), Task.new(), Task.new_ptr()
    // Detected by: NODE_CALL with callee NODE_FIELD where method is "alloc"/"alloc_ptr"
    //              OR NODE_CALL with callee NODE_FIELD where object is struct type + method is "new"/"new_ptr"

    // === Sync detection (absorbs has_atomic_or_barrier) ===
    case NODE_INTRINSIC: {
        const char *n = node->intrinsic.name;
        uint32_t nl = (uint32_t)node->intrinsic.name_len;
        if ((nl >= 7 && memcmp(n, "atomic_", 7) == 0) ||
            (nl == 7 && memcmp(n, "barrier", 7) == 0) ||
            (nl == 13 && memcmp(n, "barrier_store", 13) == 0) ||
            (nl == 12 && memcmp(n, "barrier_load", 12) == 0))
            props->has_sync = true;
        // Fall through to scan intrinsic args
        for (int i = 0; i < node->intrinsic.arg_count; i++)
            scan_func_props(c, node->intrinsic.args[i], props);
        return;
    }

    // === Transitive: follow function calls ===
    case NODE_CALL:
        // Scan call arguments and callee expression
        if (node->call.callee)
            scan_func_props(c, node->call.callee, props);
        for (int i = 0; i < node->call.arg_count; i++)
            scan_func_props(c, node->call.args[i], props);

        // Follow callee into its body (transitive propagation)
        if (node->call.callee && node->call.callee->kind == NODE_IDENT) {
            Symbol *callee = scope_lookup(c->global_scope,
                node->call.callee->ident.name,
                (uint32_t)node->call.callee->ident.name_len);
            if (callee && callee->is_function) {
                ensure_func_props(c, callee);  // lazy compute + cache
                // Merge callee properties into ours
                if (callee->props.can_yield) props->can_yield = true;
                if (callee->props.can_spawn) props->can_spawn = true;
                if (callee->props.can_alloc) props->can_alloc = true;
                if (callee->props.has_sync)  props->has_sync = true;
            }
        }

        // Function pointer tracking:
        // If callee is NODE_IDENT resolving to TYPE_FUNC_PTR variable,
        // check if variable's init was a known function. If so, follow that.
        // (Same pattern as scan_frame's indirect call tracking.)
        // If can't resolve → conservative (assume no effects).
        // Future: warn "function pointer call in restricted context"
        return;

    // === Recursive walk into all child nodes ===
    case NODE_BLOCK:
        for (int i = 0; i < node->block.stmt_count; i++)
            scan_func_props(c, node->block.stmts[i], props);
        return;

    case NODE_IF:
        scan_func_props(c, node->if_stmt.cond, props);
        scan_func_props(c, node->if_stmt.then_body, props);
        scan_func_props(c, node->if_stmt.else_body, props);
        return;

    case NODE_FOR:
        scan_func_props(c, node->for_stmt.init, props);
        scan_func_props(c, node->for_stmt.cond, props);
        scan_func_props(c, node->for_stmt.step, props);
        scan_func_props(c, node->for_stmt.body, props);
        return;

    case NODE_WHILE: case NODE_DO_WHILE:
        scan_func_props(c, node->while_stmt.cond, props);
        scan_func_props(c, node->while_stmt.body, props);
        return;

    case NODE_SWITCH:
        scan_func_props(c, node->switch_stmt.expr, props);
        for (int i = 0; i < node->switch_stmt.arm_count; i++)
            scan_func_props(c, node->switch_stmt.arms[i].body, props);
        return;

    case NODE_RETURN:
        scan_func_props(c, node->ret.expr, props);
        return;

    case NODE_EXPR_STMT:
        scan_func_props(c, node->expr_stmt.expr, props);
        return;

    case NODE_VAR_DECL:
        scan_func_props(c, node->var_decl.init, props);
        return;

    case NODE_ASSIGN:
        scan_func_props(c, node->assign.target, props);
        scan_func_props(c, node->assign.value, props);
        return;

    case NODE_BINARY:
        scan_func_props(c, node->binary.left, props);
        scan_func_props(c, node->binary.right, props);
        return;

    case NODE_UNARY:
        scan_func_props(c, node->unary.operand, props);
        return;

    case NODE_FIELD:
        scan_func_props(c, node->field.object, props);
        return;

    case NODE_INDEX:
        scan_func_props(c, node->index_expr.object, props);
        scan_func_props(c, node->index_expr.index, props);
        return;

    case NODE_ORELSE:
        scan_func_props(c, node->orelse.expr, props);
        return;

    case NODE_DEFER:
        scan_func_props(c, node->defer.body, props);
        return;

    case NODE_CRITICAL:
        scan_func_props(c, node->critical.body, props);
        return;

    case NODE_ONCE:
        scan_func_props(c, node->once.body, props);
        return;

    default:
        return;
    }
}
```

### Context Checking

At each restricted context entry, scan the body and check for banned effects.
This is NOT a flag check — it's a one-time body scan at the entry point that
catches both direct and transitive cases.

```c
// Helper: scan a body subtree for banned effects, report errors
static void check_body_effects(Checker *c, Node *body, int line,
                                bool ban_yield, const char *yield_reason,
                                bool ban_spawn, const char *spawn_reason,
                                bool ban_alloc, const char *alloc_reason);
```

Applied at 3 entry points:

**NODE_CRITICAL:**
```c
case NODE_CRITICAL:
    // Check body for banned effects BEFORE entering
    check_body_effects(c, node->critical.body, node->loc.line,
        true,  "cannot yield inside @critical — interrupts stay disabled across suspend",
        true,  "cannot spawn inside @critical — thread creation with interrupts disabled",
        false, NULL);  // alloc OK in @critical
    c->critical_depth++;
    check_stmt(c, node->critical.body);
    c->critical_depth--;
    break;
```

**NODE_DEFER:**
```c
case NODE_DEFER:
    check_body_effects(c, node->defer.body, node->loc.line,
        true,  "cannot yield inside defer — corrupts state machine cleanup",
        false, NULL,   // spawn OK in defer (unusual but not unsafe)
        false, NULL);  // alloc OK in defer
    c->defer_depth++;
    check_stmt(c, node->defer.body);
    c->defer_depth--;
    break;
```

**Interrupt handler (in check_func_body):**
```c
if (node->kind == NODE_INTERRUPT) {
    // Scan entire interrupt body for banned effects
    check_body_effects(c, body, node->loc.line,
        false, NULL,   // yield not applicable (not async)
        true,  "cannot spawn inside interrupt handler — pthread_create in ISR is unsafe",
        true,  "cannot allocate inside interrupt handler — heap allocation may deadlock");
    c->in_interrupt = true;
    check_stmt(c, body);
    c->in_interrupt = false;
}
```

### How check_body_effects Works

It creates a temporary FuncProps, scans the body subtree (NOT the whole function —
just the restricted block), then checks the results:

```c
static void check_body_effects(Checker *c, Node *body, int line,
                                bool ban_yield, const char *yield_reason,
                                bool ban_spawn, const char *spawn_reason,
                                bool ban_alloc, const char *alloc_reason) {
    if (!body) return;
    FuncProps props = {0};
    scan_func_props(c, body, &props);

    if (ban_yield && props.can_yield)
        checker_error(c, line, "%s", yield_reason);
    if (ban_spawn && props.can_spawn)
        checker_error(c, line, "%s", spawn_reason);
    if (ban_alloc && props.can_alloc)
        checker_error(c, line, "%s", alloc_reason);
}
```

Note: this scans the BODY subtree, not the whole function. `@critical { helper(); }`
scans the @critical block's body. `scan_func_props` follows `helper()` transitively
via `ensure_func_props`, which caches on `helper`'s Symbol. So the transitive scan
is O(N) total across the program (each function body scanned at most once).

### What Gets Absorbed

**`has_atomic_or_barrier()` (60 lines) → `props.has_sync`**
Currently a standalone recursive scanner called from NODE_SPAWN handler. After this
change, NODE_SPAWN calls `ensure_func_props(c, func_sym)` and reads `func_sym->props.has_sync`.
The standalone function is deleted. Same result, now cached.

**`scan_unsafe_global_access()` partial absorption**
This function does TWO things: (1) detects unsafe global access (bool), and (2) returns
the NAME of the offending global (for error messages). The bool part (`accesses_unsafe_global`)
can be absorbed into FuncProps. The name-returning part needs the existing function.
Partial absorption — the bool is cached, the detailed scan runs only when needed for
the error message.

### What Stays Unchanged

**Context flags for scope-exit control flow:**
`return`, `break`, `continue`, `goto` inside `@critical` or `defer` — these are
NOT effects. They're scope exits that skip cleanup code. The existing flag checks
(`critical_depth > 0`, `defer_depth > 0`) are correct and stay:

```c
case NODE_RETURN:
    if (c->defer_depth > 0) error("return in defer");
    if (c->critical_depth > 0) error("return in @critical");
    // ... rest of return checking
```

These are LOCAL checks — "is THIS statement in a restricted context?" No transitive
analysis needed. A function that calls a function that returns is fine — only a
direct return inside @critical is a problem.

**The split:**
- Scope exits (return/break/continue/goto): flags (local, no transitivity needed)
- Operations (yield/spawn/alloc): function summaries (transitive, cached)

This is a clean separation. No overlap, no redundancy.

## What Changes In Each File

### types.h
- Add `FuncProps` struct definition (8 fields)
- Add `FuncProps props;` field to `Symbol` struct

### checker.c
- Add `ensure_func_props()` — lazy computation with DFS cycle detection (~15 lines)
- Add `scan_func_props()` — recursive AST walker (~100 lines, replaces has_atomic_or_barrier)
- Add `check_body_effects()` — helper for context entry points (~15 lines)
- Modify NODE_CRITICAL handler — add `check_body_effects` call (~3 lines)
- Modify NODE_DEFER handler — add `check_body_effects` call (~3 lines)
- Modify check_func_body for NODE_INTERRUPT — add `check_body_effects` call (~3 lines)
- Modify NODE_SPAWN handler — replace `has_atomic_or_barrier()` call with `props.has_sync` (~5 lines)
- Delete `has_atomic_or_barrier()` function (~60 lines removed)
- Net: ~80 lines added, ~60 lines removed = ~20 net lines

### checker.h
- No changes (FuncProps is on Symbol in types.h, ensure_func_props is static in checker.c)

### Other files
- **emitter.c**: No changes (effects are compile-time only)
- **zercheck.c**: No changes (handle tracking is separate)
- **parser.c**: No changes (no new syntax)
- **lexer.c**: No changes (no new keywords)
- **ast.h**: No changes (no new node types)

## Edge Cases

### cinclude Functions (C code, no body)
`func_node->func_decl.body == NULL`. `ensure_func_props` returns early — all
properties default to false. This is correct: C functions can't yield, spawn,
or use ZER's Slab allocator.

### Forward Declarations
Same as cinclude — no body at declaration time. BUT: `register_decl` updates
`func_node` when the body-bearing definition is encountered. By the time
`ensure_func_props` is called (during body checking, pass 2), the Symbol has
the full body. No issue.

### Recursive Functions
`void a() { a(); }` — `ensure_func_props(a)` sets `in_progress`, scans body,
encounters call to `a`, calls `ensure_func_props(a)` again, sees `in_progress`,
returns. The recursion doesn't add effects that weren't already found in the
body. Conservative and correct.

### Mutual Recursion
`void a() { b(); } void b() { a(); yield; }` — scanning `a`: encounter call
to `b`, scan `b`, find yield (set can_yield), encounter call to `a`, see
`a.in_progress`, return. `b.props.can_yield = true`. Back in `a`'s scan:
merge `b`'s props → `a.props.can_yield = true`. Correct.

### Function Pointers
```zer
void (*fp)() = yielder;
@critical { fp(); }
```
`fp()` is NODE_CALL with NODE_IDENT callee, but callee resolves to TYPE_FUNC_PTR
variable, not a function. `scope_lookup` finds `fp` but `fp->is_function` is false.
The scanner can't follow.

**Mitigation:** Check if fp's init expression is a known function name (same pattern
as `scan_frame`'s indirect call tracking for recursion detection). If `fp = yielder`,
follow `yielder`'s body. This covers the common case.

**Genuinely dynamic cases** (array of function pointers, callbacks from C, computed
dispatch): can't follow at compile time. The scanner returns conservative false
(no effects detected). If the developer puts a yielding function pointer call inside
@critical, it's their responsibility. Same limitation as Rust's `unsafe` blocks.

**Future:** Could add a warning "function pointer call inside @critical — cannot verify
effects" for extra safety. Not needed for initial implementation.

### Async Functions
`async void worker() { yield; }` — scanner finds NODE_YIELD in body,
sets `can_yield = true`. If @critical calls `worker()`, the check catches it.
The `is_async` flag on the function declaration is independent of the effect scan.

### Comptime Functions
`comptime u32 MAX(u32 a, u32 b) { ... }` — comptime functions are pure by
definition. They can't yield, spawn, or allocate. The scanner correctly returns
all false. Additionally, comptime calls are resolved at compile time and don't
generate runtime code, so they're skipped in the transitive follow.

### Nested Restricted Contexts
```zer
@critical {
    defer {
        // This is inside BOTH @critical and defer
        // check_body_effects runs at EACH entry point
    }
}
```
@critical scans its body (including the defer block). defer scans its body.
Both checks run independently. If the inner body yields, BOTH catch it with
different error messages ("yield in @critical" vs "yield in defer"). The first
error is sufficient — the second is redundant but harmless.

### Cross-Module Calls
```zer
// module_a.zer
void yielder() { yield; }

// main.zer
import module_a;
@critical { module_a.yielder(); }
```
`module_a.yielder` is registered in global scope during pass 1 (with mangled name).
NODE_CALL handler rewrites `module_a.yielder()` to use the mangled name. `scope_lookup`
finds the Symbol. `ensure_func_props` follows the body from the imported module's AST.
The cross-module case works identically to same-file calls.

### Module-Qualified Calls
`config.func()` is rewritten to unqualified `func()` with mangled lookup before
reaching the call handler (BUG-416/432 mechanism). By the time the effect scanner
sees the NODE_CALL, it's already rewritten. No special handling needed.

## Testing Strategy

### Positive Tests (must compile + run)

```
tests/zer/func_props_basic.zer        — function with yield called outside @critical (OK)
tests/zer/func_props_nested_ok.zer    — helper without yield called inside @critical (OK)
tests/zer/func_props_alloc_ok.zer     — slab.alloc outside interrupt handler (OK)
tests/zer/func_props_spawn_ok.zer     — spawn outside @critical and interrupt (OK)
```

### Negative Tests (must fail to compile)

Move from `rust_tests/limitations/` to `tests/zer_fail/`:
```
tests/zer_fail/async_critical_yield.zer   — yield inside @critical (direct)
tests/zer_fail/async_spawn_inside.zer     — spawn inside async (direct)
```

New negative tests:
```
tests/zer_fail/critical_yield_transitive.zer  — call yielding function inside @critical
tests/zer_fail/critical_spawn_transitive.zer  — call spawning function inside @critical
tests/zer_fail/defer_yield_direct.zer         — yield inside defer body
tests/zer_fail/defer_yield_transitive.zer     — call yielding function inside defer
tests/zer_fail/isr_spawn_direct.zer           — spawn inside interrupt handler
tests/zer_fail/isr_spawn_transitive.zer       — call spawning function inside interrupt
tests/zer_fail/isr_alloc_transitive.zer       — call allocating function inside interrupt
```

### Interaction Tests

```
tests/zer/func_props_recursive.zer    — recursive function that yields, called outside @critical (OK)
tests/zer/func_props_mutual_rec.zer   — mutual recursion with yield, not in restricted context (OK)
tests/zer/func_props_cross_module.zer — cross-module transitive yield detection
tests/zer/func_props_funcptr.zer      — function pointer with known init, tracked correctly
```

### Regression: has_atomic_or_barrier Replacement

The spawn handler's sync detection must still work after absorbing has_atomic_or_barrier:
```
tests/zer/spawn_with_atomic.zer       — spawn accessing global with @atomic → warning (not error)
tests/zer/spawn_without_sync.zer      — spawn accessing global without sync → error
```
These tests likely already exist. Verify they pass after the change.

## Relationship to Existing Systems

### Systems This Replaces/Absorbs

| System | Current | After |
|---|---|---|
| `has_atomic_or_barrier()` | 60-line standalone scanner, no caching | `props.has_sync`, cached, one walk |
| Direct yield/await flag checks | Missing (BUG-507/508) | `check_body_effects` at entry points |
| Direct spawn ISR check | Missing | `check_body_effects` at interrupt entry |

### Systems This Coexists With

| System | Why it stays |
|---|---|
| `critical_depth` flag | Still needed for return/break/continue/goto in @critical |
| `defer_depth` flag | Still needed for return/break/continue/goto in defer |
| `in_loop` flag | break/continue outside loop — unrelated to effects |
| `in_async` flag | Async state machine emission — emitter needs this |
| `in_interrupt` flag | ISR global tracking, compound assign checks |
| `in_naked` flag | ASM-only validation in naked functions |
| `scan_unsafe_global_access()` | Needs to return global NAME for error message, not just bool |

### Systems This Could Absorb In Future

| System | How | Priority |
|---|---|---|
| `scan_unsafe_global_access()` | `props.accesses_unsafe_global` + separate name-finder | Low — works fine as-is |
| `StackFrame.callees` | FuncProps scanner already walks calls | Low — StackFrame serves stack depth |
| `ProvSummary` | Could be a FuncProps field | Low — different use pattern |

No urgency to absorb these. The framework is extensible — add fields when needed.

## Performance

### Worst Case
Every function in the program is called from a restricted context.
Each function scanned once (cached). Total: O(N) where N = total AST nodes.
Same as a single pass of the type checker.

### Typical Case
Only functions called from @critical/defer/interrupt are scanned transitively.
Most functions never trigger `ensure_func_props`. The lazy approach means zero
overhead for code without restricted contexts.

### vs Current
`has_atomic_or_barrier()` re-scans the SAME function body every time it's called
from a different spawn site. FuncProps caches — first call scans, subsequent calls
are O(1) lookup. Strict improvement.

`scan_unsafe_global_access()` also re-scans. Partial absorption would cache the
bool, only running the detailed scan for the error message. Strict improvement.

## Summary

| What | Details |
|---|---|
| **Problem** | 5 missing checker validations. Flags can't catch transitive cases. |
| **Solution** | Function summaries — inferred properties cached on Symbol. |
| **Model** | Same as GCC IPA / LLVM function attributes. Production-proven. |
| **Why not effects** | ZER sees all bodies. Inference is complete. Annotations redundant. |
| **Why not table-driven** | Tables organize flag checks. Still can't follow function calls. |
| **What changes** | types.h (8-line struct), checker.c (~20 net lines). Nothing else. |
| **What it fixes** | BUG-507, BUG-508, + 3 more. Direct + transitive. |
| **What it absorbs** | `has_atomic_or_barrier()` (60 lines deleted). |
| **What it doesn't touch** | Emitter, zercheck, parser, lexer, AST. |
| **Cycle detection** | Proper DFS (`computed` + `in_progress`). No depth limit hack. |
| **Caching** | Lazy, per-Symbol. Each function scanned at most once. |
| **Testing** | ~4 positive + ~9 negative + ~4 interaction = ~17 new tests. |
| **Framework** | Extensible. New properties = add bool + detection + restriction. |
