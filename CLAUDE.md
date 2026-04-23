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

16. **`unsafe asm` is REQUIRED for inline asm (2026-04-23).** Bare `asm(...)` is rejected at compile time with message "use 'unsafe asm(...)' as explicit escape hatch marker". Only allowed inside `naked` functions (Phase 1 verified: `zer_asm_allowed_in_context`). Users prefer `@intrinsic()` calls; `unsafe asm` is an escape hatch for edge cases. See `docs/asm_plan.md`.
    ```
    naked void handler() {
        unsafe asm("cli");        // OK — Rust-style explicit marker required
        // asm("nop");             // COMPILE ERROR: bare 'asm' not allowed
    }
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
@barrier()               full memory barrier (seq_cst)
@barrier_store()         store barrier (release)
@barrier_load()          load barrier (acquire)
@barrier_acq_rel()       acquire+release fence            [D-Alpha-2]
@offset(T, field)        offsetof
@container(*T, ptr, f)   container_of (field-validated, provenance-tracked)
@trap()                  crash intentionally
@probe(addr)             safe MMIO read — returns ?u32, null if address faults
@unreachable()           GCC unreachable hint (UB if reached)  [D-Alpha-2]
@expect(val, expected)   branch prediction hint                [D-Alpha-2]
```

### Atomic Intrinsics (15, all SEQ_CST ordering)
```
# Load / store / cas:
@atomic_load(*T) -> T
@atomic_store(*T, val)
@atomic_cas(*T, expected, desired) -> bool

# Fetch-old (returns value BEFORE op):
@atomic_add(*T, val) -> T          # existing
@atomic_sub(*T, val) -> T
@atomic_or(*T, val) -> T
@atomic_and(*T, val) -> T
@atomic_xor(*T, val) -> T
@atomic_nand(*T, val) -> T         [D-Alpha-1]
@atomic_xchg(*T, val) -> T         [D-Alpha-1] swap, returns old

# Fetch-new (returns value AFTER op):
@atomic_add_fetch(*T, val) -> T    [D-Alpha-1]
@atomic_sub_fetch(*T, val) -> T    [D-Alpha-1]
@atomic_or_fetch(*T, val) -> T     [D-Alpha-1]
@atomic_and_fetch(*T, val) -> T    [D-Alpha-1]
@atomic_xor_fetch(*T, val) -> T    [D-Alpha-1]
```
All atomics: first arg must be `*shared T` where T is integer of width 1/2/4/8 bytes.
Ordering parameter (relaxed/acquire/release/acq_rel/seq_cst) deferred to later batch.

### Bit Query / Byte Swap Intrinsics (D-Alpha-2)
```
@bswap16(u16) -> u16     byte swap
@bswap32(u32) -> u32
@bswap64(u64) -> u64
@popcount(x) -> u32      count 1 bits  (dispatches on u32 vs u64 width)
@ctz(x) -> u32           count trailing zeros
@clz(x) -> u32           count leading zeros
@parity(x) -> u32        0=even / 1=odd parity
@ffs(x) -> u32           find first set bit, 1-indexed (0 if input is 0)
```
All use GCC builtins — auto-port to x86-64/ARM64/RISC-V without per-arch work.

### Interrupt Control Intrinsics (D-Alpha-3, privileged — kernel mode only)
```
@cpu_disable_int()              disable interrupts globally
@cpu_enable_int()               enable interrupts globally
@cpu_wait_int()                 halt until next interrupt (hlt/wfi)
@cpu_save_int_state() -> u64    read current interrupt flag state
@cpu_restore_int_state(u64)     restore saved flag state
```
Per-arch inline asm emission (x86 cli/sti/hlt, ARM cpsid/cpsie/wfi, RISC-V csrci/csrsi/wfi).
SIGSEGV in user mode — use for kernel code only. Test pattern: place in dead branch with
`volatile u32 never_true = 0; if (never_true == 42) { ... }` to verify compilation without
executing privileged asm.

### Context Switch Intrinsics (D-Alpha-4, scheduler primitives)
```
@cpu_save_context(*u8 buf)       save callee-saved GPRs     (128+ byte buffer)
@cpu_restore_context(*u8 buf)    restore callee-saved GPRs
@cpu_save_fpu(*u8 buf)           save SIMD/FP state         (512+ byte buffer, 16-byte aligned)
@cpu_restore_fpu(*u8 buf)        restore SIMD/FP state
```
Callee-saved only (rbx/r12-r15 on x86, x19-x28 on ARM64, s0-s11 on RISC-V).
Full RSP/RIP save requires naked functions — kernel-integration scope.
Buffer arg can be `*u8` or `u8[N]` — checker accepts both. Test via dead-branch pattern.

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

All language features (primitives, pointers, optionals, structs/enums/unions, containers, concurrency, async, defer, goto, comptime, intrinsics, MMIO, move struct, IR pipeline, etc.) are **complete on both Checker and Emitter paths**. Feature-level detail, per-feature status, and recent additions are tracked in:
- `BUGS-FIXED.md` for recent fixes and session entries
- `docs/compiler-internals.md` for implementation patterns and invariants
- `docs/reference.md` for user-facing feature documentation

IR pipeline is load-bearing since 2026-04-17 (commit `82335c3`). No AST fallback for function bodies. IR data structures in `ir.c`/`ir.h`, lowering in `ir_lower.c`, C emission from IR in `emitter.c`. See `docs/compiler-internals.md` "AST→IR emission safety invariant" for rules when touching IR emitter handlers.

<!-- Historical per-feature table removed 2026-04-19 — 90+ Done/Done rows
     that rotted as features landed. Above summary replaces them; git log,
     BUGS-FIXED.md, and docs/compiler-internals.md (IR invariants section)
     have authoritative history. -->

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

### Proof Internals — MANDATORY for Coq/Iris work

**MANDATORY — read `docs/proof-internals.md` BEFORE modifying any `proofs/operational/**/*.v` file.** It has a "Fresh-session reading order" section at the top that sequences what to read (~65 min of context, saves 2-5 hours of rediscovery). Covers: Docker/MSYS build quirks, Iris name collisions (`expr`, `val`, `state` shadow our types after `Require Import weakestpre`), typeclass subtyping (`::`), `IntoVal`/`AsVal` for `wp_value`, `destruct` intropattern pitfalls, ghost-map delete-vs-update design, Coq nested-comment lexer traps, gset/sets imports, `()` vs `tt` for unit values, `-Q` bindings per subset, and a 12-row common-errors + fixes table.

**Current proof state (2026-04-21):** 47 Iris/Coq files, **~330 axiom-free theorems** across **7 subsets** (λZER-Handle/Move/Opaque/Escape/MMIO as operational + λZER-Typing as predicate-based covering all typing sections). Zero admits. All 203 rows in `docs/safety_list.md` have real proofs — NO `True. Qed.` placeholders remain for substantive rows. `make check-proofs` verifies zero admits. Layer 2: 106 theorem-linked `.zer` tests all passing.

**Sections at operational step-based depth:** A (handle, 18), B (move, 8), H (MMIO, 9), J-core (opaque, 6), O (escape, 12). Total 53 rows via step rules + resource algebra + state_interp + step specs.

**Sections at real Coq predicate depth:** C, D, E, F, G, I, J-rest, K, L, M, N, P, Q, R, S, T (~120 rows) — all in `lambda_zer_typing/typing.v`. Bool-returning predicates + theorems about them. Decidable, mechanically checkable. Not step-based, but REAL proofs (not placeholders).

**Not safety-semantic:** U (35 rows — pure well-formedness, correctly marked `—`).

**Level 3 — Architecture 1 extract-and-link VST (2026-04-21, 7 real extractions):** Pure predicate functions extracted from zercheck.c/zercheck_ir.c/checker.c into `src/safety/*.c`. The SAME `.c` file is linked into zerc (via Makefile CORE_SRCS) AND verified by `make check-vst` (via CompCert clightgen). If a change breaks the Coq spec, check-vst fails — blocks PR.

Extracted so far (44 total — **PHASE 1 COMPLETE 2026-04-22**):
- `src/safety/handle_state.c` — 4 predicates: state checks
- `src/safety/range_checks.c` — 3 predicates: count/bounds/variant
- `src/safety/type_kind.c` — 7 predicates: type categories
- `src/safety/coerce_rules.c` — 5 predicates: widening + qualifier preservation
- `src/safety/context_bans.c` — 6 predicates: return/break/continue/goto/defer/asm
- `src/safety/escape_rules.c` — 3 predicates: region-tag escape (λZER-escape)
- `src/safety/provenance_rules.c` — 3 predicates: @ptrcast provenance (λZER-opaque)
- `src/safety/mmio_rules.c` — 2 predicates: @inttoptr safety (λZER-mmio)
- `src/safety/optional_rules.c` — 2 predicates: null/optional rules (typing.v N)
- `src/safety/move_rules.c` — 2 predicates: move struct tracking (λZER-move)
- `src/safety/atomic_rules.c` — 2 predicates: @atomic width/arg (typing.v E)
- `src/safety/container_rules.c` — 3 predicates: field/container/depth validity (typing.v T+K)
- `src/safety/misc_rules.c` — 2 predicates: int-switch default, bool-switch exhaustive (typing.v Q)

**Phase 2 batches (decision extraction):**
- `src/safety/isr_rules.c` — 4 predicates: `zer_alloc/spawn_allowed_in_isr/critical` (hardware ban decisions, oracle: CLAUDE.md Ban Framework + typing.v C03/C04/S04/S05)

Call sites: zercheck.c (is_handle_invalid, is_handle_consumed, is_move_struct_type, should_track_move), zercheck_ir.c (ir_is_invalid), checker.c (Pool/Ring count + control-flow handlers + return-escape + @ptrcast + @inttoptr + TYNODE_OPTIONAL + @atomic_*), types.c (type_is_*, can_implicit_coerce). All delegate.

**Architecture 1 chosen over Architecture 2** (full Coq rewrite + extract). Reasoning: LLM velocity (C >> Coq), incremental value at every phase, no heroic rewrite risk, working compiler throughout. Architecture 2 reserved for stable subsystems year 2+. See `docs/formal_verification_plan.md` Level 3 section for concrete 8-phase roadmap.

**Full verification commitment (2026-04-21):** target is seL4-level formal verification = every safety property proven at operational depth + every compiler check VST-verified + CI enforces the whole thing. Total effort: ~1,085 hrs (~1 year focused, ~3 years casual).

**The 8 phases:**
1. Phase 0 — Infrastructure DONE
2. Phase 1 — 85 pure predicates (~180 hrs) — **85/85 (100%) — FULL PHASE 1 COMPLETE 2026-04-22** (see docs/phase1_catalog.md for definitive enumeration)
   - 71 = strict milestone (operational + typing.v real theorems, excluding concurrency schematic)
   - 85 = full target (includes 14 concurrency schematic — real theorems, weaker oracle until Phase 7 upgrades)
   - Decision 2026-04-22: extract full 85. Phase 6 `check-no-inline-safety` requires it; Phase 7 upgrades Coq spec only (C unchanged, ~20 min per predicate).
   - Batches landed 2026-04-22: B1 M-section, B2 L-extras, B1b M08 redone, BUG-603 void-main fix (415/415), B3 T+K container extras, B4 P variant, B5 S stack, B6 R comptime, B7 J-extended (strict milestone 71), B8 E atomic extras, B9-11 concurrency (full 85).
3. Phase 2 — 60 decision extractions (~150 hrs) — **4/60 (7%)** — ISR/@critical bans batch 1 (counts toward Phase 1 pure predicates too)
3. Phase 2 — 60 decision extractions (~150 hrs)
4. Phase 3 — Generic AST walker (~60 hrs)
5. Phase 4 — Verified state APIs (~240 hrs)
6. Phase 5 — Phase-typed checker (~30 hrs)
7. Phase 6 — CI discipline (~30 hrs)
8. Phase 7 — Deepen 82 schematic rows to operational (~425 hrs). Sub-phases: λZER-concurrency (~150), λZER-async (~80), λZER-control-flow (~30), λZER-mmio-rest (~20), λZER-opaque-rest (~30), λZER-escape-rest (~30), λZER-typing-extra (~15), λZER-vrp (~50), λZER-variant (~15), spec reviews (~25).
9. Phase 8 — Release polish (~50 hrs)

**End state claim:** "ZER is a formally verified compiler. For every program ZER accepts, the resulting C is provably free of 200+ specified safety properties — proven in Coq, verified in VST, tested empirically. Trust base: Coq kernel + GCC + hardware." Same strength as CompCert / seL4.

**Each phase is stop-able with real value shipped.** Extractions commit one-at-a-time. Deepening subsets commit per-subset. No big-bang milestones.

**Level 3 structure:**
- `src/safety/handle_state.c` — extracted predicate, linked into zerc
- `src/safety/handle_state.h` — declarations + state constants (ZER_HS_*)
- `proofs/vst/verif_handle_state.v` — VST spec + proof (0 admits)
- `make check-vst` clightgens the SAME `src/safety/*.c` — no duplicates, no divergence

**Also in proofs/vst/:** 21 pre-extraction demonstrator proofs (verif_simple_check.v, verif_zer_checks.v, verif_zer_checks2.v). These are standalone `.c` files written for VST, NOT extracted from the compiler. They demonstrate the VST pattern but do NOT verify real compiler code — don't count them as compiler verification.

**Level 3 scope (honest):** 15-25 pure predicates extractable from zercheck.c + zercheck_ir.c. Complex functions (call-graph DFS, scope walks) need struct separation logic — 20+ hrs each, separate future work.

**Three levels distinction:**
- **Level 1 (predicates)** — `typing.v` etc.: abstract safety spec is correct.
- **Level 2 (tests)** — `tests/zer_proof/`: compiler empirically rejects known violations.
- **Level 3 (VST on extracted predicates)** — `src/safety/*.c` linked + `proofs/vst/verif_*.v`: real compiler code matches spec for EVERY input. Catches implementation bugs 1+2 can miss (e.g., `if (x = 1)` assignment-typo — spec correct, tests may miss, VST fails immediately).

**Theory ↔ implementation guarantee (what CI proves):**

The correctness chain:
```
Coq theory (typing.v / operational subsets) — HUMAN-WRITTEN, TRUSTED
    ↓  REQUIRED: must have matching extraction
src/safety/<name>.c — pure predicate
    ↓  make check-vst PROVES match
Coq spec in proofs/vst/verif_<name>.v (must match oracle, not code)
    ↓  REQUIRED: checker.c must delegate
Call sites in checker.c / zercheck.c (with /* SAFETY: ... */ link)
    ↓  compiled with GCC
Compiled zerc binary
```

Each `↓` is enforced by a CI gate:
- `check-vst` — src/safety/ matches Coq spec (exists today)
- `check-theory-extracted` — typing.v has extraction (Phase 6, not yet)
- `check-no-inline-safety` — checker.c delegates (Phase 6, not yet)
- `check-api-bypass` — no direct mutation (Phase 6, not yet)

Phase 6 is what MAKES the guarantee mechanical. Until Phase 6, the theory→implementation link depends on HUMAN DISCIPLINE.

**Adding a new Level 3 extraction (MANDATORY steps — details in `proof-internals.md` "Phase 1 extraction recipe"):**

1. Identify pure predicate (primitive args, no state mutation, no AST/struct deps)
2. Write `src/safety/<name>.c` + `.h` using VST-friendly C style: flat cascade of early-return ifs, NO nesting, NO compound conditions (`&&`/`||`). If logic wants compound, SPLIT into multiple predicates and AND at call site.
3. Wire original C call sites to delegate: `zer_predicate_name(arg) != 0`
4. Add to Makefile CORE_SRCS + LIB_SRCS + check-vst clightgen/coqc lines
5. Add `<name>.v` to `src/safety/.gitignore`
6. Write `proofs/vst/verif_<name>.v`: Coq spec uses `Z.eq_dec` (NOT `Z.eqb`) to align with proof's destruct. Standard proof: `repeat forward_if; forward; unfold <name>_coq; repeat (destruct (Z.eq_dec _ _); try lia); try entailer!`.
7. Run `make docker-build` + `make check-vst` + `make docker-check`
8. Commit (one predicate or tight batch per commit)

**When VST check-vst fails (see common-errors table in `proof-internals.md`):**
- `Use [forward_if Post]` → C has nested ifs or `&&`/`||`. Flatten to single cascade.
- `variable X not found` → VST auto-substed on `==`; use `destruct (Z.eq_dec _ _)` not by name.
- `Attempt to save an incomplete proof` → proof closed fewer goals than C has. Add more destructs.

**Spec discipline (CORRECTED 2026-04-22, was "audit-before-extraction"):**

The original rule ("audit before extract") was defensive but often REDUNDANT. Correct discipline:

**Write the Coq spec against the Level 1 ORACLE (typing.v / operational subset / safety_list.md), NOT against the current C behavior.**

When the spec is oracle-driven:
- VST FAILS if the C diverges from the rule → bugs exposed by proof failure
- VST PASSES if the C matches → correctness guarantee

When the spec is code-driven:
- VST trivially passes (tautology) → bugs are frozen into the spec and CI enforces them forever

**Why this matters:** Level 3 VST proves "C matches spec I wrote." It does NOT prove the spec is correct. A code-matching spec just formalizes whatever the code does, bugs included. An oracle-matching spec exposes bugs because C must match the oracle.

**Retroactive check of 2026-04-21 Gemini findings:** all 3 real bugs (F5 shift, F3 escape, F7 iter-limit) were catchable by Level 3 VST with disciplined spec-writing. If we'd extracted `zer_shift_eval` with a spec matching typing.v's shift rule (not matching the current int64 eval), VST would have failed — exposing the bug.

**Audit-before-extraction IS still valuable when:**
- No Level 1 oracle exists for the subsystem (schematic rows in safety_list.md)
- Extracting won't cover the code (complex recursive functions we can't extract)
- Testing multi-subsystem interactions (concurrency + async)

**Audit-before-extraction is REDUNDANT when:**
- Level 1 has operational/predicate proof (gold oracle)
- The extracted predicate maps 1:1 to a Level 1 theorem
- We write the Coq spec from the Level 1 theorem, not from the C code

**Examples of bugs the 2026-04-21 Gemini audit caught** (documented fully in `BUGS-FIXED.md` "Gemini red-team audit"): comptime shift width, escape via struct field, fail-open iteration limit. All would have been caught by disciplined Level 3 extraction against Level 1 oracles. Regression tests in `tests/zer/` and `tests/zer_fail/`.

**Per-batch checklist before extracting:**
1. Does a Level 1 oracle exist for this subsystem? (Check safety_list.md and operational subsets.)
2. If YES → extract directly. Write specs from the oracle. VST exposes any code-vs-rule divergence.
3. If NO (schematic only, or new code) → audit first. Can't write oracle-driven specs without an oracle.
- `Cannot find physical path bound to ... zer_safety.<name>` → Makefile missing clightgen/coqc line for the new file.

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

**Internal Quality Invariants (permanent — applies to all sessions):**
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

**Benchmark Baseline (Docker x86, gcc -O2 — reference when adding runtime checks):**
- Bounds check: ~0-4% overhead (branch predictor eliminates always-false check)
- Division guard: ~0-5% overhead (single branch, always predicted not-taken)
- Shift safety: ~0% overhead (ternary compiles to cmov, no branch)
- Handle gen check: ~60-130% in synthetic microbenchmark (tight loop doing nothing but pool.get), <5% in real code with actual computation per access
- Benchmarks in `benchmarks/` directory (not committed — local only)

**Proven bug patterns — found by real-code testing:**
- `const u32 MAP_SIZE = 16; h % MAP_SIZE` → false "not proven nonzero". Root cause: `eval_const_expr` doesn't resolve `NODE_IDENT`, and `sym->func_node` was never set for `NODE_GLOBAL_VAR` in `register_decl`. Fix: (1) add const symbol init lookup in division guard path, (2) set `sym->func_node = node` for global vars. Both `/` `%` and `/=` `%=` paths fixed.
- `#line` directive emitted on same line as `{` in orelse-return defer path → GCC "stray #" error. All `emit(e, "{ "); emit_defers(e);` sites changed to `emit(e, "{\n"); emit_defers(e);` (var-decl orelse return, auto-guard return, orelse break/continue).
- Windows `zerc --run`: quoting `"gcc"` in `system()` breaks `cmd.exe`. Fix: only quote gcc path if it contains spaces (bundled path vs system PATH).
- `pool.get()` is non-storable — `*Task t = pool.get(h)` is a checker error. Must use inline: `pool.get(h).field`.
- `[]T → *T` auto-coerce at call sites for extern C functions only. `puts("hello")` works without `.ptr` when `puts` is forward-declared with no body. ZER-to-ZER calls still require explicit `.ptr`. Emitter already had `.ptr` emission (line ~1265), checker was blocking it.
- Range propagation now derives bounds from `x % N` → `[0, N-1]` and `x & MASK` → `[0, MASK]`. Resolves const symbol init values. Eliminates false "index not proven" warnings for hash map `slot = hash % TABLE_SIZE; arr[slot]` pattern. `derive_expr_range()` helper used at both var-decl init and assignment paths.

**ZER Integration Tests (`tests/zer/`):**
- Real `.zer` files compiled with `zerc --run`, must exit 0
- Runner: `tests/test_zer.sh`, added to `make check`
- Current tests: `ls tests/zer/*.zer` (~300 positive tests including hash_map, ring_buffer, pool_handle, move struct patterns, async/await, shared struct, condvar, container, defer, goto, opaque levels 1-9, etc.)
- Negative tests: `ls tests/zer_fail/*.zer` (~70 tests — UAF, double-free, bounds OOB, div-zero, null deref, escape analysis, move-after-transfer, typecast safety, etc.)
- Runtime-trap tests: `ls tests/zer_trap/*.zer` (compile clean, trap at runtime — slice bounds, signed div overflow, @inttoptr safety)
- Module tests: `ls test_modules/*.zer` (~28 including diamond imports, shared cross-module, opaque wrappers)
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

**Bug patterns with lessons (cross-reference BUGS-FIXED.md for root cause):**
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
- Inline asm: `unsafe asm("...")` REQUIRED (Rust-style); bare `asm("...")` is a compile error. Only in `naked` functions.
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
- @atomic_load/store/cas + @atomic_{add,sub,or,and,xor,nand,xchg} (fetch-old) + @atomic_{add,sub,or,and,xor}_fetch (fetch-new). All SEQ_CST.
- Bit queries: @popcount, @ctz, @clz, @parity, @ffs (return u32). Byte swap: @bswap16/32/64. Control-flow hints: @unreachable(), @expect(val, exp). Full barrier: @barrier_acq_rel().
- Interrupt control (privileged, kernel-mode only): @cpu_disable_int/@cpu_enable_int/@cpu_wait_int/@cpu_save_int_state/@cpu_restore_int_state. Tests use dead-branch pattern (volatile + if(never_true)) to verify compile without executing privileged asm.
- Context switch (callee-saved subset): @cpu_save_context/@cpu_restore_context/@cpu_save_fpu/@cpu_restore_fpu. Take *u8 buffer arg. Dead-branch tested.
- No malloc/free — use Pool(T, N), Slab(T), Arena
- move struct Name { } = ownership transfer on pass/assign, use after move = error
- container Name(T) { T[N] data; u32 len; } = parameterized struct template (monomorphization)
- Name(ConcreteType) var; = stamps concrete struct, functions take *Name(Type) explicitly
- Designated init: Point p = { .x = 10, .y = 20 }; or p = { .x = 1 };
```

Failure to include these rules causes agents to write invalid ZER (e.g., using i++ which silently passes parse-error-tolerant test harnesses).

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

### Quick Lookup — All 29 Systems (moved to docs/safety-model.md)

Flat table (29 rows listing each system with location, description, and
model assignment) is in `docs/safety-model.md`. Consult when auditing
specific systems. The 4-model OVERVIEW (Models 1-4) above stays in
CLAUDE.md because it informs every new-safety-feature design decision.

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

## IR Path — see docs/compiler-internals.md

Full IR path validation history + 22 architectural invariants + walker-missing-node-kind pattern + pre-lowering architecture + test harness mapping are documented in `docs/compiler-internals.md` under "IR Path Validation" (line ~4680).

**Mandatory reading before touching `ir_lower.c`, `ir.c`, or any `IR_*` handler in `emitter.c`.** The 22 invariants prevent silent miscompilation — each was discovered via a bug (BUG-538 through BUG-594). Skipping this doc causes regressions.

Quick reminders for common IR work:
- **Entry block MUST be `bb0`** — call `start_block()` BEFORE `collect_labels()`
- **`checker_set_type()` is PUBLIC** — use it when synthesizing AST in IR lowering
- **Function bodies are IR-only** since 2026-04-19 — `emit_stmt` deleted, no AST fallback
- **`emit_rewritten_node` is NOT `emit_expr`** — zero emit_expr calls in IR function body emission
- **AST→IR migration watchpoint**: any IR handler that replaces `emit_expr(inst->expr)` with direct emission MUST port every `_zer_trap` / `_zer_bounds_check` / `_zer_shl` safety wrapper. See "AST→IR Emission Diff Audit" section below.

### ir_validate hardening (phases 1+2, 2026-04-20)

`ir_validate(IRFunc *)` (in `ir.c`) is the safety-net check that runs
in the emitter hook on every lowered function. Previously structural-
only (targets in range, no duplicate local IDs). Hardened with:

- **Per-op field invariants** for 11 op kinds (BINOP, UNOP, COPY,
  LITERAL, FIELD_READ/WRITE, INDEX_READ, ADDR_OF, DEREF_READ, CAST,
  CALL_DECOMP) + BRANCH needs-condition + CAST needs-type. Catches
  lowerer building a malformed instruction.
- **Defer balance** — every `IR_DEFER_PUSH` must have a CFG-reachable
  `IR_DEFER_FIRE` with emit_bodies=true (`src2_local != 2`). Without
  it the `defer` body is statically dead (missed cleanup / latent
  leak). Uses `cfg_reaches_fire()` DFS helper.
- **NULL-type-local** — every `func->locals[i].type` must be non-NULL.
  Lowerer forgot `resolve_type` → downstream crash.
- **Reachability (opt-in diagnostic)** — `ZER_IR_WARN_UNREACHABLE=1`
  logs unreachable blocks. **Cannot be promoted to error**: lowerer
  correctly emits IR for source dead code (`goto done; x=0; done:`)
  and there's no static way to distinguish that from a forgotten-edge
  bug. Useful for lowerer-refactor sessions.

**For fresh sessions / when editing `ir_lower.c`:**
- If new IR op added, add a field-invariant case in `ir_validate`'s
  `switch(inst->op)` (around `ir.c:445`).
- Don't try to enforce "dead code after terminator" — lowerer emits
  legitimate `RETURN; DEFER_FIRE; GOTO bb_post` cleanup patterns.
- Don't try to enforce "reachability" as error — see above.
- Defer push without reachable fire = **hard error**, aborts compile.
  If a lowerer change trips this, investigate the push path.

Remaining real gaps (future work, not safety-critical):
- Call arg count matches callee signature (needs symbol-table access)
- `FIELD_READ` field name exists on src type (needs type traversal)
- `LITERAL` kind matches dest type
- `yield`/`await` only in async function
- Use-before-define (needs dominator analysis — hardest)

Full gap audit (20 items evaluated): see
`docs/compiler-internals.md` "ir_validate gap audit" section.

## CFG Migration (zercheck.c → zercheck_ir.c) — see docs/cfg_migration_plan.md

**Phase F LANDED (2026-04-20).** zercheck_ir runs UNCONDITIONALLY on every
compile via an `ir_hook` in the emitter. Both analyzers see every
function; disagreements logged as regression signals. zercheck.c still
drives exit code (AST primary) for conservatism — the CFG analyzer gets
continuous real-world validation on every user compile without gatekeeping
safety decisions.

**Current state:**
- Phases A-E complete (100% behavior parity on 3143 programs validated).
- Phase F: unconditional dual-run via emitter hook, **0 disagreements**
  across 1115 standalone + 28 module + 2000 fuzz = 3143 programs.
- Phase G (future): flip primary to zercheck_ir, delete zercheck.c,
  tag v0.5.0. Not blocked on technical issues — blocked on "accumulate
  more real-world usage before deleting the battle-tested analyzer."

**zercheck_ir.c ≈ 2900 lines, 100% behavior-parity with zercheck.c (2810 lines).**
CFG infrastructure is the foundation for future analyses (dominator trees,
VRP-on-SSA, borrow-checker-lite) that linear-scan can't easily support.

**Control knobs:**
- `ZER_DUAL_RUN=0` — disable zercheck_ir entirely (default: on)
- `ZER_DUAL_RUN=2` — verbose: log agreements too (debug)
- (unset or `=1`): run both, log disagreements only (default behavior)

**THE critical architectural constraint (learned the hard way in Phase F):**

`ir_lower_func` **mutates the AST** (`pre_lower_orelse` at ir_lower.c:1239
replaces `NODE_ORELSE` with `NODE_IDENT` referencing a temp local). Calling
`ir_lower_func` TWICE on the same function corrupts emission: nested-orelse
temps reference locals that no longer exist in the freshly-created IRFunc.

Before Phase F, `ZER_DUAL_RUN=1` worked by accident — make check didn't
set the env var. Making dual-run unconditional exposed this: tests like
`orelse_stress.zer` (nested orelse in function arg in outer orelse fallback)
fail to emit valid C after double-lowering.

**Solution**: single-lowering via emitter hook (`Emitter.ir_hook`).
zerc_main registers `zerc_ir_hook` (in zerc_main.c) that collects each
`IRFunc` as the emitter lowers it. After emit completes, zerc_main runs
iterative FuncSummary build + main analysis on the collected IRFuncs —
NOT re-lowering, just re-analyzing the same IR.

**Do NOT call `ir_lower_func` outside the emitter.** The emitter is the
sole lowering site. All IR analysis piggybacks via the hook.

**For fresh sessions:**
- Do NOT "fix" Phases A-E — converged. See BUGS-FIXED.md 2026-04-20.
- Do NOT call `ir_lower_func` in zerc_main or a new callsite (AST mutation).
- If adding new IR-based analyses, register via `Emitter.ir_hook`.
- Disagreements from `VERIFY disagreement` log line = genuine regression.

**Critical IR lowering fact (Phase 8d, per ir_lower.c:84):**
`IR_POOL_ALLOC` / `IR_SLAB_ALLOC` / `IR_POOL_FREE` / etc. enum values exist
but are **NEVER emitted by ir_lower.c**. Pool/Slab/Task method calls flow
through generic `IR_ASSIGN` (with NODE_ORELSE wrapping NODE_CALL, or direct
NODE_CALL) and `IR_CALL` (with `inst->expr` holding the call). Specialized
handlers for those opcodes in `zercheck_ir.c` are dead code. Method detection
lives in `IR_ASSIGN` and `IR_CALL` via `ir_classify_method_call(Node*)`.

**Do NOT try to "fix" the absence of IR_POOL_ALLOC emission** — this is
intentional architecture. Add detection in IR_ASSIGN/IR_CALL method-call
paths instead.

<!-- Sections moved to docs/compiler-internals.md 2026-04-19:
       - IR Path Validation (full history)
       - IR Path Architectural Invariants (22 items)
       - Scoped Defer Emission pattern
       - IR lowering patterns that must passthrough
       - Walker missing node kind class
       - emit_rewritten_node vs emit_expr
       - Pre-lowering architecture
       - Test Harness Architecture
     Total: ~145 lines moved, preserving all content. -->

<!-- The orphan IR sub-sections that lived here (lines ~1137-1278) were
     moved verbatim to docs/compiler-internals.md in the same commit. -->


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

## Rust Test Translation — see docs/compiler-internals.md

Workflow for translating tests from Rust `tests/ui/` into ZER-equivalent
tests (`rust_tests/rt_*.zer` and `rust_tests/reject_*.zer`) is documented
in `docs/compiler-internals.md` near the end of the file. Includes: Rust
pattern → ZER equivalent mapping, common agent mistakes to avoid, test
writing methodology, current coverage status, high-value test categories.

Consult when adding Rust-equivalent test cases. Not needed for most
sessions.

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

## AST→IR Emission Diff Audit — MANDATORY after any IR lowering refactor

**Read this before touching `IR_*` handlers in `emitter.c`.** Between
2026-04-15 and 2026-04-19, the compiler shipped with 7 missing runtime
safety checks in the IR emission path. Root cause: commit `010ddea`
replaced `emit_expr(inst->expr)` (AST fallback) with direct local-ID
emission in IR handlers, and silently stripped every safety wrapper
`emit_expr` had been applying. Became effective at commit `82335c3`
(IR default flip). All 7 restored at commit `3bdcf85` (BUG-595 through
BUG-599 — see BUGS-FIXED.md). Do not recreate this class of bug.

### The regression class

`emit_expr` in the AST path wraps expressions with runtime safety:
`_zer_bounds_check(idx, len, ...)`, `_zer_trap("signed div overflow")`,
`_zer_shl(a, b)` (shift safety macro), `_zer_trap("outside mmio range")`,
`_zer_trap("unaligned address")`, `_zer_trap("slice start > end")`.

When an IR handler lowers an expression and emits C directly from
local IDs, it bypasses `emit_expr` — and therefore bypasses every
safety wrapper. Tests don't catch this because VRP proves most
real-world indexes/values safe at compile time, eliminating the need
for runtime checks. The bugs only manifest when you specifically
test with *unprovable* values.

### The audit protocol (run this before committing IR refactors)

```bash
# 1. Enumerate every runtime safety emission in emit_expr
grep -nE "_zer_trap|_zer_bounds_check|_zer_shl|_zer_shr|_zer_probe" emitter.c \
  | awk -F: '$2 < 4000'

# 2. For each match, find the IR equivalent. If none exists, write
#    a reproducer test that should trap and verify it does.
```

### Audit checklist — what emit_expr does, what IR must preserve

| AST emit_expr safety | Line (approx) | IR equivalent |
|---|---|---|
| Slice bounds check | 2045-2067 | `IR_INDEX_READ` + `emit_rewritten_node` NODE_INDEX |
| Array bounds (variable index) | 2020-2044 | Separate `emit_auto_guards` pass |
| Signed div overflow (INT_MIN/-1) | 1068 | `IR_BINOP` TOK_SLASH/TOK_PERCENT |
| Division by zero | 1055 | checker forces compile-time guard (no IR work) |
| Shift safety (`_zer_shl`/`_zer_shr`) | 1078 | `IR_BINOP` TOK_LSHIFT/TOK_RSHIFT |
| Slice `arr[a..b]` range check | 2258 | `emit_rewritten_node` NODE_SLICE |
| @inttoptr MMIO range (variable addr) | 2650 | `emit_rewritten_node` @inttoptr intrinsic |
| @inttoptr alignment | 2660 | Same site as above |
| @ptrcast type mismatch | 2410, 2547 | checker catches via provenance |
| @trap / @probe | 2694, 2696 | IR handlers present and working |
| Handle gen check | inlined in `_zer_slab_get` | runtime-level, emit-path-independent |

### Testing that catches this class

Write one reproducer per safety mechanism. The reproducer must:

1. Use values that VRP CANNOT prove safe — literal constants
   propagated through a loop so they become "runtime-unknown" from
   VRP's perspective. Example:
   ```zer
   u32 i = 10;
   for (u32 k = 0; k < 1; k += 1) { i = 10; }  // defeat VRP
   return arr[i];  // should trap, VRP has given up
   ```

2. Assert the correct runtime trap fires (check exit code 133 or
   trap message).

3. Live in `tests/zer_gaps/ast_*.zer` (audit artifact) or
   `tests/zer_trap/*.zer` (promoted to regression tests).

### The methodology that works — 3 audits in sequence

Proven effective by the 2026-04-19 late session (9 bugs fixed,
~0.15% defect density found in a subsystem that had green tests):

1. **Behavioral audit (Phase 1)** — write adversarial `.zer` programs
   that VIOLATE each safety system's claim. 50+ programs, 1-N per
   system. Each that compiles clean is a gap.

2. **Code-inspection audit (Phase 2)** — read the source (checker.c,
   zercheck.c, emitter.c) looking for fixed-size buffers, depth
   caps, TODO markers. Write targeted tests for each structural
   weakness. Finds bugs the behavioral audit missed.

3. **Diff audit (Phase 3)** — when a migration has happened
   (AST→IR, linear→CFG, etc.), grep the ORIGIN path for every
   runtime safety emission and verify the DESTINATION path has an
   equivalent. Missing one is a regression. Use `git log -S"X"`
   to find the commit that introduced the gap.

Each audit finds DIFFERENT bug classes. Running just one is
insufficient. The 2026-04-19 session: Phase 1 found 7 gaps,
Phase 2 found 1 major regression (Gap 0), Phase 3 found 6 more
regressions. No phase found what the others found.
