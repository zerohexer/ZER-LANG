# ZER(C) Language Reference

**Zero Error Risk C Extension**
**Compiler:** `zerc` | **Target:** Any platform GCC supports

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

### uN, iN — Arbitrary-Width Integers

**DESCRIPTION**
Integers of any bit width from 1 to 128 — `u21`, `i48`, `u3`, `u128`, etc.
`u` = unsigned, `i` = signed (two's complement). First-class native types:
declare, assign, and use all operators. Stored in the smallest native carrier
that holds N bits; arithmetic wraps at 2^N (unsigned) or sign-extends (signed),
so the width is honest. The standard widths (u8/16/32/64, i8/16/32/64) stay
their own keywords; `uN`/`iN` covers every other width.

**SYNTAX**
```zer
u21 addr = 2097151;      // 21-bit, range [0, 2^21-1]
u21 next = addr + 1;     // wraps to 0 at 2^21
i12 delta = -2048;       // signed 12-bit, range [-2048, 2047]
u3  flag  = 7;           // sub-byte
u48 big;                 // 48-bit
```

**NOTES**
- Widths 1..128. Same no-implicit-narrowing rule as u8..u64 (`u21 x = @truncate(u21, big);`).
- Arithmetic on bare integer literals is `u32`, so `u21 x = 1000 + 500;` is rejected (u32→u21 narrowing). Write a fitting literal (`u21 x = 1500;`) or use `uN`-typed operands (`u21 a = 1000; u21 x = a + 500;` — this wraps at 2^N).
- Carrier = smallest native int ≥ N bits (`u21` → `uint32_t`); the compiler masks arithmetic results to N bits so the wrap is at 2^N, not the carrier width.
- A single sub-byte scalar is just a `uN`; for named bit-fields, use a `packed struct` or bit-slices `reg[hi..lo]`.
- `>64`-bit arithmetic (`u128` …) works but is emulated (multi-word). For hand-tuned big-int, use the `@addc`/`@subb`/`@mulw` carry primitives.

**SEE ALSO**
u8..u64, i8..i64, @addc, @subb, @mulw, @truncate

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

**NOTES**
- Digit-group underscores are allowed in numeric literals for readability and
  are ignored by the value: `1_000.5`, `3.141_592`, `1e1_0` (and `1_000_000`
  for integers).

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

// Range propagation: proven-safe indices have ZERO overhead
for (u32 j = 0; j < 4; j += 1) {
    scores[j] = j;        // proven j in [0,3] — no bounds check emitted
}

// Inline call range: function return range proves index safe
u32 hash(u32 key) { return key % 4; }
scores[hash(42)] = 10;    // hash returns [0,3] — no bounds check, zero overhead
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
- BUT returning a **sub-slice or `&`-element of a slice/pointer PARAMETER** is
  allowed — it's a view into the caller's buffer, not your stack:
  ```zer
  [*]u8 trim([*]u8 s, u8 c) { ... return s[i..s.len]; }   // OK — view of the param
  *u8 first([*]u8 s) { return &s[0]; }                     // OK
  ```
  The compiler still rejects a *caller* that passes a local and lets the result
  escape (`g_global = trim(local_buf)` is an error); using the result while the
  buffer is alive is fine. No lifetime annotations needed.

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
- Indexing a `*T` (`ptr[i]`) is a COMPILE ERROR — `*T` is one object and carries no
  length, so the access cannot be bounds-checked (it would be a silent buffer overflow).
  Use `[*]T` (a slice — it carries a length and is bounds-checked) for a collection, or
  dereference to read the single pointee: `*ptr` (or `ptr.field` for a field).

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
struct, move struct

---

### move struct

**DESCRIPTION**
Struct with ownership transfer semantics. Passing to a function or assigning to another variable transfers ownership — the original variable becomes invalid. Use after transfer is a compile error.

Used for types representing unique resources: file descriptors, hardware handles, DMA buffers, one-shot tokens. Prevents double-close, double-use, and accidental aliasing of unique resources.

ZER is copy-by-default (opposite of Rust). `move struct` opts IN to ownership tracking for the ~5% of types that need it.

**SYNTAX**
```zer
move struct FileHandle { i32 fd; }

FileHandle f;
f.fd = 42;

// Pass to function — ownership transferred
consume(f);
f.fd;                // COMPILE ERROR — use after move

// Assignment — ownership transferred
move struct Token { u32 kind; }
Token a;
a.kind = 1;
Token b = a;         // a transferred to b
a.kind;              // COMPILE ERROR — use after move
```

**SAFETY**
- Use after move → compile error
- Double move (pass twice) → compile error
- No interaction with other features — tracked independently

**SEE ALSO**
struct, shared struct

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
ZER supports two function pointer syntaxes — both produce the same type, both work everywhere, choose by style:

- **Variant 2A** — C-style, identical to kernel C. Requires typedef for arrays and inline-return-of-funcptr.
- **Variant 2C** — ZER-native (`*(args) -> ret name`), follows the `*T name` type-first convention, typedef-free in every position.

**SYNTAX — Variant 2A (C-style)**
```zer
u32 (*fn)(u32, u32) = add;                 // local variable
void (*callback)(u32 event);               // global variable
struct Ops { u32 (*compute)(u32); }        // struct field
u32 apply(u32 (*op)(u32, u32), u32 x, u32 y);  // parameter
?void (*on_event)(u32) = null;             // optional — null = not set
typedef u32 (*BinOp)(u32, u32);            // typedef
typedef ?u32 (*OptHandler)(u32);           // typedef — returns ?u32
BinOp[4] ops;                              // array of function pointers (via typedef)
```

**SYNTAX — Variant 2C (ZER-native, typedef-free)**
```zer
*(u32, u32) -> u32 fn = add;               // local variable
*(u32 event) callback;                      // global; void return = no arrow
struct Ops { *(u32) -> u32 compute; }       // struct field
u32 apply(*(u32, u32) -> u32 op, u32 x, u32 y);  // parameter
?*(u32) -> u32 on_event = null;             // nullable funcptr
*(u32) -> ?u32 lookup;                      // funcptr returning optional
*(keep *Handler) register;                   // keep modifier on a param

// Cases that REQUIRED typedef in 2A — now inline in 2C:
*(u32, u32) -> u32 [4] ops;                  // array of funcptrs — INLINE
*(u32, u32) -> u32 select_op(u32 kind);      // return-of-funcptr — INLINE
```

Discriminator: `*` BEFORE `(` is 2C; `*` INSIDE `(` is 2A. Both produce identical AST/types, so all downstream behavior (checker, IR, emitter, all 29 safety systems) is operator-agnostic — pick by readability.

**FIELD ACCESS RULE**

ZER uses `.` for ALL field access — values, pointers, Handles. No `->` operator in expressions. The `->` thin arrow is reserved for the 2C return-type separator (type-level only).

```zer
Task v;            v.field;       // value — direct access
*Task ptr;         ptr.field;     // pointer — compiler auto-derefs
Handle(T) h;       h.field;       // Handle — compiler auto-looks-up via slab.get
```

Mental model is taught by visible types (`*T`, `?*T`, `Handle(T)`), not by the deref operator. Modeled on Rust/Zig — both modern systems languages use `.` everywhere.

**OPTIONAL FUNCTION POINTERS VS OPTIONAL RETURN**
```zer
// At declaration sites (var, param, field, global):
// ? wraps the function pointer → nullable funcptr
?void (*cb)(u32) = null;                   // nullable callback
?void (*cb)(u32) = my_handler;             // assign function
if (cb) |f| { f(42); }                     // unwrap before calling

// At typedef sites:
// ? is part of the return type → funcptr returning optional
typedef ?u32 (*Lookup)(u32 key);           // returns ?u32
Lookup fn = my_lookup;
u32 val = fn(42) orelse 0;                 // unwrap return value

// For nullable typedef'd funcptr, use ? on the typedef name:
?Lookup maybe_fn = null;                   // nullable Lookup
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

A `const` distinct POINTER typedef makes the pointee read-only, exactly like
`const *T` — writing through it, or laundering the const away with
`@cast`/`@ptrcast`/`@pun`, is a compile error:
```zer
distinct typedef *u32 MyPtr;
const MyPtr cg = g;
*cg = 7;                        // COMPILE ERROR — write through const pointer
*u32 m = @ptrcast(*u32, cg);    // COMPILE ERROR — cannot strip const
u32 v = *cg;                    // OK — reading is fine
```

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

**COMPOUND TYPES**
Distinct typedef works with all compound types. The wrapped type's operations are preserved:
```zer
distinct typedef ?u32 MaybeId;       // orelse, if-unwrap, == null all work
distinct typedef *Motor SafeMotor;   // deref (*p), field access (p.speed) work
distinct typedef [*]u8 Text;         // indexing (t[0]), .len, sub-slice work
distinct typedef Point Vec2;         // field access (v.x, v.y) works
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

### do-while

**DESCRIPTION**
Execute body at least once, then check condition. C-style `do { } while (cond);`.

**SYNTAX**
```zer
do {
    val = read_register();
} while (val & BUSY_FLAG);
```

**NOTES**
- Braces required around body.
- `break` and `continue` work as expected.

---

### for (range-based)

**DESCRIPTION**
Iterate over slice elements. `in` is a contextual keyword (not reserved).

**SYNTAX**
```zer
for (u32 item in data_slice) {
    process(item);
}
```

**NOTES**
- Collection must be a variable or field access — function calls rejected at parse time.
- Desugars to indexed for loop with bounds-checked access.

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
- Optional `?T` switch: `default => |*v| { ... }` capture
  pattern works. `switch (v) { .red => ... }` works when inner is enum
  or union. Dot-prefix arms on `?u32` / `?bool` (non-variant inner) are
  rejected — use `if (x) |v| { ... } else { ... }` instead.

**SEE ALSO**
enum, union

---

### defer

**DESCRIPTION**
Runs a statement at scope exit, in reverse order of declaration.
Fires on ALL exit paths (return, break, continue, end of block).

Handle leaks are **compile errors** — allocating without `defer free()` (or returning/storing the handle) is rejected. The compiler error tells you exactly what to add.

`yield` and `await` are **banned** inside defer bodies — both directly and transitively (calling a function that yields is also rejected). Defer cleanup must be atomic; suspending mid-cleanup corrupts the coroutine state machine.

`defer` inside another `defer` body is also **banned** — the inner defer would run at the outer defer's execution time (scope exit), which is confusing and rarely what the programmer intends.

```zer
defer {
    defer { cleanup(); }   // COMPILE ERROR — 'defer' cannot be nested
}
```

`orelse` with a **value or block fallback** is banned inside a defer body — the
defer body cannot express orelse's branch. Compute the value before the defer.
(`orelse return` / `break` / `continue` are already banned there too, because
they corrupt cleanup flow.)

```zer
defer { u32 z = maybe() orelse g; }   // COMPILE ERROR
u32 z = maybe() orelse g;             // OK — compute it first
defer { use(z); }
```

A forward `goto` that jumps **over** a later `defer` to a label past it is a
compile error: on that path the defer never registered, so firing it at the
label would run cleanup that was never set up. Register the defer before the
goto — the normal acquire/cleanup order:

```zer
void f(u32 err) {
    if (err == 1) { goto done; }
    acq();
    defer rel();          // COMPILE ERROR — the goto above skips this
    lock_count += 100;
done:
    return;
}

void ok(u32 err) {
    acq();
    defer rel();          // OK — registered before the goto, so it is armed
    if (err == 1) { goto done; }
    lock_count += 100;
done:
    return;
}
```

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
- goto fires ALL pending defers before jumping — same as return/break/continue.
- Labels work inside switch arms, defer bodies, and @critical blocks.
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

### alloc / free — Universal Heap Allocation (the default)

**DESCRIPTION**
The brainless, malloc-equivalent surface — `alloc` for allocation, `free` to
release. No `Slab`/`Pool`/`Arena` declaration, no `Handle`. It works for **any**
element type (structs and primitives), returns a **typed** pointer or slice
(never an untyped `void*`), and is fully tracked by zercheck at compile time
(use-after-free, double-free, and leak all caught). Reach for this first;
`Pool`/`Slab`/`Handle`/`Arena` (below) are the explicit-control options for when
you specifically want a named pool.

**SYNOPSIS**
```zer
*Task  t  = alloc(Task) orelse return;      // one object      (like malloc(sizeof(Task)))
[*]u32 xs = alloc(u32, n) orelse return;    // n objects, zeroed (like calloc(n, sizeof(u32)))
free(t);                                     // release a *T
free(xs);                                    // release a [*]T
```

**FORMS**
- `alloc(T)` → `?*T` — one heap object of a **struct** type `T`.
- `alloc(T, n)` → `?[*]T` — a runtime-sized array of `n` `T` (T = struct **or**
  primitive: `alloc(u8, n)`, `alloc(u32, n)`, `alloc(Node, n)`). Memory is
  auto-zeroed (calloc semantics).
- `free(x)` → `void` — releases a `*T` or a `[*]T`. Dispatches on the shape.

**EXAMPLE**
```zer
struct KvEntry { u32 key; u32 value; ?*KvEntry next; }
struct Bucket  { ?*KvEntry head; }
struct KvTable { u32 size; [*]Bucket buckets; }   // [*]T — no bound in the type

?*KvTable kv_table_create(u32 n) {
    *KvTable ht = alloc(KvTable) orelse return;         // one table
    ht.size = n;
    ht.buckets = alloc(Bucket, n) orelse {              // runtime-sized array
        free(ht);
        return null;
    };
    return ht;                                          // escapes in the returned struct
}

void kv_table_free(*KvTable ht) {
    free(ht.buckets);
    free(ht);
}
```

**ERRORS**
```zer
[*]u32 x = alloc(u32, 4) orelse return;
free(x);
x[0] = 1;              // COMPILE ERROR — use-after-free

free(x);
free(x);               // COMPILE ERROR — double free

[*]u32 y = alloc(u32, 4) orelse return;
return 0;              // COMPILE ERROR — 'y' never freed, never escaped (leak)

*u8 p = alloc(u8) orelse return;   // COMPILE ERROR — alloc(T) needs a STRUCT;
                                    // use alloc(u8, n) for a primitive array
```

**NOTES**
- `alloc(T)` is exactly `T.alloc_ptr()` under the hood (sugar); `alloc(T, n)` and
  the slice `free` are new. Prefer the `alloc`/`free` spelling in new code.
- A slice from `alloc(T, n)` **escapes** — you can store it in a returned struct
  and free it later (a runtime-sized collection that outlives the function that
  built it). This is what plain fixed arrays and `Arena` slices cannot do.
- Custom allocators (a bump allocator or object pool over one `alloc(T, N)`
  region) are ordinary ZER code — hand out slot **indices**, or hand out `*T`
  interior pointers (`*T p = &region[i]; return p;` where `region` is a heap
  slice or a slice param is allowed; `&local_array[i]` is still rejected).
- Uses `calloc` internally — same ISR restriction as `Slab` (`alloc` inside an
  interrupt handler → compile error; use `Pool` there).
- `free` needs a `*T` or `[*]T` — a cinclude `free(ptr)` on a raw C pointer is
  left alone (routes to C's `free`).

**SEE ALSO**
Slab(T), Pool(T,N), Handle(T), Arena, alloc_ptr

---

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
- `.alloc()` → `?Handle(T)` — Allocate a slot (Handle). Returns null if all slots used.
- `.alloc_ptr()` → `?*T` — Allocate a slot (pointer). Returns null if all slots used.
- `.get(h)` → `*T` — Access by handle. Traps if gen mismatch.
- `.free(h)` → `void` — Free slot by handle, increment generation.
- `.free_ptr(p)` → `void` — Free slot by pointer.

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
- `.alloc()` → `?Handle(T)` or `?*T` — target type picks the variant (see note below).
- `.alloc_ptr()` → `?*T` — explicit pointer form.
- `.get(h)` → `*T` — Access by handle. Traps if gen mismatch.
- `.free(x)` → `void` — Handle or `*T` — arg type picks the variant.
- `.free_ptr(p)` → `void` — explicit pointer form.

**Target-type routing:** `slab.alloc()` returns a Handle when the target
variable is `Handle(T)` / `?Handle(T)`, and a pointer when the target is
`*T` / `?*T`. `slab.free(x)` dispatches on the argument type. Explicit
`_ptr` forms remain supported.

**EXAMPLE**
```zer
struct Task { u32 id; const [*]u8 name; ?*Task next; }
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
- **Auto-deref:** `h.field` works — compiler auto-inserts `slab.get(h).field` with gen check. No need to write `.get()` explicitly.
- **const Handle:** `const Handle(Task) h` allows data mutation through auto-deref. Handle is a key (like `const int fd`), const key does NOT mean const data. You can't reassign `h`, but you CAN write to `h.field`.
- **if-unwrap:** `if (maybe) |t| { t.id = 42; }` works — immutable capture `|t|` still allows data mutation through Handle auto-deref.
- **Arrays:** `Handle(T)[N]` works — array of handles with auto-deref on elements.
- `?Handle(T)` is an optional handle — used as return type of `.alloc()`.
- For direct pointer access without Handle, use `alloc_ptr()` instead.

**EXAMPLE (array of handles)**
```zer
Handle(Task)[4] tasks;
for (u32 i = 0; i < 4; i += 1) {
    tasks[i] = heap.alloc() orelse { return 1; };
    tasks[i].id = i;          // auto-deref on array element
}
```

**SEE ALSO**
Pool(T,N), Slab(T), alloc_ptr

---

### alloc_ptr / free_ptr — Direct Pointer from Slab/Pool

**DESCRIPTION**
Alternative to Handle that returns `*T` directly. Same Slab/Pool, but you get a
real pointer instead of a Handle. Tracked by zercheck at compile time — UAF, double-free,
cross-function free, and return-freed all caught. 100% compile-time safe for pure ZER code.

**SYNOPSIS**
```zer
*Task t = heap.alloc_ptr() orelse { return 1; };
t.id = 1;          // direct pointer deref — no gen check overhead
heap.free_ptr(t);   // zercheck marks t as FREED
```

**EXAMPLE**
```zer
struct Task { u32 id; u32 priority; }
Slab(Task) heap;

u32 main() {
    *Task t = heap.alloc_ptr() orelse { return 1; };
    t.id = 42;
    t.priority = 3;

    *Task t2 = heap.alloc_ptr() orelse { return 2; };
    t2.id = 99;

    heap.free_ptr(t);
    heap.free_ptr(t2);
    return 0;
}
```

**ERRORS**
```zer
heap.free_ptr(t);
t.id = 1;            // COMPILE ERROR — use-after-free

heap.free_ptr(t);
heap.free_ptr(t);     // COMPILE ERROR — double free

// Cross-function:
void destroy(*Task t) { heap.free_ptr(t); }
destroy(t);
t.id = 1;             // COMPILE ERROR — the compiler knows destroy() frees its parameter
```

**NOTES**
- `alloc_ptr()` returns `?*T` (null sentinel). Use `orelse` to unwrap.
- `free_ptr(*T)` finds the slot by pointer address and frees it. Argument type must match pool/slab element type — `*Motor` to `Task` pool is a compile error.
- Interior pointers tracked: `*u32 p = &t.id; free_ptr(t); *p` → compile error. A pointer to a field shares the parent's allocation.
- Can mix Handle and alloc_ptr on the same Slab/Pool.
- `const Handle(Task)` prevents mutation through auto-deref — `h.id = 42` on const Handle is a compile error.
- For `*opaque` (C interop), runtime checks (~1ns) cover the remaining cases the compiler can't track.
- GLOBALS: storing an `alloc_ptr` pointer in a global then
  freeing it requires resetting the global (`g = null;`) immediately after
  the free — before the function returns or calls another ZER function.
  Reading back a freed global, returning while it dangles, or calling ZER
  code in the free→reset window are all compile errors.
- INDIRECT CALLS: handles/pointers passed to a funcptr call are
  consume-maybe — after `fp(h)`, freeing or using `h` is a compile error.
  Pass data (`pool.get(h).field`) if the caller keeps ownership, or hand
  ownership entirely (caller stops touching `h`).
- VARIABLE-INDEX FREES: `heap.free(arr[k])` may free any tracked
  element of `arr` — mixing literal- and variable-index frees on the same
  array is a compile error in both orders.

**SEE ALSO**
Handle(T), Pool(T,N), Slab(T)

---

### Task.alloc() / Task.free() — Auto-Slab Allocation

**DESCRIPTION**
Allocate a struct without declaring a Slab. The compiler auto-creates a global
Slab per struct type behind the scenes. Same safety as explicit Slab. The
target type (or argument type for `free`) picks between Handle and `*T`
automatically — you write the same method name in both cases.

**SYNOPSIS**
```zer
// Handle path — target is Handle(T), routes to auto-Slab alloc:
Handle(Task) t = Task.alloc() orelse { return 1; };
t.id = 42;           // auto-deref
Task.free(t);        // arg is Handle → free

// Pointer path — target is *T, routes to auto-Slab alloc_ptr:
*Task t = Task.alloc() orelse { return 1; };
t.id = 42;           // direct deref
Task.free(t);        // arg is *T → free_ptr

// Explicit forms still work (same result as target-type routing):
*Task t = Task.alloc_ptr() orelse { return 1; };
Task.free_ptr(t);
```

**METHODS**
- `T.alloc()` → `?Handle(T)` or `?*T` — variant picked from target type
- `T.free(x)` → `void` — Handle or `*T` — variant picked from arg type
- `T.alloc_ptr()` → `?*T` — explicit pointer form (same as `T.alloc()` with `*T` target)
- `T.free_ptr(p)` → `void` — explicit pointer form (same as `T.free(p)` when p is `*T`)

**EXAMPLE**
```zer
struct Task { u32 id; u32 priority; }
struct Node { u32 value; }

u32 main() {
    // No Slab declaration needed — auto-created per struct type.
    // One method name for both forms; target type picks the variant.
    Handle(Task) t = Task.alloc() orelse { return 1; };
    t.id = 42;

    *Node n = Node.alloc() orelse { return 2; };
    n.value = 99;

    Task.free(t);   // Handle arg
    Node.free(n);   // *T arg
    return 0;
}
```

**NOTES**
- One auto-Slab per struct type, program-wide (shared across modules like C's malloc heap).
- Uses `calloc` internally — same ISR restriction as Slab.
- `T.free()` type-checks argument — `*Motor` passed to `Task.free()` is a compile error.
- Can mix with explicit Slab/Pool in the same program.
- Uniform allocator vocabulary across ZER: Pool / Slab / Arena / Task sugar all use `alloc` / `free` (no `new` / `delete`).
- **Preferred spelling:** the free-standing `alloc(T)` / `free(x)` (see "alloc / free — Universal Heap Allocation" above) is the recommended surface for auto-slab allocation, and adds `alloc(T, n)` for runtime-sized arrays. `Type.alloc_ptr()` / `Type.free_ptr()` remain as equivalents.

**SEE ALSO**
alloc / free, Slab(T), Pool(T,N), Handle(T), alloc_ptr

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

**SAFETY**
- Cannot be used in a GLOBAL variable initializer, even with a constant
  argument — it does not produce a compile-time-constant value at file scope.
  Use a literal, or compute it inside a function body. The same restriction
  applies to `@addc`, `@subb` and `@mulw`.
```zer
u8 sat = @saturate(u8, 300);       // COMPILE ERROR — global initializer
u32 main() {
    u8 ok = @saturate(u8, 300);    // OK — inside a function body
    return ok;
}
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
- `--no-strict-mmio` flag allows @inttoptr without mmio declarations —
  it relaxes the RANGE strictness only. The runtime ALIGNMENT trap is
  still emitted for variable addresses (alignment is a property of the
  target pointer type, not of mmio declarations), and constant
  addresses are alignment-checked at compile time regardless.
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
- `@ptrcast` between two DIFFERENT struct/union pointee types is a compile
  error — "type confusion — use @pun(...)". Identity casts,
  primitive byte-views (`*u32 → *u8`), and `*opaque` round-trips stay allowed.

**SEE ALSO**
*opaque, @pun, @container

---

### @pun(*T, src)

**DESCRIPTION**
Ergonomic explicit type-punning intrinsic. Casts a pointer to a different
pointer type with a runtime type_id check that traps on mismatch.
Equivalent to `@ptrcast(*T, @ptrcast(*opaque, src))` inlined as a single
intrinsic call.

Direct C-style cast `(*T)src` between different pointer types is rejected
at compile time. `@pun` is the audit-visible escape: the user explicitly
opts in to the type-erasure event, the runtime check catches genuine
type confusion before any memory read.

**EXAMPLE**
```zer
*Sensor s = sensors.get();
*Sensor back = @pun(*Sensor, s);   // identity — type_id matches, no trap

*Motor m = @pun(*Motor, s);        // type_id mismatch — runtime trap
                                   // "@pun type mismatch" before m is used
```

**NOTES**
- Compile-time: target must be a pointer, source must be a pointer,
  const stripping rejected, volatile stripping rejected.
- Runtime: traps on type_id mismatch via `_zer_trap("@pun type mismatch")`.
- For raw byte access (parsing, serialization), use `[*]u8` slices —
  not pointer casting. Slices are bounds-checked, len-carrying, and the
  right tool for byte-level data.
- For direct `(*T)src` where types match (identity), no `@pun` needed —
  the compiler allows the cast directly with zero overhead.

**WHEN TO USE**
- You have a `*T` and genuinely need to view its bits as `*U` (different
  type) and you accept the runtime check.
- Audit visibility matters: `grep '@pun'` finds every type-erasure event
  in the codebase.

**WHEN NOT TO USE**
- Identity casts (`*T → *T`) — just assign directly.
- Byte parsing — use `[*]u8` slices.
- Passing data through opaque APIs — use `*opaque` round-trip directly.

**SEE ALSO**
@ptrcast, *opaque, [*]u8

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

### @barrier(), @barrier_store(), @barrier_load(), @barrier_acq_rel()

**DESCRIPTION**
Memory barriers. Full (seq_cst), store-only (release), load-only (acquire),
or combined (acquire+release).
Emits GCC `__atomic_thread_fence()`.

---

### @unreachable(), @expect(val, expected)

**DESCRIPTION**
Control-flow hints. `@unreachable()` marks a path that cannot execute (UB if reached).
`@expect(val, expected)` is a branch prediction hint; returns `val` unchanged.

**EXAMPLE**
```zer
if (@expect(valid, 1)) { }
else { @unreachable(); }
```

---

### @bswap16(x), @bswap32(x), @bswap64(x)

**DESCRIPTION**
Byte swap — reverse byte order. For host/network byte order conversion.
Emits GCC `__builtin_bswap*()`.

**EXAMPLE**
```zer
u32 net = @bswap32(host);
```

---

### @popcount(x), @ctz(x), @clz(x), @parity(x), @ffs(x)

**DESCRIPTION**
Bit query operations. All return `u32`.
- `@popcount(x)` — count 1-bits
- `@ctz(x)` — count trailing zeros (UB if x=0)
- `@clz(x)` — count leading zeros (UB if x=0)
- `@parity(x)` — 0=even / 1=odd
- `@ffs(x)` — position of lowest 1-bit, 1-indexed (0 if x=0)

Input must be integer. Width-dispatched (ll suffix for 64-bit inputs).
Emits GCC `__builtin_*` / `__builtin_*ll`.

---

### @addc(a, b, carry_in), @subb(a, b, borrow_in), @mulw(a, b)

**DESCRIPTION**
Safe carry primitives for multi-word integer arithmetic — the building blocks
for big integers (u128, u256, …). Pure value operations, no asm, same safety
class as `@popcount`. Each returns a spellable 2-field struct:
- `@addc(a, b, carry_in)` → `AddCarry64 { u64 sum; u8 carry; }` — add with carry
- `@subb(a, b, borrow_in)` → `SubBorrow64 { u64 diff; u8 borrow; }` — subtract with borrow
- `@mulw(a, b)` → `MulWide64 { u64 lo; u64 hi; }` — full 64×64 → 128 multiply

Lower to GCC builtins: `__builtin_add_overflow` / `__builtin_sub_overflow` /
`unsigned __int128` multiply (portable fallback where `__int128` is absent).
GCC emits the hardware carry-flag / wide-multiply instructions.

**EXAMPLE**
```zer
// 128-bit add: chain @addc across two u64 limbs
AddCarry64 lo = @addc(a_lo, b_lo, 0);
AddCarry64 hi = @addc(a_hi, b_hi, lo.carry);
// result = { hi.sum : lo.sum }, carry-out = hi.carry
```

**NOTES**
- Plain `a + b` on a `u64` is `@addc(a, b, 0).sum` — the carry primitive is the
  general form; `+` is the restriction that discards the carry.
- For a fixed >64-bit integer, build `struct { u64[k] limb; }` and loop `@addc`
  across the limbs (how a library `U256` is written).

**SEE ALSO**
uN/iN, u64, @popcount

---

### @cpu_disable_int(), @cpu_enable_int(), @cpu_wait_int(), @cpu_save_int_state(), @cpu_restore_int_state(state)

**DESCRIPTION**
Privileged interrupt control. Per-arch inline asm (x86: cli/sti/hlt,
ARM: cpsid/cpsie/wfi, RISC-V: csrci/csrsi/wfi).
Faults with SIGSEGV in user mode — kernel code only.

**EXAMPLE**
```zer
u64 saved = @cpu_save_int_state();
@cpu_disable_int();
// critical section
@cpu_restore_int_state(saved);
```

**NOTES**
- For scope-bounded application code, prefer `@critical { body }`.
- These intrinsics are for fine-grained kernel control.

---

### @cpu_save_context(buf), @cpu_restore_context(buf), @cpu_save_fpu(buf), @cpu_restore_fpu(buf)

**DESCRIPTION**
Scheduler primitives. Save/restore callee-saved GPRs and SIMD/FP state
to/from a user-provided u8 buffer. Per-arch inline asm.

**EXAMPLE**
```zer
u8[128] ctx;
u8[512] fpu;
@cpu_save_context(&ctx[0]);
@cpu_save_fpu(&fpu[0]);
// switch stacks here (naked function, kernel-specific)
@cpu_restore_fpu(&fpu[0]);
@cpu_restore_context(&ctx[0]);
```

**NOTES**
- Buffer size: 128+ bytes for context, 512+ (16-byte aligned) for FPU.
- Callee-saved only (rbx/r12-r15 x86; x19-x28 ARM64; s0-s11 RISC-V).
- Full RSP/RIP save-restore needs a naked function — kernel-integration scope.
- `fxsave` on x86-64 requires 16-byte aligned buffer.

---

## SYSTEM / KERNEL INTRINSICS

> Every signature in this section was **measured against the compiler**
> (2026-08-20) by probing arity, argument kinds and return type, not copied
> from a design note. If a signature here disagrees with `zerc`, the doc is
> wrong — please report it.
>
> **Privilege.** Most of these execute privileged instructions (CPL 0 / EL1 /
> M-mode). Calling one from user space raises SIGSEGV/SIGILL. To verify that
> such code *compiles* without executing it, use the dead-branch pattern:
> ```zer
> u32 main() {
>     volatile u32 never_true = 0;
>     if (never_true == 42) { @cpu_write_cr3(0); }
>     return 0;
> }
> ```
> The `volatile` is load-bearing — without it the branch is folded away.
>
> **Portability.** Non-x86 targets get an architecture-appropriate instruction
> where one exists and a safe no-op fallback where one does not. These are
> hardware-domain operations: ZER verifies the *program* use of the value
> (types, bounds, qualifiers, provenance), never the *hardware* meaning. See
> CLAUDE.md's program-consequence vs hardware-consequence split.

### CPU identity and inspection (non-privileged)

**SIGNATURES**
```
@cpu_id()               -> u32     raw CPU identifier
@cpu_get_pc()           -> u64     current program counter
@cpu_read_sp()          -> u64     stack pointer
@cpu_read_tp()          -> u64     thread pointer / TLS base
@cpu_read_flags()       -> u64     RFLAGS / NZCV
@cpu_read_counter()     -> u64     cycle / timestamp counter (RDTSC-class)
@cpu_vendor_id()        -> u64     CPUID leaf 0 EBX (first 4 vendor chars)
@cpu_feature_bits()     -> u64     CPUID leaf 1 ECX:EDX packed
@cpu_model_id()         -> u32     CPUID leaf 1 EAX (family/model/stepping)
@cpu_core_id()          -> u32     physical core id
@cpu_num_cores()        -> u32     logical core count
@cpu_cache_line_size()  -> u32     L1 line size in bytes
@cpu_current_mode()     -> u32     privilege mode (0 = user)
@cpu_get_priv_level()   -> u32     privilege level
@cpu_cpuid(leaf, subleaf)     -> u64   (EBX << 32) | EAX
@cpu_cpuid_ecx(leaf, subleaf) -> u64   (EDX << 32) | ECX
```

**EXAMPLE**
```zer
u32 main() {
    u64 sp = @cpu_read_sp();
    u32 cores = @cpu_num_cores();
    u32 line = @cpu_cache_line_size();
    if (cores == 0) { return 1; }
    if (line == 0) { return 2; }
    if (sp == 0) { return 3; }
    return 0;
}
```

**NOTES**
- All non-privileged — safe to call from user code.
- `@cpu_cpuid` packs two 32-bit registers into one `u64`; split with
  `(u32)v` for the low half and `(u32)(v >> 32)` for the high half.

---

### Hardware random numbers

**SIGNATURES**
```
@cpu_rdrand() -> ?u64      RDRAND — null when the hardware declines
@cpu_rdseed() -> ?u64      RDSEED — null when the hardware declines
```

**DESCRIPTION**
These return an **optional**, not a raw integer, because the instruction can
legitimately fail (the entropy pool is momentarily empty) and reports that in
the carry flag. The optional makes the failure impossible to ignore — a caller
must unwrap, which is exactly the point.

**EXAMPLE**
```zer
u32 main() {
    // retry a bounded number of times, then fall back
    u64 seed = 0;
    for (u32 i = 0; i < 10; i += 1) {
        if (@cpu_rdrand()) |v| { seed = v; }
    }
    if (seed == 0) { return 0; }   // no entropy available — caller decides
    return 0;
}
```

**NOTES**
- Non-privileged on x86 when the CPU supports RDRAND/RDSEED.
- Never write `@cpu_rdrand() orelse 0` in security-sensitive code — a zero
  "random" value is a silent weakness. Retry, or fail loudly.

---

### Spin-wait, idle and power

**SIGNATURES**
```
@cpu_pause()                       PAUSE / YIELD — spin-loop hint
@cpu_wfe()                         ARM WFE — wait for event
@cpu_sev()                         ARM SEV — signal event
@cpu_idle_hint()                   non-blocking low-power hint
@cpu_deep_sleep()                  enter deepest idle state (WFI / HLT)
@cpu_reset()                       safe halt-forever fallback
@cpu_flush_pipeline()              instruction-pipeline sync barrier
@cpu_breakpoint()                  trap to the debugger (INT3 / BKPT / EBREAK)
@cpu_monitor_addr(ptr)             x86 MONITOR — arm an address watch
@cpu_mwait()                       x86 MWAIT — wait for a monitored write
@cpu_umonitor(ptr)                 user-mode MONITOR (WAITPKG)
@cpu_umwait(hint, deadline)        user-mode MWAIT (0 = C0.2, 1 = C0.1)
@wait_on_address(ptr, expected)    block while *ptr == expected
```

**EXAMPLE**
```zer
u32 main() {
    volatile u32 flagv = 1; u32 flag = flagv;
    u32 spins = 0;
    while (flag == 0) { @cpu_pause(); spins += 1; }
    @cpu_idle_hint();
    return 0;
}
```

**NOTES**
- `@cpu_pause` and `@cpu_idle_hint` are non-blocking and non-privileged.
- `@cpu_monitor_addr` / `@cpu_umonitor` take a **pointer or array**, not an
  integer address; use `@inttoptr` first if you have a raw address.
- `@wait_on_address` requires a pointer first argument.

---

### MMU / paging

**SIGNATURES**
```
@mmu_enable()                  turn the MMU on
@mmu_disable()                 turn the MMU off
@mmu_is_enabled()   -> bool    query
@mmu_get_pt()       -> u64     current page-table base
@mmu_set_pt(base)              set page-table base (switches address space)
@mmu_get_kernel_pt() -> u64    kernel page-table base
@mmu_set_kernel_pt(base)       set kernel page-table base
@mmu_get_fault_addr()   -> u64 faulting address
@mmu_get_fault_status() -> u64 fault status register
@mmu_sync()                    paging-structure sync barrier
```

**EXAMPLE**
```zer
u32 main() {
    volatile u32 never_true = 0;
    if (never_true == 42) {          // dead branch: compiles, never executes
        u64 pt = @mmu_get_pt();
        @mmu_set_pt(pt);
        @mmu_sync();
        if (@mmu_is_enabled()) { return 1; }
    }
    return 0;
}
```

**NOTES**
- **All of these are privileged, `@mmu_is_enabled` included** — it reads a
  control register, so calling it from user space traps just like the others.
  (Measured: an earlier draft of this page called it non-privileged and put it
  outside the dead branch; the example trapped at runtime.) Keep every MMU
  intrinsic inside the dead branch when you only want to check that it
  compiles.
- `@mmu_set_pt` takes an **integer** physical address, not a pointer.
- `@mmu_is_enabled` returns `bool`, so it is usable directly as a condition.

---

### TLB maintenance

**SIGNATURES**
```
@tlb_flush_all()               flush the whole TLB
@tlb_flush_global()            flush global entries too
@tlb_flush_addr(addr)          flush one address
@tlb_flush_range(addr, len)    flush an address range
@tlb_flush_asid(asid)          flush one address-space id
```

**EXAMPLE**
```zer
u32 main() {
    volatile u32 never_true = 0;
    if (never_true == 42) {
        @tlb_flush_addr(0x1000);
        @tlb_flush_range(0x1000, 0x2000);
        @tlb_flush_all();
    }
    return 0;
}
```

**NOTES**
- All arguments are **integers** (addresses / ids), not pointers.
- All privileged.

---

### Cache maintenance

**SIGNATURES**
```
@cache_flush_line(ptr)                  flush one line (CLFLUSH)
@cache_flushopt(ptr)                    CLFLUSHOPT — ordered alternative
@cache_writeback(ptr)                   CLWB — write back, keep valid (NVDIMM)
@cache_zero_line(ptr)                   zero a whole cache line (DC ZVA)
@cache_flush_range(ptr, size)           flush a range
@cache_clean_range(ptr, size)           clean (write back) a range
@cache_invalidate_range(ptr, size)      invalidate a range
@cache_invalidate_icache(ptr, size)     invalidate the instruction cache
@cpu_cache_disable()                    CR0.CD = 1 + WBINVD (privileged)
@cpu_cache_enable()                     CR0.CD = 0 (privileged)
@nt_store(ptr, value)                   non-temporal store (MOVNTI)
@barrier_dma()                          barrier ordering CPU vs DMA accesses
```

**EXAMPLE**
```zer
u8[256] dma_buf;

u32 main() {
    @cache_clean_range(&dma_buf, 256);   // make writes visible to the device
    @barrier_dma();
    @cache_invalidate_range(&dma_buf, 256);  // discard stale lines before reading
    return 0;
}
```

**NOTES**
- The pointer argument is a **pointer or array**, and the size a plain integer.
- `@cache_invalidate_icache` is what you need after writing instructions
  (JIT, bootloader relocation) before jumping to them.
- `@barrier_dma` is a distinct barrier from `@barrier()` — it orders CPU
  accesses against a device master, which on some targets is a different
  instruction from a plain memory fence.

---

### Model-specific, control and debug registers (x86, privileged)

**SIGNATURES**
```
@cpu_read_msr(msr)      -> u64      RDMSR
@cpu_write_msr(msr, v)              WRMSR
@cpu_read_cr0()  -> u64             CR0 (paging, WP, cache disable)
@cpu_write_cr0(v)
@cpu_read_cr2()  -> u64             CR2 — page-fault address
@cpu_read_cr3()  -> u64             CR3 — page-directory base
@cpu_write_cr3(v)                   switches address space
@cpu_read_cr4()  -> u64             CR4 — feature enables
@cpu_write_cr4(v)
@cpu_read_xcr0() -> u64             XSAVE feature mask
@cpu_write_xcr0(v)                  XSETBV
@cpu_read_dr(idx)  -> u64           debug register DR0-DR3, DR6, DR7
@cpu_write_dr(idx, v)
@cpu_read_pmc(idx) -> u64           RDPMC (needs CR4.PCE = 1 for user access)
@cpu_read_fsbase()  -> u64          needs CR4.FSGSBASE = 1
@cpu_read_gsbase()  -> u64
@cpu_write_fsbase(v)
@cpu_write_gsbase(v)
```

**EXAMPLE**
```zer
u32 main() {
    volatile u32 never_true = 0;
    if (never_true == 42) {
        u64 cr3 = @cpu_read_cr3();
        @cpu_write_cr3(cr3);
        u64 v = @cpu_read_msr(0xC0000080);   // IA32_EFER
        @cpu_write_msr(0xC0000080, v);
    }
    return 0;
}
```

**NOTES**
- Every argument is an **integer**. All privileged; non-x86 targets fall back
  to a no-op.
- `@cpu_read_cr2` is how a page-fault handler learns the faulting address on
  x86; the ARM/RISC-V equivalent is `@mmu_get_fault_addr`.

---

### Port I/O (x86)

**SIGNATURES**
```
@port_in8(port)   -> u8
@port_in16(port)  -> u16
@port_in32(port)  -> u32
@port_out8(port, value)
@port_out16(port, value)
@port_out32(port, value)
```

**EXAMPLE**
```zer
u32 main() {
    volatile u32 never_true = 0;
    if (never_true == 42) {
        @port_out8(0x3F8, 65);          // write 'A' to COM1
        u8 status = @port_in8(0x3FD);
        if (status == 0) { return 1; }
    }
    return 0;
}
```

**NOTES**
- The return widths differ per variant (`u8` / `u16` / `u32`) — the compiler
  will reject an assignment that would narrow silently.
- Privileged: requires CPL <= IOPL.

---

### Extended processor state (XSAVE / FXSAVE)

**SIGNATURES**
```
@cpu_xsave(buf, mask)      XSAVE   — buf is a pointer/array, mask an integer
@cpu_xrstor(buf, mask)     XRSTOR
@cpu_fxsave(buf)           FXSAVE  — legacy, 512-byte 16-byte-aligned buffer
@cpu_fxrstor(buf)          FXRSTOR
@cpu_fpu_init()            FNINIT
```

**EXAMPLE**
```zer
u8[512] fpu_area;

u32 main() {
    volatile u32 never_true = 0;
    if (never_true == 42) {
        @cpu_fxsave(&fpu_area);
        @cpu_fpu_init();
        @cpu_fxrstor(&fpu_area);
    }
    return 0;
}
```

**NOTES**
- Buffer must be large enough and correctly aligned — ZER checks that the
  argument *is* a pointer or array, not that it is big enough. Sizing is a
  hardware-domain fact (it depends on the enabled XSAVE features).

---

### Privileged transitions and interrupt lifecycle

**SIGNATURES**
```
@cpu_syscall()             user -> kernel trap (syscall / svc #0 / ecall)
@cpu_sysret()              kernel -> user return (sysretq / eret / sret)
@cpu_iret()                interrupt return (iretq / eret / mret)
@cpu_set_priv_stack(sp)    set the kernel stack used on syscall entry
@cpu_hypercall()           guest -> hypervisor (vmcall / hvc #0 / ecall)
@cpu_sbi_call()            RISC-V ecall to M-mode firmware
@cpu_smc_call()            ARM TrustZone smc #0
@cpu_eoi()                 end-of-interrupt to LAPIC / GICv3
@cpu_endbr()               ENDBR64 — CET-IBT landing pad (a NOP without CET)
```

**EXAMPLE**
```zer
u32 main() {
    volatile u32 never_true = 0;
    if (never_true == 42) {
        @cpu_set_priv_stack(0x8000);
        @cpu_eoi();
        @cpu_iret();
    }
    @cpu_endbr();          // non-privileged: a multi-byte NOP without CET
    return 0;
}
```

**NOTES**
- These require correctly-set system-register context **before** the
  transition instruction (CS/RIP/RFLAGS on x86; ELR/SPSR on ARM;
  sepc/sstatus on RISC-V). ZER cannot verify that — it is a hardware-domain
  precondition, and getting it wrong is a hardware-consequence failure.
- `@cpu_set_priv_stack` takes an **integer** stack address.

---

### @config(key, default)

**DESCRIPTION**
Build-configuration hook. **Currently a passthrough: it always evaluates to
the `default` argument**, and its type is the type of that default. There is
no mechanism yet to supply a value for `key` from the build, so today it is
only useful as a marker for a knob you intend to make configurable later.

**SYNTAX**
```zer
u32 main() {
    u32 baud = @config("uart.baud", 115200);   // always 115200 today
    if (baud != 115200) { return 1; }
    return 0;
}
```

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

### (Type)expr — C-Style Cast

**DESCRIPTION**
Explicit type conversion using C-style syntax. Narrowing truncates by default.

**SYNTAX**
```zer
(TargetType)expression
```

**EXAMPLE**
```zer
u8 small = 42;
u32 big = (u32)small;          // widening
u16 trunc = (u16)big;          // narrowing (truncate)
f32 ratio = (f32)big;          // int → float value convert
(*Motor)opaque_ctx             // pointer cast (provenance checked)
(*opaque)sensor_ptr            // type erase
```

**NOTES**
- Widening: always safe, no data loss.
- Narrowing: always truncates (keeps low bits). Use `@saturate` for clamping.
- `@bitcast` required for raw bit reinterpretation (e.g., u32 bits → f32).
- `@truncate`, `@ptrcast`, `@inttoptr` still work — `(Type)expr` is sugar.

---

### @atomic_load, @atomic_store, @atomic_cas

**DESCRIPTION**
Atomic load, store, compare-and-swap. Sequential consistency.
Uses GCC `__atomic_load_n` / `__atomic_store_n` / `__atomic_compare_exchange_n`.

**EXAMPLE**
```zer
u32 val = @atomic_load(&shared);
@atomic_store(&shared, 42);
bool swapped = @atomic_cas(&lock, 0, 1);
```

---

### @atomic_add, @atomic_sub, @atomic_or, @atomic_and, @atomic_xor, @atomic_nand, @atomic_xchg

**DESCRIPTION**
Atomic read-modify-write. Returns value BEFORE the operation.
`@atomic_xchg` swaps the value. All sequential consistency.

**EXAMPLE**
```zer
u32 old = @atomic_add(&counter, 1);
u32 old_lock = @atomic_xchg(&lock, 1);
```

---

### @atomic_add_fetch, @atomic_sub_fetch, @atomic_or_fetch, @atomic_and_fetch, @atomic_xor_fetch

**DESCRIPTION**
Atomic read-modify-write. Returns value AFTER the operation (new value).

**EXAMPLE**
```zer
u32 new_count = @atomic_add_fetch(&counter, 1);  // returns counter + 1
```

---

**Atomic restrictions (all `@atomic_*` intrinsics):**
- The target must be SHARED-CAPABLE storage. A **stack local** is rejected — an
  atomic on private frame memory is meaningless, since no other thread can hold a
  stable reference to it. Use a global, a global struct field, or take the address
  as a parameter (`void bump(*u32 p) { @atomic_add(p, 1); }`)
- First argument must be the address of an INTEGER global or struct field —
  `@atomic_add(&counter, 1)`, `@atomic_store(&s.flag, 1)`. Width must be 1, 2, 4
  or 8 bytes; `u128`, a non-native width like `u24`, a float, or a struct is
  rejected. The target does NOT need to be a `shared struct` — `shared` is a
  mutex, and taking a lock to perform a lock-free operation would defeat the
  point. Atomics on a plain global are the intended lock-free idiom
- Width must be 1, 2, 4, or 8 bytes
- Not allowed on packed struct fields (alignment)
- 64-bit atomics on 32-bit targets warn about libatomic dependency

---

---

### @critical { }

**DESCRIPTION**
Interrupt-disabled block. Disables interrupts on entry, re-enables on exit.
Per-architecture interrupt disable/enable.

`return`, `break`, `continue`, and `goto` are **banned** inside `@critical` blocks — jumping out would skip the interrupt re-enable, leaving the system with interrupts permanently disabled.

`yield`, `await`, and `spawn` are also **banned** inside `@critical` — both directly and transitively (calling a function that yields/spawns is also rejected). Yield/await would suspend with interrupts disabled (system hang). Spawn would create a thread with interrupts disabled (hardware-unsafe).

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
- Shared globals accessed from interrupt handlers must be volatile. This holds
  even when the ISR reaches the global INDIRECTLY — through a helper, or through
  a function bound to a local function pointer (`*() fp = bump; fp();`).
- Compound assign (`reg |= 1`) on shared volatile → compile error (non-atomic RMW).
- INDEXING a volatile `*T` (`reg[i]`) is bounds-checked against the `mmio`
  declaration, but only when the compiler can DERIVE the bound — which it can do
  only for a pointer obtained directly from `@inttoptr(*T, <const addr>)` inside
  a declared range. A parameter, alias or struct field carries no bound, so
  indexing one is a compile error rather than an unguarded access:

```zer
mmio 0x40020000..0x40020FFF;

u32 ok() {
    volatile *u32 base = @inttoptr(*u32, 0x40020000);
    return base[2];                       // OK — bound derived, range-checked
}

u32 bad(volatile *u32 reg, u32 i) {
    return reg[i];                        // COMPILE ERROR — no bound for a param
}
```

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
Inline assembly. Escape hatch for operations not covered by verified intrinsics.
Allowed only inside `naked` functions.

**SYNTAX**
```zer
asm("cpsid i");        // disable interrupts
asm("wfi");             // wait for interrupt

// With operands (GCC extended syntax):
asm("mov %0, %1" : "=r"(out) : "r"(in));
```

**WHEN TO USE**
- Prefer `@intrinsic()` calls — verified, safe, portable across archs.
- Use `asm` only for operations not yet covered by intrinsics (new vendor extensions, experimental hardware, niche use cases).
- For external asm code, use `cinclude "foo.S"` instead.

**AUDIT**
```bash
grep -rnE "\basm\s*[(]" src/
```

---

### naked functions

**DESCRIPTION**
`naked` marks a function whose body must be pure `asm(...)` statements plus `return`.
Today it serves two purposes, and only one of them is actually delivered:

1. **It is the permission to write `asm` at all.** Inline assembly is allowed *only*
   inside a `naked` function. This works and is the reason nearly every `naked` in
   practice exists.
2. **It is supposed to suppress the compiler-generated prologue/epilogue.** **This is
   NOT delivered.** The compiler does not emit `__attribute__((naked))`, so GCC still
   generates a full prologue and epilogue around your asm.

The compiler **warns at the declaration site** so this cannot surprise you:

```
warning: 'naked' is accepted but the attribute is NOT emitted — GCC will still
generate a prologue/epilogue for 'reset_handler'. ...
```

**What this means in practice.** Do NOT rely on any of the following inside a `naked`
function:
- exact frame layout (the compiler may spill),
- returning via your own `ret` / `iret` / `eret` (an epilogue you did not write runs),
- callee-saved registers being untouched.

Ordinary asm — a fence, a `nop`, a privileged instruction, reading a register into an
output operand — is unaffected, because those do not depend on the absence of a
prologue. Reset handlers, vector-table entries and context-switch primitives DO depend
on it: **write those in C and link them with `cinclude`.**

Tracked in `docs/limitations.md` ("naked attribute silently dropped"). Restoring true
naked semantics is a user-visible breaking change: GCC emits no `ret` for a naked
function, and GCC supports only *basic* asm inside one — which the structured
`inputs:`/`outputs:` form and ZER's Z-rule operand tracking both depend on.

**SYNTAX**
```zer
// Portable: `nop` assembles on x86, ARM and RISC-V alike.
naked void spin_hint() {
    asm("nop");
}

u32 main() { return 0; }
```

Architecture-specific bodies are written the same way, but only assemble for that
target (this one is ARM, so it needs `--target-arch=aarch64` and a cross GCC):

```
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

**QUALIFIED CALLS**
Both unqualified and module-qualified calls work:
```zer
import config;
u32 a = MAX_SIZE();           // unqualified — OK
u32 b = config.MAX_SIZE();    // qualified — also OK
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

// Declare the C function with a TYPED pointer. `?*T` because malloc can
// return NULL, and `*T` in ZER is non-null by definition.
?*u8 malloc(usize size);
void free(?*u8 ptr);

u32 main() {
    ?*u8 raw = malloc(64);
    free(raw);
    return 0;
}
```

**NOTES**
- C macros (stderr, stdout, etc.) are NOT accessible. Wrap in a C helper function.
- `_zer_` prefix is reserved — name helpers `zer_get_stderr`, not `_zer_stderr`.
- **Do not put `*opaque` in the signature of an extern C function.** ZER lowers
  `*opaque` to a two-field struct (`{ void *ptr; uint32_t type_id; }`) and emits
  *that* as the C prototype, but the real C function takes and returns `void *`.
  The pointer half survives on the supported ABIs, so it looks like it works —
  but nothing initialises `type_id`, so `@ptrcast`'s provenance check reads
  register residue and either traps on correct code or, if the residue happens to
  match a live type id, lets a wrong cast through. Use a typed pointer (`?*u8`,
  `*Sensor`, …) at the C boundary, and convert to `*opaque` on the ZER side with
  `@ptrcast` once the value is already inside ZER. Tracked in
  `docs/limitations.md` ("extern `*opaque` crosses the C boundary with an
  uninitialised `type_id`"), reproducer in
  `tests/zer_gaps/opaque_extern_type_id_uninit.zer`.

### Variadic `...` — and how to print

ZER has no built-in `print`. Formatted output is C's `printf`, declared as a bodyless
extern and called normally. That is what `...` exists for.

**SYNTAX**
```zer
i32 printf(const *u8 fmt, ...);      // declare it once, bodyless

u32 main() {
    printf("x=%d y=%s\n", 42, "ok");  // prints: x=42 y=ok
    printf("no args needed\n");        // fixed params alone are fine
    return 0;
}
```

No `cinclude` is required for `printf` — the declaration alone is enough, because the
emitted C is compiled by GCC which already has it. Use `cinclude "<stdio.h>";` when you
want the header's other declarations too.

String literals are `[*]u8` and auto-coerce to `const *u8` at an extern call site, so
`"hello"` can be passed directly. Passing a literal to a NON-const `*u8` parameter is
rejected — that would allow a write into `.rodata`.

**RULES**
- `...` is allowed **only** on a bodyless extern declaration. A ZER function with a body
  cannot be variadic — it would read untyped, unverified arguments, which is the
  unchecked-boundary ban:
  ```zer
  void g(const *u8 s, ...) { }   // ERROR: variadic '...' is only allowed on bodyless
                                 //        extern declarations, not on a ZER function
                                 //        with a body
  ```
- `...` must be the final parameter, and at least one named parameter must precede it:
  ```zer
  void f(...);                   // ERROR: '...' requires at least one named parameter before it
  ```
- A call must supply at least the fixed parameters; the variadic tail may be empty.
- Arguments in the variadic tail are **not type-checked against the format string.** This is
  C's contract, not ZER's — a `%d` paired with a `f64` is undefined behaviour exactly as it
  is in C. Everything else about the call (the pointer's validity, the slice's bounds, the
  lifetime of what you pass) is still verified normally.

**PRINTING WITHOUT VARIADICS**
For a single string, `puts` needs no variadic declaration at all:
```zer
i32 puts(const *u8 s);

u32 main() { puts("hello"); return 0; }
```

**NOTES**
- `stdout` / `stderr` are C **macros**, not variables, so they cannot be declared in ZER.
  To use `fprintf`, wrap them in a C helper:
  ```c
  /* my_io.h */
  static inline FILE *zer_get_stderr(void) { return stderr; }
  ```
  then `cinclude "my_io.h";` and declare `*opaque zer_get_stderr();` in ZER. Name it
  `zer_*`, never `_zer_*` — the `_zer_` prefix is reserved.

### Safe C Library Interop — `cinclude` + `*opaque` + `shared struct`

Two keywords make ANY C library fully safe from ZER:

**Memory safety** — wrap C pointers in `*opaque`:
```zer
cinclude "sensor.h";
*opaque sensor_open(const [*]u8 path);
void sensor_close(*opaque dev);
u32 sensor_read(*opaque dev);

u32 main() {
    *opaque dev = sensor_open("/dev/spi0");
    defer sensor_close(dev);          // zercheck: leak prevented
    u32 val = sensor_read(dev);       // zercheck: dev is ALIVE
    return 0;
}
// sensor_close(dev) fires via defer
// sensor_read(dev) after close = COMPILE ERROR (use after free)
```

**Concurrency safety** — wrap shared data in `shared struct`:
```zer
cinclude "event_lib.h";
void event_register(void (*cb)());

shared struct State { u32 counter; *opaque handle; }
State g;

void on_event() {
    g.counter += 1;    // auto-locked — safe from ANY thread
}

u32 main() {
    g.counter = 0;
    event_register(on_event);    // C library calls on_event from its own thread
    return 0;
}
```

The auto-lock fires regardless of which thread calls the function — ZER `spawn`, C `pthread_create`, OS callback, interrupt handler. The lock is on the DATA (the `shared struct`'s mutex), not on the thread creation mechanism.

**The complete safety model:**
| Tool | Protects | Mechanism |
|---|---|---|
| `*opaque` | Pointer lifecycle (UAF, double-free, leak) | zercheck compile-time + runtime type_id |
| `shared struct` | Data race prevention | Auto-lock (recursive pthread_mutex) |
| `spawn` checker | Thread creation safety | Compile-time arg validation |

**What ZER cannot protect:** C library internal bugs. If the C library itself has data races or UAF in its own code, ZER can't fix that — same boundary as Rust's `unsafe extern`.

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

**LIMITS**
- Recursive comptime call chains have a depth cap of 16. Exceeding it
  produces a clear compile error: `comptime call chain exceeded
  recursion depth (16) — split the computation, hoist constants, or
  reduce recursion depth`. Restructure to use iteration or split into
  multiple smaller comptime functions.

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

**CONDITIONS**
Accepted: literals (`1`, `0`), `const` variables, comptime function calls, expressions combining these.
```zer
comptime if (1) { ... }                    // literal
comptime if (DEBUG) { ... }                // const bool
comptime if (PLATFORM()) { ... }           // comptime function call
comptime if (VER() > 1) { ... }            // expression with comptime call
const u32 P = PLATFORM();
comptime if (P) { ... }                    // const from comptime result
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

### static_assert

**DESCRIPTION**
Compile-time assertion. Condition must evaluate to a compile-time constant. False → compile error with optional message.

**SYNTAX**
```zer
static_assert(SIZE > 0, "size must be positive");
static_assert(Color.red == 0);
```

---

### Comptime Advanced Features

**DESCRIPTION**
Comptime functions support: local variables, loops (for/while), switch, arrays, struct return, float arithmetic, and enum values.

**SYNTAX**
```zer
// Locals and loops
comptime u32 SUM(u32 n) {
    u32 total = 0;
    for (u32 i = 0; i <= n; i += 1) { total += i; }
    return total;
}

// Array indexing
comptime u32 LUT(u32 i) {
    u32[4] t;
    t[0] = 10; t[1] = 20; t[2] = 30; t[3] = 40;
    return t[i];
}

// Struct return
comptime Point ORIGIN() { return { .x = 0, .y = 0 }; }

// Float arithmetic
comptime f64 DEG_TO_RAD(f64 deg) { return deg * 3.14159 / 180.0; }

// Enum values (compile-time evaluable)
static_assert(Color.red == 0, "red is 0");
```

---

### Designated Initializers

**DESCRIPTION**
Initialize struct fields by name. Unmentioned fields auto-zero. Works in var-decl, assignment, call args, and return.

**SYNTAX**
```zer
Point p = { .x = 10, .y = 20 };
p = { .x = 100, .y = 200 };
func({ .x = 1, .y = 2 });
Point make() { return { .x = 0, .y = 0 }; }
```

---

### container — Parameterized Struct

**DESCRIPTION**
User-defined parameterized struct template. Stamps concrete struct per type argument (monomorphization). No methods — use free functions.

**SYNTAX**
```zer
container Stack(T) {
    T[64] data;
    u32 top;
}

Stack(u32) s;
void stack_push(*Stack(u32) s, u32 val) {
    s.data[s.top] = val;
    s.top += 1;
}
```

**NOTES**
- T substitution works in: `T`, `*T`, `?T`, `[*]T`, `T[N]`, and `Handle(T)` field types
  (including nested — `?Handle(T)`, `Handle(T)[N]`, and a container field holding
  another container over the same T).
- `Pool(T, N)`, `Slab(T)` and `Ring(T, N)` are NOT supported as container fields —
  the compiler cannot stamp their inline storage for a monomorphized container.
  `Handle(T)` works because a Handle is a `u64` (index + generation), so the
  stamped struct needs no per-T layout. Declare the allocator as a global and
  store `Handle(T)` in the container instead.
- Instances cached — same `Stack(u32)` reuses cached stamp.
- NOT generics — no type constraints, no SFINAE.
- Type ARGUMENT must be a plain named type (primitive or struct/enum/union).
  Composite args — `Box(?u32)`, `Box(*u32)`, `Pair(Handle(Item))`, `Box([*]u8)`
  — are a clean compile error with a wrapper-struct hint: wrap the
  composite in a named struct and instantiate with that. NESTED containers
  work — `Stack(Stack(u32))` resolves inner-first to `Stack_Stack_u32`.
- SELF-REFERENCE through a pointer is supported — this is the canonical
  linked list / tree node:
```zer
container LNode(T) { T val; ?*LNode(T) next; }

LNode(u32) a; LNode(u32) b;
a.val = 10; b.val = 20; a.next = &b;
```
  `*LNode(T)`, `?*LNode(T)` and `[*]LNode(T)` self-fields are all fine.
- BY-VALUE self-reference is a compile error — it would be an infinite-size
  struct. Use a pointer field instead:
```zer
container BNode(T) { T val; BNode(T) child; }   // COMPILE ERROR
container BNode(T) { T val; ?*BNode(T) child; } // OK
```

---

### --stack-limit N

**DESCRIPTION**
Compile error when estimated stack usage exceeds N bytes. Checks per-function frame size and entry-point call chain depth.

**SYNTAX**
```
zerc main.zer --run --stack-limit 2048
```

**NOTES**
- Recursive functions get warning (can't compute max depth).
- Function pointer calls with unknown target → error with --stack-limit (can't verify depth).

---

## C INTEROP

### keep parameters (INFERRED — no annotation needed)

**DESCRIPTION**
`keep` marks a pointer parameter whose pointee the function retains beyond the
call (stores into a global/static, into another pointer-param's field, or
returns through a sink). Callers of a keep param may pass only static/global
(long-lived) pointers, never `&local`/arena/local-slice — otherwise the stored
pointer would dangle when the caller's frame dies.

**`keep` is now INFERRED — you never have to write it.** The compiler detects
the retention from the function body and infers keep automatically, including
*transitively* (a param forwarded to another function's keep param becomes keep
too). The `keep` keyword is still **accepted** as an optional explicit marker
(e.g. to document an API contract before the storing line exists), but it is
never *required*.

**SYNTAX**
```zer
void register_callback(*Handler h) {   // no `keep` needed — inferred from the store
    global_handler = h;                 // retention detected → h inferred keep
}
```

**EXAMPLE**
```zer
register_callback(&local_handler);   // COMPILE ERROR — local can't satisfy (inferred) keep
register_callback(&global_handler);  // OK — global persists
```

**NOTES**
- Inference covers: direct store to global/static, store through a pointer-param
  field, store of an alias, a call-result launder (`g = idfn(p)`), and a
  by-value STRUCT/UNION param whose pointer fields are persisted.
- **Transitive (closes BH-15):** `void outer(*T p) { inner(p); }` where
  `inner`'s param is (inferred or explicit) keep → `outer`'s `p` is inferred
  keep, so `outer(&local)` is rejected. This is sound across forward references
  and modules (a dedicated post-body pass resolves keep before enforcing).
- **Struct fields:** storing a keep-derived borrow into a struct field no longer
  requires a `keep` field — the borrow is provably static, so it is always safe.
- **Function pointers:** a funcptr CALL worst-cases its pointer params as keep
  (the target is invisible and could retain the pointer). This applies both to
  passing `&local` *directly* to a funcptr param AND to *forwarding* a param to
  a funcptr (`void fwd(*T p, *(*T) cb){ cb(p); }` infers `p` keep), so a stack
  pointer can never reach a retaining callback through any funcptr indirection —
  the keep/escape property is 100% sound. Consequence: a read-only callback must
  be given a long-lived (global/static) context, not a stack-local one — the same
  rule that already applies to a direct funcptr call.

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
`&  |  ^  ~  <<  >>` — Shift by >= width OR < 0 returns 0 (defined).
This covers negative shift counts too: a signed count that is negative
(e.g. `i32 n = -1; x << n`) returns 0 rather than falling into C
undefined behavior.

### Comparison
`==  !=  <  >  <=  >=` — Returns bool.

### Logical
`&&  ||  !` — Short-circuit evaluation.

### Assignment
`=  +=  -=  *=  /=  %=  &=  |=  ^=  <<=  >>=`

### Bit Extraction

Bit-slices operate on a scalar **value**, not on a pointer:

```zer
u32 main() {
    u32 reg = 0x0300;
    u32 field = reg[9..8];     // extract bits 9:8  -> 3
    reg[7..4] = 0x0F;          // set bits 7:4
    reg[7..0] += 3;            // compound assign — read-modify-write of the field
    if (field != 3) { return 1; }
    return 0;
}
```

**On an MMIO register, dereference first.** `reg[hi..lo]` where `reg` is a
`volatile *u32` does NOT bit-extract — `[a..b]` on a pointer parses as a slice
range, so it fails with *"slice start (9) is greater than end (8)"* or
*"cannot slice type '\*u32'"*. Read the register into a value, then slice it:

```zer
mmio 0x40000000..0x40000FFF;

u32 main() {
    volatile *u32 reg = @inttoptr(*u32, 0x40000000);
    u32 v = *reg;              // one volatile read
    u32 field = v[9..8];       // then bit-extract from the value
    if (field > 3) { return 1; }
    return 0;
}
```

Reading into a value first is also the correct hardware idiom: it makes the
number of volatile accesses explicit, so extracting three fields from one
register is one read rather than three.
Every compound operator works on a bit-slice target (`+= -= *= /= %= &= |= ^=
<<= >>=`): the current field value is read, the operation applied, the result
written back into those bits. This is the register read-modify-write idiom.

Positions may be runtime values (`reg[hi..lo]`). If a runtime position is at or
beyond the width of the target, the write is a **defined no-op** — the target is
unchanged — matching ZER's rule that a shift of at least the type width is `0`
rather than undefined. A position known at compile time to be out of range is a
compile error instead.

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
| Thread data race | Spawn target body scanned for non-shared global access → error/warning |
| Dangling @ptrtoint | `return @ptrtoint(&local)` → compile error (direct + indirect via struct fields) |
| Stack overflow | `--stack-limit N` per-function + call chain check. Funcptr indirect calls flagged. |
| Division by zero (call) | `x / func()` where func() return range unknown → compile error |
| Wrong pointer cast | Provenance tracking through *opaque round-trips |

---

## COMPILER

### Usage

```bash
zerc source.zer                          # compile to source.exe (default — no .c visible)
zerc source.zer --run                    # compile + execute (no .c visible)
zerc source.zer --emit-c                 # emit C to source.c (kept)
zerc source.zer -o output.c              # emit C to specific file (kept)
zerc source.zer -o output                # compile to specific exe (no .c visible)
zerc source.zer --lib                    # library mode (no preamble/main)
zerc source.zer --no-strict-mmio         # allow @inttoptr without mmio ranges
zerc source.zer --target-bits 64         # set usize width
zerc source.zer --gcc arm-none-eabi-gcc  # cross-compile
zerc source.zer --target-arch=aarch64    # cross-compile to aarch64
zerc source.zer --target-arch=riscv64    # cross-compile to riscv64
zerc source.zer --target-features=avx512f         # enable AVX-512F SIMD
zerc source.zer --target-features=aes,sha,bmi1    # enable x86 CPU extensions (comma-separated)
zerc source.zer --probe-mode=hosted               # @probe with signal handler (default)
zerc source.zer --probe-mode=raw                  # @probe direct read, no fault recovery
zerc source.zer --probe-mode=disabled             # reject any @probe usage at compile time
```

### Pipeline

```
source.zer → Lexer → Parser → AST → Checker → ZER-CHECK → Emitter → .c → GCC → binary
```

---

## CONCURRENCY

### shared struct — Auto-Locked Thread-Safe Data
```zer
shared struct Counter { u32 value; u32 total; }
Counter g;
g.value = 42;              // auto: lock → write → unlock
g.total = g.value + 1;     // same lock scope (consecutive access grouped)
```
- Copying a **whole shared struct by value** (`Counter c = g;`, an assignment, a
  return, or a by-value argument) is a compile error — the embedded lock would be
  cloned, so the copy would lock a different lock than the original, and the
  multi-field read is torn. Read individual fields (`u32 v = g.value;`, each
  auto-locked) or pass a pointer `*Counter`.
- `switch` over a **union field of a shared struct** is safe: the union is copied
  out under the lock, so a value capture `.v => |x| { ... }` reads a private
  snapshot. A **mutable pointer capture `|*x|` of a shared union variant** is a
  compile error (it would alias the shared bytes past the auto-lock) — copy the
  field into a local, mutate it, then assign it back as its own statement.

### shared(rw) struct — Reader-Writer Lock
```zer
shared(rw) struct Config { u32 threshold; u32 retries; }
Config cfg;
cfg.threshold = 100;       // auto write lock (exclusive)
u32 t = cfg.threshold;     // auto read lock (multiple readers OK)
```

### spawn — Thread Creation
```zer
// Fire-and-forget (only shared ptr or value args):
spawn worker(&shared_state);    // OK — shared struct auto-locked
spawn handler(42, true);        // OK — value args copied

// Scoped spawn (allows *T — thread joined before scope exit):
ThreadHandle th = spawn compute(&local_data);
th.join();                      // MUST join — compile error if not
```

**SCOPED-BORROW RULES** — a local lent to a scoped spawn via `&x` is exclusively
borrowed by that thread until `.join()`:
- Reading or writing `x` before the join → compile error (data race)
- Passing `&x` to any call before the join → compile error (the callee could
  write through it while the thread holds it). Passing `x` **by value** is fine.
- Lending the same local to a **second** live scoped spawn → compile error. Join
  the first thread, then spawn the second.
- `&threadlocal` to a scoped spawn → compile error. Each thread has its own copy,
  so the child would write the parent's slot. Pass it by value instead.
- All `&` arguments are tracked, not just the first; `.join()` releases every one.
- A `.join()` **inside a branch** does not release the borrow for code after that
  branch — the other path never joined, so the thread may still be running:
  ```zer
  ThreadHandle th = spawn worker(&work);
  if (err) { th.join(); return 0; }
  work.x = 2;              // compile error — path 2 never joined
  ```
  This also means joining on *every* arm is not recognised as unconditional.
  Hoist the join out of the branch instead: `if (err) { ... } th.join();`

**SAFETY CHECKS**
- Non-shared `*T` to fire-and-forget spawn → compile error
- Optional pointer/slice (`?*T`, `?[*]T`) to a stack local → compile error (same
  rule as the bare pointer — the `?` wrapper does not exempt it)
- A local **array** passed where the parameter is `[*]T` → compile error (it
  coerces to a slice over stack memory)
- A by-value **struct/union carrying a pointer into a local** → compile error,
  including when the struct comes back from a call (`spawn w(mk(&loc))`)
- A by-value **struct/union carrying ANY non-shared pointer or slice** to a
  fire-and-forget spawn → compile error, whether the pointee is stack or heap.
  The thread receives a copy of the pointer, so parent and child both hold it.
  A pointer to a `shared struct` is exempt (auto-locked), and a struct with no
  pointers at all is always fine
- Handle to spawn → compile error (pool.get not thread-safe). This covers a
  Handle at **any nesting** — bare, inside a struct, inside a nested struct,
  behind `?`, in an array — not just a bare `Handle(T)` argument
- Scoped spawn: freeing a payload the child still holds **before** `.join()` →
  compile error, including when the payload is carried inside a by-value struct
- A callback the spawn target invokes is scanned too — if it touches a
  non-shared global that is a data race, no matter how the target reaches it:
  a function name passed as an argument (`spawn worker(bump)`), a local bound
  to a function name (`*() fp = bump; fp();`), a funcptr struct field
  (`o.cb = bump; o.cb();`), a funcptr array element, or a funcptr obtained from
  a factory (`*() fp = get_fp(); fp();`, including a factory that returns
  another factory's result). A callback that touches nothing, touches only
  `threadlocal`, or synchronizes with `@atomic_*` compiles as usual
- Spawn target body scanned for non-shared global access:
  - No atomic/barrier in function → compile **error**
  - Has atomic/barrier → compile **warning** (lock-free pattern possible)
  - Transitive: follows callees 8 levels deep
- Escape hatches: `shared struct`, `threadlocal`, `@atomic_*`, `const`, and a
  **single-word** `volatile` global
- The same single-word restriction applies to a global shared between an
  **interrupt handler** and main code: `volatile` is required there, but a
  `volatile u64` on a 32-bit target, a `volatile u128`, or a volatile struct is
  rejected — the access lowers to several loads/stores, so main can read half of
  one ISR update and half of another
- `volatile` is the narrowest of these and is **not synchronization** — it gives no
  atomicity and no ordering. It is accepted only for the single-word flag idiom
  (a plain store/load of a scalar no wider than the target word). Rejected:
  a compound `g += 1` (non-atomic read-modify-write), a global wider than the
  target word such as `volatile u64` on a 32-bit target (the store lowers to two
  stores and can tear), and a volatile aggregate. For anything beyond a one-way
  flag, use `shared struct` or `@atomic_*`
- spawn inside `@critical` → compile error (direct + transitive via function summaries)
- spawn inside `async` function → compile error (thread may outlive coroutine)
- spawn inside interrupt handler → compile error (direct + transitive)

### Condvar — Thread Synchronization
```zer
@cond_wait(shared_var, shared_var.count > 0);  // wait for condition
@cond_signal(shared_var);                       // wake one waiter
@cond_broadcast(shared_var);                    // wake all waiters
@cond_timedwait(shared_var, condition, 1000);   // timeout in ms → ?void
```
- The predicate is re-checked under **only** the condition variable's own mutex, so
  it may read **only that same shared struct**. Reading a *different* shared struct
  in the predicate → compile error (it would be an unsynchronized cross-thread race):
  ```zer
  @cond_wait(gq, gq.count > 0 && gother.flag);   // ERROR: gother is a different shared struct
  @cond_wait(gq, gq.count > 0 && gq.shutdown);   // OK: both fields are gq's own
  ```
  Fold the extra state into the same `shared struct`, or signal on its change.

### threadlocal — Per-Thread Storage
```zer
threadlocal u32 counter;    // each thread has its own copy
```
- `threadlocal shared struct X g;` is rejected. The two
  annotations are mutually exclusive — `threadlocal` gives each thread
  its own copy + own mutex, so cross-thread synchronization is
  impossible. Use either `threadlocal` (per-thread isolation) OR
  `shared` (cross-thread synchronization), not both.

### @once — Thread-Safe Init
```zer
@once {
    global_config = load_defaults();
}
```
- Runs the body **exactly once** across all threads. Threads that lose the race
  **block until the winner finishes the body**, then proceed — so a loser never
  observes half-initialized state (e.g. a published pointer before its target is
  built). (Bare-metal/freestanding builds without atomics are single-core only and
  do not wait.)
- Control flow that exits the body — `return`, `break`, `continue`, `goto` — is a
  **compile error** (it would skip the one-time completion signal and hang the
  waiting threads). Put such logic in a helper function called from `@once`.

### Barrier — Thread Sync Point
```zer
Barrier bar;                // keyword type (like Arena, Pool)
@barrier_init(bar, 3);     // 3 threads must arrive
@barrier_wait(bar);         // blocks until all 3 call wait
```
- `Barrier` is a builtin type — checker validates `@barrier_init`/`@barrier_wait` args are `Barrier` type.
- Using wrong type (e.g., `u32`) → compile error.

### Semaphore — Counting Semaphore
```zer
Semaphore(3) dma_channels;    // 3 resources available
@sem_acquire(dma_channels);   // blocks until count > 0, decrements
@sem_release(dma_channels);   // increments, wakes one waiter

// Pointer param support:
void use_resource(*Semaphore s) {
    @sem_acquire(s);
    @sem_release(s);
}
```
- `Semaphore(0)` valid — producer-consumer pattern (start empty, producer releases).
- Thread-safe: has own mutex + condvar internally.
- Type-checked: `@sem_acquire` only accepts `Semaphore` or `*Semaphore`.

### Atomics
```zer
@atomic_store(&flag, 1);
u32 val = @atomic_load(&flag);
@atomic_add(&counter, 1);
bool swapped = @atomic_cas(&lock, 0, 1);
```

### async/await — Stackless Coroutines
```zer
void led_on();
void led_off();

async void blink() {
    while (true) {
        led_on();
        yield;              // pause, resume on next poll
        led_off();
        yield;
    }
}

u32 main() {
    _zer_async_blink task;
    _zer_async_blink_init(&task);
    while (true) {
        _zer_async_blink_poll(&task);   // advance one step
    }
}
```
Zero heap, zero runtime. Each task is a stack-allocated struct (~4-50 bytes).

**`yield` / `await` outside `async` is a compile error** — they only
make sense inside async functions. Using them in a regular function
emits nothing useful (no state machine exists), so the compiler rejects
at the use site rather than silently stripping.

```zer
void regular() {
    yield;   // COMPILE ERROR — 'yield' only allowed inside async function
}
```

### Deadlock Detection (Compile-Time)

ZER detects when a single statement accesses TWO different shared types — the emitter can only lock one per statement, leaving the other unprotected.

```zer
shared struct A { u32 x; }
shared struct B { u32 y; }
A a; B b;
a.x = 1; b.y = 2;          // OK — separate statements, independent locks
b.y = 2; a.x = 1;          // OK — each statement locks/unlocks independently
a.x = b.y;                 // COMPILE ERROR — one statement accesses both A and B
```

Cross-statement ordering is safe because the emitter does lock→op→unlock per statement group — no two different shared types are ever locked simultaneously.

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
