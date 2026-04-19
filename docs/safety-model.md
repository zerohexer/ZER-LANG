# ZER Safety Model — Precise Specification

Extracted from the source (checker.c, zercheck.c, emitter.c, ir_lower.c)
and cross-verified against the negative test suite (`tests/zer_fail/`).
Every claim here is backed by an implementation + at least one test.

## Methodology

For each of the 29 tracking systems:

- **CLAIM** — the single-sentence safety property the system enforces.
- **STATES / VALUES** — what the system tracks (if stateful).
- **TRANSITIONS** — events that move between states.
- **MECHANISM** — how the system is implemented at a high level.
- **ERROR TRIGGERS** — the specific conditions that produce a compile error.
- **SOUNDNESS** — why the mechanism actually covers the claim.
- **EDGE CASES / LIMITS** — known places where the system is conservative
  (false positives) or where it defers to runtime / another system.
- **TESTS** — concrete regression tests in `tests/zer_fail/` or elsewhere.

Systems are organized under the 4 safety models (Model 1-4) + Infrastructure.

---

## Infrastructure (5 systems) — not safety per se, but prerequisite plumbing

### System 1 — Typemap

**Location**: `checker.c`, `typemap_set` / `typemap_get` helpers (~42 sites).

**CLAIM**: For every AST node the checker visits, its resolved `Type*`
is stored and retrievable by later passes (emitter, zercheck, IR
lowering) without re-running type inference.

**MECHANISM**: Hash table `(Node* → Type*)` keyed by pointer identity.
Checker writes via `typemap_set`; downstream reads via
`checker_get_type(checker, node)`. Arena-backed so memory lives with
the checker.

**SOUNDNESS**: Pointer identity is preserved — AST nodes are allocated
once and never moved. A node that was type-checked has its type
recorded. Re-reading always returns the same result for the same
node.

**EDGE CASES**: Synthesized AST nodes (inserted by IR lowering) must
explicitly call `checker_set_type(c, node, type)` if later code will
consume the node via `checker_get_type`. Missing this results in
`ty_i32` fallback and type confusion in the emitter. See BUG-573,
BUG-574 for historical cases.

**TESTS**: All positive test coverage — any test that compiles
exercises the typemap.

---

### System 2 — Type ID

**Location**: `checker.c`, `next_type_id++` assignments (6 sites).

**CLAIM**: Every user-defined struct / enum / union gets a unique
32-bit identifier used for runtime `*opaque` provenance checks.

**MECHANISM**: `Checker.next_type_id` counter. Assigned at struct /
enum / union declaration time. Emitted into the `_zer_opaque`
runtime wrapper so `*opaque` values carry their origin type at
runtime.

**SOUNDNESS**: Each type declaration runs exactly once in the
checker. Uniqueness is monotonic (increments). Cross-module: each
module's types have disjoint IDs.

**EDGE CASES**: `cinclude`'d types and extern pointers have type_id
= 0 (unknown). `@ptrcast` skips the runtime check when type_id = 0,
otherwise the cast cost is 2 comparisons + branch.

**TESTS**: `tests/zer/opaque_level2.zer`, `opaque_level345.zer`,
any test using `@ptrcast` through `*opaque`.

---

### System 23 — Defer Stack

**Location**: `emitter.c`, `Emitter.defer_stack`.

**CLAIM**: The emitter tracks pending defer blocks at compile time
so that `return` / `break` / `continue` / `goto` / `orelse return`
paths fire all still-pending defers in LIFO order.

**MECHANISM**: `DeferStack` array on `Emitter`. `emit_defers_from(e,
base)` fires defers from current count down to `base`. The IR path
uses `IR_DEFER_PUSH` / `IR_DEFER_FIRE` instructions that carry a
`base` index so scoped fires work correctly across
loops/if-branches/switch-arms.

**SOUNDNESS**: Every early-exit path calls into the scoped-fire
mechanism. Loops use POP_ONLY to keep defers on the emit-time stack
for break/continue paths that emit earlier blocks. NODE_BLOCK
uses the same POP_ONLY trick so nested block defers are seen by
the enclosing construct's early-exit paths (BUG-590 fix).

**EDGE CASES**:
- `defer` inside `defer` body is parse-rejected.
- `return` / `break` / `continue` / `goto` inside `defer` body is
  rejected by checker (return_in_defer test).
- `yield` / `await` inside `defer` body is rejected (defer_yield_direct).

**TESTS**: `tests/zer/defer_cleanup.zer`, `defer_free.zer`,
`defer_scoped_blocks.zer`, `defer_multi_free.zer`, `defer_return_order.zer`,
`super_defer_complex.zer`, plus 20+ firmware examples.

---

### System 25 — Container Templates

**Location**: `checker.c`, `ContainerTemplate` + `ContainerInstance`
(~9 references).

**CLAIM**: The `container Name(T) { ... }` declaration creates a type
template. Each call site `Name(ConcreteT)` stamps a unique concrete
struct with `T` substituted in all field types, function parameter
types, and return types.

**MECHANISM**: Templates stored per-checker. On encountering
`Name(ConcreteT)`, the checker looks up cached instances; if not
found, substitutes `T` through the template's field types and
adds a new struct to the type map.

**SOUNDNESS**: Substitution is syntactic. Each concrete instance
is a real struct with fully resolved types. Recursion depth is
bounded at 32 to prevent compiler hang on pathological templates.

**EDGE CASES**:
- Self-referential `container Node(T) { ?*Node(T) next; }` works
  because the `?*Node(T)` is a POINTER (not inlined).
- Attempted by-value self-reference is detected and rejected.

**TESTS**: `tests/zer/container_stack.zer`,
`zig_tests/zt_container_*.zer` (12 tests).

---

### System 26 — Comptime Evaluator

**Location**: `checker.c`, `ComptimeCtx` + `eval_comptime_block`
(~11 references).

**CLAIM**: `comptime` functions are type-checked, evaluated at
compile time with bounded recursion, and inlined into the
emitted C as constants.

**MECHANISM**: `ComptimeCtx` holds local bindings (name → int64
value), array bindings, loop instruction counter. Depth bounded at
32 recursions + global budget of 1M operations. Float return type
dispatches to `eval_comptime_float_expr` (BUG-593 fix).

**SOUNDNESS**: Termination guaranteed by global budget. Each call
site re-evaluates with its own ComptimeCtx so context is isolated.
Only expressions reducible to literal values are accepted.

**EDGE CASES**:
- Non-comptime arguments rejected at call site.
- Float return type: dispatches differently to avoid integer
  overflow on bit-punned float values (BUG-593).

**TESTS**: `tests/zer/comptime_eval.zer`, `comptime_const_arg.zer`,
`comptime_nested_call.zer`, `comptime_array.zer`,
`comptime_struct_ret.zer`, `rust_tests/gen_comptime_float_001.zer`.

---

## Model 1 — State Machines (4 systems)

State machines track entity lifecycle. Valid operations move
between states; invalid operations from a state are compile errors.

### System 7 — Handle States

**Location**: `zercheck.c`, `HandleInfo.state`.

**CLAIM**: For every value of type `Handle(T)`, `?Handle(T)`, `*T`
(when derived from alloc_ptr), or `?*T` (same), the compiler
rejects programs that use the value after it has been freed, free
it a second time, or let it be alive at function exit without
being escaped or freed.

**STATES**:
- `HS_UNKNOWN` — variable declared, not yet tracked.
- `HS_ALIVE` — allocated, valid to use.
- `HS_FREED` — freed, any use is a compile error.
- `HS_MAYBE_FREED` — freed on some control-flow paths, alive on others. Use is still a compile error (conservative).
- `HS_TRANSFERRED` — ownership transferred to another thread via `spawn`, or to a function via `move struct` semantics. Use after transfer is a compile error.

**TRANSITIONS**:
- `pool.alloc()` / `slab.alloc()` / `arena.alloc()` / `Task.alloc()` / malloc-family / alloc_ptr: `UNKNOWN → ALIVE`
- `pool.free(h)` / `slab.free(h)` / `Task.free(h)` / free-family: `ALIVE | MAYBE_FREED → FREED`
- `spawn fn(&h)` with non-shared pointer: `ALIVE → TRANSFERRED`
- Branch merge with mixed states on two paths: `ALIVE + FREED → MAYBE_FREED`
- Leaving function scope without transfer/free/escape while `ALIVE`: leak error.
- Reassignment via `h = new_alloc`: old handle becomes orphaned and may trigger leak if not freed; new handle becomes `ALIVE`.

**ERROR TRIGGERS**:
- `use(h)` when `h.state ∈ {FREED, MAYBE_FREED, TRANSFERRED}`
- `free(h)` when `h.state ∈ {FREED, MAYBE_FREED}`
- Function exit when any local handle's `state = ALIVE` and not escaped, transferred, or stored in param-with-`keep` field.

**MECHANISM**: Linear dataflow with path merging at branch points.
`zc_check_stmt` walks the AST. Branches fork into multiple
`PathState` instances; merges union them (intersection for ALIVE
on all paths, MAYBE_FREED otherwise). Loop bodies re-analyzed
until fixed point (ceiling 32 iterations).

**EDGE CASES**:
- Same-block backward `goto` across a free: handled via 2-pass
  re-walk (BUG-404).
- **Cross-block backward goto** (free then `goto` back to a label
  before a use at a different block): not tracked; runtime gen
  check traps with `ZER TRAP: slab: use-after-free or invalid
  handle`. **Verified 2026-04-19**: compile accepts the program;
  runtime exit 133 (SIGTRAP). Test reproducer exists.
- Variable shadowing across blocks: handled via `scope_depth`
  + `find_handle_local` (BUG-488 fix).

**DEFENSE IN DEPTH**: `Handle(T)` is `u64` (index + generation
counter). `slab.get(h)` and `pool.get(h)` check generation; if
mismatched, trap at runtime. This catches static-analysis gaps.

**TESTS**: `uaf_handle.zer`, `double_free.zer`, `maybe_freed.zer`,
`goto_backward_uaf.zer`, `goto_maybe_freed_branch.zer`,
`task_free_double.zer`, `task_free_uaf.zer`, plus alloc_ptr
variants.

---

### System 6 — Alloc Coloring (source_color)

**Location**: `zercheck.c`, `HandleInfo.source_color`.

**CLAIM**: Every allocation is tagged with its memory origin:
Pool, Slab, Arena, malloc-family, or Unknown. The correct matching
free function is required; mismatches are compile errors.

**VALUES**: `ZC_COLOR_UNKNOWN | POOL | ARENA | MALLOC`.

**MECHANISM**: At alloc site, record the allocator's color on the
HandleInfo. At free site, check that the free function matches
the color (e.g., `free(arena_ptr)` is rejected because the color
is ARENA, not MALLOC).

**SOUNDNESS**: Allocator registration is source-code explicit —
checker knows which method names on which types (Pool, Slab, Arena)
produce which colors. Malloc-family and user-wrapped colors
identified via FuncSummary (system 9) and `ZC_COLOR_UNKNOWN`
escape hatch.

**EDGE CASES**:
- `*opaque` from `cinclude` is `ZC_COLOR_UNKNOWN` — caller must
  remember which allocator produced it.
- Arena-derived pointer to `arena.alloc(T)` cannot be stored in
  global (arena may reset).

**TESTS**: `arena_global_escape.zer`, `free_ptr_wrong_type.zer`.

---

### System 8 — Alloc ID

**Location**: `zercheck.c`, `HandleInfo.alloc_id`, `ZerCheck.next_alloc_id`.

**CLAIM**: Every allocation receives a unique integer ID. Aliases
(same allocation through different variables / interior pointers)
share the same ID. Freeing any alias marks all aliases FREED.

**MECHANISM**: Counter `next_alloc_id` increments on each alloc.
Assignment `b = a` propagates `a.alloc_id` to `b`. Interior pointer
`p = &b.field` inherits `b.alloc_id`. When any alias's `alloc_id`
is freed, a scan marks all HandleInfos with the matching
`alloc_id` as FREED.

**SOUNDNESS**: Unique IDs ensure no two distinct allocations share
an ID. Aliases always share — so freeing-via-alias is equivalent to
freeing the original for tracking purposes.

**EDGE CASES**: Conservative when alias source is unclear (e.g.,
passed through a function without a FuncSummary). Runtime
generation counter provides fallback.

**TESTS**: `interior_ptr_uaf.zer`, `interior_ptr_func.zer`,
`dyn_array_loop_freed.zer`, `goto_circular_swap_uaf.zer`.

---

### System 10 — Move Tracking

**Location**: `zercheck.c`, `HS_TRANSFERRED` handling.

**CLAIM**: Values of `move struct` types, or of `ThreadHandle`, or
passed to `spawn` without shared/thread wrappers, may not be used
after ownership has been transferred.

**MECHANISM**: Assignment or function-call with move-struct source
transitions the source to `HS_TRANSFERRED`. Subsequent use of
the source variable triggers a compile error. `spawn fn(&var)`
with non-shared pointer also transfers.

**SOUNDNESS**: `move struct` types are tagged at declaration
(System 20). `should_track_move(type)` recursively tests if a type
is or contains a move struct. Every write / read of such a type
is audited against the TRANSFERRED state.

**EDGE CASES**:
- Partial moves (field-level) not supported — move is whole-value.
- If-unwrap capture of move-struct optional must be pointer
  capture `|*k|`, not value capture `|k|` (latter would copy and
  leave original valid — rejected).
- Verified 2026-04-19: pass-by-value to a function correctly
  transitions the source variable to HS_TRANSFERRED. Subsequent
  use at the call site is a compile error. The previously-documented
  "return value is not a transfer" limit was vacuous — after
  `return`, function scope ends, nothing to catch.

**TESTS**: `move_array_elem.zer`, `move_switch_capture.zer`,
`move_orelse_fallback.zer`, `union_move_overwrite.zer`,
`distinct_resource_assign.zer`, `spawn_nonshared_ptr.zer`.

---

## Model 2 — Program Point Properties (7 systems)

Properties attached to values at specific program points. Updates
at each statement / assignment / branch. Invalidation on events
like "address taken."

### System 3 — Provenance (point-level)

**Location**: `checker.c`, `Symbol.provenance_type`.

**CLAIM**: For every `*opaque` value in ZER code, the type of the
original pointer is tracked at compile time. Casting `*opaque`
back to a wrong type is a compile error.

**MECHANISM**: `Symbol.provenance_type = Type*` records the type
the `*opaque` was derived from. `@ptrcast(*T, opaque)` checks that
`T` matches the recorded provenance. On assignment
`opaque2 = opaque1`, provenance propagates. On re-derivation
(fresh `@ptrcast` from a non-opaque source), provenance is
updated.

**SOUNDNESS**: All `*opaque` values in ZER source have an
origin — either they came from a typed pointer cast to `*opaque`,
or they came from outside ZER (extern / cinclude) and have
provenance = NULL / unknown. The unknown case is allowed (cast
succeeds at compile time, runs runtime check via type_id).

**EDGE CASES**:
- Extern / cinclude sources have no provenance; cast is accepted
  statically with runtime type_id check.
- Re-derivation through `@ptrcast` clears provenance and sets new.

**TESTS**: `typecast_provenance.zer`, `typecast_direct_ptr.zer`,
`opaque_alias_uaf.zer`.

---

### System 11 — Escape Flags

**Location**: `checker.c`, `Symbol.is_local_derived`,
`Symbol.is_arena_derived`, `Symbol.is_from_arena`.

**CLAIM**: Pointers derived from stack locals cannot be returned,
stored in globals, or stored through non-keep parameters.
Arena-derived pointers cannot be stored in globals (arena may be
reset).

**STATES PER SYMBOL**:
- `is_local_derived` — pointer points into a stack-allocated
  variable.
- `is_arena_derived` — pointer came from `arena.alloc(T)`.
- `is_from_arena` — variable directly holds an arena-derived value.

**TRANSITIONS**:
- `&local_var` → `is_local_derived = true`
- `arena.alloc(T)` result → `is_arena_derived = true`
- `q = p` where `p.is_*_derived = true` → `q` inherits flag
- `p = &global_var` → all escape flags clear

**ERROR TRIGGERS**:
- `return p` where `p.is_local_derived`
- `global = p` where `p.is_local_derived` or `p.is_arena_derived`
- Pass as non-`keep` pointer argument when callee stores it

**MECHANISM**: Walk through NODE_FIELD / NODE_INDEX / NODE_UNARY
(deref) chains to the root NODE_IDENT; check the root's flags.
Propagation through aliases.

**SOUNDNESS**: Conservative. Any path from a local variable to an
escape site is caught. False positives possible when alias is
actually safe (programmer uses explicit `keep`).

**EDGE CASES**:
- `@ptrtoint(&local)` + later `@inttoptr(...)` caught as
  "direct and indirect" escape.
- Array-to-slice coercion at call sites check escape.

**TESTS**: `dangling_return.zer`, `nonkeep_store_global.zer`,
`arena_global_escape.zer`, `arena_value_global_escape.zer`.

---

### System 12 — Range Propagation (VRP)

**Location**: `checker.c`, `VarRange` struct + `find_var_range`
+ `derive_expr_range`.

**CLAIM**: The checker propagates compile-time known ranges of
integer variables through statements, branches, and function
returns. When a range proves an array access is in-bounds or a
divisor is nonzero, the corresponding bounds check / division
guard is elided; otherwise a bounds check / guard is required.

**VALUES**: `VarRange { int64_t min_val, max_val; bool known_nonzero; }`

**TRANSITIONS**:
- Literal init: range is `[value, value]`.
- `x = y + 1`: new range derived from y's range + 1.
- `x % N` where N is literal: range `[0, N-1]`.
- `x & MASK` where MASK is literal: range `[0, MASK]`.
- Function call: if function has `Return Range` (System 13), propagate.
- Assignment / compound assignment invalidates existing range
  unless the RHS is derivable.
- `&var` taken: range invalidated (pointer alias may change value).

**ERROR TRIGGERS** — not an error directly, but:
- Array index unprovable + no auto-guard opportunity → "array index not proven in bounds" warning/error.
- Division / modulo by variable not proven nonzero → forced guard error requiring explicit check.

**MECHANISM**: `VarRange` stored in range stack (scope-aware).
Forward dataflow through statements. Merge at branches (union min,
union max). Loop bodies re-analyzed.

**SOUNDNESS**: Conservative — any uncertainty widens to
`[INT64_MIN, INT64_MAX]`. Proven-safe accesses must be witnessed
by a chain of derivations from constants.

**EDGE CASES**:
- `address_taken` on `&var` invalidates range conservatively
  (BUG-475).
- Struct field range propagation works on proven field-specific keys.
- Compound assignments (`+=`) invalidate then re-derive.

**TESTS**: `bounds_oob.zer`, `div_zero.zer`,
`no-autoguard-all-proven.zer`, `no-autoguard-deep-chain.zer`,
`inline_call_range.zer`, `guard_clamp_range.zer`.

---

### System 14 — Auto-Guard

**Location**: `checker.c`, `checker_auto_guard_size()` +
`mark_auto_guard()`. Consumed by emitter.

**CLAIM**: Array indices that VRP can't prove safe and that aren't
provably unsafe are marked for runtime bounds-check. The emitter
inserts an `if (idx >= size) return zero_value()` auto-guard.

**MECHANISM**: When VRP (System 12) can't prove in-bounds for an
index expression, and the check would be safe to elide via
auto-guard (not in an infinite loop, etc.), the node is tagged
with `auto_guard_size`. Emitter emits the guard before the
indexed access.

**SOUNDNESS**: Guards always fail safely (return zero or trap).
The check itself is a simple `if`, no complex reasoning required.

**EDGE CASES**:
- Loops that don't terminate don't emit guards (VRP forces
  explicit bounds in loops).
- @cstr overflow uses the same pattern (System 14 + auto-guard).

**TESTS**: `dyn_array_guard.zer`, `autoguard-warning-emitted.zer`.

---

### System 15 — Dynamic Freed

**Location**: `checker.c`, `track_dyn_freed_index`.

**CLAIM**: When `pool.free(arr[k])` is detected in a loop, the
checker tracks which index was freed. A subsequent
`pool.free(arr[j])` where `j` cannot be proven different from `k`
is a UAF error.

**MECHANISM**: Track the indexed alloc site across the loop
body. Check that subsequent accesses at the same index through
any path after free use fresh allocations.

**SOUNDNESS**: Conservative — only catches the specific pattern
of freeing + reusing an array slot. More general patterns fall
back to `alloc_id` matching (System 8).

**EDGE CASES**:
- Verified 2026-04-19: simple 2D arrays of handles (e.g.,
  `Handle(Task)[2][2]`) correctly track UAF via alloc_id
  sharing (System 8). The "multi-dimensional not covered"
  claim was wrong for the simple case.
- Index expression complexity limits what can be tracked —
  when the index is a non-trivial runtime value and the
  allocation stored there is freed through the same expression,
  the exact match isn't always proven. Runtime generation check
  is the fallback.

**TESTS**: `dyn_array_loop_freed.zer`.

---

### System 22 — Union Switch Lock

**Location**: `checker.c`, `Checker.union_switch_var`.

**CLAIM**: While a `switch` is capturing a union variant via
`|v|` pattern binding, the parent union cannot be mutated.
Assignments to union fields inside the switch body are rejected.

**MECHANISM**: Entering a switch with capture sets
`c->union_switch_var = parent_ident`. Inside the body, checker
rejects assignments whose target's root is `union_switch_var`.
Leaving the switch clears.

**SOUNDNESS**: Captures are references into the union's current
variant. Mutating the union would write through a pointer whose
type no longer matches the capture.

**EDGE CASES**:
- Nested switches need separate tracking (handled).
- Pointer-type aliases also caught via walk through deref / field /
  index chains.

**TESTS**: `union_move_overwrite.zer`, union tests in
`tests/zer/union_array_variant.zer`.

---

### System 24 — Context Flags

**Location**: `checker.c`, `Checker.in_loop`,
`Checker.in_async`, `Checker.in_interrupt`, `Checker.in_naked`,
`Checker.defer_depth`, `Checker.critical_depth`.

**CLAIM**: Control-flow constructs and contextual ban rules are
enforced by tracking which syntactic context each statement is
inside.

**VALUES**:
- `in_loop` — `break`/`continue` legal only when set.
- `in_async` — `yield`/`await` legal only when set.
- `in_interrupt` — heap allocation banned inside.
- `in_naked` — only asm / return allowed.
- `defer_depth` — > 0 inside defer body, bans control flow + yield.
- `critical_depth` — > 0 inside @critical, bans control flow + yield.

**ERROR TRIGGERS** (direct):
- `break` / `continue` when `!in_loop`
- `yield` / `await` when `!in_async`
- `return` / `break` / `continue` / `goto` when `defer_depth > 0`
- Same inside `critical_depth > 0` (would skip interrupt-re-enable).
- `slab.alloc()` / `Task.alloc()` when `in_interrupt = true`
- Non-asm, non-return statement in `in_naked` function.

**MECHANISM**: Incremented / decremented at syntactic boundaries
by `check_stmt`. Scope-exit resets.

**SOUNDNESS**: Syntactic traversal is exhaustive and deterministic.
Every construct that sets context also clears on exit.

**EDGE CASES**:
- Transitive calls (non-inlined function that yields) require
  `FuncProps` (System 29) for `can_yield` propagation.

**TESTS**: `critical_break.zer`, `critical_return.zer`,
`critical_yield_transitive.zer`, `critical_spawn_transitive.zer`,
`defer_yield_direct.zer`, `defer_yield_transitive.zer`,
`return_in_defer.zer`, `async_critical_yield.zer`,
`isr_slab_alloc.zer`, `asm_not_naked.zer`.

---

## Model 3 — Function Summaries (9 systems)

Per-function computed properties. Evaluated on-demand (with DFS
cycle detection) and cached. Callers consult summaries without
re-analyzing callee bodies.

### System 29 — FuncProps

**Location**: `checker.c`, `Symbol.props` +
`ensure_func_props()`.

**CLAIM**: For every function, four context-safety properties are
computed transitively: `can_yield`, `can_spawn`, `can_alloc`,
`has_sync`. Callers use these to enforce context bans without
inlining.

**MECHANISM**: Lazy DFS. On first need, `ensure_func_props(sym)`
walks the function body with `scan_func_props`. Directly detects
yield/spawn/alloc/sync constructs. Recursively resolves callees
via `scope_lookup(global_scope)`. Cycle detection prevents infinite
recursion.

**SOUNDNESS**: Transitive closure captures all indirect effects.
Any function reachable from f that yields → f `can_yield`. Same
for spawn, alloc, sync.

**EDGE CASES**:
- Function pointer calls: conservative — treat as `can_yield =
  can_spawn = can_alloc = true` unless target is resolved.
- Cross-module: imported functions resolved via import AST list.
- Unknown functions (prototype only, from cinclude): assumed safe
  (no yield/spawn/alloc/sync).

**TESTS**: `critical_yield_transitive.zer`,
`critical_spawn_transitive.zer`, `defer_yield_transitive.zer`.

---

### System 9 — Func Summaries (zercheck)

**Location**: `zercheck.c`, `FuncSummary`.

**CLAIM**: For each function, record which `Handle(T)` / `*T`
parameters it frees (definitely on all paths) and which it may
free (some paths). Callers update handle states at call sites
using the summary.

**FIELDS** (per function):
- `frees_param[i]` — callee definitely frees param `i` on all paths.
- `maybe_frees_param[i]` — callee may free on some paths.
- `returns_color` — `ZC_COLOR_*` of the returned value.
- `returns_param_color` — which param's color the return
  inherits (e.g., arena wrappers that return inputs).

**MECHANISM**: Two-pass analysis. Pass 1 (`building_summary = true`)
scans all functions, computing summaries without reporting errors.
Pass 2 uses the summaries during full checking.

**SOUNDNESS**: Call-site effect equivalent to inlining the callee.
Branch merges update states correctly.

**EDGE CASES**:
- Indirect / virtual calls: no summary available.
- Recursive functions: DFS with memoization breaks cycles.
- Mutual recursion: FIXED 2026-04-19. Previously, FuncSummary
  pinned each summary after first build and never refined it —
  so A's summary (computed without knowing B's free behavior)
  stayed wrong. Fixed via fixed-point iteration: `zc_build_summary`
  now refines existing summaries, returns `changed`, and the
  outer loop iterates until values stabilize. Test:
  `tests/zer_fail/mutual_recursion_uaf.zer`.

**TESTS**: `cross_func_free_ptr.zer`,
`opaque_cross_func_uaf.zer`, `opaque_return_freed.zer`.

---

### System 4 — Prov Summaries

**Location**: `checker.c`, per-function return provenance.

**CLAIM**: For each function returning `*opaque`, the underlying
type that the returned pointer was derived from is recorded.
Call sites use it to establish provenance on the return value.

**MECHANISM**: Scan function body for return statements. If all
`return expr;` paths return `*opaque` with the same underlying
provenance, record on the function symbol.

**SOUNDNESS**: Must match across all paths — conservative (single
path with different provenance clears the summary).

**TESTS**: `opaque_return_freed.zer`, `opaque_cross_func_uaf.zer`.

---

### System 5 — Param Provenance

**Location**: `checker.c`, per-parameter expected provenance.

**CLAIM**: Functions may declare that their `*opaque` params
expect a specific underlying type. Call sites are checked against
this expectation.

**MECHANISM**: Inferred from function body: what types do
`@ptrcast(*T, param)` casts assume? If all uses assume the same
`T`, the param's expected provenance is `T`. Call-site passes
must satisfy.

**SOUNDNESS**: Conservative when uses are inconsistent (e.g., param
cast to different types) — expected provenance cleared.

**TESTS**: `opaque_alias_uaf.zer`,
`opaque_task_free_ptr_uaf.zer`.

---

### System 13 — Return Range

**Location**: `checker.c`, `Symbol.return_range_min` +
`return_range_max`.

**CLAIM**: Functions whose return value is always in a derivable
range (`h % N`, `h & MASK`, literal, or call-chain of these) have
the range recorded. Call sites use it as VRP input.

**MECHANISM**: Scan function body for all `return expr;`. If every
return's expression yields a derivable range, intersect and
record. Called transitively for chained function calls.

**EDGE CASES**:
- Functions with no return range: default to `[INT64_MIN,
  INT64_MAX]` at call site.
- Verified 2026-04-19: mutual recursion IS handled correctly
  when the return range is bounded via `% N` / `& MASK`.
  Tested `fa(x) { return fb(x-1) % MAP; }` / `fb(x) { return
  fa(x-1) % MAP; }` with MAP=16 — `arr[fa(10)]` on a size-16
  array elided the bounds check. Same pattern on a size-8
  array correctly inserted auto-guard. The "mutual recursion
  clears the range" claim was wrong.

**TESTS**: `inline_call_range.zer`, `inline_range_deep.zer`.

---

### System 18 — Stack Frames

**Location**: `checker.c`, `scan_frame()`.

**CLAIM**: Per-function estimated frame size (locals + params).
Recursion detection (via DFS on call graph). If `--stack-limit N`
flag is set, callgraph chains whose cumulative frame exceeds `N`
bytes are rejected at compile time.

**MECHANISM**: Walk function body, sum `sizeof(T)` for each local
and param. Call graph built during checker pass. Recursion detected
via strongly-connected components — forms a cycle, emit warning.

**EDGE CASES**:
- Indirect calls via function pointer are assumed worst-case (no
  limit).
- Varadic / inline-asm frame sizes are estimates.

**TESTS**: `recursive_functions.zer` (warns but allows).

---

### System 27 — Spawn Global Scan

**Location**: `checker.c`, scan of spawn-target body for unsafe
global access.

**CLAIM**: When a function is the target of `spawn`, its body is
scanned for access to non-shared global variables. Accessing a
non-shared global without `@atomic_*` or `@barrier` protection is
a compile error (data race).

**MECHANISM**: Pre-scan body recursively (8 levels of callee).
Any `load` / `store` / `compound-assign` on a global that isn't
`shared struct`, `threadlocal`, or `@atomic` is flagged. Warn if
protected, error if not.

**EDGE CASES**:
- Reads of `const` globals are OK.
- Function pointer targets are conservatively flagged.

**TESTS**: `spawn_const_bypass.zer`, `spawn_string_mutable.zer`.

---

### System 28 — Shared Type Collect

**Location**: `checker.c`, per-statement shared type tracker.

**CLAIM**: Within a single statement, accessing two or more
DIFFERENT shared struct types is rejected (would force acquiring
two locks simultaneously → deadlock risk). Call-graph DFS
through callees detects mutual-recursion deadlocks.

**MECHANISM**: Collect all shared struct types touched by a
statement. If |types| > 1, emit error. Cross-function: build
a static lock-order graph; cycles in the order-graph = deadlock.

**SOUNDNESS**: One-lock-per-statement guarantees no nested locking
within a statement. Lock-order graph analysis handles call-graph
depth via DFS with cycle detection.

**EDGE CASES**:
- Recursive calls with different lock orders detected.
- Single-type access (e.g., multiple fields of same shared
  struct) is fine — all same lock.

**TESTS**: `deadlock_depth5.zer`, `deadlock_depth20.zer`,
`deadlock_mutual_recursion.zer`.

---

### System 17 — ISR Tracking

**Location**: `checker.c`, ISR global access tracking.

**CLAIM**: Global variables accessed inside `interrupt` handlers
AND also accessed outside them, without being declared `volatile`,
are flagged as ISR data races.

**MECHANISM**: Two-pass. Pass 1 collects the set of globals
touched by ISR handlers. Pass 2 when seeing a non-ISR access,
checks if also in ISR set — warn/error if not `volatile`.
Compound-assign (`g += x`) on shared-with-ISR global = error
(non-atomic read-modify-write).

**EDGE CASES**:
- `threadlocal` globals OK (per-thread storage).
- `shared struct` globals OK (locking).
- `@atomic_*` access OK.

**TESTS**: `isr_slab_alloc.zer`.

---

## Model 4 — Static Annotations (4 systems)

Declaration-level constraints. Set once, checked at every use site.
Not dataflow — purely lexical.

### System 19 — MMIO Ranges

**Location**: `checker.c`, mmio_range array on `Checker`.

**CLAIM**: `@inttoptr(*T, addr)` is legal only if `addr` falls
within a declared `mmio 0xLOW..0xHIGH;` range, or if `--no-strict-mmio`
is set. Constant `addr`s are checked at compile time; variable
`addr`s get a runtime range check.

**MECHANISM**: `mmio 0x40020000..0x40020FFF;` declarations
populate the range array. `@inttoptr` checks:
1. Constant `addr` ∈ any range → OK.
2. Constant `addr` ∉ any range → compile error.
3. Variable `addr` → emit runtime range check + trap on mismatch.

Also enforces alignment: `@inttoptr(*u32, 0x...)` requires addr %
4 == 0.

**EDGE CASES**:
- `--no-strict-mmio` bypasses the range requirement (for cinclude
  compatibility).

**TESTS**: `rt_unsafe_mmio_*_oob.zer` (negative),
`rust_tests/qemu/rt_unsafe_mmio_*.zer` (positive).

---

### System 20 — Qualifier Tracking

**Location**: `checker.c` + `types.c`, `is_volatile` + `is_const`
on `Type` and `Symbol`.

**CLAIM**: `volatile` and `const` qualifiers cannot be stripped by
any cast (`@ptrcast`, `@bitcast`, `@cast`, C-style typecast).
Casts between `volatile T*` and `T*` require the volatile to be
preserved.

**MECHANISM**: `Type->is_volatile` + `->is_const` bits. Cast
operations compare both types' qualifier bits. Stripping is
rejected at compile time.

**EDGE CASES**:
- Adding qualifiers is allowed (promoting `T*` to `const T*` OK).
- `shared struct` declarations imply volatile semantics for
  atomics within the struct's methods.

**TESTS**: `typecast_volatile_strip.zer`, `typecast_const_strip.zer`,
`ptrcast_strip_volatile.zer`.

---

### System 21 — Keep Parameters

**Location**: `checker.c`, `is_keep` on `ParamDecl`.

**CLAIM**: A pointer parameter declared `keep` may be stored in
globals, static variables, or param-of-param fields. Without
`keep`, such storage is a compile error. Callers passing a local
address to a `keep` param must either declare the local `static`,
or pass a global, or pass an already-escaping-safe pointer.

**MECHANISM**: Parsed at declaration. Callsite checks: `global =
p` where `p` is a non-`keep` parameter → error. Caller-side check:
`&local_var` passed to `keep` param → error (stack memory freed on
return).

**SOUNDNESS**: `keep` is an explicit contract. Both sides (callee
storage + caller provenance) are checked. Invisible to mutator
semantics — pure lifetime annotation.

**EDGE CASES**:
- Function pointer calls auto-treat all pointer params as `keep`
  (can't verify callee's behavior).
- Array-to-slice coercion preserves `keep`.

**TESTS**: `nonkeep_store_global.zer`, `keep_store_global.zer`.

---

### System 16 — Non-Storable

**Location**: `checker.c`, `is_non_storable` marker +
`mark_non_storable()`.

**CLAIM**: Results of `pool.get(h)` / `slab.get(h)` are pointers
whose lifetime may end on any allocator mutation. They cannot be
stored in variables — only used inline.

**MECHANISM**: `pool.get()` call site marked `is_non_storable`.
`check_var_decl` / `check_assign` / call-arg / return all check:
if RHS is non-storable and result is pointer/slice/struct/union →
compile error.

**SOUNDNESS**: Handle-based access (h.field) is always safe because
the auto-deref emits `slab.get(h).field` at each use. Storing the
pointer would cache a pointer that may dangle on `free()`.

**EDGE CASES**:
- Scalar field values ARE storable — only pointer/slice/struct/union
  results blocked.
- `alloc_ptr` / `free_ptr` path doesn't use this — it has its own
  tracking via System 7 (Handle States).

**TESTS**: `handle_no_allocator.zer`, checker-level tests in
`test_checker_full.c`.

---

## Appendix A — Safety Guarantees → Systems Mapping

The 15 end-user safety claims, composed from the 29 systems above:

| # | Guarantee | Composed from |
|---|---|---|
| 1 | No buffer overflow | System 12 (Range), System 14 (Auto-Guard) + runtime `_zer_bounds_check` |
| 2 | No use-after-free | System 7 (Handle States), System 9 (Func Summaries), System 8 (Alloc ID), System 11 (Escape Flags), System 3 (Provenance) + runtime gen check |
| 3 | No double-free | System 7, System 9, System 6 (Alloc Coloring) |
| 4 | No memory leak | System 7 (ALIVE at function exit = error), System 8, System 10 (Move Tracking) |
| 5 | No null dereference | System 20 (`*T` type non-null), Type system (`?T` requires unwrap) |
| 6 | No uninit read | Emitter rule: auto-zero (no tracking needed, unconditional) |
| 7 | No integer UB | `-fwrapv` compile flag + no shift-by-≥width (ternary emission) |
| 8 | No data race | System 28 (Shared Type Collect), System 27 (Spawn Global Scan), System 29 (FuncProps `has_sync`), `shared struct` annotation |
| 9 | No deadlock | System 28 (single-statement / call-graph cycle detection) |
| 10 | No move misuse | System 10 (Move Tracking), System 7 (HS_TRANSFERRED) |
| 11 | No wrong cast | System 3 (Provenance), System 20 (Qualifier Tracking), System 4, System 5 (prov summaries) |
| 12 | No MMIO violation | System 19 (MMIO Ranges) + alignment check + System 14 (bounds for mmio indexing) |
| 13 | No ISR data race | System 17 (ISR Tracking), System 24 (Context Flags `in_interrupt`), System 29 (FuncProps `can_alloc`) |
| 14 | No context violation | System 24 (Context Flags), System 29 (FuncProps) — transitive for yield/spawn/alloc/critical |
| 15 | No stack overflow (with flag) | System 18 (Stack Frames) |

Runtime fallbacks (defense in depth): generation counters on
Handles catch rare static-analysis misses; `_zer_check_alive`
runtime for `*opaque`; fault signal handler catches bad MMIO at
boot via `@probe`.

---

## Appendix B — Non-Guarantees (explicit disclaimers)

ZER's model does NOT guarantee:

1. **Correctness of cinclude'd C code.** Memory safety stops at
   the FFI boundary. Use `*opaque` + `--wrap=malloc` runtime for
   boundary protection.
2. **Correctness of handwritten asm blocks.** Extended asm is
   treated as opaque. Programmer asserts the invariants.
3. **Absence of livelock / priority inversion.** Only deadlock
   (mutual lock-waiting) is detected.
4. **Strict absence of false positives.** Conservative analysis
   sometimes rejects safe programs. Workarounds: `keep`, explicit
   bounds, `@truncate`, etc.
5. **Hardware fault safety.** Bad MMIO alignment, I/O timeouts,
   bit flips — not modeled. Runtime traps in some cases.

---

## Methodology of this specification

- Every system section was extracted from the source files
  listed in **Location**.
- Every ERROR TRIGGER / EDGE CASE claim has been cross-verified
  against the regression test suite (`tests/zer_fail/`).
- Soundness arguments are not formal proofs — they are
  engineering-grade prose showing that the mechanism covers
  the cases the CLAIM enumerates.
- Runtime fallbacks are listed where present; pure compile-time
  systems have no runtime cost.

Gaps found during writing, then empirically verified on 2026-04-19:

**Confirmed real gaps** (fallback to runtime check):
- System 7: cross-block backward `goto` across a free. Runtime
  gen check traps (exit 133 / SIGTRAP). Compile accepts. Would
  require CFG-based zercheck refactor (~200-400 lines). Runtime
  check is good defense-in-depth; deferred.

**Fixed during verification** (originally documented as gap, now resolved):
- System 9: mutual recursion handle tracking. Fixed via iterative
  summary refinement in `zc_build_summary`. Test:
  `tests/zer_fail/mutual_recursion_uaf.zer`.

**Refuted during verification** (the spec was over-pessimistic;
updated the edge cases):
- System 10: pass-by-value transfer IS correctly caught. The
  "return not a transfer" claim was vacuous (scope ends at return).
- System 13: mutual recursion WITH `% N` range bounding IS
  propagated correctly. Only unbounded recursion clears the range.
- System 15: simple 2D-array UAF IS caught via alloc_id sharing
  (System 8). Only non-trivial runtime-indexed cases fall back.

Remaining known unverified edge cases:
- Function-pointer indirection through opaque callees (conservative by design).
- Complex alias patterns through nested globals with escape attempts.

If a program in the wild trips one, add to the test suite and
refine the spec.
