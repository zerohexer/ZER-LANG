# ZER(C) Language Reference

**Zero Error Risk C Extension**
**Version:** 0.2.1 | **Compiler:** `zerc` | **Target:** Any platform GCC supports
**1700+ tests, all passing**

---

## PRIMITIVE TYPES

### u8, u16, u32, u64

**DESCRIPTION**
Unsigned integers of 8, 16, 32, and 64 bits respectively.
Auto-zeroed on declaration. Overflow wraps (defined behavior, never UB).

**SYNTAX**
```zer
u8 a;           // 0
u16 b = 1000;
u32 c = 0xDEAD;
u64 d = 123456789;
```

**NOTES**
- No implicit narrowing: `u8 x = 300;` is a compile error. Use `@truncate(u8, 300)` or `@saturate(u8, 300)`.
- No implicit sign conversion: `u32 x = -1;` is a compile error. Use `@bitcast(u32, -1)`.
- Shift by >= width returns 0 (defined, not UB).

**SEE ALSO**
i8..i64, @truncate, @saturate, @bitcast

---

### i8, i16, i32, i64

**DESCRIPTION**
Signed integers of 8, 16, 32, and 64 bits respectively.
Auto-zeroed on declaration. Overflow wraps (defined behavior).

**SYNTAX**
```zer
i32 x = -42;
i8 small = @truncate(i8, big_value);
```

**NOTES**
- Same rules as unsigned: no implicit narrowing, no implicit sign conversion.

**SEE ALSO**
u8..u64

---

### usize

**DESCRIPTION**
Pointer-width unsigned integer. Auto-detected from GCC at compile time
(32-bit or 64-bit). Override with `--target-bits N`.

**SYNTAX**
```zer
usize len = data.len;
usize addr = @ptrtoint(ptr);
```

**NOTES**
- Returned by `@size(T)`, `@offset(T, field)`, `@ptrtoint(ptr)`.
- Returned by `.len` on arrays and slices.

**SEE ALSO**
@size, @ptrtoint

---

### f32, f64

**DESCRIPTION**
IEEE 754 floating-point numbers. 32-bit and 64-bit.

**SYNTAX**
```zer
f32 temp = 36.6;
f64 precise = 3.14159265358979;
```

---

### bool

**DESCRIPTION**
Boolean type. Only `true` or `false`. NOT an integer — no bool-to-int
or int-to-bool coercion.

**SYNTAX**
```zer
bool ready = true;
bool done = false;
```

**EXAMPLE**
```zer
bool flag = true;
if (flag) { go(); }      // OK
u32 x = flag;             // COMPILE ERROR — bool is not integer
bool b = 1;               // COMPILE ERROR — int is not bool
if (x) { }                // COMPILE ERROR — x is u32, not bool
```

**NOTES**
- Switch on bool must be exhaustive: both `true` and `false` arms required.
- Comparisons (`==`, `<`, etc.) return bool.

---

### void

**DESCRIPTION**
Return type only. Cannot declare void variables.

**SYNTAX**
```zer
void do_work() { }
```

**NOTES**
- `?void` is valid — `struct { u8 has_value; }` with NO `.value` field.
- Used as return type for `push_checked()` and similar try-operations.

---

## COMPOUND TYPES

### T[N] — Fixed Array

**DESCRIPTION**
Fixed-size array. Size goes between type and name (NOT after name like C).
Every index access is bounds-checked. Out-of-bounds traps at runtime.
Compile-time constant indices are checked at compile time.

**SYNTAX**
```zer
u8[256] buf;              // 256 bytes, auto-zeroed
u32[4] values;            // 4 u32s
i32[3][3] matrix;         // 3x3 multi-dimensional
```

**EXAMPLE**
```zer
u32[4] scores;
scores[0] = 100;
scores[3] = 200;          // OK — index 3 < 4
scores[4] = 300;          // COMPILE ERROR — index 4 >= 4

u32 i = get_index();
scores[i] = 50;           // runtime bounds check — traps if i >= 4
```

**FIELDS**
`.len` → usize — Array length (compile-time constant)

**COERCION**
T[N] auto-coerces to [*]T at function calls, var-decl init, and return:

```zer
u8[256] buf;
void process([*]u8 data) { }
process(buf);              // auto-coerces: { .ptr=buf, .len=256 }
```

**NOTES**
- Size must be a compile-time constant.
- Returning a local array as a slice is a compile error (dangling pointer).

**SEE ALSO**
[*]T

---

### [*]T — Dynamic Pointer to Many

**DESCRIPTION**
A fat pointer: `{ *T ptr; usize len; }`. Carries a pointer AND its length.
Every index access is bounds-checked. Reads as "pointer to many T".

This is ZER's replacement for C's `T*` when pointing to arrays/buffers.
Preferred over `[]T` (which is deprecated).

**SYNTAX**
```zer
[*]u8 name;               // pointer to many bytes
[*]Task items;            // pointer to many Tasks
const [*]u8 msg = "hi";  // string literal (read-only)
```

**EXAMPLE**
```zer
void process([*]u32 data) {
    for (u32 i = 0; i < data.len; i += 1) {
        data[i] += 1;     // bounds-checked: i < data.len
    }
}

u32[8] arr;
process(arr);              // auto-coerces: T[N] → [*]T
```

**FIELDS**
- `.ptr` → *T — Raw pointer to first element
- `.len` → usize — Number of elements

**SUB-SLICING**
```zer
buf[0..3]                  // elements 0,1,2 (exclusive end)
buf[2..]                   // element 2 through end
buf[..5]                   // elements 0-4
```

**NOTES**
- `[]T` is deprecated. Use `[*]T` instead. `[]T` emits a warning.
- String literals are `const [*]u8`, not `char*`.
- Cannot be null. Use `?[*]T` for nullable.

**SEE ALSO**
T[N], *T, []T

---

### []T — Slice (DEPRECATED)

**DESCRIPTION**
Same as `[*]T`. Deprecated — compiler warns "use [*]T instead".
Kept for backward compatibility. Will be removed in v1.0.

**SYNTAX**
```zer
[]u8 data;                 // WARNING: use [*]u8 instead
```

**SEE ALSO**
[*]T

---

### *T — Pointer to One

**DESCRIPTION**
Non-null pointer. The compiler guarantees `*T` is never null.
Must be initialized at declaration.

Auto-derefs for field access: `ptr.field` works (no `->` needed).

**SYNTAX**
```zer
*Task t = &my_task;
t.priority = 5;            // auto-deref, like ptr->priority in C
```

**EXAMPLE**
```zer
struct Task { u32 id; u32 priority; }

void set_priority(*Task t, u32 p) {
    t.priority = p;        // auto-deref
}
```

**ERRORS**
```zer
*Task t;                   // COMPILE ERROR — non-null pointer requires initializer
                           // use ?*Task for nullable
```

**NOTES**
- No pointer arithmetic. `ptr + 1` is a compile error. Use indexing or @ptrtoint.
- Pointer indexing (`ptr[5]`) emits a warning — use [*]T for bounds-checked access.

**SEE ALSO**
?*T, [*]T, *opaque

---

### ?*T — Optional Pointer (Nullable)

**DESCRIPTION**
Pointer that might be null. Must unwrap before use.
Zero overhead — represented as a plain C pointer where NULL = none.

**SYNTAX**
```zer
?*Task maybe = null;
?*Task found = find_task(id);
```

**EXAMPLE**
```zer
?*Task maybe = find_task(42);

// COMPILE ERROR — must unwrap first:
maybe.id = 1;

// Correct — unwrap with if:
if (maybe) |t| {
    t.id = 1;              // t is *Task, guaranteed non-null
}

// Correct — unwrap with orelse:
*Task t = maybe orelse return;
t.id = 1;
```

**SEE ALSO**
*T, ?T, orelse, if-unwrap

---

### ?T — Optional Value

**DESCRIPTION**
Carries a value or nothing. Implemented as a struct with `has_value` flag.
Must unwrap before use.

**SYNTAX**
```zer
?u32 result;               // struct { u32 value; u8 has_value; }
?bool flag;                // struct { u8 value; u8 has_value; }
?void status;              // struct { u8 has_value; } — NO .value field!
```

**EXAMPLE**
```zer
?u32 safe_divide(u32 a, u32 b) {
    if (b == 0) { return null; }
    return a / b;
}

u32 result = safe_divide(10, 3) orelse 0;  // default to 0
```

**NOTES**
- `?void` has ONE field (`has_value`). Everything else has TWO (`value` + `has_value`).
- Returning `null` sets `has_value = 0`.
- Returning a value sets `has_value = 1` and stores the value.

**SEE ALSO**
?*T, orelse, if-unwrap

---

### *opaque — Type-Erased Pointer

**DESCRIPTION**
Equivalent to C's `void*`. Cannot be dereferenced without casting back.
Provenance-tracked: the compiler remembers what type was cast in, and
rejects casting out to a different type.

**SYNTAX**
```zer
*opaque raw = @ptrcast(*opaque, &my_task);
```

**EXAMPLE**
```zer
struct Task { u32 id; }
struct Motor { u32 rpm; }

*opaque ctx = @ptrcast(*opaque, &task);   // provenance = *Task
*Task t = @ptrcast(*Task, ctx);           // OK — matches provenance
*Motor m = @ptrcast(*Motor, ctx);         // COMPILE ERROR — wrong type
```

**NOTES**
- Extern/cinclude pointers have unknown provenance (type_id=0) — cast check skipped.
- Level 1-5 *opaque tracking catches UAF through malloc/free. See docs/ZER_OPAQUE.md.

**SEE ALSO**
@ptrcast, cinclude

---

## DECLARATIONS

### struct

**DESCRIPTION**
User-defined aggregate type. No `struct` keyword needed in usage.
All fields auto-zeroed.

**SYNTAX**
```zer
struct Task {
    u32 id;
    [*]u8 name;
    u32 priority;
    ?*Task next;
}
```

**EXAMPLE**
```zer
Task t;                    // no 'struct' prefix (unlike C)
t.id = 42;
t.name = "worker";
t.priority = 3;
t.next = null;
```

**NOTES**
- No semicolon after closing `}` (unlike C).
- Pool/Slab/Ring/Arena cannot be struct fields.

**SEE ALSO**
packed struct, enum, union

---

### packed struct

**DESCRIPTION**
Struct with no padding between fields. Emits `__attribute__((packed))`.
Used for hardware registers, network packets, binary protocols.

**SYNTAX**
```zer
packed struct SensorPacket {
    u8 id;
    u16 temperature;      // unaligned — ZER handles safely
    u8 checksum;
}   // exactly 4 bytes, no padding
```

**SEE ALSO**
struct

---

### enum

**DESCRIPTION**
Named integer constants. Values are `i32` internally.
Switch on enum must be exhaustive — missing a variant is a compile error.

**SYNTAX**
```zer
enum State { idle, running, blocked, done }
```

**EXAMPLE**
```zer
State s = State.idle;      // qualified access

switch (s) {
    .idle    => { start(); }
    .running => { work(); }
    .blocked => { wait(); }
    .done    => { finish(); }
}

// Explicit values and gaps:
enum ErrorCode { ok = 0, warn = 100, err, fatal }
// ok=0, warn=100, err=101, fatal=102

// Negative values:
enum Direction { left = -1, center = 0, right = 1 }
```

**NOTES**
- Dot syntax required: `State.idle`, not bare `idle`.
- Switch arms use `.variant => { }` syntax.

**SEE ALSO**
switch, union

---

### union (Tagged)

**DESCRIPTION**
Tagged union. Tag is set automatically on assignment.
Must switch to access variant — direct field access is a compile error.

**SYNTAX**
```zer
union Message {
    SensorData sensor;
    Command command;
    Ack ack;
}
```

**EXAMPLE**
```zer
Message msg;
msg.sensor = read_sensor();     // sets tag to .sensor

switch (msg) {
    .sensor  => |data| { process(data); }   // immutable capture
    .command => |*cmd| { cmd.x = 5; }       // mutable capture (pointer)
    .ack     => |a|    { confirm(a); }
}

msg.sensor.temperature;         // COMPILE ERROR — must switch first
```

**NOTES**
- Mutable capture `|*v|` takes a pointer to the original union variant.
- Mutating the switched-on union's variant inside a capture arm is a compile error.

**SEE ALSO**
enum, switch

---

### Function

**DESCRIPTION**
Function declaration. Return type before name (like C).
All parameters are by value unless pointer.

**SYNTAX**
```zer
u32 add(u32 a, u32 b) {
    return a + b;
}
```

**EXAMPLE**
```zer
?u32 safe_divide(u32 a, u32 b) {
    if (b == 0) { return null; }
    return a / b;
}

void greet([*]u8 name) {
    // ...
}
```

**NOTES**
- `void` return = no return value.
- `?T` return = can return `null` for failure.
- `static` functions are module-internal (not visible to importers).

---

### Function Pointer

**DESCRIPTION**
Same syntax as C. Optional function pointers use null sentinel.

**SYNTAX**
```zer
u32 (*fn)(u32, u32) = add;                 // local variable
void (*callback)(u32 event);               // global variable
struct Ops { u32 (*compute)(u32); }        // struct field
u32 apply(u32 (*op)(u32, u32), u32 x, u32 y);  // parameter
?void (*on_event)(u32) = null;             // optional — null = not set
typedef u32 (*BinOp)(u32, u32);            // typedef
```

**EXAMPLE**
```zer
?void (*callback)(u32) = null;
callback = my_handler;
if (callback) |cb| { cb(42); }   // safe — unwrap before calling
```

**SEE ALSO**
typedef, distinct typedef

---

### typedef / distinct typedef

**DESCRIPTION**
`typedef` creates an alias — interchangeable with the base type.
`distinct typedef` creates a new type — NOT interchangeable. Use `@cast` to convert.

**SYNTAX**
```zer
typedef u32 Milliseconds;              // alias — u32 and Milliseconds are same
distinct typedef u32 Celsius;          // distinct — NOT interchangeable
distinct typedef u32 Fahrenheit;
```

**EXAMPLE**
```zer
Celsius c = @cast(Celsius, 100);       // wrap: u32 → Celsius
u32 raw = @cast(u32, c);              // unwrap: Celsius → u32
Fahrenheit f = @cast(Fahrenheit, c);   // COMPILE ERROR — cross-distinct
```

**SEE ALSO**
@cast

---

### const

**DESCRIPTION**
Compile-time constant. Value must be known at compile time.

**SYNTAX**
```zer
const u32 MAX = 100;
const [*]u8 NAME = "ZER";    // in .rodata (flash on embedded)
```

---

### static

**DESCRIPTION**
On local variables: persists across function calls (like C).
On functions: internal to module (not visible to importers).

**SYNTAX**
```zer
void count() {
    static u32 n;
    n += 1;
}

static void helper() { }    // not exported
```

---

## CONTROL FLOW

### if / else

**DESCRIPTION**
Conditional execution. Braces ALWAYS required (no braceless one-liners).
`else if` is supported.

**SYNTAX**
```zer
if (condition) {
    // body
}

if (a) {
    handle_a();
} else if (b) {
    handle_b();
} else {
    handle_neither();
}
```

**ERRORS**
```zer
if (x > 5) return 1;      // COMPILE ERROR — braces required
```

---

### for

**DESCRIPTION**
C-style for loop. No `++` or `--` — use `+= 1` / `-= 1`.
Loop variable is scoped to the loop body.

**SYNTAX**
```zer
for (u32 i = 0; i < 10; i += 1) {
    process(i);
}
```

**ERRORS**
```zer
for (u32 i = 0; i < 10; i++) { }   // COMPILE ERROR — no ++
```

---

### while

**DESCRIPTION**
Loop while condition is true. Braces required.

**SYNTAX**
```zer
while (running) {
    poll();
}
```

---

### switch

**DESCRIPTION**
Pattern matching on enums, integers, and bools.
Uses `=>` arrows. No `case` keyword. No fallthrough. No `break` needed.
Enum and bool switches must be exhaustive. Integer switches need `default`.

**SYNTAX**
```zer
// Enum — exhaustive
switch (state) {
    .idle    => { start(); }
    .running => { work(); }
    .done    => { finish(); }
}

// Integer — default required
switch (code) {
    0 => { ok(); }
    1, 2 => { retry(); }      // multi-value arm
    default => { error(); }
}

// Bool — exhaustive
switch (ready) {
    true  => { go(); }
    false => { wait(); }
}
```

**NOTES**
- Union switch uses capture syntax: `.variant => |val| { ... }`
- Mutable capture: `.variant => |*val| { val.field = 5; }`

**SEE ALSO**
enum, union

---

### defer

**DESCRIPTION**
Runs a statement at scope exit, in reverse order of declaration.
Fires on ALL exit paths (return, break, continue, end of block).

**SYNTAX**
```zer
defer statement;
```

**EXAMPLE**
```zer
void transfer() {
    mutex_lock(&lock);
    defer mutex_unlock(&lock);     // runs last
    cs_low();
    defer cs_high();               // runs first (reverse order)

    if (error) { return; }         // both defers fire
    do_work();
}   // defers fire: cs_high() then mutex_unlock()
```

**NOTES**
- Multiple defers in same scope run in LIFO order (last declared = first run).

---

### goto + labels

**DESCRIPTION**
Jump to a labeled location in the same function. Both forward and backward
jumps allowed. Safe because auto-zero prevents uninitialized memory and
defer fires on all scope exits.

**SYNTAX**
```zer
goto label_name;           // jump to label
label_name:                // label declaration (no semicolon needed)
```

**EXAMPLE**
```zer
// Forward goto — error cleanup pattern (replaces nested if):
u32 init() {
    *opaque buf = kmalloc(SIZE) orelse { goto fail; };
    *opaque irq = request_irq(IRQ) orelse { goto fail_irq; };
    return 0;

fail_irq:
    kfree(buf);
fail:
    return 1;
}

// Backward goto — retry loop:
u32 count = 0;
retry:
    count += 1;
    if (count < 5) { goto retry; }

// Forward goto — break out of nested loops:
for (u32 i = 0; i < n; i += 1) {
    for (u32 j = 0; j < m; j += 1) {
        if (found) { goto done; }
    }
}
done:
```

**ERRORS**
```zer
goto nowhere;              // COMPILE ERROR — label 'nowhere' not found
goto inside defer block    // COMPILE ERROR — cannot use goto inside defer
duplicate labels           // COMPILE ERROR — label 'x' already defined
```

**NOTES**
- Labels are function-scoped — cannot goto between functions.
- Max 128 labels per function.
- goto does NOT skip defer execution — defers still fire at scope exit.
- Backward goto is just a loop — same as `while(true)` with condition.

**SEE ALSO**
defer, break, continue

---

## OPTIONAL UNWRAPPING

### orelse

**DESCRIPTION**
Unwrap an optional value. If null, execute the fallback.

**SYNTAX**
```zer
u32 val = get_value() orelse 0;           // default value
u32 val = get_value() orelse return;      // bare return (NO value!)
u32 val = get_value() orelse break;       // exit loop
u32 val = get_value() orelse continue;    // skip iteration
u32 val = get_value() orelse {            // block fallback
    log_error();
    return;
};
```

**ERRORS**
```zer
u32 val = get_value() orelse return 1;    // PARSE ERROR — orelse return is bare
```

**NOTES**
- `orelse return` has no value. The return value comes from the function's return type.
- For bool-returning functions, restructure to avoid orelse in return path.

**SEE ALSO**
?T, ?*T, if-unwrap

---

### if-unwrap

**DESCRIPTION**
Unwrap an optional in an if-condition. If non-null, the captured variable
holds the unwrapped value inside the body.

**SYNTAX**
```zer
if (optional) |val| {
    // val is the unwrapped value (immutable)
}

if (optional) |*val| {
    // val is a mutable pointer to the unwrapped value
    val.field = 5;
}
```

**EXAMPLE**
```zer
?u32 result = safe_divide(10, 3);

if (result) |val| {
    use(val);              // val is u32, guaranteed non-null
} else {
    handle_error();
}
```

**SEE ALSO**
orelse, ?T, ?*T

---

## BUILTIN ALLOCATORS

### Pool(T, N) — Fixed-Slot Allocator

**DESCRIPTION**
Pre-allocated array of N slots with generation counters. Must be global.
ISR-safe — no heap, no malloc, no locking. Fixed at compile time.

Every slot has a generation counter. When freed, the generation increments.
Accessing a freed slot with an old handle traps (generation mismatch).

**SYNOPSIS**
```zer
Pool(Task, 8) tasks;       // 8 slots for Task, global only
```

**METHODS**
- `.alloc()` → `?Handle(T)` — Allocate a slot. Returns null if all slots used.
- `.get(h)` → `*T` — Access by handle. Traps if gen mismatch.
- `.free(h)` → `void` — Free slot, increment generation.

**EXAMPLE**
```zer
struct Task { u32 id; u32 priority; }
Pool(Task, 8) tasks;

u32 main() {
    Handle(Task) t = tasks.alloc() orelse { return 1; };
    tasks.get(t).id = 42;
    tasks.get(t).priority = 3;

    tasks.free(t);
    // tasks.get(t).id = 1;   // COMPILE ERROR: use-after-free
    return 0;
}
```

**ERRORS**
- Pool on stack → COMPILE ERROR — must be global
- `tasks.get(t)` after free → COMPILE ERROR (zercheck: use-after-free)
- `tasks.free(t)` twice → COMPILE ERROR (zercheck: double free)
- `tasks.alloc();` → COMPILE ERROR (ghost handle — must assign result)

**NOTES**
- Pool does NOT use heap. Safe for ISR and bare metal.
- `.get(h)` result is non-storable: `*Task t = tasks.get(h)` is a compile error.
  Must use inline: `tasks.get(h).field`.
- N must be a compile-time constant.

**SEE ALSO**
Slab(T), Handle(T), Arena

---

### Slab(T) — Dynamic Growable Allocator

**DESCRIPTION**
Dynamic slab allocator. Grows on demand via calloc. Same Handle API as Pool
but not limited to a fixed count. Must be global.

NOT ISR-safe — calloc may use a global mutex that deadlocks in interrupt context.

**SYNOPSIS**
```zer
Slab(Task) heap;           // global only
```

**METHODS**
- `.alloc()` → `?Handle(T)` — Allocate a slot. Returns null if OOM.
- `.get(h)` → `*T` — Access by handle. Traps if gen mismatch.
- `.free(h)` → `void` — Free slot, increment generation.

**EXAMPLE**
```zer
struct Task { u32 id; [*]u8 name; ?*Task next; }
Slab(Task) heap;

u32 main() {
    Handle(Task) t1 = heap.alloc() orelse { return 1; };
    heap.get(t1).id = 1;
    heap.get(t1).name = "first";

    Handle(Task) t2 = heap.alloc() orelse { return 2; };
    heap.get(t2).id = 2;

    heap.free(t1);
    heap.free(t2);
    return 0;
}
```

**ERRORS**
- `Slab.alloc()` in interrupt handler → COMPILE ERROR (calloc may deadlock)
- Same zercheck errors as Pool (UAF, double-free, ghost handle)

**NOTES**
- Use Pool for ISR-safe allocation with fixed count.
- Use Slab for dynamic allocation when count is unknown.
- Slab uses calloc internally — requires a heap (OS, RTOS, or custom allocator).

**SEE ALSO**
Pool(T,N), Handle(T), Arena

---

### Handle(T) — Slot Reference

**DESCRIPTION**
A 64-bit value: index (32 bits) + generation (32 bits). NOT a pointer.
Used to safely reference slots in Pool and Slab. Generation counter
prevents use-after-free with 100% detection (ABA-safe).

**SYNOPSIS**
```zer
Handle(Task) h = pool.alloc() orelse { return 1; };
```

**EXAMPLE**
```zer
Pool(Task, 8) tasks;

Handle(Task) h = tasks.alloc() orelse { return 1; };
tasks.get(h).id = 42;         // gen checked on every access

Handle(Task) saved = h;        // copy the handle
tasks.free(h);                 // gen incremented

// Runtime: saved has old gen → mismatch → trap
// Compile: zercheck catches this as use-after-free
```

**NOTES**
- Handle is a value type (u64). Can be copied, stored in structs, passed to functions.
- Cannot be dereferenced directly. Must use `pool.get(h)` or `slab.get(h)`.
- `?Handle(T)` is an optional handle — used as return type of `.alloc()`.

**SEE ALSO**
Pool(T,N), Slab(T)

---

### Ring(T, N) — Circular Buffer

**DESCRIPTION**
Fixed-size circular buffer. ISR-safe with memory barriers.
Must be global. Used for producer-consumer patterns (e.g., UART RX/TX).

**SYNOPSIS**
```zer
Ring(u8, 256) rx_buf;      // 256-byte circular buffer, global only
```

**METHODS**
- `.push(val)` → `void` — Push value. Overwrites oldest if full.
- `.push_checked(val)` → `?void` — Push value. Returns null if full.
- `.pop()` → `?T` — Pop oldest. Returns null if empty.

**EXAMPLE**
```zer
Ring(u8, 256) rx_buf;

// Producer (e.g., interrupt handler):
interrupt USART1 {
    u8 byte = @truncate(u8, UART1.DR);
    rx_buf.push(byte);                     // always succeeds
}

// Consumer (main loop):
while (true) {
    if (rx_buf.pop()) |byte| {
        process(byte);
    }
}

// Checked push (don't overwrite):
rx_buf.push_checked(byte) orelse {
    // buffer full — drop or handle
};
```

**NOTES**
- N must be a compile-time constant.
- ISR-safe: uses memory barriers between producer and consumer.

**SEE ALSO**
Pool(T,N)

---

### Arena — Bump Allocator

**DESCRIPTION**
Bump allocator over developer-owned memory. No heap.
Allocates forward, frees everything at once with `.reset()`.
Cannot free individual allocations.

**SYNOPSIS**
```zer
u8[4096] backing;
Arena ar = Arena.over(backing);
```

**METHODS**
- `Arena.over(buf)` → `Arena` — Create arena over an array or slice.
- `.alloc(T)` → `?*T` — Allocate one T (aligned). T must be struct/enum name.
- `.alloc_slice(T, n)` → `?[*]T` — Allocate n elements. T must be struct/enum name.
- `.reset()` → `void` — Reset offset to 0 (frees everything).
- `.unsafe_reset()` → `void` — Reset without warning.

**EXAMPLE**
```zer
struct Node { u32 id; ?*Node next; }

u8[4096] backing;
Arena ar = Arena.over(backing);

*Node a = ar.alloc(Node) orelse { return 1; };
a.id = 1;

*Node b = ar.alloc(Node) orelse { return 2; };
b.id = 2;
a.next = b;

defer ar.reset();      // free everything at scope exit
```

**ERRORS**
```zer
ar.alloc(u32)          // PARSE ERROR — T must be struct/enum name, not primitive
ar.alloc_slice(u8, n)  // PARSE ERROR — same restriction
```

Workaround for primitives:
```zer
struct Byte { u8 val; }
ar.alloc_slice(Byte, 64);
```

**NOTES**
- Arena-derived pointers cannot be stored in global/static variables (compile error).
- No individual free — arena is all-or-nothing.
- Use `defer ar.reset()` to ensure cleanup on all exit paths.

**SEE ALSO**
Pool(T,N), Slab(T)

---

## INTRINSICS

All intrinsics start with `@`.

### @truncate(T, val)

**DESCRIPTION**
Keep the low bits of val to fit into type T. For big-to-small conversions.

**EXAMPLE**
```zer
u8 low = @truncate(u8, 0x1234);    // 0x34
```

---

### @saturate(T, val)

**DESCRIPTION**
Clamp val to the min/max of type T. No data loss — just capped.

**EXAMPLE**
```zer
i8 clamped = @saturate(i8, 200);   // 127 (i8 max)
u8 clamped = @saturate(u8, -5);    // 0 (u8 min)
```

---

### @bitcast(T, val)

**DESCRIPTION**
Reinterpret the bits of val as type T. Same bit width required.
Checks qualifier preservation (const, volatile).

**EXAMPLE**
```zer
u32 bits = @bitcast(u32, my_i32);  // same bits, different type
```

---

### @cast(T, val)

**DESCRIPTION**
Convert between a distinct typedef and its base type. Only works for
distinct typedefs — not general-purpose.

**EXAMPLE**
```zer
distinct typedef u32 Celsius;
Celsius c = @cast(Celsius, 100);   // wrap
u32 raw = @cast(u32, c);          // unwrap
```

---

### @inttoptr(*T, addr)

**DESCRIPTION**
Convert integer address to pointer. Used for MMIO registers.
Requires `mmio` range declaration (compile error without it).
Address must be aligned to T's alignment.

**SYNOPSIS**
```zer
@inttoptr(*T, address)
```

**EXAMPLE**
```zer
mmio 0x40020000..0x40020FFF;       // declare valid MMIO range
volatile *u32 reg = @inttoptr(*u32, 0x40020014);
```

**ERRORS**
```zer
@inttoptr(*u32, 0x12345678)        // COMPILE ERROR — no mmio range declared
@inttoptr(*u32, 0x40020001)        // COMPILE ERROR — misaligned for u32
```

**NOTES**
- `--no-strict-mmio` flag allows @inttoptr without mmio declarations.
- For tests: `mmio 0x0..0xFFFFFFFFFFFFFFFF;` (allow all addresses).

**SEE ALSO**
@ptrtoint, mmio

---

### @ptrtoint(ptr)

**DESCRIPTION**
Convert pointer to usize integer.

**EXAMPLE**
```zer
usize addr = @ptrtoint(my_ptr);
```

---

### @ptrcast(*T, ptr)

**DESCRIPTION**
Cast pointer to a different pointer type. Provenance-tracked: the compiler
remembers what type went in through `*opaque` round-trips.

**EXAMPLE**
```zer
*opaque ctx = @ptrcast(*opaque, &sensor);  // provenance = *Sensor
*Sensor s = @ptrcast(*Sensor, ctx);        // OK — matches
*Motor m = @ptrcast(*Motor, ctx);          // COMPILE ERROR — wrong provenance
```

**NOTES**
- Checks qualifier preservation (const, volatile cannot be stripped).
- Unknown provenance (function params, cinclude) → check skipped.

**SEE ALSO**
*opaque, @container

---

### @container(*T, ptr, field)

**DESCRIPTION**
Container-of: given a pointer to a struct field, get a pointer to the
containing struct. Field existence is validated at compile time.

**EXAMPLE**
```zer
struct Device { u32 id; ListHead list; }

*ListHead ptr = &dev.list;
*Device d = @container(*Device, ptr, list);   // OK
```

---

### @size(T)

**DESCRIPTION**
Returns the size of type T in bytes as usize. Like C's sizeof.

**EXAMPLE**
```zer
usize s = @size(Task);     // e.g., 12
```

---

### @offset(T, field)

**DESCRIPTION**
Returns the byte offset of a field within struct T as usize. Like C's offsetof.

**EXAMPLE**
```zer
usize off = @offset(Task, priority);
```

---

### @trap()

**DESCRIPTION**
Intentional crash. Calls the ZER trap handler with a message.

**EXAMPLE**
```zer
if (should_never_happen) { @trap(); }
```

---

### @probe(addr)

**DESCRIPTION**
Safe MMIO read. Returns `?u32` — null if the address faults (unmapped memory).
Uses signal-based fault handler. Works on any platform.

**EXAMPLE**
```zer
?u32 val = @probe(0x40020000);
if (val) |v| {
    // hardware present
} else {
    // address faulted — hardware not present
}
```

---

### @barrier(), @barrier_store(), @barrier_load()

**DESCRIPTION**
Memory barriers. Full, store-only, or load-only.
Emits GCC `__atomic_thread_fence()`.

---

### @cstr(buf, slice)

**DESCRIPTION**
Copy a `[*]u8` slice into a fixed buffer and append NUL terminator.
For C interop (C functions expect NUL-terminated strings).

**EXAMPLE**
```zer
u8[64] cbuf;
const [*]u8 name = "hello";
*u8 cname = @cstr(cbuf, name);    // "hello\0" in cbuf
```

**NOTES**
- Returns pointer to buf. If slice doesn't fit, returns zero value (auto-guard).

---

### @atomic_add, @atomic_sub, @atomic_or, @atomic_and, @atomic_xor

**DESCRIPTION**
Atomic read-modify-write operations. Uses GCC `__atomic_*` builtins.
Value must be 1, 2, 4, or 8 bytes wide.

**EXAMPLE**
```zer
u32 counter;
@atomic_add(&counter, 1);
```

---

### @atomic_load, @atomic_store

**DESCRIPTION**
Atomic load and store with sequential consistency.

**EXAMPLE**
```zer
u32 val = @atomic_load(&shared);
@atomic_store(&shared, 42);
```

---

### @atomic_cas(ptr, expected, desired)

**DESCRIPTION**
Compare-and-swap. Returns bool (true if swapped).

**EXAMPLE**
```zer
bool swapped = @atomic_cas(&lock, 0, 1);
```

---

### @critical { }

**DESCRIPTION**
Interrupt-disabled block. Disables interrupts on entry, re-enables on exit.
Per-architecture interrupt disable/enable.

**EXAMPLE**
```zer
@critical {
    // interrupts disabled here
    shared_counter += 1;
}
// interrupts re-enabled
```

---

## HARDWARE SUPPORT

### mmio

**DESCRIPTION**
Declare valid MMIO address ranges. Required for @inttoptr (unless --no-strict-mmio).
Multiple ranges allowed. Checked at compile time for constants, runtime for variables.

**SYNTAX**
```zer
mmio 0x40020000..0x40020FFF;
mmio 0x40011000..0x4001103F;
```

---

### volatile

**DESCRIPTION**
Prevents compiler from optimizing away reads/writes. Required for MMIO registers.

**SYNTAX**
```zer
volatile *u32 reg = @inttoptr(*u32, 0x40020014);
```

**NOTES**
- Shared globals accessed from interrupt handlers must be volatile.
- Compound assign (`reg |= 1`) on shared volatile → compile error (non-atomic RMW).

---

### interrupt

**DESCRIPTION**
Interrupt handler declaration. Emits `__attribute__((interrupt))`.

**SYNTAX**
```zer
interrupt USART1 {
    // handler body
}

interrupt UART_1 as "USART1_IRQHandler" {   // explicit symbol name
    // handler body
}
```

**NOTES**
- Slab.alloc() inside interrupt → compile error (calloc may deadlock).
- Access to non-volatile shared globals → compile error.

---

### asm

**DESCRIPTION**
Inline assembly. GCC operand syntax. Raw pass-through.

**SYNTAX**
```zer
asm("cpsid i");              // disable interrupts
asm("wfi");                  // wait for interrupt

// With operands (GCC extended syntax):
asm("mov %0, %1" : "=r"(out) : "r"(in));
```

---

### naked functions

**DESCRIPTION**
Function with no compiler-generated prologue/epilogue.
Body must be pure assembly.

**SYNTAX**
```zer
naked void reset_handler() {
    asm("ldr sp, =_stack_top");
    asm("b main");
}
```

---

### section attribute

**DESCRIPTION**
Place function or variable in a specific linker section.

**SYNTAX**
```zer
section(".isr_vector") u32[64] vector_table;
```

---

## MODULES

### import

**DESCRIPTION**
Import another ZER file. Functions are visible by default.
`static` functions are not exported.

**SYNTAX**
```zer
import uart;               // imports uart.zer from same directory
import gpio;
```

**EXAMPLE**
```zer
// uart.zer:
void uart_init(u32 baud) { }
static void internal_helper() { }   // not visible to importers

// main.zer:
import uart;
u32 main() {
    uart_init(9600);       // OK
    // internal_helper();  // COMPILE ERROR — static
    return 0;
}
```

**NOTES**
- Circular imports are a compile error.
- No header files needed.

---

### cinclude

**DESCRIPTION**
Include a C header file. Passes through to `#include` in emitted C.
Does NOT register C symbols — you must declare every C function you want
to call as a ZER function signature.

**SYNTAX**
```zer
cinclude "<stdlib.h>";
cinclude "my_header.h";
```

**EXAMPLE**
```zer
cinclude "<stdlib.h>";

*opaque malloc(usize size);
void free(*opaque ptr);

u32 main() {
    *opaque raw = malloc(64);
    free(raw);
    return 0;
}
```

**NOTES**
- C macros (stderr, stdout, etc.) are NOT accessible. Wrap in a C helper function.
- `_zer_` prefix is reserved — name helpers `zer_get_stderr`, not `_zer_stderr`.

**SEE ALSO**
import, *opaque

---

## COMPTIME

### comptime functions

**DESCRIPTION**
Compile-time evaluated functions. Replaces C `#define` macros.
All arguments must be compile-time constants. Zero runtime cost.

**SYNTAX**
```zer
comptime u32 BIT(u32 n) { return 1 << n; }
comptime u32 MAX(u32 a, u32 b) {
    if (a > b) { return a; }
    return b;
}
```

**EXAMPLE**
```zer
u32 mask = BIT(3);         // → 8 at compile time
u32 big = MAX(10, 20);     // → 20 at compile time

u32 x = 5;
u32 y = BIT(x);            // COMPILE ERROR — x is not compile-time constant
```

---

### comptime if

**DESCRIPTION**
Conditional compilation. Replaces C `#ifdef`. Condition must be compile-time constant.
Only the taken branch is type-checked — dead branch is ignored entirely.

**SYNTAX**
```zer
comptime if (DEBUG) {
    // only compiled when DEBUG is true
} else {
    // only compiled when DEBUG is false
}
```

**EXAMPLE**
```zer
const bool DEBUG = true;

comptime if (DEBUG) {
    void log([*]u8 msg) { puts(msg.ptr); }
} else {
    void log([*]u8 msg) { }    // no-op in release
}
```

---

## C INTEROP

### keep parameters

**DESCRIPTION**
Functions that store pointers beyond the call must annotate with `keep`.
The compiler checks that only global/static pointers are passed to `keep` params.

**SYNTAX**
```zer
void register_callback(keep *Handler h) {
    global_handler = h;
}
```

**EXAMPLE**
```zer
register_callback(&local_handler);   // COMPILE ERROR — local can't satisfy keep
register_callback(&global_handler);  // OK — global persists
```

---

### @cstr

**DESCRIPTION**
Convert a `[*]u8` slice to a NUL-terminated C string in a buffer.

**EXAMPLE**
```zer
u8[64] buf;
const [*]u8 name = "hello";
?*opaque f = c_fopen(@cstr(buf, name), "rb");
```

**SEE ALSO**
cinclude

---

### Type Mapping (ZER <-> C)

| ZER | C | Notes |
|-----|---|-------|
| `u8, u16, u32, u64` | `uint8_t, uint16_t, uint32_t, uint64_t` | Identical |
| `i8, i16, i32, i64` | `int8_t, int16_t, int32_t, int64_t` | Identical |
| `*T` | `T*` | Non-null on both sides |
| `?*T` | `T*` (nullable) | ZER forces unwrap |
| `*opaque` | `void*` | Provenance tracked |
| `[*]u8` | `struct { uint8_t *ptr; size_t len; }` | Fat pointer |
| `bool` | `uint8_t` | NOT integer in ZER |
| `Handle(T)` | `uint64_t` | index + generation |

---

## OPERATORS

### Arithmetic
`+  -  *  /  %` — All integer overflow wraps (never UB).

### Bitwise
`&  |  ^  ~  <<  >>` — Shift by >= width returns 0 (defined).

### Comparison
`==  !=  <  >  <=  >=` — Returns bool.

### Logical
`&&  ||  !` — Short-circuit evaluation.

### Assignment
`=  +=  -=  *=  /=  %=  &=  |=  ^=  <<=  >>=`

### Bit Extraction
```zer
reg[9..8]                  // Extract bits 9:8
reg[7..4] = 0x0F;          // Set bits 7:4
```

### NOT in ZER
- `++  --` — Use += 1, -= 1
- `(T)x` — C-style casts — use @truncate, @saturate, @bitcast
- `,` — Comma operator
- `goto` — Use structured control flow

---

## SAFETY GUARANTEES

| Bug Class | How ZER Prevents It |
|-----------|-------------------|
| Buffer overflow | Bounds check on every array/[*]T access. Proven-safe indices skip check. |
| Use-after-free | Handle generation counter + zercheck compile-time analysis |
| Null dereference | `*T` non-null by type. `?*T` forces unwrap. |
| Double free | zercheck: compile error |
| Memory leak | zercheck: compile warning (alloc without free) |
| Uninitialized memory | Everything auto-zeroed |
| Integer overflow | Wraps (defined), never UB |
| Silent truncation | Must use @truncate or @saturate explicitly |
| Missing switch case | Exhaustive check for enums, bools, unions |
| Dangling pointer | Scope escape analysis on return, assign, keep, orelse |
| Union type confusion | Cannot mutate union variant during switch capture |
| Arena pointer escape | Arena-derived pointers cannot be stored in globals |
| Division by zero | Forced guard — compile error if divisor not proven nonzero |
| Invalid MMIO address | mmio range declarations + alignment check + boot probe |
| ISR data race | Shared globals without volatile → compile error |
| Wrong pointer cast | Provenance tracking through *opaque round-trips |

---

## COMPILER

### Usage

```bash
zerc source.zer                   # compile to source.exe (default — no .c visible)
zerc source.zer --run              # compile + execute (no .c visible)
zerc source.zer --emit-c           # emit C to source.c (kept)
zerc source.zer -o output.c        # emit C to specific file (kept)
zerc source.zer -o output           # compile to specific exe (no .c visible)
zerc source.zer --lib              # library mode (no preamble/main)
zerc source.zer --no-strict-mmio   # allow @inttoptr without mmio ranges
zerc source.zer --target-bits 64   # set usize width
zerc source.zer --gcc arm-none-eabi-gcc   # cross-compile
```

### Pipeline

```
source.zer → Lexer → Parser → AST → Checker → ZER-CHECK → Emitter → .c → GCC → binary
```

### Build

```bash
make docker-check      # build + test in Docker (preferred)
make check             # build + test natively
make docker-install    # build Windows binaries, install to PATH
```

---

## WHAT ZER DOES NOT HAVE

- No classes, inheritance, templates, generics
- No exceptions, try/catch
- No garbage collector
- No implicit narrowing or sign conversion
- No undefined behavior
- No `++` / `--`, no comma operator
- No C-style casts
- No header files (use `import`)
- No preprocessor (use `comptime`)
- No pointer arithmetic

---

*ZER(C) — Zero Error Risk C Extension. Same syntax, same mental model. The compiler does the safety work.*
