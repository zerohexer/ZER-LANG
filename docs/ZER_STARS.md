# ZER `[*]T` — Dynamic Pointer Type (v0.3 Design)

## The Problem

`[]T` (slice) syntax confuses C developers:
- `[]i32 data` looks like "empty array, fill in the size"
- C devs think "incomplete array type? error?"
- The `[]` hides the fact that it's a POINTER with a length
- Declaring `[*]u8 name` reads as "pointer to many bytes" — instantly clear

## Decision: Option C — Replace `[]T` with `[*]T`

Three options were evaluated:
- **Option A:** Keep `[]T` AND add `[*]T` as alias → two syntaxes, confusing
- **Option B:** `[]T` = view/borrow, `[*]T` = owned → ownership semantics, becomes borrow checker
- **Option C:** Kill `[]T`, only `[*]T` exists → one syntax, clean ✓

**Option C chosen** because:
- One way to do things, no "which should I use?" confusion
- `[*]` reads as "pointer to many" — C devs understand `*` = pointer
- No ownership complexity (Option B leads to Rust-style borrow checking)
- Zero safety change — same `{ptr, len}` internally

## What `[*]T` Actually Is

A fat pointer — pointer + length bundled together:

```c
// Emitted C (unchanged from current []T):
typedef struct {
    T *ptr;
    size_t len;
} _zer_slice_T;
```

Not an array. Not a stack allocation. A **safe pointer** that knows how far it can go. Every access `p[i]` checks `i < len`.

## The Type System After v0.3

```
T[N]        → fixed array (compile-time size, stack/global)
[*]T        → dynamic pointer to many (bounds checked, no size in declaration)
*T          → pointer to one (non-null, guaranteed)
?*T         → pointer to one (nullable, must unwrap)
?[*]T       → dynamic pointer to many (nullable)
```

### Why `*T` and `[*]T` Must Be Separate

A linked list `next` points to ONE node. An array `lanes` points to MANY items. If both were `[*]T`:

```zer
[*]Task next;       // compiler thinks: "many tasks"
next[5].id = 1;     // compiles! but next is ONE task → buffer overflow
```

The compiler can't bounds-check without knowing "one or many." This is WHY C has buffer overflows — `*T` is used for both. ZER separates them:

```zer
?*Task next;        // ONE task, nullable → next[5] = ERROR
[*]Task lanes;      // MANY tasks → lanes[5] = bounds checked
```

The NP-hard problem: the compiler cannot automatically determine if a pointer points to one or many items. Only the developer knows the intent. The type system encodes that intent.

Every safe language has this split:
- C: `*T` for both → bugs
- Rust: `&T` vs `&[T]`
- Zig: `*T` vs `[*]T`
- Go: `*T` vs `[]T`
- ZER v0.3: `*T` vs `[*]T`

## The "Linux Kernel" Test Case

### C (current):
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
```

### ZER v0.3 (with `[*]T`):
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
```

### Allocation:
```zer
Slab(Task) heap;

TaskQueue q;
q.num_lanes = 4;
q.lanes = heap.alloc_many(?*Task, 4) orelse return;

*Task t = heap.alloc_ptr() orelse return;
t.task_id = 1;
t.name = "worker";
t.priority = 0;
t.next = q.lanes[0];    // bounds checked: 0 < 4
q.lanes[0] = t;
```

## Mapping: C to ZER v0.3

| C pattern | ZER v0.3 | Meaning |
|---|---|---|
| `Task *ptr` (one) | `*Task ptr` | Non-null pointer to one |
| `Task *ptr` (nullable) | `?*Task ptr` | Nullable pointer to one |
| `Task *ptr` (array/many) | `[*]Task ptr` | Bounds-checked pointer to many |
| `char *name` | `[*]u8 name` | Sized string pointer |
| `Task **lanes` | `[*]?*Task lanes` | Checked array of nullable pointers |
| `int arr[8]` | `i32[8] arr` | Fixed array, compile-time size |
| `void *ctx` | `*opaque ctx` | Type-erased, provenance tracked |

## Safety: Identical to Current `[]T`

`[*]T` is `[]T` renamed. Every safety feature carries over with zero changes:

| Safety feature | Works with `[*]T`? |
|---|---|
| Bounds check on index | Yes — same `_zer_bounds_check` |
| Auto-guard on unproven index | Yes — same `mark_auto_guard` |
| Range propagation (modulo/AND) | Yes — same `derive_expr_range` |
| Cross-function range summaries | Yes — same `find_return_range` |
| Const safety | Yes — same const checks |
| Array→[*]T coercion | Yes — `T[N]` auto-coerces to `[*]T` |
| Scope escape detection | Yes — same `is_local_derived` |
| Compile-time OOB on literal | Yes — same checker |
| Volatile TOCTOU protection | Yes — same volatile skip |
| Level 1-5 *opaque tracking | Yes — unchanged |

## What Changes in the Codebase

### Parser (parser.c):
- `[*]` token sequence: `TOK_LBRACKET` + `TOK_STAR` + `TOK_RBRACKET` → `TYNODE_SLICE`
- Keep `[]` working during transition (backward compat)
- Eventually deprecate `[]T` in v1.0

### Checker (checker.c):
- Zero changes — `TYNODE_SLICE` and `TYPE_SLICE` unchanged
- All slice checks work on `[*]T` because it resolves to the same type

### Emitter (emitter.c):
- Zero changes — emits same `_zer_slice_T` typedef

### Types (types.c):
- Zero changes — `TYPE_SLICE` internal representation unchanged

### Tests:
- All `[]T` in test strings can be migrated to `[*]T`
- Or keep both working during transition

### Docs:
- `ZER-LANG.md` spec: add `[*]T` syntax, deprecation note for `[]T`
- `ZER-LANG-LLM.md`: update all examples
- `CLAUDE.md`: update syntax reference

## What Does NOT Change

- Handle(T) — stays for Pool/Slab access (generation counter safety)
- Pool(T, N) — fixed allocator, same API
- Slab(T) — dynamic allocator, same API
- Arena — bump allocator, same API
- `pool.get(h).field` — same access pattern (no `h.field` shortcut)
- `*T` / `?*T` — pointer to one, same as always
- zercheck — same compile-time analysis
- Level 1-5 *opaque tracking — same runtime tracking
- MMIO 5-layer safety — same bounds checking
- All emitted C code — same output

## Future Consideration: `alloc_ptr()` (v1.0?)

Slab currently returns `Handle(T)`. For v1.0, consider adding `alloc_ptr()` that returns `*T` directly:

```zer
// Current (Handle — 100% safe, verbose):
Handle(Task) h = slab.alloc() orelse return;
slab.get(h).id = 1;

// Future (alloc_ptr — 95% safe via Level 1-5, C-style):
*Task t = slab.alloc_ptr() orelse return;
t.id = 1;
```

**ABA risk with alloc_ptr:** Slab reuses slots. If slot is freed and reallocated, old pointer (from before free) sees new allocation's inline header as alive. Generation counter (Handle) catches this; inline header (alloc_ptr) doesn't.

**Mitigation:** Level 2 poison-after-free sets old pointer to NULL. Only fails if address was copied before poison.

**Decision:** v1.0 — let users choose Handle (100%) or alloc_ptr (95%) based on their safety needs. Not urgent.

## Implementation Timeline

- **v0.3:** Add `[*]T` parser support alongside `[]T`. Both work.
- **v0.3.1:** Migrate all docs and examples to `[*]T`.
- **v0.4:** Deprecation warning on `[]T` ("use [*]T instead").
- **v1.0:** Remove `[]T` support. `[*]T` only.

## Why This Matters

ZER targets C developers. C developers think in pointers:
- `*` = pointer
- `**` = pointer to pointer
- `*arr` = dereference

`[*]T` speaks their language:
- `[*]` = pointer to many (bounded)
- `*` = pointer to one

`[]T` speaks Go/Zig language:
- `[]` = slice (what's a slice? why empty brackets?)

For adoption by C/embedded community, `[*]T` is the right syntax. It says what it is — a pointer, to multiple items, with compiler-enforced bounds.
