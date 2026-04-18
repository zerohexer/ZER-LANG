# CLAUDE.md

## ZER Language — Complete Quick Reference

ZER is memory-safe C. Same syntax, same mental model — but the compiler prevents buffer overflows, use-after-free, null dereferences, and silent memory corruption. Compiles to C, then GCC handles backends.

### Primitive Types
```
u8  u16  u32  u64        unsigned integers
i8  i16  i32  i64        signed integers
usize                    pointer-width unsigned (hardcoded 32-bit currently)
f32  f64                 floating point
bool                     true / false (NOT an integer — no bool↔int coercion)
void                     return type only, cannot declare void variables
```

### Compound Types
```
u8[256] buf;             fixed array — NOTE: size AFTER type, BEFORE name
[*]u8 data;              dynamic pointer to many — {ptr, len}, bounds checked (replaces []T)
[]u8 data;               DEPRECATED — same as [*]u8, warns "use [*]T instead"
*Task ptr;               pointer — guaranteed non-null
?*Task maybe;            optional pointer — might be null (null sentinel, zero overhead)
?u32 result;             optional value — struct { u32 value; u8 has_value; }
?bool flag;              optional bool — struct { u8 value; u8 has_value; }
?void status;            optional void — struct { u8 has_value; } (NO value field!)
*opaque raw;             type-erased pointer (C's void*)
```

### Builtin Container Types
```
Pool(Task, 8) tasks;     fixed-slot allocator — compile-time count, ALWAYS global
Slab(Task) tasks;         dynamic slab allocator — grows on demand, ALWAYS global
Ring(u8, 256) rx_buf;    circular buffer — ALWAYS global
Arena scratch;            bump allocator — (over, alloc, alloc_slice, reset)
Barrier b;               thread sync point — (@barrier_init, @barrier_wait)
Handle(Task) h;          u64: index(32) + generation(32), not a pointer
Semaphore(3) slots;      counting semaphore — @sem_acquire/@sem_release, thread-safe
```

### User-Defined Containers (Monomorphization)
```
// Define parameterized struct template — NOT generics, just type stamping
container Stack(T) {
    T[64] data;
    u32 top;
}

Stack(u32) s;                // stamps concrete struct Stack_u32
Stack(Point) ps;             // stamps separate struct Stack_Point

// Functions are regular free functions, NOT methods (no `this`)
void stack_push(*Stack(u32) s, u32 val) { s.data[s.top] = val; s.top += 1; }

// T substitution works in: T, *T, ?T, []T, T[N] field types
container Wrapper(T) { *T ptr; ?T maybe; T[4] arr; }
```

### Designated Initializers
```
Point p = { .x = 10, .y = 20 };     // var-decl init
p = { .x = 100, .y = 200 };         // assignment (C99 compound literal)
Config c = { .baud = 9600 };         // partial — unmentioned fields auto-zero
```

### CRITICAL Syntax Differences from C

**These cause the most wasted turns in fresh sessions:**

1. **Braces ALWAYS required** for if/else/while/for bodies. No braceless one-liners.
   ```
   if (x > 5) { return 1; }      // OK
   if (x > 5) return 1;           // PARSE ERROR — "expected '{'"
   ```

2. **C-style casts ARE supported** for type conversion. `@saturate` and `@bitcast` remain for the rare cases.
   ```
   (u32)small                     // OK — widening (or narrowing truncate)
   (f32)count                     // OK — int to float value convert
   (*opaque)sensor                // OK — *T to *opaque (sets provenance)
   (*Motor)ctx                    // OK — *opaque to *T (provenance checked)
   (*u32)int_val                  // ERROR — use @inttoptr (mmio safety)
   (u32)ptr                       // ERROR — use @ptrtoint
   (*B)a_ptr                      // ERROR — *A to *B direct, use *opaque round-trip
   (*u32)volatile_ptr             // ERROR — cannot strip volatile
   (*u32)const_ptr                // ERROR — cannot strip const
   @saturate(i8, big)             // OK — clamp to [-128, 127] (explicit only)
   @bitcast(u32, float_val)       // OK — reinterpret bits (explicit only)
   ```

3. **Variable declarations: type before name, no `struct` keyword in usage.**
   ```
   struct Task { u32 id; }        // declaration uses 'struct' keyword
   Task t;                        // usage: just the name, NO 'struct' prefix
   *Task ptr = &t;                // pointer to Task
   ```

4. **Array declaration: size between type and name.**
   ```
   u8[256] buf;                   // ZER: type[size] name
   // NOT: u8 buf[256];           // C style — won't work
   ```

5. **Enum values: dot syntax, not bare names.**
   ```
   enum State { idle, running, done }
   State s = State.idle;          // qualified access
   switch (s) {
       .idle => { ... }           // dot-prefixed in switch arms
       .running => { ... }
       .done => { ... }
   }
   ```

6. **Switch uses `=>` arrows, no fallthrough, no `case` keyword.**
   ```
   switch (x) {
       .a => { ... }              // enum arm
       .b, .c => { ... }          // multi-value arm
       default => { ... }         // default arm (required for int switches)
   }
   ```

7. **Optional unwrapping: `orelse` and `if |capture|`.**
   ```
   u32 val = maybe_func() orelse 0;           // default value
   u32 val = maybe_func() orelse return;       // propagate failure
   u32 val = maybe_func() orelse { cleanup(); return; }  // block fallback

   if (maybe_val) |v| { use(v); }             // if-unwrap with capture
   if (maybe_val) |*v| { v.field = 5; }       // mutable capture (pointer)
   ```

8. **No `++`/`--` operators.** Use `+= 1` / `-= 1`.

9. **No `malloc`/`free`.** Use Pool, Slab, Ring, or Arena builtins.

10. **String literals are `[*]u8` (slices), not `char*`.** `[*]T` is the preferred syntax — reads as "pointer to many." `[]T` still works but emits a deprecation warning ("use [*]T instead"). Use `[*]T` in all new code.
    ```
    [*]u8 msg = "Hello";          // slice with .ptr and .len
    ```

11. **`else if` is supported.** Both forms work:
    ```
    if (a) { ... } else if (b) { ... }       // OK — parsed as nested if
    if (a) { ... } else { if (b) { ... } }   // OK — same result
    ```

12. **`orelse return` is bare — no value.** Return value comes from function's return type.
    ```
    *Task t = pool.alloc(Task) orelse return;    // OK — bare return
    *Task t = pool.alloc(Task) orelse return 1;  // PARSE ERROR
    ```

13. **`arena.alloc(T)` — T must be a struct/enum name, not a primitive keyword.**
    ```
    arena.alloc(Task)           // OK — Task is an identifier
    arena.alloc(u32)            // PARSE ERROR — u32 is a keyword
    ```

14. **For loops use `i += 1`, compound init, and C-style structure.**
    ```
    for (u32 i = 0; i < 10; i += 1) { ... }   // OK
    for (u32 i = 0; i < 10; i++) { ... }       // PARSE ERROR — no ++
    ```

15. **`goto` and labels are supported.** Forward and backward jumps. Safe because auto-zero + defer neutralize goto's traditional dangers. Banned inside `defer` and `@critical` blocks.
    ```
    goto cleanup;                              // forward jump
    // ...
    cleanup:
        free_resources();
        return 1;
    ```

### Intrinsics (@ builtins)
```
@size(T)                 sizeof — returns usize
@truncate(T, val)        keep low bits (big→small)
@saturate(T, val)        clamp to min/max of T
@bitcast(T, val)         reinterpret bits (same width required, qualifier-checked)
@cast(T, val)            distinct typedef conversion only (qualifier-checked)
@inttoptr(*T, addr)      integer to pointer (mmio-range-validated if ranges declared)
@ptrtoint(ptr)           pointer to integer (usize)
@ptrcast(*T, ptr)        pointer type cast (provenance-tracked, qualifier-checked)
@barrier()               full memory barrier
@barrier_store()         store barrier
@barrier_load()          load barrier
@offset(T, field)        offsetof
@container(*T, ptr, f)   container_of (field-validated, provenance-tracked)
@trap()                  crash intentionally
@probe(addr)             safe MMIO read — returns ?u32, null if address faults
```

### mmio Declaration (MANDATORY for @inttoptr)
```
mmio 0x40020000..0x40020FFF;   // declare valid MMIO address range
mmio 0x40011000..0x4001103F;   // multiple ranges allowed
// @inttoptr with constant address outside ranges → compile error
// @inttoptr with variable address → runtime range check + trap
// No mmio declarations + @inttoptr → compile error (strict by default)
// --no-strict-mmio flag: allow @inttoptr without mmio declarations
// For tests: mmio 0x0..0xFFFFFFFFFFFFFFFF; (allow all addresses)
```

### Provenance Tracking (@ptrcast + @container)
```
// @ptrcast tracks original type through *opaque round-trips:
*opaque ctx = @ptrcast(*opaque, &sensor);  // provenance = *Sensor
*Sensor s = @ptrcast(*Sensor, ctx);        // OK — matches provenance
*Motor m = @ptrcast(*Motor, ctx);          // COMPILE ERROR — wrong type

// @container validates field existence + tracks &struct.field origin:
*ListHead ptr = &dev.list;                  // provenance = (Device, list)
*Device d = @container(*Device, ptr, list); // OK — proven
*Other o = @container(*Other, ptr, list);   // COMPILE ERROR — wrong struct

// Unknown provenance (params, cinclude) → allowed (can't prove wrong)
// Propagates through aliases and clears+re-derives on assignment
```

### Function Pointers
```
u32 (*fn)(u32, u32) = add;              // local variable
void (*callback)(u32 event);             // global variable
struct Ops { u32 (*compute)(u32); }      // struct field
u32 apply(u32 (*op)(u32, u32), x, y);   // parameter
?void (*on_event)(u32) = null;           // optional — null sentinel
typedef u32 (*BinOp)(u32, u32);          // function pointer typedef
typedef ?u32 (*OptHandler)(u32);         // typedef with optional return
BinOp[4] ops;                            // array of function pointers (via typedef)
```
**`?` on funcptr — context-dependent (BUG-420):**
- At var/param/field/global: `?RetType (*name)(params)` → **nullable funcptr** (? wraps the pointer)
- At typedef: `typedef ?RetType (*Name)(params)` → **funcptr returning optional** (? is part of return type)
- For nullable typedef'd funcptr: `?TypedefName var = null;`

### Defer
```
void f() {
    *u8 buf = alloc();
    defer free(buf);                     // runs at scope exit, in reverse order
    if (error) { return; }               // defer fires on ALL return paths
}
```

### Comptime (compile-time evaluation — replaces C macros + #ifdef)
```
// comptime functions — type-checked, zero runtime cost:
comptime u32 BIT(u32 n) { return 1 << n; }
comptime u32 MAX(u32 a, u32 b) {
    if (a > b) { return a; }
    return b;
}
u32 mask = BIT(3);            // → 8 at compile time
u32 big = MAX(10, 20);        // → 20 at compile time
// All args MUST be compile-time constants — variables rejected
// Nested calls work: comptime u32 QUAD(u32 x) { return DOUBLE(DOUBLE(x)); }
// Const idents as args: const u32 P = BIT(0); comptime if (HAS(P, 1)) { ... }

// comptime if — conditional compilation (#ifdef replacement):
comptime if (1) {
    // this code is checked and emitted
} else {
    // this code is stripped entirely
}
// Condition: literal, const, comptime call, or expression of these
// Comptime calls in conditions: comptime if (FUNC()) { ... }
// Comparisons: comptime if (VER() > 1) { ... }
// const from comptime: const u32 P = PLATFORM(); comptime if (P) { ... }
// Only the taken branch is type-checked — dead branch ignored
```

### Thread Safety
```
// shared struct — auto-locked, no annotations needed:
shared struct Counter { u32 value; u32 total; }
Counter g;
g.value = 42;              // auto: lock → write → unlock
g.total = g.value + 1;     // same lock scope (grouped with above)

// spawn — fire-and-forget thread creation:
spawn worker(&g);           // OK — *shared struct, auto-locked
spawn process(&data);       // ERROR — non-shared pointer, data race
spawn handler(42, true);    // OK — value args copied, no sharing

// scoped spawn — ThreadHandle + join (allows *T args):
ThreadHandle th = spawn compute(&work);  // OK — *T allowed, will be joined
th.join();                  // MUST call before function returns
// zercheck: ThreadHandle not joined = compile error

// condvar — thread synchronization:
@cond_wait(shared_var, shared_var.count > 0);  // wait for condition
@cond_signal(shared_var);   // wake one waiting thread
@cond_broadcast(shared_var); // wake all waiting threads
// shared structs using condvar auto-upgrade to pthread_mutex_t

// threadlocal — per-thread storage:
threadlocal u32 counter;    // each thread has its own copy (__thread)

// Lock ordering — deadlock prevention (BUG-464 redesign):
shared struct A { u32 x; }
shared struct B { u32 y; }
A a; B b;
a.x = 1; b.y = 2;          // OK — separate statements, no nested locks
b.y = 2; a.x = 1;          // OK — separate statements, locks released between
a.x = b.y;                 // ERROR — same statement accesses both A and B
```

### Move Struct — Ownership Transfer
```
// move struct: passing or assigning transfers ownership, original invalid
move struct FileHandle { i32 fd; }
FileHandle f;
f.fd = 42;
consume(f);          // ownership transferred
f.fd;                // COMPILE ERROR — use after move

// Assignment also transfers:
move struct Token { u32 kind; }
Token a;
a.kind = 1;
Token b = a;         // ownership transfers to b
a.kind;              // COMPILE ERROR — use after move
```

ZER is copy-by-default (opposite of Rust). `move struct` opts IN to ownership tracking.
Only for types representing unique resources (file descriptors, hardware handles, one-shot tokens).
Uses zercheck's existing HS_TRANSFERRED state — zero new infrastructure.

### Hardware Support
```
volatile *u32 reg = @inttoptr(*u32, 0x4002_0014);  // MMIO register
u32 bits = reg[9..8];                               // bit extraction
interrupt USART1 { handle_rx(); }                    // interrupt handler
packed struct Packet { u8 id; u16 val; u8 crc; }    // unaligned struct
```

### What ZER Does NOT Have
- No classes, inheritance, templates, generics (use `container` keyword for type-stamped containers)
- No exceptions, try/catch
- No garbage collector
- No heap/malloc/free (use Pool/Slab/Ring/Arena)
- No implicit narrowing or sign conversion
- No undefined behavior (overflow wraps, shift by >=width = 0)
- No `++`/`--`, no comma operator
- No C-style casts
- No preprocessor (#define → `comptime` functions, #ifdef → `comptime if`)
- No header files (use `import`)
- No preprocessor (#define, #ifdef)

### Feature Judgement — What NOT to Add

When considering new features, apply the **primitives test**: if the use case can be solved with existing language primitives, it's a library concern, NOT a language feature. ZER's concurrency is complete at the language level:

- **Park/unpark** — DON'T ADD. Condvar already covers thread wake/sleep. Two mechanisms for the same thing = complexity.
- **Future/Pin** — DON'T ADD. ZER async compiles to concrete state machines (Duff's device). No trait abstraction needed — each async function IS its own type. Pin exists in Rust only because borrow checker can't handle self-referential structs.
- **Arc (ref counting)** — DON'T ADD. Pool/Slab/Handle covers allocation. Ref counting adds runtime overhead and cycle leak risk.
- **Async runtime** — DON'T ADD as language feature. User writes the poll loop (bare-metal) or wraps with epoll (Linux). Library concern, not language.
- **Unbounded channel** — DON'T ADD. `Ring(T, N)` is bounded by design — prevents OOM on embedded. Unbounded is a library-level choice.
- **Generics** — DON'T ADD. `container` keyword covers user-defined containers via monomorphization (type stamping). No type constraints, no SFINAE, no template metaprogramming — just text substitution + type checking on the result.
- **Whole-program analysis** — BANNED from architecture. zercheck is per-file with summaries. Heuristics cover callbacks. See design decisions.
- **Partial moves** — DON'T ADD. Copy field value before consuming whole struct. Adds 200 lines of per-field tracking for a one-line workaround.

### Safety Guarantees
| Bug Class | Prevention |
|---|---|
| Buffer overflow | Inline bounds check on every array/slice access; proven-safe indices skip check (range propagation); unsafe indices get auto-guard (silent if-return inserted) |
| Use-after-free | Handle generation counter + ZER-CHECK (MAYBE_FREED + leak detection + loop pass + cross-function summaries) + Level 1-5 *opaque tracking (compile-time zercheck + poison-after-free + inline header + global malloc interception) |
| Null dereference | `*T` non-null by default, `?T` requires unwrapping, local function pointer requires initializer |
| Uninitialized memory | Everything auto-zeroed |
| Integer overflow | Wraps (defined), never UB |
| Silent truncation | Must `@truncate` or `@saturate` explicitly |
| Missing switch case | Exhaustive check for enums and bools |
| Dangling pointer | Scope escape analysis (walks field/index chains, catches struct fields + globals + orelse fallbacks + @cstr buffers + array→slice coercion + struct wrapper returns + @ptrtoint(&local) direct and indirect escape) |
| Union type confusion | Cannot mutate union variant during mutable switch capture |
| Arena pointer escape | Arena-derived pointers cannot be stored in global/static variables (ALL arenas, including global — `is_from_arena` flag) |
| Division by zero | Forced guard (compile error if divisor not proven nonzero); struct fields via compound key range propagation |
| Invalid MMIO address | `mmio` declarations (compile-time) + alignment check + **MMIO index bounds from range** (compile-time) + startup @probe validation (boot-time) + `--no-strict-mmio` for unchecked access |
| Unsafe pointer indexing | Non-volatile `*T` indexing warns "use slice". Volatile `*T` from `@inttoptr` bounds-checked against `mmio` range. |
| Slab alloc in ISR | `slab.alloc()` in interrupt handler → compile error (calloc may deadlock). Use Pool instead. |
| Slab/Pool/Ring from spawn | Global Pool/Slab/Ring accessed from spawned thread → compile error (non-atomic metadata). Use shared struct wrapper or single-threaded access. |
| Container infinite recursion | `container Node(T) { ?*Node(T) next; }` → compile error (depth 32). Prevents compiler hang from self-referential monomorphization. |
| Naked non-asm code | Non-asm/non-return statements in naked function → compile error (stack not allocated without prologue). |
| Comptime loop DoS | Nested comptime loops exceeding 1M total operations → compile error (global instruction budget). |
| Move struct capture copy | `if (opt) \|k\|` value capture of move struct → compile error. Must use `\|*k\|` pointer capture. |
| Async shared struct | Shared struct field access inside async function → compile error. Lock held across yield/await = deadlock. |
| Ghost handle (leaked alloc) | `pool.alloc()` / `slab.alloc()` as bare expression → compile error (handle discarded) |
| Wrong pointer cast | 4-layer: Symbol + compound key + array-level + whole-program param provenance. Runtime `_zer_opaque{ptr, type_id}` for cinclude only |
| Handle leak | zercheck: ALIVE/MAYBE_FREED at function exit = error. Overwrite alive handle = error. Allocation coloring: arena wrappers (chained, type-punned) excluded via source_color + param color inference |
| Wrong container_of | `@container` field validation + provenance tracking from `&struct.field` |
| Volatile/const strip | `@ptrcast`, `@bitcast`, `@cast` all check qualifier preservation |
| ISR data race | Shared global without volatile → error. Compound assign on shared volatile → error (non-atomic read-modify-write) |
| Stack overflow | Recursion detection via call graph DFS → warning. `--stack-limit N` → error when per-function frame or entry-point call chain exceeds N bytes |
| Misaligned MMIO | `@inttoptr` alignment check — address must match target type alignment (u32=4, u16=2, u64=8) |
| @critical escape | `return`/`break`/`continue`/`goto` inside `@critical` → compile error (would skip interrupt re-enable) |
| Interior pointer UAF | `*u32 p = &b.field; free(b); p[0]` → compile error. Field-derived pointers share alloc_id with parent. NODE_INDEX UAF check. |
| Thread data race | `shared struct` auto-locked. Non-shared pointer to `spawn` → compile error (unless scoped with ThreadHandle+join). Handle to `spawn` → compile error. Spawn target body scanned for non-shared global access: error (no sync) or warning (has @atomic/@barrier). Transitive through callees (8 levels). Escape: volatile, shared, threadlocal, @atomic_*. |
| Deadlock | Same-statement multi-shared-type access → compile error. Emitter does lock-per-statement (no nested locks), so cross-statement ordering is safe. Only same-statement access to 2+ shared types is a real deadlock risk. |
| Ownership transfer | `spawn` with non-shared pointer marks variable HS_TRANSFERRED — use after transfer → compile error. |
| Move semantics | `move struct` — pass to function or assign transfers ownership, use after transfer → compile error. Compile-time via zercheck. |
| Thread not joined | Scoped spawn `ThreadHandle` not joined before function exit → zercheck compile error. |
| Spawn in ISR | `spawn` inside `@critical` block → compile error (thread creation with interrupts disabled). |
| Pointer through channel | Ring.push with pointer element type → compile warning (pointer may be invalid in receiver). |

### Implementation Status
| Feature | Checker | Emitter (E2E) |
|---|---|---|
| All primitives, arrays, slices | Done | Done |
| Pointers, optionals, structs | Done | Done |
| Enums, unions, switch | Done | Done |
| Function pointers | Done | Done |
| Pool + Handle | Done | Done |
| Slab (dynamic growable pool) | Done | Done |
| Ring | Done | Done |
| Arena (alloc, alloc_slice, over, reset) | Done | Done |
| Modules/imports | Done | Done (multi-file) |
| Intrinsics (@size, @truncate, etc.) | Done | Done |
| Defer | Done | Done |
| Goto + labels (forward + backward) | Done | Done (C pass-through) |
| Handle auto-deref (h.field) | Done | Done (emits get() call) |
| alloc_ptr/free_ptr (*T from Slab/Pool) | Done | Done (zercheck Level 9) |
| Handle(T)[N] array syntax | Done | Done |
| zercheck 9a: struct field *opaque tracking | Done | N/A (compile-time) |
| zercheck 9b: cross-func *T/pointer summary | Done | N/A (compile-time) |
| zercheck 9c: return freed pointer detection | Done | N/A (compile-time) |
| Task.alloc() / Task.free() (auto-Slab) | Done | Done (auto global Slab per struct) |
| Task.alloc_ptr() / Task.free_ptr() | Done | Done (auto-Slab + alloc_ptr) |
| ZER-CHECK (MAYBE_FREED, leaks, loops) | Done | N/A (analysis pass) |
| ?FuncPtr (optional function pointers) | Done | Done (null sentinel) |
| Function pointer typedef | Done | Done |
| Funcptr typedef ?RetType (*Name)() | Done | Done (BUG-420: ? binds to return type in typedef) |
| Distinct typedef (including func ptrs) | Done | Done |
| cinclude (C header inclusion) | Done | Done |
| Enum explicit values (incl. negative) | Done | Done |
| Named slice typedefs (all types) | Done | Done |
| ?[]T optional slice typedefs | Done | Done |
| Volatile emission | Done | Done |
| Array→slice coercion (call/var/return) | Done | Done |
| Mutable union capture |*v| | Done | Done (pointer to original) |
| mmio range validation | Done | Done (runtime trap for variables) |
| @ptrcast type provenance | Done | N/A (compile-time) |
| @container field+provenance | Done | N/A (compile-time) |
| comptime functions | Done | Done (inlined constants) |
| comptime if (conditional compilation) | Done | Done (dead branch stripped) |
| Value range propagation (bounds/div opt) | Done | Done (proven-safe skip check) |
| Bounds auto-guard (unproven indices) | Done | Done (auto if-return inserted) |
| Forced division guard (ident divisors) | Done | N/A (compile error with fix hint) |
| ZER-CHECK cross-function summaries | Done | N/A (analysis pass) |
| Auto-keep on fn-ptr pointer params | Done | N/A (compile-time) |
| @cstr overflow → auto-return | Done | Done (returns zero value) |
| *opaque array homogeneous provenance | Done | N/A (compile-time) |
| Cross-function *opaque prov summaries | Done | N/A (compile-time) |
| Whole-program *opaque param provenance | Done | N/A (compile-time — call-site validation) |
| Struct field range propagation | Done | Done (guards on s.field work) |
| @probe (safe MMIO read, uintptr_t) | Done | Done (universal signal() fault handler) |
| MMIO startup validation (declared ranges) | Done | Done (constructor, @probe at boot) |
| Universal fault handler (signal) | Done | Done (catches bad MMIO at runtime, any platform) |
| Interrupt safety (ISR shared globals) | Done | N/A (compile-time — missing volatile, race detection) |
| Stack depth analysis (recursion detect) | Done | N/A (compile-time — warning on recursive calls) |
| @critical (interrupt-disabled block) | Done | Done (per-arch interrupt disable/enable) |
| @atomic_* (add/sub/or/and/xor/cas/load/store) | Done | Done (GCC __atomic builtins) |
| Extended asm (GCC operand syntax) | Done | Done (raw pass-through, all archs) |
| naked functions | Done | Done (__attribute__((naked))) |
| section attribute | Done | Done (__attribute__((section))) |
| *opaque Level 1 (zercheck malloc/free) | Done | N/A (compile-time — UAF, double-free, leak) |
| *opaque Level 2 (poison-after-free) | Done | Done (auto ptr=NULL after free) |
| *opaque Level 3+4+5 (inline header+wrap) | Done | Done (--track-cptrs, --wrap=malloc) |
| MMIO index bounds from mmio range | Done | N/A (compile-time — mmio_bound on Symbol) |
| Pointer indexing warning (non-volatile) | Done | N/A (compile-time warning) |
| Slab.alloc() banned in ISR | Done | N/A (compile-time error) |
| Ghost handle (discarded alloc) | Done | N/A (compile-time error) |
| shared struct (auto-locking) | Done | Done (spinlock, block-level grouping) |
| spawn (thread creation) | Done | Done (pthread_create, contextual keyword) |
| Scoped spawn (ThreadHandle+join) | Done | Done (pthread_join, proper wrappers) |
| Condvar (@cond_wait/signal/broadcast) | Done | Done (pthread_cond_t, auto mutex upgrade) |
| threadlocal | Done | Done (__thread emission) |
| Deadlock detection (lock ordering) | Done | N/A (compile-time error) |
| Ring channel pointer warning | Done | N/A (compile-time warning) |
| @once { } (init once) | Done | Done (atomic CAS) |
| @cond_timedwait (timeout) | Done | Done (pthread_cond_timedwait) |
| @barrier_init/wait (sync point) | Done | Done (mutex+condvar, Barrier keyword type) |
| async/await (stackless coroutines) | Done | Done (Duff's device state machine) |
| Allocation coloring (source_color) | Done | N/A (compile-time — arena wrappers, param inference) |
| `move struct` (ownership transfer) | Done | N/A (compile-time — zercheck HS_TRANSFERRED) |
| Semantic fuzzer (32 generators) | Done | N/A (200 tests per make check) |
| Designated initializers (`{ .x = 10 }`) | Done | Done (C99 compound literal) |
| `container` keyword (monomorphization) | Done | Done (stamped struct per type arg) |
| `do-while` loop | Done | Done (direct C pass-through) |
| Comptime array indexing | Done | N/A (compile-time — array bindings in ComptimeCtx) |
| Comptime struct return | Done | Done (NODE_STRUCT_INIT inlined at call site) |
| Compound literals in call args | Done | Done (struct_init validated in NODE_CALL) |
| Comptime enum values | Done | N/A (compile-time — resolve_enum_field in eval_const_expr_scoped) |
| Comptime float arithmetic | Done | Done (parallel float eval path, %.17g emission) |
| `Semaphore(N)` builtin type | Done | Done (@sem_acquire/@sem_release, *Semaphore pointer params) |
| Function summaries (FuncProps) | Done | N/A (compile-time — context safety: transitive yield/spawn/alloc detection) |
| IR Phase 1-9 (data structures, lowering, 3AC, emission, zercheck, VRP) | Done | Done. **IR IS DEFAULT for all emission paths (binary AND test harnesses) since 2026-04-17. ALL test suites at 100% on IR path: test_emit 238/238, firmware 102/102, production 14/14, rust 786/786, zer 277/277, zig 36/36, module 28/28, checker 584/584, zercheck 54/54.** Phase 8: (a) on-demand locals, ident rewriting, scope conflict BUG-507, async yield BUG-508/509/510. (b) `lower_expr()` wired for ident/literal/binary/unary/field/index/string → IR_COPY + 3AC ops emit from local IDs. (c) **`can_lower_expr()` DELETED, `emit_ir_value()` DELETED (Phase 8d).** `lower_expr` is unconditional — ALL expressions get local IDs. Complex expressions (calls, intrinsics, builtins, orelse, casts, struct-init) passthrough via `IR_ASSIGN{dest,expr}` → emitter calls `emit_expr` directly. All other IR ops (BRANCH, RETURN, CALL, AWAIT, BINOP, UNOP, FIELD_READ, INDEX_READ, builtins, INTRINSIC) use local IDs exclusively via `emit_local_name()` helper. **`emit_expr` in `emit_ir_inst`: ZERO.** `emit_rewritten_node(e, node, func)` handles ALL expression types directly. `emit_builtin_inline(e, node, func)` handles pool/slab/ring/arena/Task builtins. Condvar pointer vs struct accessor detection. @atomic_* via __atomic builtins. @cstr/@cond_wait/signal/broadcast/timedwait/@sem_*/@probe direct emission. **Cross-module name mangling:** functions use symbol->module_prefix, variables use e->current_module, static vars fallback to current_module. **100% IR for function bodies: ZERO emit_stmt, ZERO emit_expr.** AST ONLY for top-level declarations. Switch (enum/union/optional) emitted directly: enum `_ZER_Name_variant` if/else chain, union `._tag` + memcpy capture, optional `.has_value`/`.value`. Arm bodies handle var-decl/defer/if(+else)/return(optional wrap)/expr-stmt. **Scoped defer fire:** loops, if-bodies, switch arms all save `defer_count` at entry, emit scoped `IR_DEFER_FIRE` + pop at exit. `src2_local=1` flag on IR_DEFER_FIRE = no-pop variant for break/continue. BUG-513 through BUG-557. **All integration tests green: 281 ZER (279 + 2 new: scope_shadow_typecast, barrier_ir_emit), 28 module, 786 rust, 36 zig, 584 checker, 54 zercheck, 39+41+22 firmware, 14 production.** BUG-573 (rewrite_idents missed NODE_TYPECAST — suffixed local leaked), BUG-574 (@barrier_init/@barrier_wait silently became no-ops on IR passthrough — emit_rewritten_node missed them), BUG-575 (labels[128] silent-drop fixed per CLAUDE.md rule #7 — stack-first dynamic), BUG-576 (ir_validate gaps closed: cond_local/src1/src2 checks + 0-sentinel fix). |

### Architecture Decision: Emit-C Permanently (decided 2026-03-25)

ZER will **NOT** have native backends, IR, or QBE. Emit-C via GCC is the permanent architecture.
- GCC handles all embedded targets, provides decades of optimization, acts as a second validation layer
- No IR layer planned — emit-C → GCC is the only compilation path
- **Never suggest IR, LLVM, QBE, or native code generation in this project**

### Self-Hosting Strategy (decided 2026-03-27)

**Develop in C, prove in ZER.** The compiler is always written in C (`.c` files). LLMs assist development. Fast iteration.

Self-hosting means zerc can emit a working copy of itself in ZER (`zerc.zer`), compile that copy, and the result is an identical compiler. The ZER mirror is the ultimate integration test — NOT the primary development codebase.

```
src/                ← active C development (LLM-assisted, fast)
  lexer.c, parser.c, checker.c, emitter.c, ...
zer-src/            ← ZER mirror (proof of self-hosting)
  lexer.zer, parser.zer, checker.zer, emitter.zer, ...
bootstrap/          ← frozen C snapshot (insurance, never touched)
  lexer.c, parser.c, ...
```

**Build chain:**
```
gcc src/*.c -o zerc              ← build from C (primary)
./zerc zer-src/main.zer -o zerc_zer.c  ← compile ZER mirror
gcc zerc_zer.c -o zerc2          ← build from emitted C
diff zerc zerc2                  ← identical = v1.0 proven
```

**Why not develop in `.zer` directly:**
- LLMs have zero ZER training data — they write C syntax and call it ZER
- C development is faster with LLM assistance today
- ZER development becomes viable when users post real ZER code online (trains future models)
- The C codebase is the productive path; the ZER mirror is the correctness proof

**v1.0 definition:** `zerc.zer` compiles itself and produces an identical compiler.

**Migration to `.zer`-primary happens when:** ZER has enough public code (GitHub repos, blog posts, Stack Overflow) that LLMs learn the syntax natively. Until then, C is the development language.

**Roadmap:**
- **v0.2 (RELEASED):** Slab(T), volatile slices, stdlib (str/fmt/io), bundled GCC, zer-convert Phase 1+2
- **v0.2.1:** comptime functions + comptime if, 4-layer MMIO safety, @ptrcast/@container provenance, safe intrinsics, zer-convert P0+P1, value range propagation, bounds auto-guard, forced division guard, zercheck 1-4, 415+ bug fixes, 1,700+ tests
- **v0.2.2:** FULL CONCURRENCY: shared struct (auto-locking), shared(rw) (rwlock), spawn (fire-and-forget + scoped ThreadHandle+join), deadlock detection (compile-time lock ordering), condvar (@cond_wait/signal/broadcast/timedwait), threadlocal, @once, @barrier_init/wait, async/await (stackless coroutines via Duff's device), Ring channel pointer safety, allocation coloring, semantic fuzzer (32 generators), 461+ bug fixes, 3,200+ tests (incl. 400 Rust-equivalent safety/concurrency tests)
- **v0.3.0 (CURRENT):** `move struct`, `Barrier` keyword type, comptime locals/loops/switch/arrays/struct-return/float/enum, `static_assert`, range-based `for (T item in slice)`, `do-while`, designated initializers + compound literals, `container` keyword (monomorphization), `--stack-limit N`, spawn global data race detection (error/warning), 786 Rust tests + 36 Zig tests + 195 ZER integration + 68 ZER negative (0 failures), flag-handler matrix audit tool (`tools/audit_matrix.sh`) found 5 missing checker validations, ctags-guided audit found 3 emitter bugs in ~5K tokens, red team audit: 42/81 Gemini attacks fixed (12 rounds) + 2 codebase analysis finds + full 25K-line audit, `Semaphore(N)` builtin, BUG-462 through BUG-506 (46 bugs fixed), systematic refactoring (25 unified helpers — R1-R3 + B1 `track_dyn_freed_index` + B2 `check_union_switch_mutation` + B4 `emit_opt_wrap_value` + B10 `handle_key_arena` + 18 prior), full codebase audit (25,757 lines): 15 distinct unwrap fixes (BUG-506 + A15/A19/A20), 5 buffer over-read fixes, 2 fixed arrays → dynamic, 2 volatile temp fixes, spawn string+struct validation, orelse emission consolidated (B3), return wrapping consolidated (B7), union typedef macro (B8), zercheck 27 arena keys (B10), zig test runner (36 tests automated), refactor plan in `docs/ZER_Refactor.md`, CFG-aware zercheck with scope-aware handle tracking (`find_handle` vs `find_handle_local`), recursive mutex with CAS lazy init, unified `emit_file_module`, VRP 100% via address_taken at TOK_AMP + compound assign invalidation, deadlock call graph DFS, async state struct temp promotion (Rust MIR-equivalent), `*opaque` comparison `.ptr` extraction, runtime MMIO alignment check, C interop safety model (`cinclude` + `*opaque` + `shared struct`), 510+ bug fixes, 4,000+ tests, FuncProps function summaries (tracking system #29 — transitive context safety via lazy DFS on Symbol)
- **v0.4:** MIR-inspired IR (flat locals, basic blocks) — replaces 29 AST walkers with one lowering pass. zercheck on CFG, VRP on SSA-like locals, async complete by construction. Still emits C → GCC. See `docs/IR_Implementation.md`
- **v1.0:** self-hosting proof (zerc.zer compiles itself identically)


### C-to-ZER Conversion Tools (implemented v0.2)

Two tools + one library for automated C-to-ZER migration. Full architecture docs in `docs/compiler-internals.md`.

**`tools/zer-convert.c`** — Phase 1: C syntax → ZER syntax (token-level transform)
- Types, operators, casts, sizeof, struct/enum/union keyword removal
- switch/case/break → ZER `.VALUE => {}` syntax (nested, multi-case fallthrough)
- typedef struct → struct Name, do-while → while(true), void* → *opaque, void** → **opaque
- Pointer decl rearrangement: `int *ptr` → `*i32 ptr` (multi-level, return types)
- Usage scanner `classify_params`: char* → []u8 (string), ?*u8 (nullable), *u8 (write-through)
- Pointer arithmetic: `ptr + N` → `ptr[N..]`, `*(ptr + N)` → `ptr[N]`
- Auto-extraction: ternary/goto/bitfields/asm → companion `_extract.h` via cinclude
- Preprocessor → comptime: `#define MAX(a,b)` → `comptime u32 MAX(u32 a, u32 b)`, `#ifdef` → `comptime if`, `#endif` → `}`, `#define GUARD` → `const bool GUARD = true;`
- Qualifier handling: `volatile` preserved and reordered, `extern`/`inline`/`restrict`/`register` stripped
- MMIO casts: `(uint32_t*)0xADDR` → `@inttoptr(*u32, 0xADDR)`, `(volatile uint32_t*)0xADDR` same
- Pointer-to-int: `(uintptr_t)ptr` → `@ptrtoint(ptr)`, `uintptr_t`/`intptr_t` → `usize`
- Number suffixes: C suffixes (U, L, UL, ULL) stripped from literals
- Include guards: `#ifndef FOO_H / #define FOO_H` detected and stripped
- Unconvertible macros: stringify (`#`), token paste (`##`), variadic (`__VA_ARGS__`) → auto-extracted to companion `_extract.h` via cinclude (zero manual work)
- `#if defined(X)` → `comptime if (X)` (expands `defined()` operator)

**`tools/zer-upgrade.c`** — Phase 2: compat builtins → safe ZER (source-to-source)
- Layer 1: strlen→.len, strcmp→bytes_equal/bytes_compare, memcpy→bytes_copy, memset→bytes_zero, strcpy/strncpy→@cstr
- Layer 2: malloc/free → Slab(T) with Handle rewriting (cross-function, struct fields, local vars)
- Post-processing: slab declarations, signature rewriting, import management

**Pipeline:** `input.c → zer-convert → input.zer → zer-upgrade → input_safe.zer`
- Multi-file: each .c converts independently, types shared via `cinclude "header.h"`
- For full ZER safety: replace `cinclude` with `import` (manual, one line per file)
- 139 regression tests in `tests/test_convert.sh` (was 108, +31 for P0/P1 fixes)

### Compiler Internals — MANDATORY READING

**MANDATORY — read `docs/compiler-internals.md` BEFORE modifying any compiler source file** (parser.c, checker.c, emitter.c, types.c, zercheck.c). It documents every emission pattern, optional handling, builtin method interception, scope system, type resolution flow, and common bug patterns. Skipping this and discovering patterns by reading source files wastes 20+ tool calls. The document exists specifically to prevent this.

Contents of `docs/compiler-internals.md`:

**Emitter Critical Patterns (causes of most bugs):**
- `?*T` → plain C pointer (NULL sentinel). `?T` (value) → struct with `.has_value`/`.value`. `?void` → NO `.value` field.
- Bounds checks: simple index uses comma operator (lvalue), side-effect index uses GCC statement expression (single-eval).
- Slice types: named typedefs `_zer_slice_T` for all types. `?[]T` → `_zer_opt_slice_T`.
- Builtin methods: Pool/Ring/Slab/Arena intercepted in emitter NODE_CALL handler.
- Function pointer syntax: name inside `(*)` — handled by `emit_type_and_name`.

**Critical Bug Fix Patterns (1-181):**
All numbered patterns from BUG-042 through BUG-337. Key themes:
- `?void` has ONE field (has_value), everything else has TWO — check at every optional null emission site
- `TYPE_DISTINCT` must be unwrapped before ANY type dispatch — use `type_unwrap_distinct()`. **This was the #1 bug class: 35+ sites fixed in one session.** Every `->kind == TYPE_X` on a type from `checker_get_type()` / `check_expr()` needs unwrap. The helpers `type_is_optional()`, `type_is_integer()`, `type_width()` unwrap internally. `type_is_optional()` and `type_unwrap_optional()` also unwrap distinct now.
- `is_null_sentinel()` function (not macro) unwraps distinct before checking pointer/func_ptr
- Named typedefs required for EVERY compound type — prevents anonymous struct duplication
- `__typeof__` instead of `__auto_type` for captures/orelse temps — preserves volatile/const
- Module mangling uses `__` (double underscore) separator — 8 sites in checker + emitter
- Scope escape checks cover return, assignment, keep params, orelse fallback, @ptrcast
- Union variant lock walks ALL deref/field/index levels and detects pointer-type aliases

**Structural Refactors (RF1-RF12):**
- RF1: Typemap in Checker struct (not globals). RF2: Unified `emit_top_level_decl()`.
- RF3: `resolve_type()` caches in typemap. RF4: Arena-allocated mangled names.
- RF5: Lightweight parser lookahead. RF8: `CONST_EVAL_FAIL` sentinel.
- RF9: Dynamic parser arrays. RF10: `is_func_ptr_start()` consolidated.
- RF11: Shared `expr_is_volatile()` / `expr_root_symbol()` helpers.
- RF12: `build_expr_key_a()` arena-allocated expr keys (no fixed `char[128]` buffers). Dynamic `ComptimeParam` arrays (stack-first `[8]` with arena overflow).

**Design Decisions (intentional, NOT bugs):**
- `@inttoptr(*T, 0)` allowed (MMIO address 0x0), shift widening spec-correct, `[]T → *T` coercion removed
- `HS_MAYBE_FREED` conservatism: if handle freed in one branch of if-without-else → MAYBE_FREED → compile error on subsequent use. Fix: add `return;` after free. This forces better code structure. NOT a borrow checker — ZER's Handle model is simpler (indices, not references). Borrow checking would add lifetime annotations for zero additional safety since Handle+zercheck already covers all cases.
- Type ID 0 for `*opaque` provenance: extern/cinclude pointers get type_id=0 (unknown). `@ptrcast` check skips type_id==0 to allow C interop. Not a security hole — C code is outside ZER's safety boundary. Future `--strict-interop` flag could force explicit type assignment.
- No pointer arithmetic: `ptr + N` deliberately rejected. Use `ptr[N]` for pointer indexing, `@ptrtoint` + math + `@inttoptr` for MMIO. Safety feature, not a gap.
- Atomic width validation: `@atomic_*` on 64-bit targets warns about libatomic requirement on 32-bit platforms (AVR, Cortex-M0). Width must be 1/2/4/8 bytes.
- `pool.get(h)` is intentionally verbose — Handle is `u64` (index+gen), carries no pool reference. BUT `h.field` now works via Handle auto-deref (compiler finds the unique Slab/Pool in scope, auto-inserts `.get(h)`). If multiple allocators for same type exist, uses `slab_source` provenance or unique-allocator fallback. Ambiguous = compile error.
- `alloc_ptr()` / `free_ptr()` — alternative to Handle that returns `*T` directly. Same zercheck tracking (ALIVE/FREED/MAYBE_FREED). 100% compile-time safe for pure ZER code. For C interop (`*opaque` boundary), Level 2+3+5 runtime backup. Both Handle and alloc_ptr can be used on the same Slab/Pool. `free_ptr()` type-checks argument — `*Motor` to `Task` pool is a compile error.
- zercheck `*opaque` coverage: 9a (struct field `ctx.data` after free = compile error), 9b (cross-function free via FuncSummary — `destroy(t)` marks `t` FREED at call site), 9c (returning freed pointer = compile error). Coverage ~98% compile-time for `*opaque`, remaining ~2% is runtime (dynamic array index, C library boundary) at ~1ns inline header check.
- Handle auto-deref safety: const Handle mutation blocked, allocator existence verified at check time (no silent `0` emission), ghost handle check covers `alloc_ptr()`.
- goto + defer interaction: goto fires all pending defers before jumping (same as return/break/continue). goto/label validation covers switch arms, defer bodies, @critical bodies.
- **Known limitation (partially resolved):** zercheck is linear (not CFG-based). Same-block backward goto UAF now caught via 2-pass re-walk (2026-04-06). Cross-block backward goto (goto from nested if-body to parent label) still falls back to runtime gen check.
- **Interior pointer UAF tracking (2026-04-07):** `*u32 p = &b.field; free_ptr(b); p[0]` — field-derived pointer shares alloc_id with parent. When parent freed, all interior pointers marked FREED. NODE_INDEX added to zercheck UAF checks (was missing — only NODE_FIELD and NODE_UNARY/deref were checked). Remaining gap: `@ptrtoint` + math + `@inttoptr` — guarded by mmio declaration requirement, not pointer tracking.
- `Task.alloc()` / `Task.free()` — auto-creates a global `Slab(T)` per struct type. No Slab declaration needed. Returns `?Handle(T)`. `Task.alloc_ptr()` returns `?*T`. `Task.free_ptr()` type-checks argument. Auto-slabs emitted after struct declarations, before function bodies (two-pass emission). One auto-Slab per struct type, program-wide.
- **Emitter two-pass:** `emit_file` now emits struct/enum/union/typedef declarations first (pass 1), then auto-slab globals, then functions/globals (pass 2). This ensures auto-slabs can use `sizeof(StructType)`.
- **CRITICAL BUG (fixed):** orelse block path (`orelse { return; }`) emitted `0` as final statement expression value for null-sentinel `?*T`. This made ALL `*T = alloc_ptr() orelse { return; }` assign 0 instead of the pointer. The bare form (`orelse return`) was correct — only the block form was broken. Fixed: emit `_zer_tmp` for null-sentinel, `_zer_tmp.value` for struct optional.
- **Real code finds real bugs:** Writing a 60-line HTTP server in ZER found this orelse bug that 1,700+ tests missed. All tests used bare `orelse return`, none used block `orelse { return; }`. Lesson: write real programs in ZER, not just tests.
- zercheck now scans defer bodies for free/delete calls — `defer pool.free(h)` no longer triggers false "never freed" warning. Also: if-branch that always exits (return/break/continue) doesn't cause MAYBE_FREED — `if (err) { free(h); return; } use(h);` is correctly seen as safe.
- **CRITICAL BUG (fixed):** Auto-slab initializer used positional `{sizeof(T), 0, 0, ...}` which put sizeof into wrong field (`pages` instead of `slot_size`). Fix: designated initializer `{ .slot_size = sizeof(T) }`. Same pattern as normal Slab emission. All `Task.alloc()` was broken until this fix.
- **Lesson: always use designated initializers for emitted struct init.** Positional initializers are fragile — field order in `_zer_slab` doesn't match naive assumption. Future emitter code MUST use `.field = value` syntax, never positional.
- `const Handle(Task)` allows data mutation through auto-deref. Handle is a KEY (like `const int fd`), not a pointer. const key ≠ const data. Assignment checker sets `through_pointer = true` for TYPE_HANDLE in field chain. This also fixes if-unwrap `|t|` + Handle auto-deref (capture is const but data mutation is allowed).
- zercheck recognizes `Task.free()` / `Task.free_ptr()` as free calls (TYPE_STRUCT method detection). Also `Task.alloc()` / `Task.alloc_ptr()` as alloc calls. ISR ban applied to Task.alloc/new_ptr same as slab.alloc.
- Pool/Slab/Arena are NOT the same thing with different names — Pool (fixed, ISR-safe, no malloc), Slab (dynamic, grows via calloc, NOT ISR-safe), Arena (bump allocator, bulk reset). Don't rename or unify them.
- `pool.get()` is non-storable — `*Task t = pool.get(h)` is a checker error. Must use inline: `pool.get(h).field`. BUT scalar field values CAN be stored: `u32 v = h.value;` works (Handle auto-deref reads the value, not the pointer). Only pointer/slice/struct/union results from get() are blocked.
- Array→slice auto-coercion at call sites already works: `process(arr)` where `process([]u8 data)` auto-converts `u8[N]` to `[]u8 {ptr, len}`.

**Known Technical Debt:**
- **Qualified module call syntax supported:** `config.func()` rewrites to unqualified `func()` with mangled lookup. Works for regular functions and comptime functions. Unqualified calls still work as before.
- checker.c is 6700+ lines (monolith) — works but large to navigate. Split not urgent.
- zercheck was NOT integrated into zerc until 2026-04-03. Now runs in pipeline: checker → zercheck → emitter. **Leaks are compile ERRORS** (since 2026-04-06 alloc_id redesign). alloc_id grouping eliminates false positives from `?Handle`/`Handle` pairs. Escape detection covers return, global store, param field store, and untrackable array assignment. UAF, double-free, and leaks are all compile errors.

**Internal Quality Notes (verified 2026-04-02):**
- Static globals (`_comptime_global_scope`, `_comptime_call_depth`) are safe — LSP is single-threaded, no concurrency
- Parser arrays are already dynamic (RF9) — stack-first `[32]` with arena overflow doubling
- Checker key buffers are now arena-allocated (RF12) — no fixed `char[128]` limits remain
- Symbol lookup is `memcmp`-based — standard for C compilers (GCC/TCC do the same)
- ZER does NOT need: lifetime/borrow tracking (memory model too simple for it), generics (`*opaque`+provenance covers it), incremental compilation (emit-C + Makefile handles it), closures (function pointers + `*opaque` context is the C pattern)
- **Bounds safety philosophy: prove, don't guard.** When array index can't be proven safe, prefer extending range propagation to make it provable (zero overhead) over adding auto-guard/bounds-check (runtime overhead). `arr[func()]` with proven return range = zero overhead. Auto-guard is the fallback, not the goal. For loop conditions (while/for), auto-guard can't work (value changes each iteration) — instead extend `find_return_range` to prove more call chains. Inline `_zer_bounds_check` trap is correct for genuinely unprovable cases (external input).
- **`find_return_range` chains:** `get_slot()` → `raw_hash()` → `% N` — return range propagates through call chains.
- **Leak detection: track ALLOCATIONS not variable names.** `alloc_id` on HandleInfo groups `?Handle mh` and `Handle h` as the same allocation. When any alias is freed/escaped, the whole group is covered. Previous approach (tracking variable names independently) caused false positives — `mh` and `h` were "strangers" even though they're the same allocation. The alloc_id redesign eliminated ALL false positives and enabled upgrading leaks from warnings to compile errors. Constant returns (`return 0`), `% N`, `& MASK`, and chained calls all handled. Order-dependent: callee checked before caller (declaration order). Cross-module: topological order ensures imported function ranges available.

**Error Display:**
- Errors show source line + caret underline: `3 | arr[10] = 1;` / `  | ^^^^^^^^^^^^`
- `Checker.source` and `Parser.source` fields hold source text (NULL = skip display)
- `print_source_line()` in checker.c, `print_source_line_p()` in parser.c (same logic, separate to avoid shared header)
- `zerc_main.c` passes `Module.source` to both parser and checker, switches `checker.source`/`checker.file_name` when checking imported module bodies
- Tests and LSP set source=NULL (memset zero) — source line display silently skipped
- Carets underline from first non-whitespace to end of line (max 60 chars)

**Benchmark Results (Docker x86, gcc -O2, 2026-04-02):**
- Bounds check: ~0-4% overhead (branch predictor eliminates always-false check)
- Division guard: ~0-5% overhead (single branch, always predicted not-taken)
- Shift safety: ~0% overhead (ternary compiles to cmov, no branch)
- Handle gen check: ~60-130% in synthetic microbenchmark (tight loop doing nothing but pool.get), <5% in real code with actual computation per access
- Benchmarks in `benchmarks/` directory (not committed — local only)

**Bugs Found by Writing Real ZER Code (2026-04-02):**
- `const u32 MAP_SIZE = 16; h % MAP_SIZE` → false "not proven nonzero". Root cause: `eval_const_expr` doesn't resolve `NODE_IDENT`, and `sym->func_node` was never set for `NODE_GLOBAL_VAR` in `register_decl`. Fix: (1) add const symbol init lookup in division guard path, (2) set `sym->func_node = node` for global vars. Both `/` `%` and `/=` `%=` paths fixed.
- `#line` directive emitted on same line as `{` in orelse-return defer path → GCC "stray #" error. All `emit(e, "{ "); emit_defers(e);` sites changed to `emit(e, "{\n"); emit_defers(e);` (var-decl orelse return, auto-guard return, orelse break/continue).
- Windows `zerc --run`: quoting `"gcc"` in `system()` breaks `cmd.exe`. Fix: only quote gcc path if it contains spaces (bundled path vs system PATH).
- `pool.get()` is non-storable — `*Task t = pool.get(h)` is a checker error. Must use inline: `pool.get(h).field`.
- `[]T → *T` auto-coerce at call sites for extern C functions only. `puts("hello")` works without `.ptr` when `puts` is forward-declared with no body. ZER-to-ZER calls still require explicit `.ptr`. Emitter already had `.ptr` emission (line ~1265), checker was blocking it.
- Range propagation now derives bounds from `x % N` → `[0, N-1]` and `x & MASK` → `[0, MASK]`. Resolves const symbol init values. Eliminates false "index not proven" warnings for hash map `slot = hash % TABLE_SIZE; arr[slot]` pattern. `derive_expr_range()` helper used at both var-decl init and assignment paths.

**ZER Integration Tests (`tests/zer/`):**
- Real `.zer` files compiled with `zerc --run`, must exit 0
- Runner: `tests/test_zer.sh`, added to `make check`
- Current tests: hash_map, ring_buffer, pool_handle, enum_switch, union_variant, defer_cleanup, extern_puts, hash_map_chained, tracked_malloc, arena_alloc, comptime_eval, bit_fields, optional_patterns, star_slice, goto_label, goto_switch_label, goto_defer, handle_autoderef, handle_autoderef_pool, alloc_ptr, alloc_ptr_pool, alloc_ptr_mixed, alloc_ptr_stress, handle_complex, handle_array, opaque_safe_patterns, task_new, task_new_ptr, task_new_complex, orelse_block_ptr, task_new_orelse, const_handle_ok, handle_if_unwrap, volatile_div, orelse_void_block, volatile_orelse, comptime_const_if, optional_null_init, comptime_if_call, opaque_level1, opaque_level2, opaque_level345, opaque_level9_complex, opaque_cross_func, opaque_mixed_features, handle_scalar_store, const_slice_return, distinct_funcptr_nested, void_optional_init, distinct_optional, distinct_optional_full, distinct_types, distinct_slice_ops, funcptr_array, defer_free, if_exit_free, optional_array, distinct_optional_ptr, volatile_field_array, comptime_negative, comptime_pool_size, const_slice_field, comptime_nested_call, funcptr_struct_reduce, alloc_ptr_func, multi_slab_cross, nested_struct_deref2, nullable_funcptr, recursive_functions, pool_exhaustion, comptime_signed, slice_subslice, bit_extract_set, packed_struct, atomic_ops, container_offset, bang_integer, forward_decl, critical_block, bitcast_int, array_3d, union_array_variant, comptime_const_arg, opaque_ptrcast_roundtrip, dyn_array_guard, autoguard_intrinsic, critical_handle, distinct_union_assign, goto_backward_safe, inline_call_range, inline_range_deep, guard_clamp_range, keep_store_global, driver_registry, defer_return_order, typecast_cstyle, defer_multi_free, super_defer_complex, super_plugin, super_ecs, super_interpreter, super_freelist, super_state_machine, super_hashmap, goto_spaghetti_safe, interior_ptr_safe, typecast_safe_complex, super_uart_parser, super_sensor_logger, super_freelist_arena, shared_struct, scoped_spawn, condvar_signal, rwlock_shared, once_init, async_coroutine, desig_init, container_stack, do_while_loop, comptime_array, comptime_struct_ret, async_do_while_yield, async_range_for_yield, async_container_yield, async_desig_init_yield, async_typecast_yield, async_intrinsic_yield, distinct_array, distinct_enum_switch, distinct_pointer_qual, distinct_bool, distinct_float, distinct_handle
- Negative tests: uaf_handle, double_free, maybe_freed, bounds_oob, div_zero, null_ptr, dangling_return, isr_slab_alloc, ghost_handle, goto_bad_label, alloc_ptr_uaf, alloc_ptr_double_free, opaque_struct_uaf, opaque_return_freed, opaque_alias_uaf, opaque_maybe_freed, opaque_double_free, cross_func_free_ptr, free_ptr_wrong_type, handle_no_allocator, ghost_alloc_ptr, task_delete_double, task_delete_uaf, opaque_cross_func_uaf, opaque_task_delete_ptr_uaf, bitcast_width, array_return, float_switch, return_in_defer, asm_not_naked, ptrcast_strip_volatile, narrowing_coerce, dyn_array_loop_freed, critical_return, critical_break, goto_backward_uaf, nonkeep_store_global, goto_spaghetti_uaf, goto_spaghetti_swap, goto_circular_swap_uaf, goto_maybe_freed_branch, interior_ptr_uaf, interior_ptr_func, typecast_provenance, typecast_volatile_strip, typecast_const_strip, typecast_direct_ptr, typecast_int_to_ptr, typecast_ptr_to_int, arena_global_escape, shared_field_ptr, spawn_nonshared_ptr, spawn_no_join, async_critical_yield, async_spawn_inside, critical_yield_transitive, critical_spawn_transitive, defer_yield_direct, defer_yield_transitive
- Module tests (`test_modules/`): main, app, diamond, use_types, use_defs, diamond2, collision_test, static_coll, gcoll, transitive, use_hal, opaque_wrap, opaque_wrap_df (negative), opaque_wrap_uaf (negative)
- Examples (not in automated tests): `examples/http_server.zer` — minimal HTTP server, needs network
- Add new tests by dropping `.zer` files in `tests/zer/` — runner picks them up automatically

### Test Locations Summary
| Directory | What | Count | Runner |
|---|---|---|---|
| `tests/zer/` | ZER integration tests (positive — must compile + run + exit 0) | 195 | `tests/test_zer.sh` |
| `tests/zer_fail/` | ZER negative tests (must fail to compile) | 74 | `tests/test_zer.sh` |
| `test_modules/` | Multi-file module tests | 66 | `test_modules/run_tests.sh` |
| `rust_tests/` | Rust test/ui translations ONLY | 786 | `rust_tests/run_tests.sh` |
| `zig_tests/` | Zig test translations ONLY | 36 | `zig_tests/run_tests.sh` |
| `test_*.c` | C unit tests (lexer/parser/checker/emitter/zercheck/fuzz) | ~1,900 | `make check` (compiled + run) |
| `examples/qemu-cortex-m3/` | Real firmware examples (QEMU Cortex-M3 + hosted) | 8 | Manual (`make qemu` or `zerc --run`) |

All runners auto-detect positive vs negative tests. `make check` runs everything.

**Firmware examples (v0.3 feature coverage):**
- `hello.zer` — MMIO, UART, enum, orelse, bounds check (v0.2)
- `rtos.zer` — Pool, Ring, Arena, Handle, spawn, union, defer (v0.2)
- `shell.zer` — Pool, Ring, Arena, spawn, switch (v0.2)
- `ringbuf_protocol.zer` — Ring, Arena, Handle, enum (v0.2)
- `stress_test.zer` — Arena, Handle, union, defer (v0.2)
- `async_sensor.zer` — async/yield, comptime, container(T), move struct, designated init (v0.3)
- `concurrency_demo.zer` — shared struct, Semaphore, @once, @critical, spawn+join (v0.3)
- `slab_registry.zer` — Slab(T), alloc_ptr/free_ptr, defer, comptime, enum switch (v0.3)

### Test Organization Rules — Where to Put New Tests

**By origin — which folder:**
| Test Origin | Positive → | Negative → |
|---|---|---|
| Bug fix / red team audit / ZER-specific safety | `tests/zer/` | `tests/zer_fail/` |
| Multi-module / import / cross-file | `test_modules/` | `test_modules/` (with `_negative` suffix) |
| Translated from Rust `tests/ui/` | `rust_tests/rt_*.zer` | `rust_tests/reject_*.zer` |
| Translated from Zig test suite | `zig_tests/zt_*.zer` | `zig_tests/zt_reject_*.zer` |
| C unit test (parser/checker/emitter) | `test_*.c` | `test_*.c` (err() helper) |

**The rule:** If a test is translated from Rust/Zig source, it goes in `rust_tests/` or `zig_tests/`. If it's a ZER-original test (bug fix, red team finding, feature test), it goes in `tests/zer/` or `tests/zer_fail/`. Don't put ZER-specific tests in `rust_tests/` — that directory is for Rust-equivalent translations only.

**Naming:**
- `tests/zer/feature_name.zer` — positive (compile + run + exit 0)
- `tests/zer_fail/feature_name.zer` — negative (must fail to compile, add `// EXPECTED: compile error` as first line)
- `rust_tests/rt_category_desc.zer` — positive Rust translation
- `rust_tests/reject_category_desc.zer` — negative Rust translation
- `test_modules/name.zer` — multi-file module test

**Bugs Fixed This Session (2026-04-02):**
- `?Handle(T)` struct field double-wrap: emitter var-decl init wrapped already-optional value in `{value, 1}`. Fix: check `init_type->kind == TYPE_OPTIONAL` before wrapping (same pattern as BUG-032 for NODE_IDENT). `hash_map_chained.zer` now uses `?Handle(Node) next` directly.
- Cross-function range propagation: `find_return_range()` scans function bodies for return expressions with `% N` or `& MASK`. Stores `return_range_min/max` on Symbol. Call sites propagate range to variables. `slot = hash(key)` where `hash()` returns `h % TABLE_SIZE` → slot proven `[0, TABLE_SIZE-1]`.
- `--run` absolute path: skip `./` prefix for paths starting with `/` (Linux) or drive letter `C:` (Windows).
- `[]T → *T` extern auto-coerce const safety: string literals and const slices to non-const `*T` param now rejected. Must declare `const *T`. Prevents `.rodata` write-through.

**VS Code Extension (VSIX) Build:**
- `make docker-vsix` — builds complete VSIX with bundled `zerc.exe`, `zer-lsp.exe`, and portable GCC (w64devkit)
- `Dockerfile.vsix` — gcc:13 + mingw cross-compile + Node.js/vsce + w64devkit portable GCC + librsvg2 for SVG→PNG icon conversion
- Extension auto-detects bundled binaries: `bin/win32-x64/zerc.exe`, `bin/win32-x64/zer-lsp.exe`, `bin/win32-x64/gcc/bin/gcc.exe`
- `editors/vscode/extension.js` — uses `findBundled()` to prefer bundled binaries, falls back to system PATH
- `editors/vscode/package.json` — name `zerc-language`, publisher `zerohexer`, MPL-2.0
- Icon: `editors/vscode/icon.svg` — black background, silver `[*]` pointer-in-brackets logo, auto-converted to PNG during build
- Auto-PATH: on first activation, if `zerc` not on system PATH, prompts user to add bundled dir permanently via PowerShell `[Environment]::SetEnvironmentVariable`. Requires VS Code restart after.
- **CRITICAL:** `where zerc` check must run BEFORE `process.env.PATH` prepend (line 52-56 injects bundled dir). If checked after, it finds the bundled binary and skips the prompt.
- **CRITICAL:** `zer.lspPath` in VS Code settings overrides bundled detection. If set to a path that doesn't exist (e.g., old mingw path), LSP fails. Fix: clear the setting.
- Marketplace: publisher "Zerohexer", display name "ZER(C) Language"

**LLM Reference:**
- `ZER-LANG-LLM.md` — compact ZER syntax reference designed for LLM consumption. Feed to any LLM to enable accurate ZER code generation. Covers all C→ZER differences, intrinsics, builtins, MMIO, ASM, common patterns, coercion rules. ~5K tokens.

### Lessons From Writing ZER Code (stdlib pitfalls)

These tripped us while writing `lib/str.zer`, `lib/fmt.zer`, `lib/io.zer`. Fresh sessions WILL hit them:

1. **`orelse return false` is a PARSE ERROR.** `orelse return` is bare — no value. Use `orelse return;` for void functions. For bool returns: restructure to avoid orelse in return, or use a temp variable.

2. **`arena.alloc_slice(u8, n)` is a PARSE ERROR.** Primitive keywords (`u8`, `u32`, etc.) can't be type arguments. Use a struct wrapper: `struct Byte { u8 val; }` then `arena.alloc_slice(Byte, n)`. Or avoid alloc_slice for primitives — use fixed-size buffers instead.

3. **`const []u8` return type requires parser lookahead.** `const` and `volatile` at global scope trigger var-decl parsing. The parser now peeks ahead to detect function declarations. Works as of this session — but if adding new qualifier keywords, they need the same lookahead treatment.

4. **Functions returning sub-slices of `const` input must return `const []u8`.** `bytes_trim(const []u8 s) → []u8` fails — the sub-slice inherits const. Return type must be `const []u8`.

5. **C macros (stderr, stdout) are NOT accessible from ZER.** They're preprocessor symbols, not variables. Wrap them in a C helper function in a `.h` file: `static inline FILE *zer_get_stderr(void) { return stderr; }`. Then `cinclude` the header and declare the function in ZER.

6. **`_zer_` prefix is RESERVED.** BUG-276 rejects any identifier starting with `_zer_`. Name helpers `zer_get_stderr` not `_zer_stderr`.

7. **Multiple files concatenated must NOT redeclare the same function.** If `fmt.zer` declares `fputc` and `io.zer` also declares `fputc`, the checker errors "redefinition." Keep declarations in one file or use ZER's import system for multi-file projects.

8. **`@ptrcast(*opaque, data.ptr)` on `const []u8 data` fails.** The `.ptr` of a const slice is `const *u8`. Casting to `*opaque` strips const. Use `@ptrcast(const *opaque, data.ptr)` instead.

9. **`cinclude` emits `#include` but does NOT register C symbols.** You must declare every C function you want to call as a ZER function signature: `i32 putchar(i32 c);`. The `cinclude` just makes the header available to GCC.

10. **`bool` return via `orelse` needs restructuring.** Can't do `*opaque f = mf orelse return false;`. Instead: `?*opaque mf = io_open(...); *opaque f = mf orelse return;` for void, or use an if-unwrap pattern for bool returns.

## Spawning Agents That Write ZER Code — MANDATORY

When spawning ANY agent that writes ZER source code (tests, examples, anything), you MUST include these rules in the agent prompt. Agents do NOT read CLAUDE.md automatically:

```
ZER SYNTAX RULES (not C — these differ):
- No ++ or --. Use += 1, -= 1
- else if supported: if (a) { } else if (b) { } else { }
- No C-style casts. Use @truncate, @saturate, @bitcast
- Braces ALWAYS required for if/else/for/while bodies
- Array decl: u8[256] buf (size between type and name)
- Pointer: *Task ptr (star before type, not after)
- Enum access: State.idle (qualified), switch arms: .idle => (dot prefix)
- Switch uses => arrows, no case keyword, no fallthrough
- orelse return is BARE — no value: x orelse return (NOT orelse return 1)
- arena.alloc(T) — T must be struct/enum name, NOT primitive keyword (u32 etc.)
- For loops: for (u32 i = 0; i < N; i += 1) { }
- String literals are []u8, not char*
- bool is NOT an integer — no bool↔int coercion
- Optional: ?*T (null sentinel), ?T (struct with .value/.has_value), ?void (has_value ONLY, no .value)
- Unwrap: if (opt) |val| { use(val); }  or  val = opt orelse default;
- goto label; and label: are supported (forward + backward). Banned inside defer and @critical blocks.
- shared struct = auto-locked mutex. shared(rw) struct = reader-writer lock.
- spawn func(args) = fire-and-forget thread (shared ptr or value args only)
- ThreadHandle th = spawn func(args); th.join(); = scoped spawn (allows *T)
- @cond_wait(shared_var, condition), @cond_signal(shared_var), @cond_broadcast(shared_var)
- @cond_timedwait(shared_var, condition, timeout_ms) returns ?void
- @once { body } = execute exactly once (thread-safe)
- @barrier_init(var, N) + @barrier_wait(var) = thread sync barrier
- Semaphore(N) var; = counting semaphore, @sem_acquire(var)/@sem_release(var)
- Both Barrier and Semaphore accept *Barrier/*Semaphore pointer params
- threadlocal u32 var = per-thread storage (__thread)
- async void func() { yield; await cond; } = stackless coroutine
- _zer_async_NAME type, _zer_async_NAME_init(&task), _zer_async_NAME_poll(&task)
- Ring(T, N) = circular buffer channel (T must be struct name, push/pop)
- @atomic_load/store/add/sub/or/and/xor/cas for atomics
- No malloc/free — use Pool(T, N), Slab(T), Arena
- move struct Name { } = ownership transfer on pass/assign, use after move = error
- container Name(T) { T[N] data; u32 len; } = parameterized struct template (monomorphization)
- Name(ConcreteType) var; = stamps concrete struct, functions take *Name(Type) explicitly
- Designated init: Point p = { .x = 10, .y = 20 }; or p = { .x = 1 };
```

Failure to include these rules causes agents to write invalid ZER (e.g., using i++ which silently passes parse-error-tolerant test harnesses).

### Lessons From Writing ZER Code (stdlib pitfalls)

These tripped us while writing `lib/str.zer`, `lib/fmt.zer`, `lib/io.zer`. Fresh sessions WILL hit them:

1. **`orelse return false` is a PARSE ERROR.** `orelse return` is bare — no value. Use `orelse return;` for void functions. For bool returns: restructure to avoid orelse in return, or use a temp variable.

2. **`arena.alloc_slice(u8, n)` is a PARSE ERROR.** Primitive keywords (`u8`, `u32`, etc.) can't be type arguments. Use a struct wrapper: `struct Byte { u8 val; }` then `arena.alloc_slice(Byte, n)`. Or avoid alloc_slice for primitives — use fixed-size buffers instead.

3. **`const []u8` return type requires parser lookahead.** `const` and `volatile` at global scope trigger var-decl parsing. The parser now peeks ahead to detect function declarations. Works as of this session — but if adding new qualifier keywords, they need the same lookahead treatment.

4. **Functions returning sub-slices of `const` input must return `const []u8`.** `bytes_trim(const []u8 s) → []u8` fails — the sub-slice inherits const. Return type must be `const []u8`.

5. **C macros (stderr, stdout) are NOT accessible from ZER.** They're preprocessor symbols, not variables. Wrap them in a C helper function in a `.h` file: `static inline FILE *zer_get_stderr(void) { return stderr; }`. Then `cinclude` the header and declare the function in ZER.

6. **`_zer_` prefix is RESERVED.** BUG-276 rejects any identifier starting with `_zer_`. Name helpers `zer_get_stderr` not `_zer_stderr`.

7. **Multiple files concatenated must NOT redeclare the same function.** If `fmt.zer` declares `fputc` and `io.zer` also declares `fputc`, the checker errors "redefinition." Keep declarations in one file or use ZER's import system for multi-file projects.

8. **`@ptrcast(*opaque, data.ptr)` on `const []u8 data` fails.** The `.ptr` of a const slice is `const *u8`. Casting to `*opaque` strips const. Use `@ptrcast(const *opaque, data.ptr)` instead.

9. **`cinclude` emits `#include` but does NOT register C symbols.** You must declare every C function you want to call as a ZER function signature: `i32 putchar(i32 c);`. The `cinclude` just makes the header available to GCC.

10. **`bool` return via `orelse` needs restructuring.** Can't do `*opaque f = mf orelse return false;`. Instead: `?*opaque mf = io_open(...); *opaque f = mf orelse return;` for void, or use an if-unwrap pattern for bool returns.

## ZER Safety Architecture — 4 Models, 29 Systems

**ZER's safety is built on 4 analysis models**, each covering a different dimension of what the compiler can observe. All 29 tracking systems map to one of these 4 models (verified against actual code). When implementing ANY new safety feature, identify which model it belongs to and follow that model's pattern.

**The 4 models exist because ZER uses C syntax.** Rust encodes all safety in ONE model (ownership types) because programmers annotate lifetimes/borrows. ZER programmers write plain C — the compiler must INFER everything. Each model infers a different kind of information. This is the same architecture as GCC/LLVM (multiple independent IPA passes), applied at the language level.

### Model 1: State Machines — entity lifecycle tracking
**What it answers:** "What STATE is this tracked entity in at this program point?"
**Pattern:** Define states (ALIVE, FREED, TRANSFERRED), define valid transitions, error on invalid transition or bad terminal state.
**When to use:** New tracked entity with a lifecycle (file handle, socket, lock, resource).

| # | System | States / Transitions |
|---|---|---|
| 7 | **Handle States** | `UNKNOWN→ALIVE→FREED/MAYBE_FREED/TRANSFERRED` — UAF, double-free, leak |
| 10 | **Move Tracking** | Uses `HS_TRANSFERRED` — ownership transfer, use-after-move |
| 6 | **Alloc Coloring** (supporting) | `ZC_COLOR_POOL/ARENA/MALLOC` — set at alloc, propagated to aliases |
| 8 | **Alloc ID** (supporting) | Unique per alloc, shared by aliases — groups `?Handle`+`Handle` as same alloc |

### Model 2: Program Point Properties — values change at control flow
**What it answers:** "What PROPERTY does this value/context have at THIS specific program point?"
**Pattern:** Property set at declaration, updated at assignments/branches, checked at use sites. Can propagate, widen (MAYBE_FREED), or invalidate (&var taken).
**When to use:** New value property that changes during execution (range, taint, escape status, provenance).

| # | System | Property tracked |
|---|---|---|
| 12 | **Range Propagation** | `VarRange {min, max, known_nonzero}` — bounds safety, division safety |
| 3 | **Provenance** | `Symbol.provenance_type` — propagates through assigns, clears on re-derive |
| 11 | **Escape Flags** | `is_local_derived`, `is_arena_derived` — propagates, clears on reassignment |
| 24 | **Context Flags** | `in_loop`, `defer_depth`, `critical_depth`, `in_async` — scope-exit validation |
| 22 | **Union Switch Lock** | `union_switch_var` — set entering switch, cleared leaving, checked during body |
| 14 | **Auto-Guard** (output) | Nodes where VRP couldn't prove safety → emitter inserts runtime guard |
| 15 | **Dynamic Freed** (output) | `pool.free(arr[k])` event → emitter inserts UAF guard for `arr[j]` |

### Model 3: Function Summaries — per-function computed properties
**What it answers:** "What does this FUNCTION do?" (cached, used at call sites)
**Pattern:** Scan function body (lazy, on first access), cache result on Symbol, check at call sites or context entry. DFS cycle detection for recursion.
**When to use:** New function-level property (purity, can_panic, can_io, touches resource X).

| # | System | Function property |
|---|---|---|
| 29 | **FuncProps** | `can_yield`, `can_spawn`, `can_alloc`, `has_sync` — context safety |
| 9 | **Func Summaries** (zercheck) | `frees_param[i]` — cross-function UAF/leak detection |
| 4 | **Prov Summaries** | Return provenance — cross-function `*opaque` tracking |
| 5 | **Param Provenance** | What type each `*opaque` param expects — call-site validation |
| 13 | **Return Range** | `return_range_min/max` — cross-function bounds: `arr[func()]` zero-overhead |
| 18 | **Stack Frames** | Frame size, callees, recursion — `--stack-limit`, recursion detection |
| 27 | **Spawn Global Scan** | Non-shared global access — data race detection |
| 28 | **Shared Type Collect** | Shared types touched — deadlock detection |
| 17 | **ISR Tracking** | Globals accessed from ISR vs main — collect-then-check for missing volatile |

### Model 4: Static Annotations — set once at declaration, checked at use
**What it answers:** "What was DECLARED about this entity?" (never changes)
**Pattern:** Set at declaration site, checked at every use site. No propagation, no state changes.
**When to use:** New declaration-level constraint (alignment, section, visibility, capability).

| # | System | Annotation |
|---|---|---|
| 16 | **Non-Storable** | `pool.get()` result — can't be stored in variables |
| 19 | **MMIO Ranges** | Declared valid address ranges — `@inttoptr` validation |
| 20 | **Qualifier Tracking** | `is_volatile`, `is_const` — prevent stripping through casts |
| 21 | **Keep Parameters** | `is_keep` — pointer param can be stored in globals |

### Infrastructure (not safety models — supports the 4 models above)
| # | System | Purpose |
|---|---|---|
| 1 | **Typemap** | `Node* → Type*` — emitter reads resolved types |
| 2 | **Type ID** | `next_type_id++` — runtime `*opaque` provenance tag |
| 23 | **Defer Stack** | Pending defer blocks — LIFO cleanup emission |
| 25 | **Container Templates** | Monomorphization stamping — type system infrastructure |
| 26 | **Comptime Evaluator** | Compile-time function evaluation — metaprogramming |

### Quick Lookup — All 29 Systems (flat)

| # | System | Location | What It Tracks | Model |
|---|---|---|---|---|
| 1 | **Typemap** | checker.c | `Node* → Type*` for every AST node | Infra |
| 2 | **Type ID** | checker.c | `next_type_id++` per struct/enum/union | Infra |
| 3 | **Provenance** | checker.c | `Symbol.provenance_type` — original type of `*opaque` | 2 |
| 4 | **Prov Summaries** | checker.c | What provenance a function's return carries | 3 |
| 5 | **Param Provenance** | checker.c | What type each `*opaque` param expects inside callee | 3 |
| 6 | **Alloc Coloring** | zercheck.c | `ZC_COLOR_POOL/ARENA/MALLOC/UNKNOWN` per handle | 1 |
| 7 | **Handle States** | zercheck.c | `HS_UNKNOWN→ALIVE→FREED/MAYBE_FREED/TRANSFERRED` | 1 |
| 8 | **Alloc ID** | zercheck.c | Unique per allocation, shared by aliases | 1 |
| 9 | **Func Summaries** | zercheck.c | `frees_param[i]` — does function free param? | 3 |
| 10 | **Move Tracking** | zercheck.c | `should_track_move()` — move struct or contains one | 1 |
| 11 | **Escape Flags** | checker.c | `is_local_derived`, `is_arena_derived`, `is_from_arena` | 2 |
| 12 | **Range Propagation** | checker.c | `VarRange {min, max, known_nonzero}` per variable | 2 |
| 13 | **Return Range** | checker.c | `return_range_min/max` per function | 3 |
| 14 | **Auto-Guard** | checker.c | Unproven array accesses needing runtime guard | 2 |
| 15 | **Dynamic Freed** | checker.c | `pool.free(arr[k])` — which index was freed | 2 |
| 16 | **Non-Storable** | checker.c | `pool.get()` results that can't be stored | 4 |
| 17 | **ISR Tracking** | checker.c | Globals shared between ISR and main code | 3 |
| 18 | **Stack Frames** | checker.c | Frame sizes, callees, recursion, indirect calls | 3 |
| 19 | **MMIO Ranges** | checker.c | Declared valid address ranges | 4 |
| 20 | **Qualifier Tracking** | checker.c | `is_volatile`, `is_const` on Symbol + Type | 4 |
| 21 | **Keep Parameters** | checker.c | `is_keep` — pointer param can be stored | 4 |
| 22 | **Union Switch Lock** | checker.c | Which union is currently being switched on | 2 |
| 23 | **Defer Stack** | emitter.c | Pending defer blocks at each scope level | Infra |
| 24 | **Context Flags** | checker.c | `in_loop`, `in_interrupt`, `in_naked`, `in_async`, etc. | 2 |
| 25 | **Container Templates** | checker.c | `ContainerTemplate` + `ContainerInstance` cache | Infra |
| 26 | **Comptime Evaluator** | checker.c | `ComptimeCtx` with locals, arrays, floats | Infra |
| 27 | **Spawn Global Scan** | checker.c | Non-shared global access from spawned function | 3 |
| 28 | **Shared Type Collect** | checker.c | Which shared types a statement touches | 3 |
| 29 | **Function Summaries** | checker.c | `FuncProps` on Symbol: can_yield/spawn/alloc/has_sync | 3 |

### Development Decision Flow

```
New safety feature → Which model?

Entity with lifecycle (alloc/free/move)?     → Model 1: State Machine
Value property that changes at assignments?  → Model 2: Point Properties
Per-function property used at call sites?    → Model 3: Function Summary
Declaration-level constraint, never changes? → Model 4: Static Annotation

Doesn't fit any? → STOP. Either:
  (a) It's a combination of existing models (most likely)
  (b) Document why a new model is needed (unlikely — these 4 cover
      what a compiler can observe: entities, points, functions, declarations)
```

### Safety Coverage by Model

| Safety guarantee | Model(s) used |
|---|---|
| Buffer overflow | **2** (VRP ranges) |
| Use-after-free | **1** (handle states) + **3** (cross-func summary) |
| Null dereference | **4** (*T non-null) + **2** (optional unwrap check) |
| Dangling pointer | **2** (escape flags at program points) |
| Division by zero | **2** (VRP: proven nonzero?) |
| Data races | **4** (shared annotation) + **3** (spawn scan, shared types) |
| Deadlock | **3** (shared type collect per function) |
| Move semantics | **1** (HS_TRANSFERRED state) |
| Context safety | **3** (FuncProps) + **2** (context flags for scope-exit) |
| Wrong pointer cast | **2** (provenance at point) + **3** (prov summaries) |
| Handle leak | **1** (ALIVE at exit = error) |
| ISR safety | **3** (FuncProps can_alloc, ISR tracking) |
| MMIO safety | **4** (mmio ranges) |
| Stack overflow | **3** (stack frames) |
| Volatile/const strip | **4** (qualifiers) |

**Unconditional safety (no model needed — always on):**
- Auto-zero: every variable initialized to zero (emitter rule)
- Integer overflow wraps: `-fwrapv` (GCC flag, defined behavior)
- Shift safety: ternary emitted (no UB on over-width shift)

**Design principle:** Safety = tracking, not banning. If a pattern is unsafe, find which tracking system can detect the violation. Only ban when NO tracking system can cover the case.

**Ban Decision Framework — when banning IS correct (check in order):**
1. **Hardware/OS constraint?** → Ban. No compiler trick fixes this. (malloc in ISR = kernel deadlock, pthread_create with interrupts disabled = hardware-unsafe)
2. **Emission impossibility?** → Ban. Can't generate valid C. (yield in defer = duplicate Duff's device case labels)
3. **Needs runtime?** → Ban. ZER has no scheduler/GC. (Go allows yield-in-critical because its runtime handles it — ZER can't)
4. **Needs type system?** → Ban. ZER has no traits/lifetimes. (Rust prevents lock-across-await via !Send — ZER can't express this in types)
5. **None of the above?** → **Track.** ZER sees all bodies, can generate correct code, no constraint prevents it.

**Cross-check references:** Zig (same quadrant — no runtime, no traits) and Rust. If both ban it, ZER should too. If Rust tracks it via types that ZER can't express, ban is correct. If Zig allows it with comptime, investigate how.

**Current bans justified by this framework:**
- `yield/await in defer` — emission impossibility (duplicate case labels in Duff's device)
- `yield/await in @critical` — needs runtime (save/restore interrupt state across suspend requires scheduler awareness)
- `spawn in @critical` — hardware constraint (pthread_create with interrupts disabled)
- `spawn in async` — needs type system (thread lifetime tracking requires borrow checker or GC)
- `alloc in interrupt` — OS constraint (malloc lock + interrupted malloc = deadlock)
- `naked non-asm` — hardware constraint (no prologue = no stack)
- `return/break/continue/goto in @critical` — hardware constraint (skips interrupt re-enable)
- `return/break/continue/goto in defer` — emission impossibility (corrupts cleanup flow)

**`*opaque` safety coverage:**
- Pure ZER: 100% compile-time (zercheck #7 handle states)
- `*opaque` alone: 100% compile-time (zercheck + #3 provenance)
- `*opaque` + `cinclude`: 100% (zercheck + `--wrap=malloc` compiled-in)
- `cinclude` alone (raw `*u8`): NOT tracked — developer wraps with `*opaque` for safety

## STRICT: No-Debt Implementation Rule

**Every change — add, fix, remove — MUST be the correct solution, not a shortcut.**

### Before Writing ANY Code: Read First, Implement Second

**DO NOT start implementing immediately.** The natural instinct is to write code as soon as the problem is understood. Fight it. This session proved that jumping to implementation causes:
- Comptime loop body: implemented with `eval_comptime_block` call (copies locals) → broke mutations → had to refactor to `ComptimeCtx` (the correct architecture). 3 rounds of fix.
- Range-for: implemented with shared AST node pointer → triple evaluation → had to refactor to clone + reject. 2 rounds of fix.
- static_assert: implemented without `check_expr` first → comptime calls unresolved → had to add ordering fix.

**The correct workflow:**
1. **Read the relevant code** — not just the function you'll change, but its callers, its callees, and the data flow. Understand HOW the data moves before changing WHERE it moves.
2. **Identify the architecture** — is this copy-on-call? Pass-by-reference? Who owns the memory? What's the scope lifetime?
3. **Design the fix** — write the approach in your head. Ask: "Does this create duplicate code? Does this share mutable state? Does this have ordering dependencies?"
4. **Then implement** — with the architecture already decided, the code writes itself.

### Implementation Checks

Before implementing ANY change, verify:
1. **No duplicate code.** If the same logic appears in 2+ places, extract a helper FIRST. Do NOT copy-paste and plan to "refactor later." Later never comes.
2. **No shared mutable AST nodes.** Parser desugaring must create SEPARATE node copies for each use site. The SAME Node pointer in multiple AST positions causes double-evaluation bugs. Always deep-copy or clone.
3. **No ordering dependencies without comments.** If function A must run before function B (e.g., `check_expr` before `eval_const_expr_scoped`), document WHY with a comment at both sites.
4. **No copy-on-call for mutable state.** If a recursive function copies its input but callers need mutations to propagate back, the architecture is WRONG. Either pass by reference (like `ComptimeCtx*`) or use save/restore pattern (like `saved_count`).
5. **Test the REAL pattern, not just the simple case.** If you add `for (item in slice)`, test with struct slices, nested loops, and side-effectful expressions — not just `u32[5]`.
6. **100% coverage, not 99%.** Don't say "works for the common case." If an edge case exists, either handle it or reject it at compile time with a clear error. Never silently produce wrong behavior.
7. **NEVER use fixed-size buffers for dynamic data.** No `int arr[64]`, `char buf[128]`, `Node *items[256]` for arrays that could grow. Use stack-first dynamic pattern: `int stack[64]; int *arr = stack; int cap = 64;` with `realloc` overflow. Fixed buffers silently drop data beyond the limit (BUG-492: `covered_ids[64]` silently ignored allocations 65+). Same pattern as parser RF9 (stack-first `[32]` with arena overflow). GCC and all production compilers use dynamic allocation — ZER must too.

8. **Scope-sensitive changes at DECLARATION sites, not USE sites.** When tracking variables across scopes (zercheck handles, VRP ranges, escape flags), modifications that depend on "which scope is this variable from?" must happen at NODE_VAR_DECL (declaration), never at NODE_IDENT/NODE_CALL (use). Uses read from any scope (find_handle), declarations write to current scope (find_handle_local). Trying to add scope logic at use sites breaks cross-scope access — loops and if bodies accessing outer variables get incorrectly treated as shadows. Proven by BUG-488 (3 failed patches at use sites before proper declaration-site fix) and BUG-494 (same pattern, same lesson).

### Before Committing

Ask: "If I read this code in 6 months, would I know WHY it's structured this way? Would I accidentally break it by changing one of the N sites?"

If the answer is no → extract helper, add comment, or restructure. Do NOT commit debt.

### Anti-Patterns Found in This Session

| Anti-Pattern | What Happened | Correct Approach |
|---|---|---|
| Copy-on-call for mutable state | `eval_comptime_block` copied locals → loop mutations lost | `ComptimeCtx*` passed by pointer with save/restore |
| Shared AST node pointer | range-for `collection` reused in 3 positions → triple eval | Clone ident node + reject non-trivial expressions |
| Missing ordering dependency | static_assert called `eval_const_expr_scoped` before `check_expr` → comptime unresolved | `check_expr` FIRST (resolves comptime), then eval |
| Inline duplicate code | for/while loop body walk copy-pasted | `CT_WALK_BODY_STMT` macro → then proper `ComptimeCtx` refactor |
| Quick fix then "refactor later" | Macro band-aid for duplicate code | Refactor NOW while you have context |

## Fix Methodology — Proper vs Pragmatic (learned from 8 red team rounds)

**MANDATORY: read this before fixing ANY bug.** Fresh sessions default to pragmatic patches. This section explains when that's wrong and how to identify the proper fix.

### The Decision Flow

```
Bug found → Can it be fixed in ONE location?
  YES → Is that location a DECLARATION site (not use site)?
    YES → Fix there. Done.
    NO  → STOP. You're patching a use site. Read "Declaration vs Use" below.
  NO  → How many sites need changing?
    2-3 → Fix each, add comment linking them.
    4+  → STOP. You need a REFACTOR, not a patch. Read "Refactor Triggers" below.
```

### Declaration vs Use Site Rule

When tracking state across scopes (handles, VRP ranges, escape flags, move structs):
- **DECLARATION site** (NODE_VAR_DECL): "a new variable enters scope" → WRITE to tracking system at CURRENT scope
- **USE site** (NODE_IDENT, NODE_CALL): "an existing variable is referenced" → READ from tracking system at ANY scope

**Never add scope-aware logic at use sites.** It breaks cross-scope access (loops, if bodies accessing outer variables). Proven by 4 failed patches across BUG-488 and BUG-494.

**Pattern:** `find_handle_local` for declarations, `find_handle` for uses. Same as checker's `scope_lookup_local` vs `scope_lookup`.

### Refactor Triggers — When Patching Won't Work

Stop patching and refactor when:
1. **Same fix needed at 4+ sites** → Extract helper function. One change point.
2. **Patch fixes bug A but breaks test B** → The assumption being patched is architectural, not local. Need new abstraction (e.g., `find_handle_local` was the abstraction that fixed BUG-488).
3. **Three failed attempts at the same bug** → Your mental model of the code is wrong. Read the full function, not just the bug site. (BUG-488: 3 patches failed because `find_handle` was used everywhere for both reads and writes — needed two-function split.)
4. **Fixed buffer hits limit** → Replace with stack-first dynamic pattern. Never increase the constant (rule #7).

### Pragmatic Fix vs Proper Fix

| Indicator | Pragmatic (avoid) | Proper (do this) |
|---|---|---|
| Fix location | Patching at the call site where bug manifests | Fixing at the root (declaration, data structure, helper) |
| Scope of change | 1 line change, 1 site | New helper/abstraction, all sites use it |
| Test impact | Fixes the test case, might break others | Fixes the class of bugs, no regressions |
| Example | `if (depth < 8)` depth limit | Call graph DFS with memoization (BUG-474) |
| Example | Ban yield in orelse (flag) | Promote temps to state struct (BUG-481) |
| Example | `find_handle` + scope check at use site | `find_handle_local` at declaration site (BUG-488) |

### The "Ban vs Track" Decision

When a pattern is unsafe, the AI's default instinct is to BAN it (compile error). The proper ZER approach is to TRACK it (add to a tracking system).

```
AI instinct: "Ban &move_struct in function calls"
Proper:      "Track &move_struct — mark source as HS_TRANSFERRED"

AI instinct: "Ban yield inside orelse blocks"
Proper:      "Promote orelse temps to async state struct"

AI instinct: "Ban variable shadowing in zercheck"
Proper:      "Add scope_depth to PathState, find_handle_local for declarations"
```

**Only ban when NO tracking system can cover the case** (e.g., naked non-asm = hardware constraint, not a tracking gap). If the user pushes back on a ban, they're usually right — look for a tracking solution.

## Implementation Workflow — Lessons Learned

These patterns were discovered through repeated mistakes. Follow them to avoid wasting turns.

### Adding New Builtin Methods (alloc_ptr, Task.alloc, etc.)
Every new builtin method requires changes in THREE places:
1. **Checker** — type-check the call, return correct type, validate args
2. **Emitter** — emit the correct C code for the call
3. **zercheck** — if the method allocates/frees, register it in zercheck tracking

Forgetting zercheck = the method works but UAF/double-free are not caught. This happened with `alloc_ptr()` — checker + emitter worked, zercheck didn't recognize it.

### Adding New AST Node Types (goto, label, etc.)
New node types require changes in FIVE places:
1. **Lexer** — keyword token (e.g., `TOK_GOTO`)
2. **AST** — node kind + data struct (e.g., `NODE_GOTO` + `goto_stmt`)
3. **Parser** — parse the syntax, create the node
4. **Checker** — validate (label exists, not in defer block, etc.)
5. **Emitter** — emit the C equivalent

Also: update EVERY recursive AST walker that handles all node types. **7 walkers now use exhaustive switch (no `default:`)** — GCC `-Wswitch` warns automatically when a new NODE_ type is missing:
- `scan_frame` (checker.c) — stack depth analysis
- `collect_labels` (checker.c) — goto label collection
- `validate_gotos` (checker.c) — goto target validation
- `zc_check_expr` (zercheck.c) — expression UAF/double-free checks
- `zc_check_stmt` (zercheck.c) — statement-level handle tracking
- `emit_auto_guards` (emitter.c) — bounds check guard emission
- `emit_top_level_decl` (emitter.c) — top-level declaration emission

3 walkers keep intentional `default:`: `emit_expr`/`emit_stmt` (emit `/* unhandled */` diagnostic), `resolve_type_for_emit` (returns `ty_void` fallback). These produce visible output on unknown nodes, not silent skips.

When adding a new NODE_ type, compile and check for `-Wswitch` warnings — they show exactly which walkers need updating. This prevents the entire class of "walker missing node type" bugs (BUG-433/434/435/437/452).

### Two-Pass Emission Pattern
`emit_file()` uses two-pass emission:
- Pass 1: struct/enum/union/typedef declarations
- Auto-Slab globals (between passes)
- Pass 2: functions, global variables, everything else

This is needed because auto-slabs use `sizeof(StructType)` which requires the struct to be declared first. If adding new auto-generated globals, insert them between the passes.

### Testing New Features — Interaction Test Checklist
After implementing any feature, test these interactions:
1. Feature + `orelse return` (bare) AND `orelse { return; }` (block) — BOTH forms, they take different emitter paths
2. Feature + `defer` (cleanup on exit)
3. Feature + `const`/`volatile` (qualifiers) — especially const + Handle (const key ≠ const data)
4. Feature + loops (`for`/`while` + `break`/`continue`)
5. Feature + `switch` (arms, captures)
6. Feature + other NEW features from same session
7. Feature + `*opaque` (C interop boundary)
8. Feature as bare expression (ghost handle check)
9. Feature + `if (opt) |t|` unwrap (immutable capture) — Handle auto-deref must allow data writes
10. Feature + struct field access (nested struct containing Handle/pointer)
11. Feature in interrupt handler (`c->in_interrupt` — ISR ban needed?)
12. Feature + zercheck (does zercheck RECOGNIZE new alloc/free patterns?)
13. Feature + `else if` chains — source mapping `#line` must not follow `else` on same line (BUG-418)
14. Any new coercion must work in ALL 4 value-flow sites: var-decl init, assignment, call args, return (BUG-419)
15. Feature + `@critical` — control flow banned inside (return/break/continue/goto skip interrupt re-enable)
16. Feature + `defer` + `return expr` — return expression must evaluate BEFORE defers fire (BUG-442). Test: `defer free(h); return h.field;` must work (not UAF)
17. Feature + `keep` parameter — non-keep pointer params cannot be stored in global/static (BUG-440). Test: function storing param to global without `keep` must error

### Write Real Code After Implementing Features — MANDATORY

Unit tests find syntax bugs. REAL CODE finds interaction bugs. After implementing ANY new feature:

1. Write a **real program** (10-60 lines) that uses the feature in a realistic pattern
2. Compile with `zerc --run` in Docker
3. If it crashes or gives wrong output → you found a bug unit tests missed

This session's proof: writing a 60-line HTTP server found the orelse block null-sentinel bug that 1,700+ unit tests missed. Testing `Task.alloc()` with both orelse forms found the auto-slab initializer bug.

**Bug patterns found ONLY by real code (not unit tests):**
- `orelse { return; }` (block form) emitting 0 instead of pointer — tests all used bare `orelse return`
- Auto-slab `{sizeof(T), 0, 0}` positional init putting sizeof in wrong field — tests used explicit Slab
- zercheck not recognizing `Task.free()` as free — tests used `heap.free()` directly
- const Handle blocking if-unwrap capture writes — tests didn't combine if-unwrap + Handle + assign
- `else if` chain emitting `else #line N` (stray # in GCC) — tests had source=NULL, skipping #line
- `slice = array` assignment missing coercion — tests only used var-decl init, not assignment
- `typedef ?u32 (*Handler)(u32)` creating optional funcptr — tests used void return funcptrs

- `u32 val = get_tok(&state).field; return val;` falsely rejected as "pointer to local" — scalar can't carry pointer
The audit round found 6 bugs, real code found 5 more, real-code session 2 found 4 more. Both are needed.

### Windows VSIX Workflow
- `make docker-install` — builds + installs to mingw PATH
- VSIX build: `docker build -f Dockerfile.vsix .`
- `package.json` name MUST match marketplace listing name
- `zer.lspPath` in VS Code settings overrides bundled binary detection — clear it if LSP fails
- `where zerc` check must run BEFORE `process.env.PATH` injection in extension.js
- Multiple extension versions can coexist — remove old ones from `.vscode/extensions/`

## IR Path Validation — MANDATORY context for fresh sessions (2026-04-17)

`emitter_init` sets `use_ir = true` by default. ALL test harnesses (test_emit.c, test_firmware*.c, test_production.c, test_zercheck.c) exercise the IR path via `emit_file`. **Before 2026-04-17, they silently ran AST** because `memset(e, 0)` zero-initialized `use_ir=false`. Flipping the default surfaced 20 latent IR bugs (BUG-538 through BUG-557, see BUGS-FIXED.md).

**Lesson for future IR changes:** The integration tests (`tests/test_zer.sh`, `rust_tests/`, `zig_tests/`, `test_modules/`) run `zerc` binary which always had `use_ir=true`. The C unit tests run `emit_file` directly. Both paths now validate IR. Don't add `e->use_ir = false` anywhere without a compelling reason.

**Also critical: `zerc --run` historically returned exit 0 even when compiled binary trapped.** Many rust_tests/ were "passing" while the emitted program trapped at runtime (SIGTRAP = exit 133). Post-IR-validation changes, tests that previously masked trap-failures now surface as compile errors and must actually be fixed. If a test was passing before and fails now with a GCC error, don't assume regression — check git history to see if the test was ever running correctly.

**FIXED 2026-04-18 (BUG-581).** `zerc_main.c` now uses `WEXITSTATUS(run_ret)` on POSIX. Fresh sessions can now trust `--run` exit codes. But this is what EXPOSED the other latent bugs below — don't assume "test passes via `--run`" means the test is correct; many tests had wrong behavior that was masked for ~8 months. If you see test runners with `KNOWN_FAIL` skip lists (`tests/test_zer.sh`, `rust_tests/run_tests.sh`, `zig_tests/run_tests.sh`), those entries are pre-existing failures surfaced by BUG-581 — see `docs/limitations.md`.

### IR Path Architectural Invariants — CRITICAL for fresh sessions

These are the non-obvious rules that took multiple sessions + bug fixes to discover. Violating any of them causes silent miscompilation.

**1. Entry block MUST be `bb0`** (BUG-588). In `ir_lower_func` / `ir_lower_interrupt`, call `start_block(&ctx)` BEFORE `collect_labels(&ctx, body)`. If you swap the order, label blocks get IDs 0, 1, 2... and the entry gets a higher ID. The emitter iterates blocks in ID order so C linear execution starts at a random label, not the function body. Manifests as SIGTRAP or garbage reads (any function with a label goes wrong).

**2. `checker_set_type(c, node, type)` is PUBLIC — use it when synthesizing AST in IR lowering.** Added BUG-582/579 session. Without it, `lower_expr` on synthesized NODE_BINARY / NODE_FIELD / NODE_UNARY nodes falls back to `ty_i32` because `checker_get_type` returns NULL for nodes not in the typemap. Wrong-typed IR locals get declared (e.g., pointer-to-union emitted as int32_t), and C compilation gives nonsense errors. Rule: every synthesized AST node that will be lowered should get its type annotated via `checker_set_type` at creation.

**3. Switch lowering: enum/union/optional now full IR-lower** (BUG-579). No more IR_NOP passthrough. Arm bodies go through `lower_stmt` — for/while/nested-switch/orelse/continue/break all work automatically. See `ir_lower.c` NODE_SWITCH for template (build comparison AST, lower_expr it, IR_BRANCH, lower arm body).

**4. Union variant writes must update `_tag`** (BUG-582). `u.variant = val` AND `u.variant[i] = val` (and deeper chains) — `emit_rewritten_node` NODE_ASSIGN walks target through NODE_INDEX/NODE_FIELD/NODE_UNARY to find the NODE_FIELD whose object is union, then emits `{_p->_tag = N; target = val;}` statement expression. AST path had simple case at `emitter.c:1210`; IR path (`emit_rewritten_node`) was missing it until BUG-582.

**5. Switch arm captures need UNIQUE names across all switches in a function** (BUG-585). `|v|` captures from different switches would collide in IR's flat namespace. Lowering now generates `v_capN` unique names and uses `rewrite_capture_name()` to replace the source name ONLY inside that arm's body. Do not dedup arm captures even with same name+type.

**6. Non-null optional switch arms need value comparison** (BUG-584). `switch (?u32 v) { 5 => ..., 10 => ... }` must check `has_value && (value == arm_lit)`, not just `has_value`. For `?Enum` arms, resolve the variant name to its numeric value via `sw_eff->optional.inner->enum_type.variants[i].value`.

**7. `@once` IR_BRANCH marker pattern** (BUG-583). `NODE_ONCE` lowers to an `IR_BRANCH` with `expr=node` and `cond_local=-1`. The emitter detects this marker and emits the atomic CAS pattern (`static flag + __atomic_exchange_n ACQ_REL`). Don't assume all IR_BRANCH instructions use cond_local.

**8. `IR_ASSIGN` handles array-to-array via memcpy.** Mirror of BUG-548 for IR_COPY. Needed for union array-variant captures. When `dst_eff = TYPE_ARRAY && src_eff = TYPE_ARRAY`, emit `memcpy(dst, expr, sizeof(dst));` — C can't assign arrays directly.

**9. `(bool)integer` must truthy-convert** (BUG-586). ZER's bool is `uint8_t`, so `(uint8_t)5` gives 5, not 1. IR_CAST emitter checks target_eff = TYPE_BOOL and uses `(uint8_t)!!(x)` instead of plain cast.

**10. Funcptr array call indices support more than NODE_IDENT** (BUG-587). `arr[0](...)`, `arr[i](...)`, `arr[i+1](...)` all need to emit correctly. The IR_CALL NODE_INDEX-callee path now handles NODE_INT_LIT + falls back to `emit_rewritten_node` for complex indices.

**11. `lower_expr(NODE_FIELD)` has type inference fallback.** When `checker_get_type(field)` is NULL (synthesized node), infer from object type + field name: `has_value` → bool, `value` → optional.inner, `_tag` → i32, struct fields → field.type, union variants → variant.type. Without this, temps get NULL type and the emitter skips declaring them → "_zer_tN undeclared" GCC error.

**12. `IRLocal.scope_depth` + `IRLocal.hidden` for variable shadowing** (BUG-590). `IRFunc.current_scope` is incremented/decremented by `NODE_BLOCK` in ir_lower. `ir_add_local` sets `scope_depth` on creation and creates a suffixed local when name/type/scope_depth doesn't match an existing local. `NODE_BLOCK` on exit marks all locals created within (skipping temps + captures) as `hidden=true`. `ir_find_local` prefers non-hidden matches; falls back to LAST hidden match if nothing in-scope (preserves existing behavior for emitter passes outside normal traversal). Without this, inner `Handle h` shadowing outer made outer references after the block resolve to the freed inner local → UAF.

**13. `NODE_BLOCK` defer semantics + POP_ONLY pattern** (BUG-590). Standalone `{ }` blocks fire their own defers at block exit. Enclosing constructs (loop body, if-branch, switch arm) MUST set `ctx->block_defers_managed++` before calling `lower_stmt` on the body — otherwise NODE_BLOCK double-fires defers. NODE_BLOCK uses the POP_ONLY trick: emit no-pop scoped fire in the current block, then create a bb_post and switch to it for the POP_ONLY op. Mirrors BUG-544 loop pattern — keeps defer bodies on the emit-time stack for early-exit paths (return/break/continue/orelse-return) that have earlier block IDs than the current position.

**14. `await` condition re-evaluated on every poll** (BUG-591). IR_AWAIT's lowering stores the cond AST on `inst->expr` (NOT a pre-computed cond_local). The emitter emits `case N:;` then a FRESH evaluation via `emit_rewritten_node(cond)`. Without this, resume (state=N) skips past the cond evaluation — stale value used, task never completes.

**15. Signed vs unsigned comparison in IR_BINOP** (BUG-592). When IR_BINOP detects a comparison op and one operand is signed while the other is unsigned, cast the unsigned side to the signed side's type before emitting. Otherwise C's usual-arithmetic-conversion promotes the signed side to unsigned → `(int32_t)-5 < (uint32_t)0` is `false`. Also IR_LITERAL emits `(DstType)N` cast (not `N_ULL`) so comparisons downstream see the right signedness.

**16. `@once` is per-declaration, not per-function** (BUG-583 related). Each `@once { body }` block declares its OWN one-shot flag — matching Rust's `std::sync::Once`. Multiple `@once` blocks in the same function are INDEPENDENT (each fires once). To test "fire once across multiple invocations", wrap `@once` in a helper function and call it multiple times (the SAME flag prevents re-execution across calls).

**17. `Arena.over(buf)` returns an Arena VALUE** — it does not mutate an existing arena in place. Usage: `Arena ar = Arena.over(buf);` to initialize. Bare `ar.over(buf)` as a method call discards the return value and does nothing useful.

**18. IR path auto-locks shared struct access via IR_LOCK/IR_UNLOCK** (BUG-594). `ir_lower.c` NODE_BLOCK: for each source statement, `find_shared_root_in_stmt_ir` detects the root ident of a shared struct (directly or via pointer param), emits `IR_LOCK` with `expr=root, src2_local=is_write?1:0` before lowering, and `IR_UNLOCK` after. Emitter IR_LOCK/IR_UNLOCK cases call `emit_shared_lock_mode`/`emit_shared_unlock` (same helpers the dead AST path used before 2026-04-19 cleanup). Without this, function bodies with `*SharedStruct` parameters would emit raw field access → data races. Was latent for the entire v0.4.0 IR transition until BUG-581 made exit codes honest.

**19. Function bodies are IR-only since 2026-04-19** (post-BUG-594 cleanup). `emit_stmt` is DELETED, `--no-ir` / `--use-ir` flags REMOVED. `emit_func_decl` calls `ir_lower_func` + `emit_func_from_ir`; if `ir_lower_func` returns NULL or `ir_validate` fails, it `abort()`s with an "INTERNAL ERROR, please report" message — no silent AST fallback. Interrupts also route through IR now (`ir_lower_interrupt` + `emit_func_from_ir`, which detects `func->is_interrupt` and emits `void __attribute__((interrupt)) NAME_IRQHandler(void)` signature). Top-level declarations (structs, enums, globals, cinclude, mmio) still use AST — they're declarative data, not code. `emit_expr` remains because global initializers use it. Walker audit uses `emit_defers_from` as the end-of-span marker now (was `emit_stmt`).

**20. Validator exceptions for flag-use locals** (BUG-594 follow-up). Two IR ops use `cond_local` / `src2_local` as encoded FLAGS, not local-id references. Validator must skip the range check for these:
- `IR_DEFER_FIRE.cond_local` = defer-stack base index (0..defer_count)
- `IR_DEFER_FIRE.src2_local` = pop mode (0/1/2)
- `IR_LOCK.src2_local` = write-lock flag (0/1)
When adding a new IR op that reuses these fields as flags, add it to the exception list in `ir_validate` (ir.c).

**21. Task sugar uses alloc/free** (2026-04-19 rename, was new/delete). `Task.alloc()` → `?Handle(T)`. `Task.alloc_ptr()` → `?*T`. `Task.free(h)` / `Task.free_ptr(p)`. Same verbs as Pool/Slab/Arena — one vocabulary across all allocators. `new`/`delete` names are GONE, not aliased. ZER has no constructors/destructors, so C++ verbs were always a misnomer.

**22. V3 target-type routing for allocators** (2026-04-19). `Task.alloc()` and `slab.alloc()` route to `_ptr` variant internally when the target type is `*T` / `?*T`. `Task.free()` and `slab.free()` route to `_ptr` variant when arg type is `*T`. Implemented via AST rewrite in `route_alloc_to_ptr_if_needed` / `route_free_to_ptr_if_needed` (checker.c) — mutates NODE_FIELD.field_name from `alloc` → `alloc_ptr` (or `free` → `free_ptr`) before dispatch. Hooks: check_var_decl init, check_assign value, TYPE_SLAB + TYPE_STRUCT builtin dispatch. Receiver-type guards: alloc-routing only fires on struct-type-ident or TYPE_SLAB (skips Pool/Arena which have no _ptr variant). Example: `*Task t = Task.alloc() orelse return;` — target `*T` → rewritten to `Task.alloc_ptr()` transparently.

### Scoped Defer Emission — the pattern

Emitter's `defer_stack` is compile-time, not runtime. Every block-level construct that introduces a defer scope must bracket the body with defer tracking:

```c
int base = ctx->defer_count;        // save
lower_stmt(ctx, body);              // may push defers
emit_defer_fire_scoped(ctx, base, pop=true, line);  // emit + pop
ctx->defer_count = base;            // restore
```

Applied in: NODE_FOR/NODE_WHILE/NODE_DO_WHILE body exits, NODE_IF then/else bodies, integer switch arm bodies (in ir_lower.c). Enum/union/optional switch arms use emitter-side save/restore (emitter.c, arm_defer_base) because those arms are AST passthrough, not IR-lowered.

`IR_DEFER_FIRE` encoding:
- `cond_local == -1`: fire all (function exit)
- `cond_local == base`: scoped fire from count-1 down to base
- `src2_local == 1`: no-pop (for break/continue — other paths still need the defers on stack)
- `src2_local == 0` (default): pop after fire

### IR lowering patterns that MUST passthrough (not decompose)

Some expressions lose semantics if `lower_expr` decomposes the operand into a temp:

- **`&x` (TOK_AMP)**: `lower_expr` decomposes the operand first → temp holds VALUE of x → `&temp` is wrong address. Passthrough preserves lvalue. Already fixed in BUG-551.
- **NODE_ORELSE fallback**: value fallback needs explicit IR_ASSIGN to dest (BUG-539). Nested NODE_ORELSE needs recursive `lower_orelse_to_dest` (BUG-553).
- **NODE_NULL_LIT → struct optional**: detect src is `*void` placeholder, emit `{0, 0}` not `{val, 1}` (BUG-538).

When adding any new IR op or expression handler: if it takes a COMPILE-TIME position (source code line), ask "does this need the lvalue, or the value?" For lvalues (addr-of, writes), decomposition is wrong.

### "Walker missing node kind" — recurring bug class

Multiple tree walkers must recurse through expression node kinds. Missing one node kind = silent bug. Confirmed instances:

| Walker | Bug | Class |
|---|---|---|
| `rewrite_idents` (ir_lower.c) | BUG-573: missed NODE_TYPECAST → shadowed locals leaked original name | Audit |
| `find_orelse` (ir_lower.c) | BUG-577: missed NODE_ASSIGN and all deeper contexts → orelse reached emit_rewritten_node with "unhandled" default | Today |
| `pre_lower_orelse` (ir_lower.c, new in v0.4.6) | The *fix* — universal tree walker that replaces every orelse with a tmp-ident before emission | Prevention |
| `emit_auto_guards` (emitter.c) | Prior gap (BUG-565) — fixed by adding auto-guard call in IR block loop | v0.4.0 era |

**Prevention tool:** `tools/walker_audit.sh` cross-references cases in `emit_expr` (AST emitter) vs `emit_rewritten_node` (IR emitter). Run before release. Missing NODE_X in IR emitter = either add it, or add it to the known-pre-lowered list with documentation explaining why it never reaches emission.

**NODE_ORELSE is a KNOWN pre-lowered exception** — intentionally NOT in emit_rewritten_node. `pre_lower_orelse` walks every AST expression before passthrough, replaces orelse with `NODE_IDENT(tmp)`. Adding a NODE_ORELSE case to emit_rewritten_node was tried (v0.4.4 attempt) and REVERTED — it introduced regressions with block-return fallbacks. Pre-lowering is the correct model.

### `emit_rewritten_node` is NOT a "fake emit_expr"

The IR path uses `emit_rewritten_node` to emit C from AST fragments in IR_ASSIGN passthrough. A natural question: isn't this just emit_expr under another name?

**No — key differences:**
- `emit_expr` operates on raw source AST (all node kinds possible).
- `emit_rewritten_node` operates on PRE-LOWERED AST (simpler shape; orelse already replaced, shadowed idents already renamed, compound orelse-in-call-arg already decomposed).
- `emit_rewritten_node` has no dependency on emit_expr (zero `emit_expr(` calls in its range — verified by `grep -c` in tools/walker_audit.sh).
- IR loop structure uses goto basic blocks (no C while/for). Terminating orelse fallbacks `break`/`continue`/`return` go through IR_GOTO to basic blocks, NOT C statements. That's why they need pre-lowering at statement level — IR's CFG has no enclosing C loop to break from.

**The IR invariant: zero `emit_expr` calls in IR function body emission.** All complexity lives in lowering (ir_lower.c). Emitter stays minimal.

### Pre-lowering architecture for expression-wrapping constructs

When a new language construct can contain an expression with complex sub-semantics (orelse, if-unwrap, etc.), two choices:

**Option A — recurse in emit_rewritten_node**: Add a case that handles the construct inline as a GCC statement expression. Simple but limited — can't express IR-level control flow (break to basic block, goto label, etc.).

**Option B — pre-lower in ir_lower.c**: Add a transform that replaces the construct with a simpler AST shape (e.g., tmp-ident) before emission. More code but fully general — supports IR control flow.

ZER chose **Option B for orelse** (`pre_lower_orelse`). New constructs that involve CFG (loops, async, defer-in-block) should follow the same pattern. Option A is acceptable for value-only constructs that never need to emit IR gotos.

### Test Harness Architecture (know which suite exercises what)

| Suite | Path | What it tests |
|---|---|---|
| test_lexer/parser/checker/zercheck | C unit via `emit_file` | Per-component correctness, emission C validity |
| test_emit | C unit → GCC → run exit code | End-to-end ZER source → exit code match |
| test_firmware/2/3, test_production | C unit → GCC → run | Realistic firmware + CRC/bootloader patterns |
| tests/test_zer.sh | `zerc --run` on `.zer` files | Integration, full zerc pipeline |
| rust_tests/ | `zerc --run` | Rust-equivalent safety tests (translated) |
| zig_tests/ | `zerc --run` | Zig safety translations |
| test_modules/ | `zerc` on multi-file | Imports, cross-module mangling |

**If test_emit passes but rust_tests fails:** likely an issue with `--run` mode in zerc_main.c (not emitter).

**If rust_tests passes but test_emit fails:** likely IR/emit_file bug that `zerc --run` masks (maybe exit code ignored). This was the state before BUG-538–557 were fixed.

**If both fail:** Real compiler bug, reproduce with minimal `.zer` + `./zerc --emit-c`.

## First Session Workflow

When starting a new session or lacking context:

1. Read `CLAUDE.md` (this file) — has FULL language reference above, rules, conventions
2. **MANDATORY — read `docs/compiler-internals.md` BEFORE modifying any compiler source file** (parser.c, checker.c, emitter.c, types.c, zercheck.c). It documents every emission pattern, optional handling, builtin method interception, scope system, type resolution flow, and common bug patterns. Skipping this and discovering patterns by reading source files wastes 20+ tool calls. The document exists specifically to prevent this.
3. Read `BUGS-FIXED.md` — 506+ past bugs with root causes. Prevents re-introducing fixed bugs. Read `docs/future_plans.md` for architecture roadmap (table-driven compiler, container keyword, monomorphization).
4. `ZER-LANG.md` — full language spec (only if CLAUDE.md quick reference is insufficient)
5. **Use `make tags` + grep for code navigation** — generates ctags index (2,183 entries). Use `grep "function_name" tags` to find file+line+signature. NEVER read full source files speculatively. Read only the specific lines around grep results. This is 40x more efficient than brute-force reading.
6. Run `make docker-check` (preferred) or `make check` to verify everything passes before making changes
7. The compiler pipeline is: ZER source → Lexer → Parser → AST → Type Checker → ZER-CHECK → C Emitter → GCC. New IR path (v0.4): AST → Checker → IR (flat locals + basic blocks) → zercheck on IR → emit C from IR → GCC. Use `--emit-ir` to see IR output.

### Bug Hunting Workflow (principle-first, not brute-force)

When looking for bugs, do NOT read entire files. Instead:
1. Find ONE instance of the bug (from user report, test failure, or targeted grep)
2. Derive the GENERAL PRINCIPLE the bug violates (e.g., "TYPE_DISTINCT must be unwrapped")
3. Grep for ALL violations of that principle (`grep "->kind == TYPE_" file.c`)
4. Fix all instances, not just the reported one
This approach found 13 bugs in ~5K tokens. Brute-force reading of the same code found the same bugs in ~200K tokens.

### Flag-Handler Matrix Audit (automated cross-reference)

Run `bash tools/audit_matrix.sh checker.c` to automatically find missing checker validations. The script cross-references control-flow NODE_ handlers (return, break, continue, goto, yield, await, spawn) against context flags (in_loop, defer_depth, critical_depth, in_async, in_interrupt, in_naked). Missing checks = potential bugs.

First run found 5 bugs: yield/await missing defer_depth + critical_depth checks, spawn missing in_interrupt check.

The pattern is generic — define axes (rows × columns), extract handler bodies, cross-reference. Extend to:
- Emitter: NODE_ handlers × type_unwrap_distinct calls
- zercheck: NODE_ handlers × handle state checks
- Any subsystem with flags × handlers

### Interaction Test Gap Analysis (ctags + ls)

To find untested feature combinations:
1. `grep "NODE_" ast.h` — list all features
2. `ls tests/zer/ | grep "feature_name"` — check existing coverage
3. Difference = gap. Write tests for high-risk pairs (features that share state/control flow).

Bug-producing pairs (from history): async × orelse, distinct × optional, spawn × shared, move × switch. Independent pairs (no interaction): defer × comptime, goto × volatile.

## Project Architecture

- **zerc** = the compiler binary (`zerc_main.c` + all lib sources)
- **zer-lsp** = LSP server (`zer_lsp.c` + all lib sources)
- Source files: `lexer.c/h`, `parser.c/h`, `ast.c/h`, `types.c/h`, `checker.c/h`, `emitter.c/h`, `zercheck.c/h`, `ir.c/h` (IR data structures + validation), `ir_lower.c` (AST → IR lowering), `zercheck_ir.c` (handle tracking on CFG), `vrp_ir.c` (value range on IR)
- Test files: `test_lexer.c`, `test_parser.c`, `test_parser_edge.c`, `test_checker.c`, `test_checker_full.c`, `test_extra.c`, `test_gaps.c`, `test_emit.c`, `test_zercheck.c`, `test_firmware_patterns.c`, `test_fuzz.c`, `tests/test_semantic_fuzz.c`
- **Semantic fuzzer** (`tests/test_semantic_fuzz.c`): **32 generators** covering every ZER feature — alloc, cast, defer, interior ptr, *opaque, arena wrappers, pool/slab, goto+defer, comptime, handle alias, enum switch, while+break, Task.alloc, funcptr callback, union capture, ring buffer, nested struct deref, defer+orelse block, packed struct, slice subslice, bool/int cast, signed/unsigned cast, distinct typedef, bit extraction, non-keep escape, arena global escape. 200 tests per `make check` run, verified with 2,500 tests across 5 seeds. **When adding new features, add generator functions** — `gen_safe_<feature>()` + `gen_unsafe_<feature>()` + new case in switch.
- E2E tests in `test_emit.c`: ZER source → parse → check → emit C → GCC compile → run → verify exit code
- Cross-platform: `test_emit.c` uses `#ifdef _WIN32` macros (`TEST_EXE`, `TEST_RUN`, `GCC_COMPILE`) for `.exe` extension and path separators. Works on both Windows and Linux/Docker.
- Spec: `ZER-LANG.md` (full language spec), `zer-type-system.md` (type design), `zer-check-design.md` (ZER-CHECK design)
- **Default behavior:** `zerc main.zer` compiles to `main.exe` (or `main` on Linux) — the `.c` intermediate is temp, deleted after GCC. No `.c` visible to user. Looks native.
- Compiler flags: `--run` (compile+execute), `--emit-c` (keep `.c` output, old behavior), `--emit-ir` (print IR to stdout, exit — debugging), `--lib` (no preamble/runtime, for C interop), `--no-strict-mmio` (allow @inttoptr without mmio declarations), `--target-bits N` (usize width override), `--gcc PATH` (specify cross-compiler for auto-detect), `--stack-limit N` (error when estimated stack usage exceeds N bytes)
- `-o file.c` → emits C (kept). `-o file.exe` or `-o file` → compiles to exe (temp .c deleted).
- usize width: auto-detected from GCC via `__SIZEOF_SIZE_T__` probe at startup. Falls back to 32 if GCC not found. `--target-bits` overrides.
- GCC flags: emitted C requires `-fwrapv` (ZER defines signed overflow as wrapping). `zerc --run` adds this automatically.
- Emitted C uses GCC extensions: statement expressions `({...})`, `__auto_type`, `_Alignof`, `__attribute__((packed))`
- User-defined struct/enum/union names emit as-is (no `_zer_` prefix). Only internal names are prefixed.

## Building & Testing

**PREFERRED: Use Docker** to avoid Windows Defender false positives on compiled executables:
```
make docker-check          # build + run ALL tests in container (gcc:13 image)
make docker-test-convert   # run only conversion tool tests (zer-convert + zer-upgrade)
make docker-build          # just build zerc in container
make docker-shell          # interactive bash inside container for debugging
```

**Fallback: Native (triggers AV on Windows corporate laptops):**
```
make zerc             # build the compiler
make check            # build + run all tests
```

**DO NOT run `make check` or execute compiled binaries outside Docker** on the dev machine — Windows flags rapid create-execute-delete of unsigned .exe files as malware.

**WARNING:** Do NOT use bind mounts (`docker run -v $(pwd):/zer`) for test runs. Compiled binaries would land on the Windows filesystem and Defender sees them again. The Dockerfile uses `COPY` — all compilation stays inside the container's filesystem. Keep it that way.

**POSIX portability:** `zerc_main.c` uses `popen`/`pclose` for GCC auto-detection. These require `_POSIX_C_SOURCE 200809L` on strict C99. Without it, `popen` gets implicit `int` return → truncated 64-bit pointer → segfault. Already fixed (BUG-417). If adding other POSIX functions, check that the feature test macro covers them.

The codebase is **cross-platform** (Windows + Linux/Docker):
- `test_emit.c`: `#ifdef _WIN32` macros for `.exe` extension and path separators
- `test_modules/run_tests.sh`: detects platform via `$OSTYPE` for executable extension
- `zerc_main.c --run`: platform-appropriate exe path and run command
- `Dockerfile`: uses `gcc:13`, copies sources, runs `make check`

## Rust Test Translation Workflow

When adding more tests from Rust's test suite (`rust-lang/rust/tests/ui/`):

### Process
1. **Fetch Rust source** — use WebFetch on `https://raw.githubusercontent.com/rust-lang/rust/master/tests/ui/CATEGORY/FILE.rs`
2. **Assess applicability** — skip tests that use Rust-specific features (closures, traits, generics, iterators, HashMap, Debug, Drop trait destructors, lifetime annotations)
3. **Translate the SAFETY INTENT** — not the syntax. Map Rust patterns to ZER equivalents:
   - `Box<T>` → `Slab(T)` alloc_ptr / `Pool(T, N)` alloc
   - `Drop` trait → `defer`
   - `Rc<T>` / non-Send → non-shared `*T` to spawn (should fail)
   - `Arc<Mutex<T>>` → `shared struct`
   - `RwLock<T>` → `shared(rw) struct`
   - `mpsc::channel` → `Ring(T, N)`
   - `thread::spawn` → `spawn` / `ThreadHandle`
   - `Condvar` → `@cond_wait` / `@cond_signal`
   - `unsafe` raw pointer → `@inttoptr` / `@ptrcast`
   - `Box<dyn Any>` → `*opaque` + `@ptrcast`
   - `const fn` → `comptime`
   - `#[cfg]` → `comptime if`
4. **Write as hand-written ZER** — do NOT use agents for Pool/Slab/Arena patterns (agents get `pool.alloc()` wrong 80% of the time). Write yourself.
5. **Mark negative tests** — add `// EXPECTED: compile error` comment in file
6. **Test** — `docker run --rm -v ... gcc:13 bash -c '... /tmp/zerc file.zer --run 2>&1'`
7. **File naming** — `rt_RUST_FILE_NAME.zer` for source-translated, `gen_CATEGORY_NNN.zer` for generated

### Common agent mistakes (avoid these)
- `pool.alloc(Type)` — WRONG, should be `pool.alloc()` (no args)
- `orelse return 1` — WRONG, must be bare `orelse return`
- `*Widget ptrs[10]` — WRONG, use `Widget[10] arr` or struct wrapper
- `Arena.buf = ...` — WRONG, use `Arena.over(buf)`
- Accessing `.has_value`/`.value` on optionals directly — not allowed

### CRITICAL: How to Write Tests That Actually Find Bugs

**The natural instinct is to write tests that pass. Fight it.** The goal is to BREAK the compiler.

**Rule 1: For every positive test, write the negative counterpart.**
You wrote `rt_move_struct_return_ok` (return move struct, caller uses it). Good.
Now write: what happens if caller uses the original AFTER returning it? What if both if/else arms consume, then you use it after the if? Every safe pattern has an unsafe mirror — write both.

**Rule 2: Test through indirection, not just direct use.**
Direct: `consume(move_struct)` → use after move. Easy to catch.
Indirect: `consume(wrapper_containing_move_struct)` → use inner field. HARDER to catch.
These find real bugs: BUG-468 session found that `move struct` inside a regular struct wasn't tracked.
Always test: nested structs, array elements, function return values, orelse unwrap results.

**Rule 3: Test at merge points — if/else, switch, loops.**
- if WITHOUT else + transfer → MAYBE state (found BUG-468)
- if/else BOTH transfer → DEFINITELY transferred
- Loop body transfers → next iteration is use-after-move
- Switch: some arms transfer, others don't → MAYBE state
These are where zercheck path merging has bugs. Direct transfers are easy; conditional transfers are hard.

**Rule 4: Test dead code paths.**
`return t; u32 k = t.kind;` — code after return is unreachable. Does zercheck still flag `t.kind` as use-after-move? (Answer: no — this is a current limitation.) These edge cases reveal tracking gaps.

**Rule 5: After writing all tests, verify negative tests ACTUALLY reject.**
Run each negative test manually: `./zerc test.zer -o /dev/null`. If exit code is 0, the test compiled when it shouldn't have — you found a bug/limitation. Do NOT just trust `make check` green output; the runner marks non-compiling tests as "pass (correctly rejected)" but you need to confirm the rejection is for the RIGHT reason.

**Rule 6: Tests must verify VALUES, not just "compiles and runs."**
`return 0` at the end is a smoke test — it passes even if every computation produces garbage. Tests MUST check computed results: `if (result != expected) { return 1; }`. The async capture ghost bug hid for weeks because the test only checked "compiles and runs" without verifying the captured value survived yield+resume. A smoke test that "passes" is worse than no test — it gives false confidence.

**Rule 7: Don't only test the feature — test its interaction with OTHER features.**
Move struct + defer, move struct + orelse, move struct + switch capture, move struct inside Pool handle field. The feature works in isolation; the bugs are in combinations.

**Patterns that found bugs in practice (2026-04-09/10):**
- `if (c) { consume(move_struct); } use(move_struct)` → found BUG-468 (HS_TRANSFERRED not merged)
- `consume(regular_struct_containing_move_field)` → found nested move struct limitation
- `return move_struct; use_after_return` → found return-doesn't-transfer limitation
- `pool_b.free(handle_from_pool_a)` → found BUG-471 (pool.free missing type check)
- `spawn local_func_with_cross_module_shared_param` → found BUG-472 (spawn wrapper can't find local func with cross-module type)

**Rule 7: NEVER "simplify" a failing test to make it pass.**
When a test hits a compiler bug, the instinct is to simplify the test (remove spawn, remove cross-module type, use single-thread instead). THIS IS THE SAME AS REWRITING. The test found a bug — keep it exactly as written, move to limitations. Write a SEPARATE simpler test if you want coverage for the non-broken path. This rule was violated in this session (shared_user.zer simplified to remove spawn) and immediately caught by the user.

### CRITICAL: Never Hide Limitations by Rewriting Tests

When a test fails due to a **compiler limitation** (not a test bug):

1. **DO NOT rewrite the test** to use a different ZER pattern that avoids the limitation. This hides the gap and creates a false positive — you think the feature works when it doesn't.
2. **Only rewrite if the pattern genuinely doesn't exist in ZER** (e.g., closures → function pointers, generics → concrete types). If ZER HAS the feature but the compiler doesn't handle it correctly, that's a bug to fix — not a test to rewrite.
3. **Keep the failing test as-is** — move it to `rust_tests/limitations/` with a comment explaining what zercheck/checker can't handle.
4. **Write a SECOND test** using the workaround pattern if needed for coverage, but keep the original.
5. **Log the limitation** in `docs/compiler-internals.md` with: what pattern fails, why it fails, estimated fix complexity.
6. **Investigate the root cause** — most "limitations" are actually bugs (like BUG-462). Try to fix before labeling as limitation.

**Example of what NOT to do:**
- Test: `ents[0] = m0 orelse return; ... pool.free(ents[0]);` — fails with false "handle leaked"
- WRONG: Rewrite to use named variables `h0 = m0 orelse return` (hides the limitation)
- RIGHT: Keep original, investigate why `handle_key_from_expr` fails through `orelse`, fix zercheck

**Why this matters:** Every rewritten test is a limitation we forgot about. When we later claim "493 tests pass, 0 failures," it should mean the compiler handles ALL those patterns — not that we rewrote the hard ones to be easy.

**This rule found 6 bugs in one session (BUG-462 through BUG-467):**
- BUG-462: handle array test rewritten to named vars → orelse unwrap missing in assignment aliasing
- BUG-463: struct field alias test labeled "limitation" → NODE_FIELD only checked root, not prefixes
- BUG-464: deadlock interleave test labeled "limitation" → lock ordering model was wrong
- BUG-465: funcptr spawn test rewritten to integer dispatch → emit_type_and_name not used for spawn struct
- BUG-466: *opaque vtable array rewritten to separate vars → constant-index homogeneity check too strict
- BUG-467: `?*T[N]` array test used typedef workaround → parser precedence swap missing for pointer-wrapped arrays

Every single "limitation" was a fixable bug. The `limitations/` directory is now empty.

### Coverage status (as of v0.3.0+, updated 2026-04-09)
- `tests/ui/threads-sendsync/` — **COMPLETE** (51/67)
- `tests/ui/consts/` — 16 patterns (shift safety, div-by-zero, OOB, overflow wraps, comptime)
- `tests/ui/borrowck/` — 30 patterns (field sensitivity, nested call free, scope escape, union borrow, struct field alias UAF, interior pointer prefix walk, cross-func double/maybe free, loop alloc/free, switch all-arms free)
- `tests/ui/moves/` — 37 patterns (ownership chain, conditional, loop, cross-function, struct field move, move struct: return/nested/compose/switch/while/if-else-both/partial-field)
- `tests/ui/drop/` — 18 patterns (struct-as-object, dynamic drop, defer ordering, LIFO cleanup, nested defer, switch cleanup, multiple early return)
- `tests/ui/unsafe/` — 11 patterns (mmio, inttoptr, provenance, mmio OOB, ISR slab reject, volatile rw, ptrcast correct/wrong)
- `tests/ui/nll/` — 9 patterns (interior ptr, drop conflict, subpath invalidation, borrowed-local escape, return-before-defer, sequential alloc, switch arm alloc, block scope, cross-block reuse)
- Generated tests: 164 `gen_*` + 43 `rc_*` + 27 `safety_*` + 21 `conc_*` = 255 (all audited)
- Concurrency: 40+ tests — shared struct, spawn (scoped/fire-forget/reject), condvar (signal/broadcast/timeout), atomics (CAS/load/store/add/sub/or/and/xor), deadlock detection, Ring channel, barrier, @once, threadlocal, async/await, shared(rw)
- Total: 661 Rust-equivalent tests in `rust_tests/`, 0 limitations remaining
- **All limitations fixed:** BUG-468 (conditional move), BUG-469 (nested move in struct), BUG-470 (return transfer), BUG-471 (pool.free type check)
- **Systematic refactoring:** 16 unified helpers across zercheck.c/checker.c/emitter.c. See `docs/refactoring_gaps.md` for full analysis.
- **CFG-aware zercheck:** `PathState.terminated` flag + dynamic fixed-point iteration (ceiling 32). Replaces block_always_exits hack, 2-pass+widen hack, backward-goto re-walk hack.

### High-Value Test Categories for Finding Bugs
From analysis of Rust's test tree, these categories stress ZER's model the hardest:
- **borrowck/**: UAF, cross-function free (FuncSummary), field sensitivity (interior pointers), scope escape, union borrow safety
- **moves/**: Ownership transfer, conditional free (MAYBE_FREED), loop UAF, nested loop propagation
- **nll/**: Drop+borrow conflict (defer ordering), borrowed-local escape, subpath invalidation
- **unsafe/**: `@inttoptr` without mmio, provenance mismatch, MMIO bounds

When adding new tests, prioritize these categories. Skip: closures, generics, traits, iterators, async runtime (Rust-specific features ZER doesn't have).

## Git Rules

- NEVER add Co-Authored-By or any Claude/AI attribution to commits
- All commits must be under zerohexer@gmail.com only
- No other authors, no co-authors, no AI mentions in commit messages

## Testing Methodology — MANDATORY

Every compiler component (lexer, parser, AST, type checker, ZER-CHECK, safety passes,
C emitter) MUST follow specification-based testing with full positive/negative coverage.

### The Rule

For EVERY rule in ZER-LANG.md:
1. **Positive test**: valid code that exercises the rule — must compile/pass
2. **Negative test**: code that violates the rule — must produce an error

NO EXCEPTIONS. If a spec rule exists, both tests must exist.

### Checklist Derivation

1. Read the spec section
2. Extract every rule (explicit or implied by example)
3. Write positive test proving the rule works
4. Write negative test proving the violation is caught
5. If either test fails, fix the implementation BEFORE moving on

### When Adding New Features

- Write tests FIRST (or alongside), never after
- Every new AST node kind must have parser tests
- Every new type check must have positive + negative checker tests
- Every new coercion rule needs both directions tested
- Every error message must be triggered by at least one test

### Edge Case Protocol

Before marking ANY component as "done":
1. Run the full test suite (all components)
2. Write adversarial tests — code that SHOULD break
3. Test boundary conditions (empty input, max values, deeply nested)
4. Test interactions between features (orelse + if-unwrap, defer + loop + break)
5. Verify error messages are helpful (file, line, what went wrong)

### Regression Prevention

- Never delete a passing test
- If a bug is found, write a test that reproduces it BEFORE fixing
- All tests must pass before any commit
- Run `make docker-check` (or `make check`) before every push

### Pipeline Integration Testing — CRITICAL

**Problem this solves:** zercheck existed for months with 50+ passing unit tests, but was NEVER called from the actual compiler (`zerc_main.c`). All safety promises were lies — the tests tested zercheck in isolation, not whether `zerc` actually invoked it. This was caught on 2026-04-03.

**The rule:** For every safety system (checker, zercheck, emitter safety checks), there MUST be a `.zer` file in `tests/zer_fail/` that exercises it through the FULL compiler pipeline:

```
tests/zer/          ← positive: must compile + run + exit 0
tests/zer_fail/     ← negative: must FAIL to compile
```

**Current negative tests (`tests/zer_fail/`):**
- `uaf_handle.zer` — use-after-free on Pool handle (zercheck)
- `double_free.zer` — double free on Pool handle (zercheck)
- `maybe_freed.zer` — use handle after conditional free (zercheck MAYBE_FREED)
- `bounds_oob.zer` — compile-time array OOB (checker)
- `div_zero.zer` — division by zero literal (checker)
- `null_ptr.zer` — non-null pointer without init (checker)
- `dangling_return.zer` — return pointer to local (checker escape analysis)

**When adding a new safety feature:**
1. Write the checker/zercheck/emitter code
2. Write C-level unit tests (test_checker_full.c, test_zercheck.c, test_emit.c)
3. **ALSO write a `tests/zer_fail/feature_name.zer`** that exercises the feature through `zerc`
4. If the negative test compiles when it shouldn't → the feature isn't integrated into the pipeline

**The test runner (`tests/test_zer.sh`):**
- Positive tests: `zerc file.zer --run` must exit 0
- Negative tests: `zerc file.zer -o /dev/null` must exit non-zero
- Both run as part of `make check`

## Debugging Workflow — ZER Source Code Bugs

### The Rule
COMMIT before debugging. Always. No exceptions.
If you can't commit (tests failing), stash.
`git checkout -- file` reverts EVERYTHING including your fix.

### Correct Workflow

1. **Reproduce first**
   Write the minimal ZER program that triggers the bug.
   Smallest possible. One struct. One function. If you can't reproduce in 10 lines, simplify.

2. **State what you expect vs what you get**
   "current.next should return ?*Node (kind=14) but returns *Node (kind=13)"
   Not "the orelse is broken somehow."

3. **One debug line, targeted**
   Add ONE fprintf at the exact decision point. Run. Read output. Remove debug. Fix.
   Never add multiple debug prints across multiple files.

4. **Confirm root cause before fixing**
   "kind=13 is_optional=0 on pointer auto-deref path" — confirmed root cause.
   "I think the typemap might be overwritten" — hypothesis, needs confirmation.

5. **Fix one thing**
   The fix should be 1-5 lines. If growing beyond that, you're fixing the wrong thing.

6. **`make docker-check` immediately after fix**
   950+ tests must pass. If not — revert, re-examine root cause.

7. **Update BUGS-FIXED.md** with: symptom, root cause, fix, test reference.

8. **Commit before anything else**

### Always Update Docs After Changes — MANDATORY

After any bug fix or feature change that passes `make check`, update ALL relevant docs in the SAME commit. Never leave doc updates for later. Future sessions depend on these being accurate.

- `BUGS-FIXED.md` — add the bug with symptom, root cause, fix, test reference
- `docs/compiler-internals.md` — if ANY emitter pattern, checker behavior, type handling, builtin method, or preamble changed. This is the primary reference future sessions read. Stale info here causes bugs.
- `docs/reference.md` — if ANY language feature, syntax, intrinsic, builtin method, or type behavior changed. This is the user-facing language reference. Must reflect what `zerc` actually compiles, not what's spec'd but unimplemented.
- `README.md` — if test counts, features, or status changed
- `ZER-LANG.md` — if spec behavior changed
- `CLAUDE.md` — if syntax rules, implementation status table, or workflow changed

**Only update docs after `make check` passes and you have high confidence the change is correct.** Do not update docs for speculative or in-progress work.

### Compiler Crash Debugging — Use ASan

When the compiler (`zerc`) crashes with a segfault:

1. **Build with AddressSanitizer** to get exact crash location:
   ```
   docker run --rm zer-check bash -c 'cd /zer && gcc -g -fsanitize=address -O0 -I. -o zerc_asan lexer.c parser.c ast.c types.c checker.c emitter.c zercheck.c zerc_main.c && ./zerc_asan /tmp/crash.zer -o /tmp/crash.c 2>&1'
   ```

2. ASan output gives file:line and stack trace — go directly to the crash location.

3. Common crash patterns:
   - **Wrong variable used** — `arg_node` vs `karg` (BUG-441): validated one variable but dereferenced another. ASan showed `checker.c:3148` → immediately found `arg_node` should be `karg`.
   - **NULL type dereference** — `type->pointer.inner->kind` where `inner` is NULL. Add NULL check.
   - **Missing `type_unwrap_distinct()`** — accessing `.struct_type` on TYPE_DISTINCT.

This saved hours of debugging in BUG-441 — ASan pinpointed the exact line in 1 command instead of printf-based bisection.

### Red Flags — Stop and Revert

- Fix growing beyond 10 lines
- Adding platform-specific ifdefs to fix a type bug
- Restructuring unrelated code to make the fix work
- Debug prints spreading across multiple files
- Circular reasoning ("but it should work because...")
- More than 2 rounds of debug-without-fix

When you see these: `git checkout -- <file>`. Start over with a cleaner hypothesis.

### Anti-Circular Rule

If you've spent more than 3 debug cycles on the same bug without a confirmed root cause:
1. STOP. Revert all uncommitted changes.
2. State the bug in ONE sentence: "X returns Y but should return Z."
3. Add ONE fprintf at the exact decision point.
4. Run. Read. The output tells you where to look next.
5. If the output contradicts your hypothesis, your hypothesis is wrong. Form a new one.

The pattern that causes circular debugging: assuming the fix location before confirming the root cause. You end up modifying code that isn't the problem, which creates new symptoms that look like the original bug.

### Lessons From BUG-416/417 Debugging Session

**"Pragmatic fix" means "we don't understand the root cause yet."** The previous session added a name-based struct matching fallback in the emitter because `find_unique_allocator` returned NULL. The hypothesis was "pointer identity fails across modules." Debug fprintf showed the pointers were identical — the REAL bug was `find_unique_allocator` finding the same allocator TWICE (raw name + mangled name from BUG-233) and returning NULL for "ambiguous." One-line fix: `if (found && found->type == t) continue;`.

**When a function returns NULL, check WHY it returns NULL, not just that it does.** `find_unique_allocator` has two NULL-return paths: "not found" and "ambiguous" (two matches). The previous session assumed "not found." Debug fprintf on the match sites immediately showed "found_already=1" — ambiguity, not absence.

**Environment-specific crashes mask real bugs.** The popen segfault (BUG-417) prevented the previous session from testing properly, leading to the wrong diagnosis. The popen crash was a C99 portability bug (`popen` not declared → implicit `int` return → 64-bit pointer truncated). Always check `-Wall` output for `implicit declaration of function` warnings.

**Dual symbol registration is a known pattern in multi-module.** Imported globals exist under BOTH raw name and mangled name in global scope (BUG-233 design). Any code scanning global scope for unique matches must handle this: check `found->type == candidate->type` before declaring ambiguity. This pattern exists because: (1) raw name needed for unqualified calls from the same module, (2) mangled name needed for cross-module disambiguation.

**`_POSIX_C_SOURCE` is required for `popen`/`pclose` on strict C99.** Without it, these POSIX functions get implicit `int` declaration. On 64-bit, this truncates `FILE*` pointer → SIGSEGV. The `zerc_main.c` fix: `#define _POSIX_C_SOURCE 200809L` before `<stdio.h>` (guarded `#ifndef _WIN32`). This bug does NOT manifest on Windows, Docker `gcc:13`, or when compiling with `-std=gnu99`.

### The Pointer Auto-Deref Pattern (ZER-specific)

When a field access bug presents:
- **Value path** (`a.field`): struct field lookup at line ~848
- **Pointer path** (`ptr.field`): auto-deref then field lookup at line ~878
Check BOTH paths separately. They have different code and different bugs.

### Prompt Template for Debugging Sessions

When asking Claude to debug a ZER compiler bug:

```
Here is the minimal ZER program that fails: [paste]
Here is the exact error: [paste]
Here is what I expect: [one sentence]
Here is the relevant compiler code: [paste the ONE function]
Add one debug print to confirm the root cause.
Do not restructure. Do not fix yet. Just confirm.
```

## Confirmed NOT Bugs — DO NOT Re-Investigate

These were thoroughly investigated during the 2026-04-14 full codebase audit (25,757 lines) and confirmed correct:

1. **`types.c can_implicit_coerce` missing distinct unwrap** — BY DESIGN. Distinct types intentionally block implicit conversion. Only T→?T (line 367) unwraps.
2. **`type_equals` nominal for distinct** — BY DESIGN. Pointer identity = same definition.
3. **Const/volatile laundering checks without unwrap** (checker.c lines 3179, 3190, 3888, 3896, 6278, 6286, 7577, 7588) — Belt-and-suspenders. `can_implicit_coerce` catches at type level. Missing unwrap gives generic error instead of specific message. NOT safety holes.
4. **`pathstate_equal` asymmetric check** (zercheck.c:62) — Correct for loop convergence. New handles from loop body are expected.
5. **`import_asts[64]`** (zerc_main.c:460) — Graceful degradation, not crash.
6. **Emitter `asname[128]`** (emitter.c:1746) — Used with `%.*s` which stops at null. Safe.
7. **`@offset` struct type check** (checker.c:5254) — Cosmetic only, not safety.
8. **`scan_unsafe_global_access` static depth counter** (checker.c:6109) — Safe, LSP is single-threaded.

## Agent-Verify Workflow — Bug Hunting & Test Writing

When looking for bugs or writing new tests, use the spawn-then-verify pattern:

### The Pattern

1. **Spawn agent(s)** to audit code or write tests (they act as fresh sessions)
2. **Verify every finding** yourself before acting — agents make mistakes
3. **Fix confirmed issues**, reject false positives

### For Bug Hunting

Spawn an agent to audit a specific file (e.g., emitter.c). Tell it to:
- Read CLAUDE.md and docs/compiler-internals.md first
- Look for specific bug patterns (optional handling, type emission, intrinsic gaps)
- Report with exact line numbers, triggering ZER code, wrong C output, expected C output
- NOT fix anything — just report

Then verify each finding by reading the actual code at the cited lines. Agents find real bugs (e.g., BUG-042 `?Enum` anonymous struct) but also report false positives.

### For Test Writing

Spawn an agent to write new E2E tests. **MUST include the ZER syntax rules block** from "Spawning Agents" section above. Then verify:
- ZER syntax is correct (switch arms have braces, no `++`)
- Expected exit codes are mathematically correct
- Tests actually compile and pass with `make check`

### Multi-Round Audit Protocol

For pre-release auditing, run multiple rounds. Each round gets more targeted as obvious bugs are fixed. The prompt quality matters — here's the pattern that converged from 12 bugs to 1 across 5 rounds:

**Round 1 prompt (broad):**
- "Read CLAUDE.md, compiler-internals.md, BUGS-FIXED.md, then read checker.c/emitter.c"
- "Look for: optional handling gaps, slice emission, intrinsic validation, missing error cases"
- Results: finds obvious gaps (bool coercion, missing handlers)

**Round 2+ prompts (targeted interactions):**
- List ALL bugs already fixed — "Previous rounds fixed: [list]. Do NOT re-report these."
- Ask for INTERACTION bugs: "Look for two features combining incorrectly"
- Give specific combinations to check: "defer + orelse + break in for loop", "volatile + @inttoptr", "?void + orelse return in var-decl path"
- Say: "If you find ZERO bugs, say CLEAN — that's a valid outcome."

**Key prompt elements that improve agent quality:**
1. Tell what was ALREADY fixed (prevents re-reporting — agents waste turns on known bugs)
2. Tell it to check INTERACTIONS, not basic rules (negative test sweep covers those)
3. Give specific interaction patterns (agents check exactly what you ask)
4. Allow "CLEAN" as valid output (prevents agents from inventing false positives)
5. Include the ZER syntax rules block (prevents agents from writing invalid test code)

**Expected convergence:** Round 1: 10+ bugs. Round 2: 5-10. Round 3: 2-3. Round 4+: 0-1. If round 3+ still finds 5+, the checker/emitter has structural issues — stop and refactor.

**After auditing:** Run a systematic negative test sweep — write one test for every `checker_error()` call site that has no test. This closes the gaps permanently so future audits find only interaction bugs.

### Why This Works

- Agents explore without consuming your main context window
- Fresh perspective catches blind spots
- Verification step catches agent mistakes (wrong syntax, false positives)
- Multi-round convergence proves stability — each round finds fewer bugs
- You get both breadth (agent exploration) and depth (your verification)

## Diff-Based Post-Release Audit (when you suspect drift)

After a large structural change (IR transition, major refactor, multi-session
work), don't only rely on green tests. Green can mean "test passed" OR "test
skipped silently" OR "test passed for the wrong reason." The 2026-04-18 audit
of the 029919e..HEAD diff (141 commits, ~10k new lines) found three real
issues that no test suite caught. Use this checklist before declaring a
milestone done.

### The audit protocol (run before claiming a milestone complete)

1. **Stat the diff**: `git diff <anchor>..HEAD --stat | sort -t'|' -k2 -rn` —
   lists the biggest-delta files first. Focus audit effort there.
2. **Grep the new code for drift markers**:
   - `TODO|FIXME|XXX|HACK` in new additions (`git diff <anchor>..HEAD -- file.c | grep "^+" | grep TODO`)
   - `unhandled|shouldn't happen|should not happen` — default cases that emit garbage if hit
   - `fprintf\(out|emit\(e,` with comment-only content and no following emit
3. **Compile a real .zer → emitted C** and grep the output for stray tokens.
   Dead stubs leave fingerprints: `grep "/\* " output.c | head` catches
   comment-only emissions with no payload. The 2026-04-18 audit found
   `/* forward */ ` on line 1 of every multi-module output this way.
4. **Check skip lists hygiene**: `grep -A2 KNOWN_FAIL tests/test_zer.sh
   rust_tests/run_tests.sh zig_tests/run_tests.sh`. Every entry must
   correspond to a still-active issue in `docs/limitations.md`. Entries
   that refer to bugs since fixed are falsely masking green status.
5. **Verify every new .c file is linked**: `grep -E "\\.c\\b" Makefile`
   vs `ls *.c`. Unlinked .c files are either WIP (check the roadmap
   doc — `docs/IR_Implementation.md` etc.) or forgotten.
6. **Run `bash tools/walker_audit.sh`** — this catches the #1 silent-bug
   class (missing NODE_ kind in `emit_rewritten_node`) by cross-referencing
   the IR emitter against the AST emitter.

### Dead-stub pattern (the most dangerous drift)

A "dead stub" is code that writes a comment prefix but never completes
the emission:

```c
/* BAD — prints the comment then exits the branch */
if (cond) {
    fprintf(out, "/* forward */ ");
    /* "The actual definition will follow below" — except it never does */
}
```

This pollutes emitted output silently because `/* forward */` followed
by the next line's real code is still valid C — just ugly. Tests pass.
grep-for-comments in emitted output catches it.

**Prevention**: when you catch yourself writing a partial emit ("I'll
fill this in after I finish the loop"), either commit with the emit
complete OR add a compile-error `#error` so the code can't build. Never
leave "will follow" stubs.

### Real-code output grep (catches things tests don't)

After any emitter change, run:

```bash
./zerc some_real_test.zer --emit-c -o /tmp/r.c
grep -nE "/\* (TODO|forward|unhandled|stub|placeholder) \*/" /tmp/r.c
grep -nE "^/\* [a-z]+ \*/$" /tmp/r.c   # comment-only lines
```

Anything matching is a signal: either a dead stub, or a code path that
fell through an `/* unhandled node %d */0` fallback — both are silent
miscompiles the test harness can't detect because `0` is a valid C
literal that compiles clean.

### When WIP files are NOT dead code

Two files in the repo (as of 2026-04-18) compile cleanly but are NOT
in the Makefile: `zercheck_ir.c` (452 lines) and `vrp_ir.c` (349
lines). These are Phase 8-9 placeholders per the IR roadmap — the
IR-native equivalents of the current AST-based `zercheck.c` and VRP.
**Don't delete them.** The pattern "compile-clean but unlinked" is
usually intentional WIP; check `docs/future_plans.md` and
`docs/IR_Implementation.md` before concluding dead code.

### What this audit found (reference for calibration)

2 real bugs + 1 stale skip-list entry in +10,000 lines across 141
commits. That's ~0.02% defect density, which is what "green tests"
*should* mean but rarely does without this kind of audit.

Key insight: the bugs weren't in code that had been validated — they
were in code paths the tests thought they were exercising but weren't.
`zerc --run` returning 0 for a SIGTRAP'd program (BUG-581 era) and
`/* forward */` never emitting its body (this audit) both fell into
the same category: the validation story, not the code, was broken.

When the next "everything passes" milestone happens, run this audit.
