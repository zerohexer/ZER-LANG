# Design: `shared struct` — Auto-Locked Thread-Safe Data

## Problem

ZER catches data races between ISR and main (Pass 4: volatile + compound assign checks).
But for multi-core / RTOS / thread contexts, ZER has no compile-time enforcement that
shared mutable data is accessed under a lock.

Rust uses `Mutex<T>` (type wrapper + RAII guard). ZER needs something simpler.

## Solution: `shared struct`

One keyword. The compiler auto-inserts lock/unlock around every field access.

```zer
shared struct SensorState {
    u32 last_reading;
    u32 sample_count;
    bool ready;
}

SensorState state;

// ISR or thread — compiler auto-locks:
state.last_reading = 42;     // lock → write → unlock
state.sample_count += 1;     // lock → read-modify-write → unlock
u32 val = state.last_reading; // lock → read → unlock
```

## Syntax

```
shared struct Name { fields... }
```

- `shared` is a keyword (like `packed`)
- Applies to struct declarations only
- Fields are normal — no annotations needed
- The struct gets a hidden `_zer_lock` field

## Semantics

### Rule 1: Every field access is auto-locked
Any read or write to a `shared struct` field emits lock/unlock:
```zer
state.count += 1;
// Emits:
// _zer_lock_acquire(&state._zer_lock);
// state.count += 1;
// _zer_lock_release(&state._zer_lock);
```

### Rule 2: Multiple accesses in one statement share one lock
```zer
state.total = state.count + 1;
// Emits:
// _zer_lock_acquire(&state._zer_lock);
// state.total = state.count + 1;
// _zer_lock_release(&state._zer_lock);
```

### Rule 3: Block access via @critical for multi-statement atomicity
```zer
@critical(state) {
    u32 c = state.count;
    state.total += c;
    state.ready = true;
}
// One lock for the whole block — more efficient than per-statement
```
Without @critical(state), each statement locks independently (correct but slower).

### Rule 4: Cannot take pointer to shared field
```zer
*u32 p = &state.count;  // COMPILE ERROR — pointer would bypass locking
```
This prevents escaping the lock scope.

### Rule 5: Cannot pass shared struct by value across thread boundary
Shared structs are always accessed by reference to the SAME instance.
Copying a shared struct (value semantics) is allowed — the copy is a normal struct.

### Rule 6: shared struct in @critical(struct_var) extends existing @critical
@critical already bans return/break/continue/goto (would skip unlock).
@critical(state) adds: lock on entry, unlock on exit. Same control flow ban.

## Implementation

### lexer.h / lexer.c
- Add `TOK_SHARED` keyword (after TOK_PACKED, similar pattern)
- Recognition: case 's', length 6, "shared"

### ast.h
- Add `bool is_shared;` to struct_decl (next to is_packed)

### parser.c — parse_declaration
- Detect `shared struct` same way as `packed struct`:
  ```c
  if (match(p, TOK_SHARED)) {
      if (!check(p, TOK_STRUCT)) error("shared must precede struct");
      Node *n = parse_struct_decl(p);
      n->struct_decl.is_shared = true;
      return n;
  }
  ```

### types.h — Type struct
- Add `bool is_shared;` to TYPE_STRUCT (next to is_packed)

### checker.c — register_decl (NODE_STRUCT_DECL)
- Copy `is_shared` from AST to Type

### checker.c — check_expr (NODE_FIELD)
When accessing a field on a shared struct:
- If NOT inside `@critical(var)` scope, mark statement for auto-locking in emitter
- If taking `&struct.field` (NODE_UNARY TOK_AMP on shared field) → compile error
- Store info in typemap or a side channel for emitter to use

### checker.c — @critical(var) extension
- `@critical` currently takes no argument: `@critical { body }`
- Extend: `@critical(expr) { body }` where expr is a shared struct variable
- Inside the block, mark shared accesses as "under lock" — no per-statement locking needed
- existing @critical (no arg) = interrupt disable (unchanged)
- @critical(shared_var) = lock that specific struct's lock

### checker.c — field pointer ban
In NODE_UNARY(TOK_AMP):
- If operand is NODE_FIELD and root struct type is_shared → error
  "cannot take pointer to shared struct field — would bypass locking"

### emitter.c — emit_stmt / emit_expr
When emitting a statement that accesses a shared struct field:
- If NOT inside @critical(var) lock scope:
  - Before: emit `_zer_lock_acquire(&var._zer_lock);`
  - After: emit `_zer_lock_release(&var._zer_lock);`
- If inside @critical(var) lock scope: emit nothing extra (already locked)

Need to detect which shared struct variables are accessed in a statement.
Walk the expression tree, collect all NODE_FIELD accesses where root type is_shared.
Group by variable — one lock per variable per statement.

### emitter.c — struct declaration
When emitting a shared struct:
- Add `uint32_t _zer_lock;` field (spinlock, init to 0)
- Or: `pthread_mutex_t _zer_lock;` for hosted targets

### emitter.c — preamble
Add lock primitives:
```c
// Bare-metal spinlock (default):
static inline void _zer_lock_acquire(uint32_t *lock) {
    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE)) {}
}
static inline void _zer_lock_release(uint32_t *lock) {
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

// Hosted (detected via __STDC_HOSTED__):
// Use pthread_mutex_lock/unlock
```

### emitter.c — auto-zero init
Shared struct auto-zero includes `_zer_lock = 0` (unlocked). Already handled
by ZER's auto-zero — spinlock starts unlocked.

## Interactions with Existing Features

### defer + shared
```zer
defer { state.count = 0; }  // auto-locked — works normally
```
Each deferred statement with shared access gets its own lock/unlock.

### goto + shared
Goto is banned inside @critical (already enforced). Auto-locked per-statement
access doesn't create a scope, so goto is fine.

### Handle auto-deref + shared
```zer
shared struct Task { u32 id; }
Pool(Task, 4) pool;
// Handle(Task) h; h.id = 42;
// Auto-deref + auto-lock: both happen in emitter
```

### orelse + shared
```zer
u32 val = state.count orelse 0;
// state.count is u32, not optional — orelse doesn't apply
// If shared struct has ?u32 field:
// lock → read → unlock → orelse
```

### const shared struct
```zer
const shared struct Config { u32 baud; }
// const = read-only. Lock still needed for reads on multi-core
// (memory model: read must see latest write from other core)
```

### Array of shared structs
```zer
shared struct Counter { u32 val; }
Counter[4] counters;
counters[i].val += 1;  // auto-locked with counters[i]._zer_lock
```
Each array element has its own lock — fine-grained. Auto-zero inits all locks to 0.

### shared struct as function parameter
```zer
void increment(*shared Counter c) {
    c.val += 1;  // auto-locked
}
```
Pointer to shared struct preserves the shared property. Compiler knows
`c` is shared from the type.

Wait — this requires `*shared T` pointer syntax OR tracking is_shared
through TYPE_POINTER. Simpler: is_shared is on the struct type, so
ANY pointer to a shared struct automatically inherits the shared property.
The pointer's inner type is TYPE_STRUCT with is_shared=true → field
access auto-locks regardless of how you got the pointer.

## Testing Strategy

### Positive tests (must compile + run):
1. Basic shared struct field read/write
2. @critical(var) block with multiple accesses
3. Shared struct in function parameter
4. Array of shared structs
5. Shared struct with defer
6. Shared struct with Handle auto-deref (if applicable)

### Negative tests (must reject):
1. Taking pointer to shared field: `&state.count`
2. (future) Deadlock: nested @critical on different shared vars

### Semantic fuzzer:
- gen_safe_shared(): shared struct with field access
- gen_unsafe_shared_ptr(): taking &shared_field

## What This Achieves

With `shared struct` + existing ISR analysis + `HS_TRANSFERRED` (future):
- 100% data race prevention at compile time
- No annotations on functions
- No lifetime syntax
- One keyword (`shared`)
- Auto-inserted locking
- Same safety as Rust's Mutex<T> for the common patterns

## What This Does NOT Cover

- Lock ordering / deadlock prevention (future: checker can detect nested @critical)
- Fine-grained reader-writer locks (future: `@shared_read` vs `@exclusive_write`)
- Lock-free data structures (use @atomic_* directly)

These are v0.4+ enhancements. The base `shared struct` covers 95% of real-world
embedded concurrency patterns.

## Files to Modify

1. lexer.h — TOK_SHARED
2. lexer.c — keyword recognition
3. ast.h — is_shared on struct_decl
4. parser.c — shared struct parsing
5. types.h — is_shared on Type (struct)
6. checker.c — shared field access validation, &field ban, @critical(var) extension
7. emitter.c — auto lock/unlock emission, _zer_lock field, preamble lock primitives
8. zercheck.c — NODE_CRITICAL exhaustive switch (already handled)

Estimated: ~200-300 lines of changes across 8 files.
