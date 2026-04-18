# ZER Syntactic Sugar — Handle Auto-Deref + Task.alloc() + [*]T

## The Problem

ZER's allocator model is safe but verbose compared to C:

```c
// C — 2 lines, done:
struct task *t = malloc(sizeof(struct task));
t->id = 1;
```

```zer
// ZER current — 3 lines, ceremony:
Slab(Task) heap;                              // must declare allocator
Handle(Task) h = heap.alloc() orelse return;  // get handle, not pointer
heap.get(h).id = 1;                           // .get(h) every field access
```

Three pain points:
1. `Slab(Task) heap;` — must declare allocator explicitly
2. `heap.get(h).id` — verbose field access through Handle
3. `Slab(Task) heap;` — must exist before you can allocate

All three can be fixed with syntactic sugar. Zero safety compromise — same Handle, same gen check, same 100% ABA protection underneath.

## Design 1: Handle Auto-Deref (`h.field` → `slab.get(h).field`)

### What the user writes:
```zer
Slab(Task) heap;
Handle(Task) t = heap.alloc() orelse return;
t.id = 1;              // just dot-access, like C
t.name = "worker";     // clean
t.next = other;        // clean
```

### What the compiler emits:
```c
// Identical to heap.get(t).id = 1:
((Task*)_zer_slab_get(&heap, t))->id = 1;
((Task*)_zer_slab_get(&heap, t))->name = ...;
((Task*)_zer_slab_get(&heap, t))->next = ...;
```

### Safety: 100% — identical to current pool.get(h)

The auto-deref is pure syntactic sugar. The emitter still emits the exact same `_zer_pool_get()` / `_zer_slab_get()` with generation counter check. The pointer is created and consumed in one C expression — never stored, never escapes.

| Safety property | With `.get(h)` | With auto-deref `h.field` |
|---|---|---|
| Generation counter check | Yes | Yes — same emitted code |
| Non-storable pointer | Yes | Yes — pointer lives one expression |
| ABA detection | Yes — gen mismatch traps | Yes — same gen check |
| Use-after-free | Compile error (zercheck) | Compile error (zercheck) |
| Double free | Compile error (zercheck) | Compile error (zercheck) |

### How it works in the checker (NODE_FIELD, ~line 2900):

```
1. NODE_FIELD on obj where obj->kind == TYPE_HANDLE
2. Get elem type: obj->handle.elem
3. Unwrap distinct, check it's TYPE_STRUCT
4. Look up the struct field normally (same as pointer auto-deref path)
5. Resolve which Slab/Pool this Handle came from (see "Slab Resolution" below)
6. Store the slab Symbol reference for the emitter
7. Return the field type as result
```

This mirrors the existing pointer auto-deref pattern at checker.c line 3001-3018, which already does:
```c
if (obj->kind == TYPE_POINTER &&
    type_unwrap_distinct(obj->pointer.inner)->kind == TYPE_STRUCT) {
    // look up field in inner struct, return field type
}
```

Handle auto-deref is the same pattern — just resolving through Handle's elem type instead of pointer's inner type.

### How it works in the emitter (NODE_FIELD, ~line 1273):

Current emitter for NODE_FIELD:
```c
case NODE_FIELD:
    emit_expr(e, node->field.object);
    if (obj_type->kind == TYPE_POINTER)
        emit(e, "->%.*s", ...);    // pointer: ->field
    else
        emit(e, ".%.*s", ...);     // struct: .field
```

Add Handle path:
```c
    if (obj_type->kind == TYPE_HANDLE) {
        // Emit: ((ElemType*)_zer_slab_get(&slab_name, handle_expr))->field
        // slab_name comes from the slab_source stored by checker
    }
```

### Slab Resolution — Which Allocator Does This Handle Belong To?

Three-layer resolution, from most specific to least:

**Layer 1: Provenance tracking (covers 99% of cases)**

When a Handle is assigned from `pool.alloc()` or `slab.alloc()`, the checker tags the variable's Symbol with `slab_source` — a pointer to the allocator's Symbol.

```zer
Slab(Task) workers;
Slab(Task) io_tasks;

Handle(Task) t = workers.alloc() orelse return;
// checker sets: t.slab_source = &workers_symbol

t.id = 1;
// checker reads: t.slab_source = workers
// emitter emits: ((Task*)_zer_slab_get(&workers, t))->id = 1
```

Provenance propagates through:
- Direct assignment: `Handle(Task) t2 = t;` → t2 inherits slab_source
- Function returns: `find_return_range()` already tracks return provenance — same pattern for slab_source

**Layer 2: Unique allocator lookup (covers ambiguous cases)**

If slab_source is NULL (e.g., Handle passed as function parameter), walk scopes to find all Slab(T)/Pool(T,N) with matching element type:

```c
Symbol *find_unique_allocator(Scope *s, Type *elem_type) {
    Symbol *found = NULL;
    for (Scope *sc = s; sc; sc = sc->parent) {
        for (uint32_t i = 0; i < sc->symbol_count; i++) {
            Type *t = sc->symbols[i].type;
            if (!t) continue;
            if ((t->kind == TYPE_SLAB && type_equals(t->slab.elem, elem_type)) ||
                (t->kind == TYPE_POOL && type_equals(t->pool.elem, elem_type))) {
                if (found) return NULL; // ambiguous
                found = &sc->symbols[i];
            }
        }
    }
    return found;
}
```

- Exactly one found → auto-resolve, no ambiguity
- Zero found → compile error: "no Slab/Pool for type Task in scope"
- Multiple found → Layer 3

**Layer 3: Ambiguity error (rare — multiple slabs of same type, no provenance)**

```zer
Slab(Task) workers;
Slab(Task) io_tasks;

void process(Handle(Task) h) {
    h.id = 1;   // ERROR: ambiguous slab for Handle(Task)
                 //        — use workers.get(h).id or io_tasks.get(h).id
}
```

Compile error with clear fix: use explicit `.get()`. This is the fallback — works exactly like today. No regression.

### What changes in types.h:

```c
struct Symbol {
    // ... existing fields ...

    /* Handle auto-deref: which Slab/Pool this handle was allocated from */
    Symbol *slab_source;    /* NULL = unknown (parameter, conditional) */
};
```

One field. Set at alloc() call site. Propagated through assignment. Read at NODE_FIELD emission.

### Edge cases:

**Conditional assignment from different slabs:**
```zer
Slab(Task) workers;
Slab(Task) io_tasks;
Handle(Task) t;
if (cond) {
    t = workers.alloc() orelse return;
} else {
    t = io_tasks.alloc() orelse return;
}
t.id = 1;   // ERROR: slab source is ambiguous (workers OR io_tasks)
            // fix: restructure or use explicit get()
```

Same pattern as `provenance_type` for `*opaque` — conditional assignment from different sources = ambiguous = error.

**Handle in struct field:**
```zer
struct Job {
    Handle(Task) task;
}
Job j;
j.task = heap.alloc() orelse return;
j.task.id = 1;    // slab_source propagated through struct field?
                   // → use Layer 2 (unique allocator lookup) as fallback
```

Layer 2 handles this — find unique Slab(Task) in scope. If only one exists (common case), auto-resolve works.

**Handle passed to function:**
```zer
void set_id(Handle(Task) h, u32 id) {
    h.id = id;    // slab_source = NULL (parameter)
                   // → Layer 2: find unique Slab(Task) in scope
                   // → if exactly one global Slab(Task) exists, auto-resolve
                   // → if multiple or none, compile error
}
```

### Estimated implementation:

| Component | Lines | What |
|---|---|---|
| types.h | ~1 | Add `Symbol *slab_source` |
| checker.c NODE_CALL | ~10 | Set slab_source on var from alloc() |
| checker.c NODE_ASSIGN | ~5 | Propagate slab_source |
| checker.c NODE_FIELD | ~20 | Handle auto-deref + 3-layer resolution |
| emitter.c NODE_FIELD | ~15 | Emit _zer_slab_get / _zer_pool_get wrapper |
| scope walker | ~20 | find_unique_allocator helper |
| **Total** | **~70** | Pure sugar, no new types or concepts |

## Design 2: Task.alloc() / Task.free() — Implicit Slab

### What the user writes:
```zer
// No Slab declaration needed:
Handle(Task) t = Task.alloc() orelse return;
t.id = 1;
t.name = "worker";
Task.free(t);
```

### What the compiler does:

1. Sees `Task.alloc()` — no explicit Slab(Task) in scope
2. Auto-generates a hidden module-level global: `Slab(Task) _zer_auto_slab_Task;`
3. Routes `Task.alloc()` → `_zer_auto_slab_Task.alloc()`
4. Routes `Task.free(h)` → `_zer_auto_slab_Task.free(h)`
5. Handle auto-deref uses the auto-generated slab as slab_source

### The emitted C:
```c
// Auto-generated at module level:
_zer_slab _zer_auto_slab_Task = {0};

// Task.alloc() becomes:
uint8_t _zer_aok0 = 0;
uint64_t _zer_ah0 = _zer_slab_alloc(&_zer_auto_slab_Task, sizeof(Task), &_zer_aok0);
// ... same slab alloc pattern as explicit Slab

// t.id = 1 becomes:
((Task*)_zer_slab_get(&_zer_auto_slab_Task, t))->id = 1;

// Task.free(t) becomes:
_zer_slab_free(&_zer_auto_slab_Task, t);
```

### Safety: identical to explicit Slab

Same Slab, same Handle, same generation counter, same zercheck. The auto-Slab is just a compiler-generated global variable — nothing special about it.

### Coexistence with explicit allocators:

Both modes work in the same file, same project, same function:

```zer
// Explicit — for ISR, bare metal, control:
Pool(Task, 8) isr_tasks;
Handle(Task) t1 = isr_tasks.alloc() orelse return;
t1.id = 1;    // auto-deref uses isr_tasks

// Implicit — for OS, application, convenience:
Handle(Task) t2 = Task.alloc() orelse return;
t2.id = 2;    // auto-deref uses auto-generated Slab
Task.free(t2);
```

No conflict. Explicit allocators are used when declared. `Task.alloc()` creates an auto-Slab only if needed.

### --no-heap flag (optional safety net):

```
zerc --no-heap main.zer
```

With this flag, `Task.alloc()` is a compile error:
```
error: Task.alloc() requires heap allocation (calloc)
       use --no-heap to enforce no dynamic allocation
       fix: use Pool(Task, N) for fixed allocation
```

Without the flag (default), `Task.alloc()` just works. The flag exists for MISRA compliance and tiny bare-metal targets where malloc/calloc truly doesn't exist.

### When is --no-heap needed?

| Environment | Has heap? | Task.alloc() works? |
|---|---|---|
| Linux/macOS/Windows app | Yes | Yes |
| Linux kernel module | Yes (kmalloc) | Yes |
| FreeRTOS / Zephyr RTOS | Yes (pvPortMalloc) | Yes |
| ESP32, STM32F4+ (256KB+) | Yes | Yes |
| Arduino | Yes (limited) | Yes |
| Cortex-M0, 2KB RAM | Maybe | User decides |
| MISRA C certified | Banned by spec | Use --no-heap |

95%+ of use cases: `Task.alloc()` works out of the box. The `--no-heap` flag is opt-in for the 5% that truly can't use dynamic allocation.

### Checker implementation:

In `check_expr` for `NODE_FIELD` where object is a type name (NODE_IDENT resolving to a struct type):

```
1. See Task.alloc() — callee is NODE_FIELD where object = "Task" (struct type)
2. Check if --no-heap → error
3. Look for existing Slab(Task) auto-slab in module scope
4. If not found → create one, add to module scope as _zer_auto_slab_Task
5. Return ?Handle(Task) as result (same as slab.alloc())
6. For Task.free(h) → route to auto-slab.free(h)
```

### One auto-Slab per type, program-wide (like C's malloc):

C has ONE global heap — `malloc()` goes to the same pool no matter which `.c` file calls it. ZER's auto-Slab works the same way: one `_zer_auto_slab_Task` per type, shared across the entire program. Not per-module — that would break cross-module Handle passing.

```zer
// module_a.zer:
Handle(Task) t = Task.alloc() orelse return;
send_to_b(t);    // pass handle to another module — works

// module_b.zer:
void process(Handle(Task) t) {
    t.id = 1;     // works — same global auto-Slab(Task)
    Task.free(t);  // works — same slab
}
```

The emitter generates one `_zer_auto_slab_Task` as a program-level global, same as how libc's heap is process-wide. Multiple modules using `Task.alloc()` all route to the same slab.

If you want SEPARATE allocation pools (e.g., ISR pool vs application pool), use explicit allocators:
```zer
Pool(Task, 8) isr_tasks;     // ISR — no heap, fixed
Slab(Task) app_tasks;        // Application — explicit, separate from auto-slab
```

## Design 3: [*]T — Dynamic Pointer to Many (ALREADY IMPLEMENTED)

See `docs/ZER_STARS.md` for full design. Summary:

- `[*]T` replaces `[]T` — reads as "pointer to many"
- Same internal `{ptr, len}` fat pointer, same bounds checking
- Parser change only (~10 lines), zero checker/emitter changes
- Both `[]T` and `[*]T` work now, `[]T` deprecated at v1.0
- Already implemented and passing all 1700+ tests

## The Complete Picture: C to ZER Mapping

### Before (current ZER — verbose):
```zer
Slab(Task) heap;

struct Task {
    u32 task_id;
    []u8 name;
    u32 priority;
    ?*Task next;
}

struct TaskQueue {
    u32 num_lanes;
    []?*Task lanes;
}

// Usage:
Handle(Task) t = heap.alloc() orelse return;
heap.get(t).task_id = 1;
heap.get(t).name = "worker";
heap.get(t).priority = 0;
heap.get(t).next = null;
```

### After (with all three sugars):
```zer
struct Task {
    u32 task_id;
    [*]u8 name;
    u32 priority;
    ?*Task next;
}

struct TaskQueue {
    u32 num_lanes;
    [*]?*Task lanes;
}

// Usage — no Slab declaration, no .get(), [*]T syntax:
Handle(Task) t = Task.alloc() orelse return;
t.task_id = 1;
t.name = "worker";
t.priority = 0;
t.next = null;
Task.free(t);
```

### C equivalent:
```c
struct task {
    int task_id;
    char *name;
    int priority;
    struct task *next;
};

struct task_queue {
    int num_lanes;
    struct task **lanes;
};

// Usage:
struct task *t = malloc(sizeof(struct task));
t->task_id = 1;
t->name = "worker";
t->priority = 0;
t->next = NULL;
free(t);
```

### Comparison table:

| Operation | C | ZER (after sugar) | Safety |
|---|---|---|---|
| Declare struct | `struct task {...};` | `struct Task {...}` | Same |
| String field | `char *name;` | `[*]u8 name;` | Bounds checked |
| Nullable ptr | `struct task *next;` | `?*Task next;` | Must unwrap |
| Array of ptrs | `struct task **lanes;` | `[*]?*Task lanes;` | Bounds checked |
| Allocate | `malloc(sizeof(...))` | `Task.alloc() orelse return;` | Gen-checked Handle |
| Field access | `t->id = 1;` | `t.id = 1;` | Gen check on every access |
| Free | `free(t);` | `Task.free(t);` | Double-free = compile error |
| UAF | Silent corruption | Compile error (zercheck) | 100% caught |

Same line count. Same mental model. ZER just catches the bugs C doesn't.

## What Does NOT Change

These sugars are purely syntactic. Nothing changes in the safety model:

- Handle(T) — same u64 with index + generation
- Generation counter — same ABA detection (100%)
- zercheck — same compile-time ALIVE/FREED/MAYBE_FREED tracking
- Level 1-5 *opaque — same runtime tracking for C interop pointers
- Pool(T, N) — still exists for ISR-safe, heap-free allocation
- Arena — still exists for bump allocation
- Ring(T, N) — still exists for circular buffers
- Bounds checking — same [*]T / []T checks
- Scope escape — same is_local_derived / is_arena_derived
- Provenance — same @ptrcast / @container tracking
- Non-storable get() — auto-deref pointer still lives one expression only

## alloc_ptr() / free_ptr() — IMPLEMENTED (zercheck Level 9)

`*Task t = slab.alloc_ptr()` returns a real pointer, not Handle. zercheck tracks it at compile time — UAF, double-free, cross-function free, return-freed all caught. 100% compile-time safe for pure ZER code.

```zer
Slab(Task) heap;
*Task t = heap.alloc_ptr() orelse { return 1; };
t.id = 42;           // direct pointer deref — zero overhead
heap.free_ptr(t);
t.id = 99;           // COMPILE ERROR — zercheck: use-after-free
```

**Handle vs alloc_ptr tradeoff:**

| | Handle(Task) | *Task from alloc_ptr() |
|---|---|---|
| Gen check on every access | Yes (100% ABA safe) | No (direct deref) |
| Compile-time UAF (same func) | zercheck | zercheck (Level 9) |
| Compile-time UAF (cross func) | zercheck + gen | zercheck FuncSummary (9b) |
| ABA (slot reuse, aliased ptr) | Gen check catches | Level 3+5 runtime (~1ns) |
| Syntax | `Handle(Task) t` | `*Task t` |
| Performance | Gen check per access | Zero overhead |

Both are valid. Both can coexist on the same Slab/Pool. User chooses: Handle for maximum paranoia, `*Task` for C-style directness.

**Type checking:** `free_ptr()` validates argument type matches pool/slab element — `*Motor` to `Task` pool is a compile error.

**Ghost check:** bare `heap.alloc_ptr();` without assignment is a compile error (leaked allocation).

## *opaque Safety — Compile-Time vs Runtime Coverage

### Current coverage breakdown (after 9a+9b+9c):

| `*opaque` case | Compile-time? | Runtime? | How |
|---|---|---|---|
| Direct variable UAF | 100% compile | — | zercheck ALIVE/FREED |
| Alias UAF (same function) | 100% compile | — | zercheck alias tracking |
| Struct field `ctx.data` after free | 100% compile | — | 9a: untracked key FREED registration |
| Cross-function free (FuncSummary) | 100% compile | — | 9b: *T params tracked in summary |
| Return freed pointer | 100% compile | — | 9c: NODE_RETURN state check |
| Constant array index `cache[3]` | 100% compile | — | zercheck compound key `cache.3` |
| Dynamic array index `cache[slot]` | — | ~1ns | Level 3 inline header at `@ptrcast` |
| C library boundary (`lib_store(p)`) | — | ~1ns | Level 3+5 header check |
| Integer round-trip (`@ptrtoint`→`@inttoptr`) | — | ~1ns | Level 3 header check |
| Pointer-to-pointer (`**opaque pp = &p`) | — | ~1ns | Level 3 header check |
| Pointer math on raw integers | — | ~1ns | Level 3 header check |
| setjmp/longjmp restoring stale pointer | — | ~1ns | Level 3 header check |

**Total: ~98% compile-time, ~2% runtime at ~1ns per check.**

### The ~2% runtime cases — why they can't be compile-time:

These all involve information that doesn't exist until the program runs:
- Dynamic array index: `slot` is a runtime value, compiler can't track which element
- C library: compiler can't see inside compiled C code
- Integer round-trip: after `@ptrtoint`, it's just a number — math can modify it
- Pointer-to-pointer: `*pp` dereferences through indirection compiler can't resolve

**No language on earth solves these at compile time.** Not Rust, not Ada, not anything. The information doesn't exist in the source code. Runtime check is the mathematically correct answer.

### Potential optimization: check every `*opaque` USE (not just `@ptrcast`)

Currently the Level 3 inline header check fires at `@ptrcast` (when `*opaque` is cast back to `*Task`). This is the earliest USEFUL point — you can't do anything with `*opaque` without casting first.

A future optimization could check at EVERY `*opaque` expression (function calls, stores, comparisons):

```zer
free(p);
lib_store(p);    // currently: not checked here (checked later at @ptrcast)
                  // optimization: check HERE — catch 1 line earlier
```

**Status:** Not implemented. Not a safety gap (bug is caught either way). Just makes the error message point to a closer line. Implement if real users report confusing error locations.

### The 5+4 safety levels (all implemented):

| Level | What | When | Cost |
|---|---|---|---|
| 1 | zercheck (ALIVE/FREED/MAYBE_FREED) | Compile | Zero |
| 2 | Poison after free (ptr = NULL) | Runtime (at free) | 1 instruction |
| 3+4 | Inline 16-byte header (magic `0x5A455243`) | Runtime (at `@ptrcast`) | ~1ns |
| 5 | `--wrap=malloc` global interception | Runtime (link time) | Same as 3 |
| 9 | zercheck for `*Task` from `alloc_ptr` | Compile | Zero |
| 9a | zercheck struct field tracking | Compile | Zero |
| 9b | zercheck cross-function `*T` summary | Compile | Zero |
| 9c | zercheck return freed detection | Compile | Zero |

## Implementation Status

1. **[*]T syntax** — DONE
2. **Handle auto-deref** — DONE (checker + emitter, ~80 lines)
3. **alloc_ptr() / free_ptr()** — DONE (checker + emitter + zercheck Level 9, ~100 lines)
4. **zercheck 9a+9b+9c** — DONE (~70 lines)
5. **Handle(T)[N] arrays** — DONE (parser, ~10 lines)
6. **Audit fixes** — 6 bugs found and fixed (goto/switch/defer, free_ptr type check, const Handle, ghost alloc_ptr, no-allocator error)
7. **Task.alloc()** — NOT YET (auto-Slab creation, ~50 lines)

## Why This Matters

ZER's goal: C developers adopt ZER without changing how they think. The mental model is pointers, structs, manual memory management. ZER just prevents the bugs.

With these sugars:
- Declaration looks like C (struct fields, pointer types)
- Allocation: `Handle(Task) t` for safety, `*Task t` for C-style — both work
- Field access looks like C (dot operator, no ceremony, auto-deref)
- Safety is invisible (gen checks, bounds checks, zercheck — all automatic)

The compiler does more work. The developer writes the same code. The bugs disappear.
