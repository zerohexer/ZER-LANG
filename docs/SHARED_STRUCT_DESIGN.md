# Design: `shared struct` + Thread Safety — Full Concurrency Model

## Status

### IMPLEMENTED (v0.2.14):
- `shared` keyword (lexer TOK_SHARED, parser, AST is_shared)
- `shared struct` with hidden `_zer_lock` field (uint32_t spinlock)
- Auto lock/unlock: block-level grouping of consecutive shared accesses
- All statement types covered: NODE_EXPR_STMT, NODE_VAR_DECL, NODE_RETURN, NODE_IF, NODE_WHILE, NODE_FOR, NODE_SWITCH
- `&shared.field` banned at compile time (pointer bypass)
- Lock primitives in preamble: `_zer_lock_acquire` (atomic exchange), `_zer_lock_release` (atomic store)
- Tests: `shared_struct.zer` (positive), `shared_field_ptr.zer` (negative)

### TO IMPLEMENT (this session):
- `spawn` keyword for thread creation
- `HS_TRANSFERRED` state for ownership transfer
- Deadlock detection for nested shared struct access

### FUTURE (v0.4+):
- Reader-writer locks
- Lock-free patterns
- Thread join/detach

---

## What's Done: shared struct

```zer
shared struct Counter { u32 value; u32 total; }
Counter g;

u32 main() {
    g.value = 42;       // auto: lock → write → unlock
    g.total = g.value;  // same lock scope (grouped)
    u32 local = 10;     // no shared access → lock released
    g.value = local;    // new lock scope
    return 0;
}
```

### Block-Level Grouping
Consecutive statements accessing the SAME shared variable share one lock:
```
g.a = 1;        ┐
g.b = 2;        │ ONE lock scope
g.c = g.a + g.b;┘
u32 x = 42;     ← not shared → lock released
g.a = x;        ┐
if (g.a > 0) {} │ ONE lock scope (if condition included)
g.b = g.a;      ┘
```

Implementation: NODE_BLOCK emitter scans ahead with `find_shared_root_in_stmt()`.
When next statement accesses the same shared root, it stays in the lock scope.
When a non-matching statement is encountered, the lock is released.

### Field Pointer Ban
```zer
*u32 p = &g.value;  // COMPILE ERROR — bypasses locking
```
Checked in NODE_UNARY TOK_AMP: if operand is NODE_FIELD on shared struct → error.

### Emitted C
```c
struct Counter {
    uint32_t value;
    uint32_t total;
    uint32_t _zer_lock;  // hidden, auto-zero = unlocked
};

// Lock primitives:
static inline void _zer_lock_acquire(uint32_t *lock) {
    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE)) {}
}
static inline void _zer_lock_release(uint32_t *lock) {
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}
```

---

## Design: `spawn` — Thread Creation

### Syntax
```zer
spawn worker(arg1, arg2);

// Or with handle for join:
?ThreadHandle th = spawn worker(arg1, arg2);
```

### Semantics
- Creates a new thread running `worker(arg1, arg2)`
- Arguments are COPIED (value semantics) — no shared references
- Pointer arguments trigger compile error unless:
  - Pointer to `shared struct` (auto-locked, safe)
  - Pointer marked `keep` (caller guarantees lifetime)
- Function must exist (no closures, no lambdas)

### Lexer
- `TOK_SPAWN` keyword

### Parser
```
spawn_stmt := 'spawn' IDENT '(' args ')' ';'
```
Parses as NODE_SPAWN with function name + args.

### AST
```c
struct {
    const char *func_name;
    size_t func_name_len;
    Node **args;
    int arg_count;
} spawn_stmt;
```

### Checker
1. Validate function exists and has matching params
2. For each argument:
   - Value types (u32, bool, struct by value): OK — copied
   - `*shared_struct`: OK — auto-locked
   - `*T` non-shared: ERROR — "cannot pass non-shared pointer to spawn — data race. Use shared struct or copy by value"
   - `Handle(T)`: ERROR — "cannot pass Handle to spawn — pool.get() is not thread-safe"
3. Register the spawn call for HS_TRANSFERRED tracking

### Emitter
```c
// spawn worker(42, &shared_state);
// Emits:
{
    struct _zer_spawn_args_0 { uint32_t a0; struct State *a1; };
    struct _zer_spawn_args_0 *_args = malloc(sizeof(struct _zer_spawn_args_0));
    _args->a0 = 42;
    _args->a1 = &shared_state;
    pthread_t _th;
    pthread_create(&_th, NULL, _zer_spawn_worker_0, _args);
    pthread_detach(_th);
}
```
With a generated wrapper:
```c
static void *_zer_spawn_worker_0(void *_raw) {
    struct _zer_spawn_args_0 *a = _raw;
    worker(a->a0, a->a1);
    free(a);
    return NULL;
}
```

### Platform Detection
- `__STDC_HOSTED__` → pthread (Linux/macOS/Windows with pthreads)
- Bare-metal: spawn not available (single-core, use interrupts)
- RTOS: future — `xTaskCreate` for FreeRTOS, `k_thread_create` for Zephyr

---

## Design: HS_TRANSFERRED — Ownership Transfer

### Problem
After `spawn worker(&data)`, the caller should NOT use `data` — the thread owns it now.

### Solution
New HandleInfo state: `HS_TRANSFERRED` (alongside HS_ALIVE, HS_FREED, HS_MAYBE_FREED).

### When Set
At `spawn` call site: for each non-shared pointer argument, mark the source variable as HS_TRANSFERRED in zercheck PathState.

### Effect
Using a transferred variable after spawn → compile error:
```zer
struct Work { u32 task_id; }
Work w;
w.task_id = 42;
spawn process(&w);
w.task_id = 99;   // ERROR: 'w' transferred to thread at line N
```

### Shared Exception
`*shared_struct` args are NOT transferred — they're shared by design:
```zer
shared struct State { u32 count; }
State s;
s.count = 0;
spawn worker(&s);
s.count += 1;      // OK — shared struct, auto-locked
```

### Implementation in zercheck.c
In `zc_check_stmt` NODE_SPAWN case:
```c
for each arg:
    if (arg is pointer && !arg_type.is_shared) {
        HandleInfo *h = find_handle(ps, arg_key);
        if (h) h->state = HS_TRANSFERRED;
        h->transfer_line = node->loc.line;
    }
```

In all UAF check sites (NODE_FIELD, NODE_INDEX, NODE_CALL args):
```c
if (h->state == HS_TRANSFERRED) {
    zc_error("use after transfer: '%s' transferred to thread at line %d");
}
```

---

## Design: Deadlock Detection

### Problem
Two shared structs locked in different order across threads:
```zer
// Thread 1: lock A, then lock B
shared struct A { u32 x; }
shared struct B { u32 y; }
A a; B b;

// main:
a.x = 1;  // locks a
b.y = 2;  // locks b (while a may still be locked in grouped scope)

// worker:
b.y = 3;  // locks b
a.x = 4;  // locks a → DEADLOCK if thread 1 holds a and waits for b
```

### Solution: Lock Ordering
Assign each shared struct a lock order ID (based on declaration order).
At compile time, check that consecutive shared accesses within one lock group
are always in ascending order. If not → compile error with hint.

### Implementation
1. Each `shared struct` type gets a `lock_order_id` (uint32_t, assigned in register_decl)
2. In NODE_BLOCK grouping: when a new shared root is encountered, check its lock_order_id
   against the current lock. If lower → "potential deadlock: lock ordering violation —
   access 'a' (order N) before 'b' (order M) in another context"
3. Conservative: if ANY function accesses two different shared structs, warn unless
   they're always accessed in the same order

### Limitation
Full deadlock detection requires whole-program analysis across threads.
Lock ordering catches the common case (two shared structs in different order).
Complex deadlock scenarios (3+ locks, indirect via function calls) would need
transitive lock-order tracking in function summaries — v0.4+.

---

## Files Modified (implemented)

1. `lexer.h` — TOK_SHARED added to TokenType enum
2. `lexer.c` — keyword recognition ('s', length 6, "shared") + token_type_name
3. `ast.h` — `bool is_shared` in struct_decl
4. `parser.c` — `shared struct` prefix detection (same pattern as `packed struct`)
5. `types.h` — `bool is_shared` in TYPE_STRUCT
6. `checker.c` — copy is_shared from AST to Type, &shared.field pointer ban in TOK_AMP
7. `emitter.c` — _zer_lock field emission, lock primitives in preamble, block-level grouping with find_shared_root_in_stmt + find_shared_root, per-statement removed in favor of block-level

## Files to Modify (spawn + transferred + deadlock)

1. `lexer.h` — TOK_SPAWN
2. `lexer.c` — keyword recognition
3. `ast.h` — NODE_SPAWN + spawn_stmt struct
4. `parser.c` — parse spawn statement
5. `checker.c` — validate spawn args (no non-shared pointers), register spawn context
6. `emitter.c` — emit pthread_create wrapper + arg struct
7. `zercheck.h` — HS_TRANSFERRED state, transfer_line field
8. `zercheck.c` — NODE_SPAWN handler, HS_TRANSFERRED checks at use sites
9. `checker.c` — lock ordering IDs on shared struct types, ordering check in emitter grouping

## Testing Strategy

### Positive (must compile + run):
- spawn with value args
- spawn with *shared struct arg
- shared struct accessed from main after spawn (auto-locked)
- Multiple shared structs in consistent order

### Negative (must reject):
- spawn with *T non-shared → "cannot pass non-shared pointer to spawn"
- Use variable after spawn transfer → "use after transfer"
- spawn with Handle → "cannot pass Handle to spawn"
- &shared.field → "pointer bypasses locking"
- (future) Lock ordering violation → "potential deadlock"

### Semantic fuzzer:
- gen_safe_spawn(): spawn with value args + shared struct
- gen_unsafe_spawn_ptr(): spawn with non-shared pointer
- gen_unsafe_transferred(): use after spawn transfer
