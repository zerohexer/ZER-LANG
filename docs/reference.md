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

u32 wide = 300;
u3  narrow = (u3)wide;   // C-style cast — truncates to 3 bits, so 4
```

**NOTES**
- Widths 1..128. Same no-implicit-narrowing rule as u8..u64 — narrow explicitly with either `@truncate(u21, big)` or a C-style cast `(u21)big`. Both wrap to N bits, not to the carrier width (BUG-946: `(u3)300` is 4, not 44).
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
- A literal too large for the type becomes an **infinity**, which is a normal
  IEEE value and works at both global and function scope:
  ```zer
  f64 huge = 1e400;         // +inf
  f64 low  = -1e400;        // -inf
  f64 tiny = 1e-400;        // underflows to 0.0
  ```
  This is the same "arithmetic gets a defined value" rule that makes integer
  overflow wrap and float-to-int saturate — no trap, no undefined behaviour.
- Converting a float to an integer SATURATES and maps NaN to 0. See
  *Converting a float to an integer* in the SAFETY section.

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
- An EXPLICIT C-style cast converts in both directions and is well-defined —
  what is banned is the *implicit* coercion above, not the conversion:

```zer
u32 main() {
    bool t = true;
    u32 n = 42;
    u32 a = (u32)t;        // 1  — a bool is always exactly 0 or 1
    bool c = (bool)n;      // true — any non-zero becomes true
    bool d = (bool)0;      // false
    if (a != 1) { return 1; }
    if (!c) { return 2; }
    if (d) { return 3; }
    return 0;
}
```

  `(bool)n` is emitted as `!!n`, so a cast can never produce a bool holding a
  value outside {0, 1} — the exhaustive `switch` on a bool stays sound, and
  there is no bool analogue of the enum-forging doors.

---

### Literals

**DESCRIPTION**
Integer literals are decimal, hexadecimal (`0x`), or binary (`0b`); `_` digit
separators are ignored anywhere after the first digit. There is **no octal**:
`0o17` is a syntax error, and a leading zero is just decimal (`017` is
seventeen, not fifteen as in C). A bare integer literal is `u32`; a literal too
big for 32 bits (`0x1_0000_0000`) initialises a `u64` fine.

A **character literal** `'A'` is a `u8`. It widens to `u32` / `u64`
implicitly; it does not convert to a signed type (`i8 c = 'A';` is an error).
Escapes, shared with string literals: `\n \t \r \\ \0 \xHH`, plus `\'` in a
character and `\"` in a string. Any other escape is a compile error (`'\q'` is
"invalid escape sequence in character literal"; in a string, "invalid escape
sequence in string").

**EXAMPLE**
```zer
u32 main() {
    u32 dec  = 1_000_000;        // decimal, underscores ignored
    u32 hex  = 0x4002_0014;      // hexadecimal
    u32 bin  = 0b1010_0101;      // binary
    u32 lead = 017;              // DECIMAL 17 — there is no octal
    u64 big  = 0x1_0000_0000;    // wider than 32 bits: fine in a u64
    u8  ch   = 'A';              // character literal: a u8 (65)
    u32 wide = 'A';              // widens implicitly
    u8  nl   = '\n';             // escapes: \n \t \r \\ \' \0 \xHH
    u8  quote = '\'';
    u8  byte = '\x7F';
    const [*]u8 s = "tab\there\x21\\\"\n";   // string escapes: \n \t \r \\ \" \0 \xHH

    if (dec != 1000000)    { return 1; }
    if (hex != 0x40020014) { return 2; }
    if (bin != 165)        { return 3; }
    if (lead != 17)        { return 4; }
    if (big != 4294967296) { return 5; }
    if (ch != 65 || wide != 65) { return 6; }
    if (nl != 10)          { return 7; }
    if (quote != 39)       { return 8; }
    if (byte != 127)       { return 9; }
    if (s.len != 12)       { return 10; }
    if (s[8] != 33)        { return 11; }   // '\x21' is '!'
    return 0;
}
```

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
Every index access is bounds-checked. Compile-time constant indices, and indices
whose range the compiler can prove, are checked at compile time.

An index the compiler cannot prove gets an **auto-guard**, and the auto-guard's
runtime form is an early `return` of the function's zero value — **not** a trap.
That is a real behavioural difference from `[*]T`, which traps:

| container | unprovable index, out of range at run time |
|---|---|
| `T[N]` (fixed array) | auto-guard: the enclosing function returns its zero value, silently |
| `[*]T` (slice) | `_zer_trap` — `SIGTRAP`, with the file and line |

So a fixed-array access that goes out of range does not crash: it abandons the
rest of the function and hands the caller a zero. Inside `@critical` or a held
lock the guard traps instead (an early return would leak the interrupt-disable or
the mutex) — see "SAFETY RULES YOU WILL HIT". If you want the loud behaviour
everywhere, index a slice, or write the explicit `if (i >= N) { ... }`, which
also removes the guard entirely. A counted loop whose counter PROVABLY runs past
the end is a compile error, not a guard — see "Bounds: four verdicts".

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
scores[i] = 50;           // auto-guard — if i >= 4 the function returns early

// Range propagation: proven-safe indices have ZERO overhead
for (u32 j = 0; j < 4; j += 1) {
    scores[j] = j;        // proven j in [0,3] — no bounds check emitted
}

// Inline call range: function return range proves index safe
u32 hash(u32 key) { return key % 4; }
scores[hash(42)] = 10;    // hash returns [0,3] — no bounds check, zero overhead
```

**FIELDS — READ-ONLY**
`.len` → usize — Array length (compile-time constant)

It is a constant, so it is not an assignment target: `a.len = n` and `&a.len` are
compile errors.

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

**FIELDS — READ-ONLY**
- `.ptr` → *T — Raw pointer to first element
- `.len` → usize — Number of elements

Both are readable and neither is writable. `s.len = n`, `s.len += n`, `s.ptr = p` and
`&s.len` / `&s.ptr` are compile errors, at every depth and through every route (a struct
field, an array element, a global, a pointer auto-deref, a value unwrapped from an
`orelse`). The header is what makes bounds checking mean anything — every index emits a
check against `.len` — so a writable header forges the bound: `s.len = 100; s[50] = 1;`
would pass the check and write past the storage.

Build a view by SLICING (`a[i..j]`), which produces a header the compiler can vouch for.
A user struct may still have fields named `ptr` or `len`; the rule applies to slices and
arrays, not to the names.

**SUB-SLICING**
```zer
buf[0..3]                  // elements 0,1,2 (exclusive end)
buf[2..]                   // element 2 through end
buf[..5]                   // elements 0-4
```

**ANY ELEMENT TYPE**
The element may be anything a variable can have: a primitive, `uN`, an enum, a
struct, a union, a `Handle(T)`, a pointer (`[*]?*T`), a value optional
(`[*]?u32`), a function pointer, or another `[*]T`. Each element type gets one
named C typedef, so two views of the same element type are the same C type and
may be assigned, passed and returned freely. (Before BUG-1027 every element kind
past primitive/struct/union was named `_zer_slice_u128` and read with a 16-byte
stride — an enum slice returned the wrong element.)

```zer
enum State { idle, run }
struct Job { u32 id; }
typedef u32 (*Op)(u32);
u32 dbl(u32 x) { return x * 2; }

u32 count_run([*]State s) {
    u32 n = 0;
    for (u32 i = 0; i < s.len; i += 1) { if (s[i] == State.run) { n += 1; } }
    return n;
}

u32 main() {
    State[3] st;
    st[1] = State.run;
    st[2] = State.run;
    if (count_run(st) != 2) { return 1; }        // T[N] -> [*]T for an enum element
    Job a; Job b;
    a.id = 5; b.id = 7;
    ?*Job[2] ptrs;
    ptrs[0] = &a; ptrs[1] = &b;
    [*]?*Job view = ptrs;                       // pointer elements
    if (view[1]) |t| { if (t.id != 7) { return 2; } } else { return 3; }
    ?u32[2] maybe;
    maybe[0] = 9;
    [*]?u32 mv = maybe;                          // value-optional elements
    u32 got = mv[0] orelse 0;
    if (got != 9) { return 4; }
    Op[2] ops;
    ops[0] = dbl; ops[1] = dbl;
    [*]Op fns = ops;                             // function-pointer elements
    if (fns[1](4) != 8) { return 5; }            // indexed call is bounds-checked
    return 0;
}
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
- A value optional cannot be COMPARED directly — `opt == 5`, `opt < n`,
  `opt == other_opt` are all compile errors. Only `opt == null` / `opt != null`
  is allowed. The reason is that a comparison would read the payload without
  consulting `has_value`, and auto-zero makes an absent `.value` equal to 0, so
  `if (x == 0)` would pass silently on a value that is not there. Unwrap first:
  `if (x) |v| { … v == 5 … }` or `(x orelse 0) == 5`. The parentheses are
  REQUIRED: `orelse` binds loosest of all operators, so `x orelse 0 == 5` parses
  as `x orelse (0 == 5)` and is rejected ("orelse fallback type 'bool' doesn't
  match 'u32'"). A NULL-SENTINEL optional (`?*T`, `?FuncPtr`) IS the pointer at
  runtime and compares normally.

```zer
u32 main() {
    ?u32 x = 5;
    if ((x orelse 0) == 5) { return 0; }   // parentheses required: orelse binds loosest
    return 1;
}
```
- `??T` (an optional of an optional) is a compile error, and so is an optional of a
  builtin CONTAINER — `?Arena`, `?Pool(T, N)`, `?Slab(T)`, `?Ring(T, N)`, `?Barrier`,
  `?Semaphore(N)` — a container is a unique resource addressed by name, not a value
  (declare the container itself, or hold a `*Barrier` / `*Semaphore`). `?T[N]` is an
  ARRAY of optionals and `?Handle(T)` / `?[*]T` / `?*opaque` are all fine.

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

**EXAMPLE**
```zer
struct Task {
    u32 id;
    const [*]u8 name;          // const: a string literal is read-only
    u32 priority;
    ?*Task next;
}

u32 main() {
    Task t;                    // no 'struct' prefix (unlike C)
    t.id = 42;
    t.name = "worker";
    t.priority = 3;
    t.next = null;
    if (t.name.len != 6) { return 1; }
    return 0;
}
```

**NOTES**
- No semicolon after closing `}` (unlike C).
- A string literal is read-only data, so a field that holds one must be
  `const [*]u8`; with a plain `[*]u8 name` field, `t.name = "worker"` is
  rejected ("string literal is read-only — use 'const [*]u8' for string storage").
- Pool/Slab/Ring/Arena cannot be struct fields or union variants ("Pool/Ring/Slab/Arena
  cannot be struct fields — must be global or static variables"). Declare the
  allocator as a global and keep the data in the struct.
- A struct may reference ITSELF through a pointer — `struct Node { u32 v; ?*Node next; }`
  — and may reference any struct declared EARLIER in the file. It cannot reference one
  declared LATER: there is no forward declaration, so a mutually-referencing pair
  (`A` holds `?*B`, `B` holds `?*A`) is not expressible with plain structs. Use
  `container` for that shape (containers bind their field types at instantiation, so
  `container A(T) { ?*B(T) x; } container B(T) { ?*A(T) y; }` compiles), or merge the two
  into one struct with a variant tag. Tracked in `docs/limitations.md`.

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
u32 add(u32 a, u32 b) { return a + b; }
void on_evt(u32 e) { }
?u32 look(u32 k) { return k; }

void (*g_callback)(u32 event) = on_evt;    // global — MUST be initialized
?void (*g_optional)(u32) = null;           // optional — null = not set
struct Ops { u32 (*compute)(u32); }        // struct field
u32 apply(u32 (*op)(u32, u32), u32 x, u32 y);  // parameter
typedef u32 (*BinOp)(u32, u32);            // typedef
typedef ?u32 (*OptHandler)(u32);           // typedef — returns ?u32
BinOp[4] g_ops;                            // array of function pointers (via typedef)

i32 main() {
    u32 (*fn)(u32, u32) = add;             // local variable
    return (i32)fn(1, 2) - 3;
}
```

**SYNTAX — Variant 2C (ZER-native, typedef-free)**
```zer
u32 add2(u32 a, u32 b) { return a + b; }
void on_evt2(u32 e) { }
?u32 look2(u32 k) { return k; }

*(u32 event) g_cb2 = on_evt2;               // global; void return = no arrow
                                            // (MUST be initialized — see below)
?*(u32) -> u32 g_opt2 = null;               // nullable funcptr
*(u32) -> ?u32 g_look2 = look2;             // funcptr returning optional
struct Ops2 { *(u32) -> u32 compute; }      // struct field
u32 apply2(*(u32, u32) -> u32 op, u32 x, u32 y);  // parameter

// Cases that REQUIRED typedef in 2A — now inline in 2C:
?*(u32, u32) -> u32 [4] g_ops2;              // array of funcptrs — INLINE
*(u32, u32) -> u32 select_op(u32 kind);      // return-of-funcptr — INLINE

i32 main() {
    *(u32, u32) -> u32 fn = add2;           // local variable
    return (i32)fn(1, 2) - 3;
}
```

**ARRAYS OF FUNCTION POINTERS** work in both syntaxes, and the element type
should be NULLABLE:

```zer
u32 add(u32 a, u32 b) { return a + b; }
u32 mul(u32 a, u32 b) { return a * b; }
typedef u32 (*BinOp)(u32, u32);

u32 main() {
    ?*(u32, u32) -> u32 [3] ops;    // 2C, typedef-free
    ops[0] = add;
    ops[1] = mul;
    u32 acc = 0;
    for (u32 i = 0; i < 3; i += 1) {
        if (ops[i]) |f| { acc += f(6, 3); }   // unwrap per element; ops[2] is null
    }
    if (acc != 9 + 18) { return 1; }        // add(6,3) + mul(6,3)

    ?BinOp[2] td;                   // 2A, via typedef — same shape
    td[0] = add;
    if (td[0]) |f| { if (f(1, 2) != 3) { return 2; } }
    return 0;
}
```

A NON-nullable element type (`BinOp[3]`, `*(u32) -> u32 [3]`) is accepted but
should be avoided: auto-zero fills the array with NULL and ZER has no array
initializer, so every element starts out holding the one value its type forbids.
Assign every element before any use, or — better — use the `?` element type
above, where the unwrap makes the not-yet-registered case explicit.

Calling an element that was never assigned is **caught at run time** (BUG-1019):
every indirect call through a function pointer carries a null guard and traps
with `call through a null function pointer`. The guard is compiled in, so it
fires on bare metal too — where a raw jump through address 0 would otherwise land
on the reset vector or ordinary memory with nothing to notice it. The same guard
covers a struct field of funcptr type, and a plain local that was assigned from
either carrier. `__typeof__` is used to hoist the callee, so a side-effecting
callee expression such as `table[next()](a, b)` still evaluates exactly once. A
DIRECT call by name is never guarded.

This is a runtime check, not a compile-time proof: prefer `?BinOp[3]` with an
`if (ops[i]) |f|` unwrap, which turns the same question into a compile-time one.

`keep` on a funcptr parameter is written `*(keep *Handler)` — see the `keep`
section. An INDIRECT call cannot see its target, so ZER worst-cases every
reference-carrying parameter of a funcptr as `keep`: passing a stack pointer to
a callback is rejected whether the pointer is bare (`*T`), optional (`?*T`), or
a field of a by-value struct.

Discriminator: `*` BEFORE `(` is 2C; `*` INSIDE `(` is 2A. Both produce identical AST/types, so all downstream behavior (checker, IR, emitter, all 29 safety systems) is operator-agnostic — pick by readability.

**A NON-OPTIONAL FUNCTION POINTER REQUIRES AN INITIALIZER**, at global scope as
well as local. Auto-zero would otherwise leave it NULL and the call would be a
raw jump through address 0. For the register-a-callback-later idiom — which is
what a bare global funcptr usually means — use the optional and unwrap it:

```zer
?void (*saved_cb)(u32 val) = null;         // registered later

void register_cb(void (*cb)(u32 val)) { saved_cb = cb; }

void fire(u32 v) {
    if (saved_cb) |cb| { cb(v); }          // called only once registered
}
```

(The global sibling of this rule was missing until BUG-866: the local check had
grown a funcptr arm and the global one never did, so the same declaration was
accepted at file scope and rejected one scope deeper.)

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

A `const` integer is usable **anywhere a compile-time constant is required** —
array sizes, `Pool` / `Ring` counts, and arithmetic over those. A `const` slice
initialised from a string literal is the way to declare a global string.

**SYNTAX**
```zer
const u32 MAX = 100;
const [*]u8 NAME = "ZER";    // in .rodata (flash on embedded)
```

**EXAMPLE**
```zer
const u32 RX_CAP = 64;
const u32 SLOTS  = 8;

struct Frame { u32 len; }

u8[RX_CAP] rx_buffer;          // const as an array size
u8[RX_CAP * 2] scratch;        // arithmetic over consts folds too
Pool(Frame, SLOTS) frames;     // and as an allocator count
Ring(Frame, SLOTS) inbox;

const [*]u8 BANNER = "ready\n";

i32 main() {
    u8[RX_CAP] local;
    local[0] = 1;
    rx_buffer[RX_CAP - 1] = 2;
    if (@size(u8[RX_CAP]) != 64) { return 1; }
    if (BANNER.len != 6) { return 2; }
    return 0;
}
```

**NOTES**
- The bound is still checked against the *resolved* size: with `const u32 N = 4`,
  `u8[N] b; b[9] = 1;` is a compile error, not a runtime trap.
- A **mutable** global is not a constant. `u32 N = 4; u8[N] b;` is rejected —
  ZER has no variable-length arrays.
- A `const` global is emitted as a C `const` object (read-only data — flash on
  embedded), and its value is FOLDED wherever a constant is needed, so a const may
  initialise another global and derive from other consts:
  `const u32 A = 5; const u32 B = A + 1; u32 G = B;` is fine (`G` starts at 6).
- A string literal is read-only. `[*]u8 S = "x";` is rejected at global and
  local scope alike; declare it `const [*]u8`.

**SEE ALSO**
comptime, static, Pool, Ring

---

### Global variable initializers

**DESCRIPTION**
A global's initializer must be a **compile-time constant**, because it becomes a
C file-scope initializer. This is checked in ZER terms, at the ZER line, over the
*whole* initializer expression — not just its outermost node.

**EXAMPLE**
```zer
const u32 BASE = 0x10;
const u32 NEXT = BASE + 1;        // a const may initialise another global: folded to 17

u32 A = 5;                         // literal
u32 B = BASE;                      // OK — folded to 16
u32 C = @truncate(u32, 300);       // native-width @truncate folds
u32 D = @popcount(0xF0);           // bit query with a constant argument
usize E = @size(u32) * 4;          // @size arithmetic
u32[NEXT] table;                   // and as an array size

u32 main() {
    if (B != 16 || NEXT != 17 || D != 4 || E != 16 || table.len != 17) { return 1; }
    return 0;
}
```

**NOTES**
- Rejected anywhere in the initializer, not only at the top: a **function call**
  (`u32 G = f() + 1;`), and any intrinsic that does not lower to a constant —
  every `@atomic_*`, the `@cpu_*` / `@port_in*` / `@mmu_*` / `@tlb_*` / `@cache_*`
  runtime reads, `@probe`, `@expect`, `@saturate`, `@bitcast`, `@addc` / `@subb` /
  `@mulw`, and `@truncate` to a non-native `uN`/`iN` width.
- An assignment inside an initializer (`u32 G = (x = f());`) is reachable and is
  checked the same way.
- Aggregates cannot be initialised at global scope: there is no array-literal
  syntax, and a designated initializer (`S g = { .x = 1 };`) is a var-decl,
  assignment, call-argument and return form only. Assign the fields from an init
  function instead.

**SEE ALSO**
const, comptime, @size

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
- An arm body may be a single expression terminated by a comma instead of a
  `{ }` block: `0 => note(),`.
- Switching on a `?Enum` / `?Union` with enum-dot arms is allowed; when the
  optional is **null, no arm runs** and control continues after the switch.

```zer
enum Color { red, green }
?Color pick(bool some) { if (some) { return Color.green; } return null; }
u32 hits;
void note() { hits += 1; }

u32 main() {
    u32 code = 2;
    u32 r = 0;
    // An arm body may be a single expression followed by a comma instead of a block.
    switch (code) {
        0 => note(),
        1, 2 => r = 7,
        default => { r = 99; }
    }
    if (r != 7) { return 1; }

    // Switching on an optional enum: when the optional is null, NO arm runs.
    switch (pick(false)) {
        .red   => { return 2; }
        .green => { return 3; }
    }
    switch (pick(true)) {
        .red   => { return 4; }
        .green => { r = 0; }
    }
    return r;
}
```

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

A forward `goto` may jump **over** a later `defer`. The defer is *armed* only
when its registration actually executes, so on the path that skips it the
defer never fires — no cleanup runs for an acquire that never happened:

```zer
u32 lock_count;
void acq() { lock_count += 1; }
void rel() { lock_count -= 1; }

void f(u32 err) {
    if (err == 1) { goto done; }   // jumps over the registration below
    acq();
    defer rel();                   // armed only on the path that reaches it
    lock_count += 100;
done:
    return;
}

u32 main() {
    f(1);                          // defer skipped -> never fires -> no stray rel()
    if (lock_count != 0) { return 1; }
    f(0);                          // defer registered -> fires at scope exit
    if (lock_count != 100) { return 2; }
    return 0;
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
- Labels are function-scoped — cannot goto between functions. There is no fixed
  limit on the number of labels in a function.
- A `goto` fires the defers of every scope it LEAVES, in LIFO order — the same as
  `return` / `break` / `continue`. A backward goto that stays inside the scope of
  an armed defer does not fire it; the defer still runs once, at scope exit.
- Labels work inside switch arms. A label inside a `defer` body is a compile
  error ("cannot place a label inside a defer body") — goto is banned there, so it
  could never be a jump target.
- A label inside a `@critical` or `@once` block is also a compile error. `goto` is
  banned inside both, so the only jump that could reach such a label comes from
  OUTSIDE — it would enter the section without disabling interrupts, or run the
  `@once` body again without its one-time guard.
- Backward goto is just a loop — same as `while(true)` with condition.

```zer
u32 rels;
void rel() { rels += 1; }

u32 leave(u32 k) {
    {
        defer rel();
        if (k == 1) { goto out; }   // LEAVES the inner scope: fires rel() first
    }
    rels += 10;
out:
    return rels;
}

u32 retry() {
    u32 n = 0;
    defer rel();
again:
    n += 1;
    if (n < 3) { goto again; }      // stays inside the defer's scope: does not fire it
    return n;                       // fires rel() once, here
}

u32 main() {
    if (leave(1) != 1) { return 1; }
    rels = 0;
    if (leave(0) != 11) { return 2; }
    rels = 0;
    if (retry() != 3 || rels != 1) { return 3; }
    return 0;
}
```

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

**NOTES**
- `|val|` is a COPY of the payload. `|*val|` is a pointer INTO the optional the
  condition names, so a write through it changes that optional in place —
  whether it is a local, a global, a struct field (also through a pointer
  parameter), or an array element (the index is evaluated once).
- `|*val|` on an optional that is a field of a `packed struct` is a compile
  error ("mutable capture '|*v|' of a packed struct field would be a misaligned
  pointer"): capture by value and assign the field back.

```zer
struct S { ?u32 w; }
struct P { u32 x; }
?u32 g;
?P gs;

void set(*S p) { if (p.w) |*v| { *v = 3; } }   // writes through the pointer

u32 main() {
    ?u32[2] arr;
    arr[1] = 5;
    if (arr[1]) |*v| { *v = 4; }     // array element, in place
    S s;
    s.w = 5;
    set(&s);
    g = 5;
    if (g) |*v| { *v = 6; }          // global, in place
    P one = { .x = 1 };
    gs = one;
    if (gs) |*v| { v.x = 8; }        // a field of the payload
    u32 a = arr[1] orelse 99;
    u32 b = s.w orelse 99;
    u32 d = g orelse 99;
    P dflt = { .x = 99 };
    P pe = gs orelse dflt;
    if (a != 4 || b != 3 || d != 6 || pe.x != 8) { return 1; }
    return 0;
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
- Neither may run inside an `interrupt` handler or a `@critical` block — the
  libc heap lock may deadlock there — and the ban is TRANSITIVE: a helper that
  allocates or frees cannot be called from either context (BUG-1036: the
  `alloc(T, n)` / `free(slice)` forms were missed through a helper while the
  direct spelling and the `alloc(T)` form were caught).

<!-- audit: expect-error: cannot allocate inside interrupt handler -->
```zer
void helper() { ?[*]u8 b = alloc(u8, 16); if (b) |bb| { free(bb); } }
interrupt IRQ1 { helper(); }        // COMPILE ERROR — alloc reachable from an ISR
u32 main() { return 0; }
```

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
- A callee may free an **optional** parameter, or an optional field of a parameter,
  by unwrapping it: `void drop(?*T p) { *T q = p orelse return; free(q); }`. The
  caller sees that free — `drop(mp)` discharges `mp`, and a later unwrap of `mp`
  or a second free is an error. The `orelse return` path is the null path: nothing
  was there to free. The capture form `if (p) |q| { free(q); }` counts too — its
  else path is the null path. A free under an unrelated branch
  (`if (c) { free(q); }`) is still "may not be freed on all paths".
- After a free, `p = null;` / `h.p = null;` is a RESET of the variable or slot, not a
  use of the freed pointee. Re-unwrapping it after the reset is still refused.

**AN ALLOCATION HELD IN A GLOBAL**
A global that receives an allocation is tracked exactly like a local: a use
after `free(g)`, a second `free(g)`, and a read through a local ALIAS of it
(`g = s; free(g); s[0]`) are all compile errors. A global must also not be left
DANGLING when the function that freed it returns — reset it with `g = null;`
after the free. Only an OPTIONAL global can hold `null`, so a global that owns
an allocation should be declared `?*T` / `?[*]T`; for a non-optional one the
error says so ("a non-optional '[*]u32' global cannot be reset; declare it
'?[*]u32 g' …" — the diagnostic currently spells the type with the older `[]`).

```zer
struct W { u32 v; }
?[*]u32 gs;                        // optional, so it can be reset
?*W gp;

void init() { gs = alloc(u32, 4) orelse return; }
u32 fini() {
    [*]u32 s = gs orelse return;
    s[0] = 5;
    u32 v = s[0];
    free(s);
    gs = null;                     // reset: the global no longer dangles
    return v;
}

u32 main() {
    init();
    if (fini() != 5) { return 1; }
    gp = alloc(W);
    *W q = gp orelse return;
    q.v = 7;
    u32 r = q.v;
    free(q);
    gp = null;
    return r - 7;
}
```

<!-- audit: expect-error: use after free: 'g' is freed -->
```zer
[*]u32 g;
u32 run() {
    g = alloc(u32, 4) orelse return;
    free(g);
    return g[0];                 // ERROR — use after free through a global
}
u32 main() { return run(); }
```

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

A RECYCLED slot is zeroed before it is handed back, so the auto-zero guarantee
holds for the second and every later allocation of a slot, not only the first.
(It did not until BUG-861: a fresh page is calloc-clean, so the divergence only
appeared after the first free/alloc cycle — and a `?*T` field came back non-null
and dangling, which safe ZER could unwrap and dereference.) `Slab` behaves
identically, and `Arena.alloc` always has.

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
- `const Handle(Task)` is a const KEY, not const data: `h.id = 42` through a const
  Handle compiles (like writing through a `const int fd`). See Handle(T).

```zer
struct Task { u32 id; }
Slab(Task) heap;
u32 main() {
    Handle(Task) h0 = heap.alloc() orelse { return 1; };
    const Handle(Task) h = h0;
    h.id = 42;                 // OK — const KEY, not const data
    u32 v = h.id;
    heap.free(h0);
    return v - 42;
}
```

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

Discarding a `push_checked` result is a compile error: reporting overflow is its
entire purpose, so throwing the report away makes it identical to `push`. Write
`rb.push_checked(x) orelse { ... };`, or call `push` if dropping on overflow is
what you mean. Discarding a `pop()` result is allowed — that is a "drop one".

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
u8[4096] backing;                        // the storage is a global array
Arena g_ar;                              // a global arena is ASSIGNED in a function
u8[4096] g_backing;

u32 main() {
    Arena ar = Arena.over(backing);      // local arena
    g_ar = Arena.over(g_backing);        // global arena
    return 0;
}
```

`Arena.over(...)` is a call, so it cannot appear in a global initializer — declare
the arena at global scope and assign it inside a function (this is what every
firmware example does).

`over` is a CONSTRUCTOR: it RETURNS an arena, it does not mutate one. Writing
`ar.over(buf);` as a bare statement is a compile error — it would build an arena
into a discarded temporary and leave `ar` at capacity 0, so every later
allocation would return null forever. Allocating from an arena that never
received a backing store anywhere in the program is a compile error too.

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

u32 main() {
    Arena ar = Arena.over(backing);
    defer ar.reset();      // free everything at scope exit

    *Node a = ar.alloc(Node) orelse { return 1; };
    a.id = 1;

    *Node b = ar.alloc(Node) orelse { return 2; };
    b.id = 2;
    a.next = b;            // linking two allocations from the SAME arena is fine

    if (a.next) |n| { if (n.id != 2) { return 3; } }
    return 0;
}
```

**LINKING.** Storing an arena pointer into another object from the SAME arena is
allowed — reaching the stored pointer requires dereferencing the container, and
the container is invalidated by the same `reset()`. Storing one into an object
from a DIFFERENT arena is a compile error, because their lifetimes are not tied
together (a local arena's buffer dies with the frame). Storing one into a
global/static remains a compile error.

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
- Two allocations from the SAME arena may point at each other; from DIFFERENT
  arenas they may not.
- `alloc_slice(T, n)` refuses any request whose byte count `sizeof(T) * n`
  overflows, so a slice can never report a length the arena does not hold.
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

**NOTES**
- The clamp is exact for every source and target width, `u128` / `i128` and
  `uN` / `iN` included. An unsigned source is never compared against a negative
  bound (BUG-1060: `@saturate(i32, s.len)` used to give `-2147483648`, because C
  made `u64 < -2147483648LL` an unsigned comparison).
- A FLOAT source is the same operation as the `(T)x` cast: NaN becomes 0, and the
  boundaries are exact (`@saturate(i64, 9223372036854775808.0)` is `i64` max, not
  min). See *Converting a float to an integer*.

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
- A CONSTANT address is checked at compile time: outside every range, misaligned,
  or too wide for the target pointer (`--target-bits 32` and `0x1_0000_0010` →
  "@inttoptr address 0x100000010 does not fit in a 32-bit pointer") is an error.
- A VARIABLE address is checked at run time, in full 64-bit width before it is
  narrowed to a pointer: it traps if it does not fit in a pointer, and traps if
  the whole access (`sizeof(T)` bytes from the address) is not inside one declared
  range. The address operand must be at most 64 bits wide — a `u128` is rejected
  ("narrow it explicitly with @truncate").
- `--no-strict-mmio` flag allows @inttoptr without mmio declarations —
  it relaxes the RANGE strictness only. The runtime ALIGNMENT trap is
  still emitted for variable addresses (alignment is a property of the
  target pointer type, not of mmio declarations), and constant
  addresses are alignment-checked at compile time regardless.
- For tests: `mmio 0x0..0xFFFFFFFFFFFFFFFF;` (allow all addresses).

<!-- audit: expect-trap: @inttoptr: address outside mmio range -->
```zer
mmio 0x40000000..0x400000FF;
u64 wild() { return 0xFFFFFFFFFFFFFFFE; }
u32 main() {
    volatile *u32 r = @inttoptr(*u32, wild());   // TRAPS — outside the range (no wrap-around)
    return *r;
}
```

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

**NOTES**
- The integer is TRACKED like the pointer it came from: `@ptrtoint(&local)`
  cannot be stored in a global or through a pointer parameter, and neither can
  any arithmetic or cast on it — `g = a + 0`, `g = 0 - a`, `g = -a`,
  `g = (usize)a` and `g = f(a)` are all refused (BUG-1039 closed the direct
  arithmetic spellings). Store the DATA, not the address.

<!-- audit: expect-error: integer derived from @ptrtoint of a local -->
```zer
usize g;
u32 main() {
    u32 l = 5;
    usize a = @ptrtoint(&l);
    g = 0 - a;               // COMPILE ERROR — a frame address, laundered
    return 0;
}
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

**THE RUNTIME CHECK ONLY EXISTS BETWEEN struct / enum / union TYPES**
Only those three carry a runtime `type_id`. When either pointee is anything
else — `u32`, `f64`, a slice, a funcptr — the emitted check compares against
`0` ("unknown provenance") and can never fire. So `@pun` is checked between
two named aggregate types, and **unchecked** in every other combination. Two
compile-time rules cover what the runtime check cannot:

- **Widening is rejected.** A target pointee LARGER than the source pointee
  reads past the source object:
  ```zer
  struct Big { u64 a; u64 b; }
  u32 small = 7;  *u32 sp = &small;
  *Big bp = @pun(*Big, sp);      // ERROR — 16-byte target, 4-byte source
  ```
- **Forging is rejected.** If the runtime check cannot fire, the pointee types
  differ, and the TARGET carries a value whose validity the rest of the program
  trusts — a pointer, slice, funcptr, enum, `bool`, optional, `Handle`, or any
  struct/array containing one — the pun is refused:
  ```zer
  struct P { *u32 p; }
  u64 addr = 0x1000;  *u64 ap = &addr;
  *P pp = @pun(*P, ap);          // ERROR — would forge a pointer from an integer
  ```
  This is what keeps `@inttoptr` (with its mandatory `mmio` declaration) the
  only way an integer becomes a pointer, and keeps a forged enum out of an
  exhaustive `switch`. Use `@inttoptr` for an address, a `[*]u8` slice to parse
  bytes, or pun between two struct types (which IS runtime-checked).

Reinterpreting bits as plain integers or floats forges nothing — every bit
pattern is a legal `u32` — so those puns still compile in both directions:

```zer
struct A { u32 x; }
A a;  *A pa = &a;
*u8 bytes = @pun(*u8, pa);       // OK — byte view of a struct

u32 raw = 9;  *u32 rp = &raw;
struct Plain { u32 y; }
*Plain pl = @pun(*Plain, rp);    // OK — target carries no invariant
```

**NOTES**
- Compile-time: target must be a pointer, source must be a pointer,
  const stripping rejected, volatile stripping rejected, plus the widening
  and forging rules above.
- Runtime: traps on type_id mismatch via `_zer_trap("@pun type mismatch")` —
  only between struct/enum/union pointees, see above.
- For raw byte access (parsing, serialization), use `[*]u8` slices —
  not pointer casting. Slices are bounds-checked, len-carrying, and the
  right tool for byte-level data.
- For direct `(*T)src` where types match (identity), no `@pun` needed —
  the compiler allows the cast directly with zero overhead.
- A cast is **transparent to lifetime and ownership**. `(*T)p`, `@pun`,
  `@ptrcast`, `@cast`, `@container` and `@cstr` all name the SAME object as
  their source, so the escape, arena, `keep` and use-after-free rules see
  straight through them. Writing a cast to quiet a lifetime error will not
  work — and should not: the emitted C for an identity cast is byte-identical
  to the code without it.

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

**THE POINTER MUST POINT AT A FIELD**
`@container` subtracts the field's offset. The compiler tracks where a pointer
came from and knows three answers, not two:

| where the pointer came from | verdict |
|---|---|
| `&outer.field` — a field of the named struct | OK |
| `&outer.other` / a field of a *different* struct | compile error (wrong field / wrong struct) |
| `&wholeObject`, `&arr[i]` — a complete object that is nobody's field | compile error |
| a parameter, a `cinclude` pointer — unknown | allowed (cannot be proven wrong) |

```zer
struct Inner { u32 a; }
struct Outer { u64 pad; Inner in; }

Inner i;
*Inner ip = &i;
*Outer o = @container(*Outer, ip, in);   // ERROR — `i` is nobody's field;
                                         // this would read BEFORE the object
```

The whole-object case is rejected because subtracting the field offset from the
address of a complete object produces a pointer *before* it — every read through
it is out of bounds. The fact rides through aliases (`*Inner b = a;`) and is
cleared when the pointer is re-pointed at a real field, so the correct program
still compiles. An element of an array (`&arr[i]`) is a complete object too, and
is rejected for the same reason.

---

### @size(T)

**DESCRIPTION**
Returns the size of type T in bytes as usize. Like C's sizeof.

**EXAMPLE**
```zer
usize s = @size(Task);     // e.g., 12
```

**NOTES**
- The operand may be a type name, a `uN`/`iN` spelling, or a VARIABLE — for a
  variable it is the size of the variable's type (BUG-1038: `@size(x)` used to
  be spelled `sizeof(struct x)` and `@size(u21)` was an undefined identifier).
  A `uN` reports its CARRIER: `@size(u21)` is 4, `@size(u3)` is 1.

```zer
struct Job { u32 id; u64 pad; }
u32 main() {
    u21 x = 5;
    Job t;
    usize a = @size(Job);    // 16
    usize b = @size(t);      // 16 — a variable: the size of its type
    usize c = @size(u21);    // 4  — the carrier of a uN
    usize d = @size(x);      // 4
    if (a != b || c != 4 || d != 4) { return 1; }
    return 0;
}
```

---

### @offset(T, field)

**DESCRIPTION**
Returns the byte offset of a field within struct T as usize. Like C's offsetof.

**EXAMPLE**
```zer
usize off = @offset(Task, priority);
```

**NOTES**
- The first argument must name a STRUCT — including through a `distinct
  typedef` of one. A non-struct type is rejected.
- A UNION is rejected with its own reason: ZER unions are TAGGED, so a variant
  has no fixed offset. Take the offset of the struct field that holds the union.
- The second argument must be a plain field NAME, not an expression.

---

### @trap()

**DESCRIPTION**
Intentional crash. Calls the ZER trap handler with a message.

**EXAMPLE**
```zer
if (should_never_happen) { @trap(); }
```

---

### @try_enum(EnumType, value)

**DESCRIPTION**
Checked integer-to-enum conversion. Returns `?EnumType` — the enum value if the
integer is a declared variant, `null` if it is not.

ZER has no implicit integer-to-enum conversion: `Color c = 99;` is a compile error,
and so is arithmetic on enum operands, because both can produce a value outside the
declared variant set. `@try_enum` is the route when the integer comes from outside
the program (a hardware register, a decoded message) and may legitimately hold a
reserved encoding — the failure lands in the type and is handled with the same
`orelse` / `if |v|` control flow as any other optional.

Use `@bitcast(EnumType, value)` instead when the value is certain to be a variant;
that route traps at runtime if it is not.

**EXAMPLE**
```zer
enum Mode { idle, running, halted }

u32 decode(u32 raw) {
    // supply a default for a reserved encoding
    Mode m = @try_enum(Mode, raw) orelse Mode.idle;

    // or branch on it
    if (@try_enum(Mode, raw)) |v| {
        switch (v) {
            .idle => { return 10; }
            .running => { return 11; }
            .halted => { return 12; }
        }
    }
    // reserved encoding — a state this build does not model
    return 90 + (u32)m;
}

u32 main() {
    if (decode(7) != 90) { return 1; }   // 7 is not a variant: the idle default, 90 + 0
    if (decode(1) != 11) { return 2; }   // 1 is Mode.running
    return 0;
}
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

### @config(key, default)

**DESCRIPTION**

Build-configuration hook. Yields `default`, and takes its TYPE from `default`.

**STATUS — read this before using it.** The hook is wired through the checker and both
emitter paths, but there is no way to supply a value: the compiler has no `--config` flag,
so `@config` **always** yields the default today. It is a placeholder for build-time
configuration, not a working feature. `comptime` constants are the mechanism that works.

Both arguments are ordinary expressions and both are type-checked, so `key` must be a
literal or a declared identifier — a bare undeclared name is an "undefined identifier"
error, not a key.

```zer
u32 main() {
    u32 baud = @config("baud", 9600);   // yields 9600
    if (baud != 9600) { return 1; }
    return 0;
}
```

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
Bit query operations. All return `u32`, and all are TOTAL — every input has a
defined answer, `0` included.
- `@popcount(x)` — count 1-bits
- `@ctz(x)` — count trailing zeros; **`x = 0` gives the operand's WIDTH** (32 or 64)
- `@clz(x)` — count leading zeros; **`x = 0` gives the operand's WIDTH** (32 or 64)
- `@parity(x)` — 0=even / 1=odd
- `@ffs(x)` — position of lowest 1-bit, 1-indexed; `x = 0` gives 0

Input must be integer. Width-dispatched (ll suffix for 64-bit inputs). There are
only TWO widths: a `u8` / `u16` operand is widened and counted at 32 bits, so
`@clz((u8)0x80)` is 24, not 0 — shift it up first (`@clz((u32)b << 24)`) when you
want the count within the narrow width. `@ctz` / `@popcount` / `@parity` / `@ffs`
are unaffected by the widening.
Emits GCC `__builtin_*` / `__builtin_*ll` — but NOT bare: `__builtin_ctz(0)` and
`__builtin_clz(0)` are undefined in C, so ZER emits the zero test alongside
(`(x) == 0 ? 32 : __builtin_ctz(x)`). This entry used to say "UB if x=0",
describing the C builtin rather than what ZER emits; no guard of your own is
needed, and ZER has no undefined behavior here.

```zer
volatile u32 zero = 0;
volatile u64 wide = 0;

u32 main() {
    if (@ctz(zero) != 32) { return 1; }     // width, not UB
    if (@clz(zero) != 32) { return 2; }
    if (@ffs(zero) != 0)  { return 3; }
    if (@ctz(wide) != 64) { return 4; }     // 64-bit operand -> 64
    if (@popcount(zero) != 0) { return 5; }
    u8 b = 0x80;
    if (@clz(b) != 24) { return 6; }        // a u8 is counted at 32 bits
    if (@clz((u32)b << 24) != 0) { return 7; }
    if (@ctz(b) != 7) { return 8; }
    return 0;
}
```

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

### @port_in8(port), @port_in16(port), @port_in32(port), @port_out8(port, val), @port_out16(port, val), @port_out32(port, val)

**DESCRIPTION**
x86 port-mapped I/O. `in`/`out` instructions of the matching width.
Privileged on x86 (requires CPL <= IOPL) — SIGSEGV in ordinary user mode.
Non-x86 targets emit a no-op fallback (reads yield 0).

**SIGNATURE**
```
@port_in8(u64 port)  -> u8      @port_out8(u64 port, u64 val)  -> void
@port_in16(u64 port) -> u16     @port_out16(u64 port, u64 val) -> void
@port_in32(u64 port) -> u32     @port_out32(u64 port, u64 val) -> void
```

**EXAMPLE**
```zer
volatile u32 uart_ready;

void serial_write(u8 ch) {
    // 0x3F8 is the legacy COM1 data port.
    @port_out8(0x3F8, ch);
}

u8 serial_read() {
    return @port_in8(0x3F8);
}
```

**NOTES**
- The port argument is typed `u64` and truncated to 16 bits on emission;
  x86 port numbers are 16-bit, so a value above `0xFFFF` silently wraps.
- Kernel/bootloader code only. In user mode these fault.

**SEE ALSO**
@inttoptr, mmio

---

### @cpu_read_msr(msr), @cpu_write_msr(msr, val)

**DESCRIPTION**
x86 Model-Specific Register access (`rdmsr` / `wrmsr`). Privileged.
Non-x86 targets read 0 and ignore writes.

**SIGNATURE**
```
@cpu_read_msr(u64 msr) -> u64
@cpu_write_msr(u64 msr, u64 val) -> void
```

**EXAMPLE**
```zer
volatile u32 arm;

void enable_syscall() {
    if (arm == 0) { return; }        // guard: privileged, kernel only
    u64 efer = @cpu_read_msr(0xC0000080);
    @cpu_write_msr(0xC0000080, efer | 1);
}
```

**NOTES**
- Common MSRs: `0xC0000080` EFER, `0xC0000100` FS_BASE, `0xC0000101` GS_BASE.
- Prefer `@cpu_read_fsbase` / `@cpu_write_gsbase` where they apply — they use
  the FSGSBASE instructions and do not need CPL 0.

---

### @cpu_read_cr0(), @cpu_write_cr0(v), @cpu_read_cr2(), @cpu_read_cr3(), @cpu_write_cr3(v), @cpu_read_cr4(), @cpu_write_cr4(v)

**DESCRIPTION**
x86 control-register access. All privileged. `CR2` is read-only (it holds the
faulting linear address after a page fault), so there is no `@cpu_write_cr2`.
Non-x86 targets read 0 and ignore writes.

**SIGNATURE**
```
@cpu_read_cr0() -> u64      @cpu_write_cr0(u64 v) -> void
@cpu_read_cr2() -> u64      (read only — page-fault address)
@cpu_read_cr3() -> u64      @cpu_write_cr3(u64 v) -> void
@cpu_read_cr4() -> u64      @cpu_write_cr4(u64 v) -> void
```

**EXAMPLE**
```zer
volatile u32 arm;

u64 page_fault_address() {
    if (arm == 0) { return 0; }
    return @cpu_read_cr2();
}

void switch_address_space(u64 pml4_phys) {
    if (arm == 0) { return; }
    @cpu_write_cr3(pml4_phys);        // also flushes non-global TLB entries
}
```

**NOTES**
- `@cpu_write_cr3` implicitly flushes non-global TLB entries.
- CR0 bit 16 is WP, bit 31 is PG; CR4 bit 7 is PGE, bit 20 is SMEP.

---

### @cpu_read_xcr0(), @cpu_write_xcr0(v), @cpu_read_dr(idx), @cpu_write_dr(idx, v), @cpu_read_pmc(idx)

**DESCRIPTION**
XSAVE feature mask (`xgetbv`/`xsetbv`), debug registers DR0-DR3/DR6/DR7, and
the performance counter read (`rdpmc`). All x86; all privileged except
`@cpu_read_pmc`, which needs `CR4.PCE = 1` to be usable from user mode.

**SIGNATURE**
```
@cpu_read_xcr0() -> u64          @cpu_write_xcr0(u64 v) -> void
@cpu_read_dr(u64 idx) -> u64     @cpu_write_dr(u64 idx, u64 v) -> void
@cpu_read_pmc(u64 idx) -> u64
```

**EXAMPLE**
```zer
volatile u32 arm;

void enable_avx() {
    if (arm == 0) { return; }
    u64 mask = @cpu_read_xcr0();
    @cpu_write_xcr0(mask | 7);       // x87 | SSE | AVX state
}

u64 hw_breakpoint0() {
    if (arm == 0) { return 0; }
    return @cpu_read_dr(0);
}
```

**NOTES**
- `idx` for `@cpu_read_dr` / `@cpu_write_dr` is a runtime value dispatched by a
  switch; valid indices are 0-3, 6 and 7.

---

### @cpu_read_fsbase(), @cpu_read_gsbase(), @cpu_write_fsbase(v), @cpu_write_gsbase(v)

**DESCRIPTION**
x86-64 FS/GS segment-base access via the FSGSBASE instructions.
Requires `CR4.FSGSBASE = 1`; with that bit set they run at any privilege level.
Non-x86 targets read 0 and ignore writes.

**SIGNATURE**
```
@cpu_read_fsbase() -> u64    @cpu_write_fsbase(u64 v) -> void
@cpu_read_gsbase() -> u64    @cpu_write_gsbase(u64 v) -> void
```

**EXAMPLE**
```zer
volatile u32 arm;

u64 tls_base() {
    if (arm == 0) { return 0; }
    return @cpu_read_fsbase();
}
```

**NOTES**
- `@cpu_read_tp()` is the portable way to read the thread pointer; prefer it
  unless you specifically need the x86 segment base.

---

### @cpu_xsave(buf, mask), @cpu_xrstor(buf, mask), @cpu_fxsave(buf), @cpu_fxrstor(buf), @cpu_fpu_init()

**DESCRIPTION**
Extended processor-state save/restore. `xsave`/`xrstor` take a component mask
(AVX, AVX-512, …); `fxsave`/`fxrstor` are the legacy pre-XSAVE pair.
`@cpu_fpu_init` issues `fninit`.

**SIGNATURE**
```
@cpu_xsave(*u8 buf, u64 mask) -> void     @cpu_xrstor(*u8 buf, u64 mask) -> void
@cpu_fxsave(*u8 buf) -> void              @cpu_fxrstor(*u8 buf) -> void
@cpu_fpu_init() -> void
```

**EXAMPLE**
```zer
volatile u32 arm;

void save_fpu_state() {
    u8[512] area;
    if (arm == 0) { return; }
    @cpu_fxsave(&area[0]);
    @cpu_fxrstor(&area[0]);
}
```

**NOTES**
- `fxsave` needs a 512-byte, 16-byte-aligned buffer; `xsave` needs 64-byte
  alignment and a size that depends on the enabled components.
- The buffer argument accepts `*u8` or `u8[N]`.

---

### @cpu_cpuid(leaf, subleaf), @cpu_cpuid_ecx(leaf, subleaf), @cpu_vendor_id(), @cpu_feature_bits(), @cpu_model_id()

**DESCRIPTION**
x86 CPUID. `@cpu_cpuid` returns `(EBX << 32) | EAX` and `@cpu_cpuid_ecx`
returns `(EDX << 32) | ECX` for the same leaf, so the two together give all
four result registers. The three no-argument forms are fixed-leaf shorthands.
Non-privileged.

**SIGNATURE**
```
@cpu_cpuid(u64 leaf, u64 subleaf) -> u64        // (EBX << 32) | EAX
@cpu_cpuid_ecx(u64 leaf, u64 subleaf) -> u64    // (EDX << 32) | ECX
@cpu_vendor_id()    -> u64      // leaf 0 EBX — first 4 vendor chars
@cpu_feature_bits() -> u64      // leaf 1, ECX:EDX packed
@cpu_model_id()     -> u32      // leaf 1 EAX — family/model/stepping
```

**EXAMPLE**
```zer
i32 main() {
    u64 lo = @cpu_cpuid(1, 0);
    u64 hi = @cpu_cpuid_ecx(1, 0);
    u32 eax = @truncate(u32, lo);
    if (eax == 0 && hi == 0) { return 1; }
    return 0;
}
```

**NOTES**
- Leaf/subleaf are `u64` in ZER and truncated to 32 bits on emission.
- On non-x86 targets every form returns 0.

---

### @cpu_read_sp(), @cpu_read_tp(), @cpu_read_flags(), @cpu_get_pc(), @cpu_read_counter(), @cpu_id(), @cpu_core_id(), @cpu_num_cores(), @cpu_cache_line_size(), @cpu_current_mode(), @cpu_get_priv_level()

**DESCRIPTION**
Non-privileged CPU-state reads. Safe to call from ordinary user code.

**SIGNATURE**
```
@cpu_read_sp()           -> u64   stack pointer
@cpu_read_tp()           -> u64   thread pointer / TLS base
@cpu_read_flags()        -> u64   RFLAGS (x86) / NZCV (ARM)
@cpu_get_pc()            -> u64   current instruction pointer
@cpu_read_counter()      -> u64   cycle/time counter (rdtsc / cntvct_el0 / rdtime)
@cpu_id()                -> u32   current core number
@cpu_core_id()           -> u32   physical core id
@cpu_num_cores()         -> u32   logical core count
@cpu_cache_line_size()   -> u32   L1 line size in bytes
@cpu_current_mode()      -> u32   privilege mode
@cpu_get_priv_level()    -> u32   privilege level (0 = user)
```

**EXAMPLE**
```zer
i32 main() {
    u64 t0 = @cpu_read_counter();
    u64 sp = @cpu_read_sp();
    u32 line = @cpu_cache_line_size();
    if (sp == 0) { return 1; }
    u64 t1 = @cpu_read_counter();
    if (t1 < t0) { return 2; }
    if (line == 0) { return 3; }
    return 0;
}
```

**NOTES**
- `@cpu_core_id`, `@cpu_current_mode`, `@cpu_num_cores` and
  `@cpu_cache_line_size` are stubs on targets where the value is not readable
  without a syscall — they return 0, 0, 1 and 64 respectively. Treat them as
  hints, not facts.
- `@cpu_read_counter` is a free-running counter, not wall-clock time.

---

### @cpu_rdrand(), @cpu_rdseed()

**DESCRIPTION**
Hardware random-number instructions. Both return `?u64` — **null when the
instruction reports failure**, which is a real and expected outcome
(`rdseed` in particular fails under load). Non-privileged.

**SIGNATURE**
```
@cpu_rdrand() -> ?u64
@cpu_rdseed() -> ?u64
```

**EXAMPLE**
```zer
u64 random_or(u64 fallback) {
    return @cpu_rdrand() orelse fallback;
}

i32 main() {
    if (@cpu_rdrand()) |v| { if (v == v) { return 0; } }
    return 0;
}
```

**NOTES**
- The optional is the point: a bare `u64` return would silently hand back a
  non-random value on failure. Unwrap it, and retry or fall back.
- Not available on every x86 part and absent on most non-x86 targets, where the
  result is null.

---

### @cpu_pause(), @cpu_wfe(), @cpu_sev(), @cpu_idle_hint(), @cpu_deep_sleep(), @cpu_monitor_addr(p), @cpu_mwait(), @cpu_umonitor(p), @cpu_umwait(hint, deadline)

**DESCRIPTION**
Spin-wait and idle hints.
`@cpu_pause` is the spin-loop hint (`pause` / `yield` / Zihintpause).
`@cpu_wfe` / `@cpu_sev` are the ARM wait-for-event pair (on x86 `wfe` degrades
to `pause` and `sev` to a full fence).
`@cpu_monitor_addr` + `@cpu_mwait` are the privileged x86 MONITOR/MWAIT pair;
`@cpu_umonitor` + `@cpu_umwait` are their user-mode WAITPKG equivalents.
`@cpu_deep_sleep` enters the deepest idle state and is privileged.

**SIGNATURE**
```
@cpu_pause() -> void          @cpu_idle_hint() -> void
@cpu_wfe()   -> void          @cpu_sev() -> void
@cpu_deep_sleep() -> void
@cpu_monitor_addr(*u8 p) -> void   @cpu_mwait() -> void
@cpu_umonitor(*u8 p) -> void       @cpu_umwait(u64 hint, u64 deadline) -> void
```

**EXAMPLE**
```zer
volatile u32 flag;

void spin_until_set() {
    while (flag == 0) {
        @cpu_pause();
    }
}
```

**NOTES**
- `@cpu_pause` and `@cpu_idle_hint` are non-blocking and safe anywhere.
- MONITOR/MWAIT require the monitored line to be written by another agent;
  they are not a general sleep.
- `@cpu_umwait` hint: 0 = C0.2 (deeper), 1 = C0.1 (faster wake).

---

### @wait_on_address(addr, expected)

**DESCRIPTION**
Spin until the 32-bit word at `addr` differs from `expected`, using the
platform pause hint between polls. This is a **spin**, not a futex — the thread
is never descheduled.

**SIGNATURE**
```
@wait_on_address(*T addr, <integer> expected) -> void
```

**EXAMPLE**
```zer
volatile u32 ready;

void wait_for_ready() {
    @wait_on_address(&ready, 0);   // returns once `ready` becomes non-zero
}
```

**NOTES**
- The load is an acquire load, so a value published by another thread with a
  release store is visible after this returns.
- Burns a core while waiting. For anything longer than a few microseconds use a
  condvar (`@cond_wait`) instead.

---

### @cpu_syscall(), @cpu_sysret(), @cpu_iret(), @cpu_set_priv_stack(sp), @cpu_hypercall(), @cpu_sbi_call(), @cpu_smc_call()

**DESCRIPTION**
Privilege-mode transitions. Every one of these requires the surrounding system
registers to be set up first (CS/RIP/RFLAGS on x86, ELR/SPSR on ARM,
sepc/sstatus on RISC-V); they are the instruction, not the sequence.

**SIGNATURE**
```
@cpu_syscall()  -> void    user -> kernel trap   (syscall / svc #0 / ecall)
@cpu_sysret()   -> void    kernel -> user return (sysretq / eret / sret)
@cpu_iret()     -> void    interrupt return      (iretq / eret / mret)
@cpu_set_priv_stack(u64 sp) -> void   kernel stack for syscall entry
@cpu_hypercall() -> void   guest -> hypervisor   (vmcall / hvc #0 / ecall)
@cpu_sbi_call()  -> void   RISC-V ecall to M-mode firmware
@cpu_smc_call()  -> void   ARM TrustZone smc #0
```

**EXAMPLE**
```zer
volatile u32 arm;

void enter_user_mode(u64 kernel_sp) {
    if (arm == 0) { return; }
    @cpu_set_priv_stack(kernel_sp);
    @cpu_sysret();
}
```

**NOTES**
- All privileged. Calling any of them from user code faults.
- ZER checks the *call*; it cannot check that the system-register context is
  correct — that is hardware-consequence, outside the language boundary.

---

### @cpu_eoi(), @cpu_endbr(), @cpu_breakpoint(), @cpu_flush_pipeline(), @cpu_reset(), @cpu_cache_disable(), @cpu_cache_enable()

**DESCRIPTION**
Interrupt-lifecycle, control-flow-integrity and machine-control primitives.

**SIGNATURE**
```
@cpu_eoi()            -> void   end-of-interrupt to LAPIC / GICv3 (privileged)
@cpu_endbr()          -> void   ENDBR64 landing pad; a multi-byte NOP without CET-IBT
@cpu_breakpoint()     -> void   debug trap (int3 / brk #0 / ebreak)
@cpu_flush_pipeline() -> void   instruction-pipeline flush (mfence+lfence / isb / fence.i)
@cpu_reset()          -> void   halt forever — a real reset is platform-specific
@cpu_cache_disable()  -> void   CR0.CD = 1 plus WBINVD (privileged)
@cpu_cache_enable()   -> void   CR0.CD = 0 (privileged)
```

**EXAMPLE**
```zer
volatile u32 arm;

void isr_epilogue() {
    if (arm == 0) { return; }
    @cpu_eoi();
}

void after_patching_code() {
    @cpu_flush_pipeline();
}
```

**NOTES**
- `@cpu_flush_pipeline` is required after writing instructions you are about to
  execute; a data-cache flush alone is not enough.
- `@cpu_breakpoint` traps unconditionally — it is not a conditional assert.

---

### @mmu_enable(), @mmu_disable(), @mmu_is_enabled(), @mmu_set_pt(v), @mmu_get_pt(), @mmu_set_kernel_pt(v), @mmu_get_kernel_pt(), @mmu_get_fault_addr(), @mmu_get_fault_status(), @mmu_sync()

**DESCRIPTION**
Memory-management-unit control. On x86 the page-table root is CR3 and
`@mmu_enable` sets CR0.PG; on ARM64 they map to `SCTLR_EL1` / `TTBR0_EL1` /
`TTBR1_EL1`; on RISC-V to `satp`. All privileged.

**SIGNATURE**
```
@mmu_enable()  -> void        @mmu_disable() -> void
@mmu_is_enabled() -> bool
@mmu_set_pt(u64 root) -> void          @mmu_get_pt() -> u64
@mmu_set_kernel_pt(u64 root) -> void   @mmu_get_kernel_pt() -> u64
@mmu_get_fault_addr()   -> u64   faulting address (CR2 / FAR_EL1 / stval)
@mmu_get_fault_status() -> u64   fault cause     (ESR_EL1 / scause)
@mmu_sync() -> void              barrier making table writes visible to the walker
```

**EXAMPLE**
```zer
volatile u32 arm;

void install_page_tables(u64 root) {
    if (arm == 0) { return; }
    @mmu_set_pt(root);
    @mmu_sync();
    @tlb_flush_all();
    if (!@mmu_is_enabled()) { @mmu_enable(); }
}

u64 fault_address() {
    if (arm == 0) { return 0; }
    return @mmu_get_fault_addr();
}
```

**NOTES**
- `@mmu_set_kernel_pt` is the ARM64 `TTBR1_EL1` (high-half) root; on x86 there
  is one root, so it aliases `@mmu_set_pt`.
- Always `@mmu_sync()` after writing page-table entries and before relying on
  them — the table walker is not coherent with ordinary stores on every arch.

---

### @tlb_flush_all(), @tlb_flush_global(), @tlb_flush_addr(a), @tlb_flush_asid(a), @tlb_flush_range(start, len)

**DESCRIPTION**
TLB invalidation. Privileged.

**SIGNATURE**
```
@tlb_flush_all()    -> void        non-global entries
@tlb_flush_global() -> void        including global entries
@tlb_flush_addr(u64 addr)  -> void single page
@tlb_flush_asid(u64 asid)  -> void one address-space id
@tlb_flush_range(u64 start, u64 len) -> void
```

**EXAMPLE**
```zer
volatile u32 arm;

void unmap_page(u64 va) {
    if (arm == 0) { return; }
    // ... clear the PTE ...
    @mmu_sync();
    @tlb_flush_addr(va);
}
```

**NOTES**
- Flush *after* the table write and the `@mmu_sync()`, never before.
- On a multi-core system a local flush is not enough — a TLB shootdown IPI to
  the other cores is the caller's responsibility.

---

### @cache_flush_line(p), @cache_flush_range(p, len), @cache_clean_range(p, len), @cache_invalidate_range(p, len), @cache_invalidate_icache(p, len), @cache_zero_line(p), @cache_flushopt(p), @cache_writeback(p), @nt_store(p, val), @barrier_dma()

**DESCRIPTION**
Cache maintenance and non-temporal stores — the DMA and persistent-memory
primitives.

- **clean** = write dirty lines back, keep them valid.
- **invalidate** = drop lines without writing back (data loss if dirty).
- **flush** = clean + invalidate.

**SIGNATURE**
```
@cache_flush_line(*u8 p)                  -> void  clean+invalidate one line
@cache_flush_range(*u8 p, u64 len)        -> void
@cache_clean_range(*u8 p, u64 len)        -> void  writeback only
@cache_invalidate_range(*u8 p, u64 len)   -> void  discard only
@cache_invalidate_icache(*u8 p, u64 len)  -> void  instruction cache
@cache_zero_line(*u8 p)                   -> void  zero a whole line (ARM `dc zva`)
@cache_flushopt(*u8 p)                    -> void  CLFLUSHOPT — ordered flush
@cache_writeback(*u8 p)                   -> void  CLWB — writeback, keep valid
@nt_store(*u8 p, u64 val)                 -> void  MOVNTI — bypass cache
@barrier_dma()                            -> void  ordering barrier for device DMA
```

**EXAMPLE**
```zer
u8[64] dma_buf;

void publish_to_device() {
    dma_buf[0] = 1;
    @cache_clean_range(&dma_buf[0], 64);   // make the write visible to the device
    @barrier_dma();
}

void consume_from_device() {
    @cache_invalidate_range(&dma_buf[0], 64);  // drop stale lines before reading
    @barrier_dma();
    if (dma_buf[0] == 0) { return; }
}
```

**NOTES**
- Before a device READS your buffer: clean. After a device WROTE your buffer:
  invalidate. Getting this backwards loses data silently.
- `@cache_invalidate_icache` is required after writing code you will execute;
  pair it with `@cpu_flush_pipeline()`.
- `@cache_writeback` (CLWB) is the persistent-memory primitive — it keeps the
  line valid, so a following read is still a cache hit.
- Several of these are ARM/RISC-V concepts with no x86 instruction; on a target
  that lacks one, ZER emits the nearest available barrier or evaluates the
  arguments and does nothing. `@cache_zero_line` and `@tlb_flush_asid` are the
  two where the no-op is most likely to surprise — check the emitted C if you
  depend on the effect.

---

### @cstr(buf, slice)

**DESCRIPTION**
Copy a `[*]u8` slice into a fixed buffer and append NUL terminator.
For C interop (C functions expect NUL-terminated strings).

**EXAMPLE**
```zer
u32 main() {
    u8[64] cbuf;
    const [*]u8 name = "hello";
    *u8 cname = @cstr(cbuf, name);    // "hello\0" in cbuf, returns &cbuf[0]
    if (cbuf[5] != 0) { return 1; }
    if (*cname != 'h') { return 2; }
    return 0;
}
```

**NOTES**
- Returns a pointer to the start of the buffer. If the slice plus its NUL does
  not fit, it TRAPS at run time with `@cstr buffer overflow` — it never
  truncates silently.

- Takes exactly TWO arguments, and their shapes are an ALLOW-list, not a
  deny-list. The destination must be a fixed array `u8[N]`, a slice `[*]u8`, or
  a `volatile`/`*opaque` pointer at a hardware boundary; the source must be a
  slice (a string literal is one — slice an array with `arr[0..]`). Anything
  else is rejected. Before this was an allow-list, `@cstr(some_u32, sl)` was
  accepted BY DEFAULT and memcpy'd through an address taken from an integer:
  no mmio range, no alignment check, no bounds check.
- The destination may not be `const`.

<!-- audit: expect-trap: @cstr buffer overflow -->
```zer
u32 main() {
    u8[4] small;
    const [*]u8 name = "hello";          // 5 bytes + NUL does not fit in 4
    *u8 c = @cstr(small, name);          // TRAPS
    return *c;
}
```

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

**THE TARGET MUST BE A KEYWORD TYPE — a bare user type name is NOT a cast**
`(Name)expr` is parsed as the parenthesised expression `(Name)` followed by
`expr`, because `(x)` has to keep meaning "the variable x". Only these start a
cast: the primitive keywords (`u8`..`u64`, `i8`..`i64`, `usize`, `f32`, `f64`,
`bool`, `void`, `opaque`), `?`, `const`, `volatile`, and `*`. So a POINTER to a
user type works and a bare user type does not:

```zer
(*Motor)ctx        // OK — the `*` makes it unambiguous
(Motor)x           // NOT a cast — the compiler reports a syntax error at `x`
(State)n           // NOT a cast — and int -> enum needs a checked door anyway
(Meters)n          // NOT a cast — use @cast for a distinct typedef
```

Use the named intrinsic for each of these: `@cast(Meters, n)` for a distinct
typedef, `@bitcast(State, n)` for an enum (checked at runtime against the
variant set), `@ptrcast` / `@pun` between pointer types, `@inttoptr` for an
address.

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

### Converting a float to an integer — DEFINED, and it SATURATES

C leaves a float-to-integer conversion UNDEFINED when the value does not fit, and the
result really does vary: the same emitted C gave `4294967295` at `-O0` and `0` at `-O2`.
ZER defines it. **The conversion SATURATES to the target's range, and NaN becomes 0.**

```zer
i32 printf(const *u8 fmt, ...);

u32 main() {
    f64 big  = 1e20;
    f64 neg  = -1.5;
    f64 frac = 3.7;

    if ((u32)frac != 3)          { return 1; }   // in range — plain truncation toward zero
    if ((u32)neg  != 0)          { return 2; }   // below the range — clamps to 0
    if ((u32)big  != 4294967295) { return 3; }   // above the range — clamps to the max
    if ((i32)big  != 2147483647) { return 4; }
    if ((i32)(-1e20) != -2147483648) { return 5; }
    if ((u8)300.0 != 255)        { return 6; }

    printf("float->int saturates\n");
    return 0;
}
```

The rule matches Rust's `as`, and it follows ZER's existing split: a **memory** violation
halts (slice OOB, misaligned `@inttoptr`, a bad `@pun`), while an **arithmetic** result
gets a defined value — which is already why integer overflow wraps rather than trapping.

`@saturate(T, x)` is the same operation under its own name, so both spellings agree:

```zer
u32 main() {
    f32 over = 2147483648.0;
    if (@saturate(i32, over) != 2147483647) { return 1; }
    if ((i32)over            != 2147483647) { return 2; }
    return 0;
}
```

**NaN is tested first, deliberately.** Every comparison against NaN is false, so a range
check written the obvious way falls straight through to the raw cast — the exact UB being
removed. This holds at EVERY width, `u128` / `i128` and `u65`..`u127` included — those
used to keep only a NaN trap followed by a raw cast (undefined for an infinity or an
out-of-range value); they saturate like every other width since BUG-1064.

**It works in a global initializer too**, where the value is a compile-time constant:

```zer
u32 gbig = (u32)1e20;      // 4294967295
u32 gneg = (u32)(-1.5);    // 0
i8  gmin = (i8)(-1e20);    // -128
u32 main() {
    if (gbig != 4294967295) { return 1; }
    if (gneg != 0)          { return 2; }
    if ((i32)gmin != -128)  { return 3; }
    return 0;
}
```

A literal that does not fit is a **warning**, not an error — rejecting `(u32)(-1.5)` while
the same value through a variable is defined would be two spellings disagreeing.

**`@truncate` on a float is a compile error.** `@truncate` means "keep the low bits" and a
float has none; giving one primitive two unrelated meanings by operand type is the kind of
overload this language avoids. Use the cast or `@saturate`, which already say it:

<!-- audit: skip -->
```zer
u32 bad(f32 x) {
    return @truncate(u32, x);   // ERROR — @truncate has no meaning on a float
}
```

### Forging an enum traps at the point of forgery

ZER has no int-to-enum cast, so the only way to produce a value outside an enum's declared
variants is a bit-level conversion. Those are legitimate — reading an enum out of a
hardware register is a real firmware idiom — so ZER **tracks** rather than bans: the
conversion compiles, and a guard traps if the value is not a declared variant. Without it
an exhaustive `switch` silently runs its last arm on a value that matches none.

There are exactly **three** doors, and each carries the guard:

| conversion | guarded |
|---|---|
| `@bitcast(Enum, n)` | yes |
| `@truncate(Enum, n)` | yes |
| `@saturate(Enum, n)` | yes |

`@cast` is **not** a fourth door: it requires a distinct typedef and cannot name a bare
enum. The guard also recurses through struct fields, optional payloads and array elements,
so `@bitcast(Box, 7)` where `struct Box { State s; }` is checked too.

```zer
enum State { idle, running, done }

u32 main() {
    State s = @bitcast(State, 2);   // 2 IS a declared variant — no trap
    switch (s) {
        .idle    => { return 1; }
        .running => { return 2; }
        .done    => { return 0; }
    }
}
```

`@bitcast(State, 7)` compiles and traps at run time with
`@bitcast produced a value that is not a declared variant of this enum`.

### Argument counts are checked

An intrinsic that takes nothing used to accept anything and silently discard it, which
makes a typo look purposeful — the reader's reasonable belief is that the arguments do
something. Every intrinsic now reports its own arity, and so does `spawn`:

<!-- audit: skip -->
```zer
void worker(u32 v) { }

u32 bad() {
    @trap(1, 2, 3);        // ERROR — @trap takes no arguments, the message is fixed
    @barrier(1);           // ERROR — a fence takes no arguments
    spawn worker();        // ERROR — spawn target 'worker' expects 1 argument, got 0
    spawn worker(1, 2, 3); // ERROR — expects 1 argument, got 3
    return 0;
}
```

`spawn` is worth calling out: the direct call `worker(1,2,3)` was always rejected, but the
`spawn` spelling of the same program used to reach the C compiler instead — extra arguments
dropped, missing ones reading an auto-zeroed slot.

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
- A read-modify-write (`reg |= 1`, `g = g + 1`) of a volatile global shared with an
  interrupt handler is a compile error OUTSIDE `@critical` (non-atomic RMW) — see
  `interrupt` for the full set of ISR rules.
- INDEXING a volatile `*T` (`reg[i]`) is bounds-checked against the `mmio`
  declaration, but only when the compiler can DERIVE the bound — which it can do
  only for a pointer obtained directly from `@inttoptr(*T, <const addr>)` inside
  a declared range AND never reassigned or address-taken afterwards (for a local,
  anywhere in its function; for a global, anywhere in the program — a remap in
  another function counts, and so does a reassignment later in a loop body, which
  reaches an earlier index on the next iteration). A parameter, alias, struct
  field or reassigned pointer carries no bound, so indexing one is a compile
  error rather than an unguarded access. The element may be a struct: a register
  BLOCK `volatile *Regs r` indexes as `r[i].sr`, bounded by `sizeof(Regs)`.

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

```zer
mmio 0x40000000..0x400000FF;
struct Regs { u32 cr; u32 sr; u32 dr; u32 pad; }   // one 16-byte register block

u32 status(u32 i) {
    volatile *Regs r = @inttoptr(*Regs, 0x40000000);
    return r[i].sr;          // bound: 256 / 16 = 16 blocks; an unproven i is guarded
}
```

<!-- audit: expect-error: never reassigned -->
```zer
mmio 0x40000000..0x400000FF;
u32 main() {
    volatile *u32 r = @inttoptr(*u32, 0x40000000);
    r = @inttoptr(*u32, 0x400000F0);      // reassigned: the declaration's bound is gone
    return r[10];                         // ERROR — would read 0x40000118, outside the range
}
```

- A SLICE over an array that lives in volatile or const memory keeps the
  qualifier. For a volatile register block's array field the view must be
  `volatile [*]T`, and for a const object's array field `const [*]T` — at every
  place a view is formed (a var-decl, an assignment, a call argument, a
  sub-slice `r.arr[0..2]`, a return). Dropping it is a compile error ("cannot
  form a non-volatile slice over a volatile array field …"): without `volatile`
  the C compiler may merge or delete the device accesses through the view, and
  without `const` the view could write to read-only memory.

```zer
struct Fifo { u32[4] slot; }
volatile Fifo gdev;
struct Tab { u32[4] v; }

void kick(volatile [*]u32 s) { s[0] = 1; s[0] = 2; }     // both stores happen
u32 sum(const [*]u32 s) {
    u32 t = 0;
    for (u32 i = 0; i < s.len; i += 1) { t += s[i]; }
    return t;
}

u32 main() {
    kick(gdev.slot);                  // volatile field -> volatile [*]u32
    volatile [*]u32 w = gdev.slot;
    w[1] = 3;
    const Tab ct;
    if (gdev.slot[0] != 2 || gdev.slot[1] != 3) { return 1; }
    if (sum(ct.v) != 0) { return 2; }  // const field -> const [*]u32
    return 0;
}
```

<!-- audit: expect-error: cannot form a non-volatile slice over a volatile array field -->
```zer
mmio 0x40000000..0x400000FF;
struct Uart { u32[4] fifo; }
void kick([*]u32 s) { s[0] = 1; s[0] = 2; }   // a plain view: the first store may vanish
u32 main() {
    volatile *Uart u = @inttoptr(*Uart, 0x40000000);
    kick(u.fifo);                              // ERROR — drops volatile
    return 0;
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
- A global touched from an interrupt handler AND from other code must be
  `volatile` ("global 'g' is accessed from both interrupt and main code — must
  be declared volatile"). Reaching it INDIRECTLY counts: through a helper, a
  function pointer, or a global pointer whose initializer is `&g` (the ISR
  writing `*gp` and main reading `g` touch the same global).
- A read-modify-write (`g += 1`, `g = g | 4`) of such a global must run inside
  `@critical` (or use `@atomic_*`) on EVERY side that does one — an interrupt can
  land between the read and the write. A plain single-word read or store needs
  neither.
- TWO interrupt handlers sharing a global are treated exactly like an ISR and
  main: with nested priorities one handler preempts the other mid-update.
- `Pool`, `Ring`, `Slab` and `Arena` cannot be shared between an interrupt
  handler and other code — their bookkeeping is updated in several non-atomic
  steps, and `volatile` cannot fix that. Give each context its own, or hand
  values across in a volatile single-word variable or an `@atomic_*` cell.
- `@once` must not be reachable from an interrupt handler ("@once is reachable
  from an interrupt handler"): its state flag is not interrupt-safe, so the body
  can run twice or be observed half-done.
- A hosted x86-64 GCC refuses `__attribute__((interrupt))`, so a program with an
  `interrupt` block builds only for a bare-metal target; the checker still
  checks every rule above on any host.

<!-- audit: compile-only: an interrupt handler only builds for a bare-metal target -->
```zer
volatile u32 ticks;               // touched by an ISR and by main: must be volatile

interrupt TIM2 {
    @critical { ticks += 1; }     // a read-modify-write: inside @critical
}

u32 main() {
    @critical { ticks += 4; }     // ...on BOTH sides
    u32 now = ticks;              // a plain single-word read needs no @critical
    return now - now;
}
```

<!-- audit: expect-error: shared between interrupt and another interrupt handler code -->
```zer
volatile u32 g;
interrupt TIM2 { g += 1; }        // ERROR — TIM3 can preempt TIM2 mid-update
interrupt TIM3 { g += 1; }
u32 main() { return 0; }
```

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

**STRUCTURED FORM**
Besides the GCC-style string, a structured `asm { }` block names every operand
by register. `instructions:` is one string, `outputs:` / `inputs:` map a
register name to a ZER lvalue / value, `clobbers:` is a list, and **`safety:`
is MANDATORY and must be at least 30 characters** — the audit trail; shorter is a
compile error. Like the string form it is allowed only in a `naked` function.

```zer
u64 out_val;
const u64 in_val = 42;

naked void copy_reg() {
    asm {
        instructions: "mov %1, %0"
        outputs: { "rax" = out_val }
        inputs:  { "rdi" = in_val }
        clobbers: [ "memory" ]
        safety: "plain register move — no memory access, no flags consumed"
    }
}

u32 main() {
    volatile u32 never = 0;
    if (never == 42) { copy_reg(); }    // compiled, not executed on the host
    return 0;
}
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
A function whose body is pure `asm(...)` statements plus `return` — the only
place `asm` is allowed.

**The `naked` attribute itself is NOT emitted today.** The compiler says so
("'naked' is accepted for asm permission but the attribute is NOT emitted: GCC
will still generate a prologue/epilogue"): you do not get your own frame layout,
your own `ret` / `iret` / `eret`, or untouched callee-saved registers. For true
naked semantics, write the function in C or assembly and bring it in with
`cinclude`.

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

**WHOLE-PROGRAM CHECKS**
The rules that need to see the whole program run over EVERY module, not just
the main file (BUG-1037): the per-statement deadlock check on `shared` structs,
`--stack-limit` (an imported function's frame counts in main's chain), the
Arena / Barrier "never initialised" check (an arena declared and used in a
module may receive its backing store in main), and `*opaque` call-site
provenance. A container global (`Pool`, `Ring`, `Slab`, `Arena`) declared in a
module is addressable from its importers like any other global — `scratch =
Arena.over(mem);` in main for a module's `Arena scratch;` compiles (BUG-1040).

Known limit: two modules declaring the same NON-static global name resolve to
whichever was registered first inside the checker — see `docs/limitations.md`.
Make such globals `static`, or give them distinct names.

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

i32 abs(i32 x);                  // declare every C function you call
i64 labs(i64 x);

u32 main() {
    if (abs(-5) != 5) { return 1; }
    if (labs(-7) != 7) { return 2; }
    return 0;
}
```

**NOTES**
- The ZER declaration is emitted next to the header's own, so it must agree with
  it in C terms or GCC rejects the pair. `u8` is `unsigned char`, not `char`, so a
  C `const char *` parameter cannot be redeclared as `const *u8` beside its header
  (`atoi`, `strlen`), and `*opaque` is ZER's tracked pointer, not `void *`, so
  `*opaque malloc(usize)` beside `<stdlib.h>` does not build either. A function
  that is a MACRO in the header (`toupper` at `-O2`) cannot be redeclared at all.
  For those, put a small wrapper with ZER-friendly types in your own header and
  declare the wrapper — or use ZER's own `alloc` / `free` instead of `malloc`.
- C macros (stderr, stdout, etc.) are NOT accessible. Wrap in a C helper function.
- `_zer_` prefix is reserved — name helpers `zer_get_stderr`, not `_zer_stderr`.

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

These two examples name a C library's own header (`sensor.h`, `event_lib.h`), so
they are checked but cannot be built without that library.

**Memory safety** — wrap C pointers in `*opaque`:
<!-- audit: compile-only: needs the C library's sensor.h -->
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
<!-- audit: compile-only: needs the C library's event_lib.h -->
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

The auto-lock fires regardless of which THREAD calls the function — ZER `spawn`, C `pthread_create`, an OS callback thread. The lock is on the DATA (the `shared struct`'s mutex), not on the thread creation mechanism.

An `interrupt` handler is not a thread, and a mutex is the wrong tool there (the
interrupted code may hold it). A `shared struct` global touched from an ISR and
from main is rejected with the ISR rules instead: it must be `volatile`, and a
read-modify-write of it must be done inside `@critical` or with `@atomic_*` (see
`interrupt`).

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
- A comptime body may use variable declarations, assignments (including
  compound and array-element forms), `if` / `else`, `for` / `while` /
  `do-while` with `break` and `continue`, `switch`, nested blocks and
  `return`. `goto`, labels, `defer`, `@critical`, `@once`, `spawn`, `asm`,
  a call to a run-time (non-comptime) function, and any condition or
  return value that does not fold to a constant are compile errors that
  name the construct (`... is not supported in a comptime body`). The
  interpreter never skips a statement it cannot model.
- A divisor that folds to zero (`x / 0`, `x /= 0`, `x %= 0`) is a compile
  error, not a folded 0.
- A comptime body sees only its PARAMETERS and its own locals: reading a
  global `const` inside the body (`comptime u32 L() { return LEVEL; }`) is
  "body could not be evaluated at compile time". Pass the constant as an
  argument instead — `comptime u32 L(u32 lvl) { return lvl; }` called as
  `L(LEVEL)` folds.

```zer
comptime u32 FIRST_OVER(u32 limit) {
    u32 x = 0;
    while (x < 100) {
        x += 3;
        if (x > limit) { break; }
    }
    return x;
}
u32 main() { return FIRST_OVER(10) - 12; }   // 0
```

---

### comptime if

**DESCRIPTION**
Conditional compilation. Replaces C `#ifdef`. Condition must be compile-time constant.
Only the taken branch is type-checked — dead branch is ignored entirely.

`comptime if` is a STATEMENT: it goes inside a function body. It cannot appear
at file scope to choose between two top-level declarations (that is a parse
error, "expected type at 'if'"); put the `comptime if` inside the function
instead, as below.

**SYNTAX**
<!-- audit: skip -->
```zer
comptime if (DEBUG) {
    // only compiled when DEBUG is true
} else {
    // only compiled when DEBUG is false
}
```

**CONDITIONS**
Accepted: `true` / `false` and integer literals, `const bool` and `const`
integer globals, comptime function calls with constant arguments (a
`comptime bool` function included), and expressions combining these with
`!`, `&&`, `||`, comparisons and arithmetic. A non-zero integer is taken as
true. `static_assert` accepts exactly the same conditions.

**EXAMPLE**
```zer
i32 puts(const *u8 s);
const bool DEBUG = true;
const u32 LEVEL = 2;
comptime bool VERBOSE(u32 lvl) { return lvl > 1; }

void log(const [*]u8 msg) {
    comptime if (DEBUG) {
        puts(msg.ptr);          // compiled only when DEBUG is true
    } else {
        // release: the call vanishes; this branch is not even type-checked
        no_such_function(msg);
    }
}

u32 main() {
    comptime if (VERBOSE(LEVEL)) { log("verbose"); }   // comptime call, const argument
    comptime if (LEVEL >= 2 && !DEBUG) { return 1; }   // expressions over consts
    log("hello");
    return 0;
}
```

---

### static_assert

**DESCRIPTION**
Compile-time assertion. Condition must evaluate to a compile-time constant. False → compile error with optional message.
Allowed at file scope and inside a function body; takes the same conditions as
`comptime if`.

**SYNTAX**
```zer
enum Color { red, green }
const u32 SIZE = 4;
const bool DEBUG = true;

static_assert(SIZE > 0, "size must be positive");
static_assert(Color.red == 0);
static_assert(DEBUG && SIZE == 4, "a const bool and an integer const");

u32 main() {
    static_assert(true, "inside a function too");
    return 0;
}
```

<!-- audit: expect-error: static_assert failed: buffer too small -->
```zer
const u32 SIZE = 4;
static_assert(SIZE > 8, "buffer too small");      // ERROR — prints the message
u32 main() { return 0; }
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
- A by-value CYCLE through several containers is the same error, at any cycle
  length. Only the direct case used to be caught, and the two-container form
  crashed the compiler (BUG-864):
```zer
container A(T) { B(T) x; }    // COMPILE ERROR — closes a containment cycle
container B(T) { A(T) y; }
```
  Make any one link a pointer and the cycle is finite and legal:
```zer
container A(T) { ?*B(T) x; }
container B(T) { ?*A(T) y; }
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
- Recursive functions get a warning (their depth cannot be computed).
- A call through a function pointer whose target the compiler cannot pin down is
  an ERROR under `--stack-limit`, because the budget cannot be verified — this
  includes a call through a struct FIELD (`o.f(1)`) or array ELEMENT (`tbl[i](x)`),
  a local funcptr, and a GLOBAL funcptr that any function reassigns. An `interrupt`
  handler is an entry point like `main`, and gets the same error.
- Frames are sized for the TARGET: a pointer local is 8 bytes by default on a
  64-bit host and 4 with `--target-bits 32`.
- Every stack diagnostic names the function's own source line
  (`main.zer:5: error: function 'big' local stack 200 bytes exceeds --stack-limit 64`).
- Imported modules count: an imported callee's frame is part of main's chain.

<!-- audit: expect-error: function 'big' local stack 200 bytes exceeds --stack-limit 64 -->
```zer
// zerc-flags: --stack-limit 64
u32 big() { u8[200] b; b[0] = 1; return b[0]; }    // ERROR — a 200-byte frame
u32 main() { return big() - 1; }
```

<!-- audit: expect-error: call chain contains function pointer call with unknown target -->
```zer
// zerc-flags: --stack-limit 4096
u32 small(u32 n) { return n + 1; }
struct Ops { *(u32) -> u32 f; }
u32 callit(Ops o) { return o.f(1); }       // ERROR — a funcptr FIELD: target unknown
u32 main() {
    Ops o = { .f = small };
    return callit(o) - 2;
}
```

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
- **Function pointers:** a funcptr CALL worst-cases every REFERENCE-CARRYING
  parameter as keep, because the target is invisible and could retain what it is
  handed. "Reference-carrying" is the recursive property, not a spelling: `*T`,
  `*opaque`, `?*T`, an array of those, and a **by-value struct or union with a
  pointer field** all count. This applies both to passing `&local` *directly* to
  such a parameter AND to *forwarding* a parameter to a funcptr
  (`void fwd(*T p, *(*T) cb){ cb(p); }` infers `p` keep), so a stack pointer
  cannot reach a retaining callback through a funcptr indirection.
  Consequence: a read-only callback must be given a long-lived (global/static)
  context, not a stack-local one — the same rule that already applies to a direct
  funcptr call.
- **The one exemption, stated plainly:** a parameter whose own reference IS a
  SLICE (`[*]T`, or `?[*]T`) is NOT worst-cased, so passing a local array to a
  `[*]T` callback still compiles. That is a deliberate ergonomic carve-out for
  the read-only-view idiom and it is **not** sound: a target that stores the
  slice in a global leaves it dangling. If a `[*]T` callback may retain its
  argument, give it a global buffer. Tracked in `docs/limitations.md`.

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
"Width" is the ZER width of the left operand, not its C carrier: `i5 x = -16;
x >> 5` is 0 even though an `i5` is stored in an 8-bit carrier (BUG-1065).
This covers negative shift counts too: a signed count that is negative
(e.g. `i32 n = -1; x << n`) returns 0 rather than falling into C
undefined behavior.

**The width is the RESULT type's, and a shift does not promote.** The result of
`a << n` has the common type of `a` and `n` (a literal count takes `a`'s type),
exactly like `+`; there is no C-style promotion to `int`. So `u8 a = 200;
u32 x = a << 4;` is a u8 shift — 3200 wraps to 128 — and `a << 8` is 0, where C
would give 3200 and 51200. Widen first when you want the wide result:
`u32 x = (u32)a << 4;` is 3200. The same rule folds at file scope: a global
`const u32 S = B << 4;` with `const u8 B = 200;` is 128, and a constant count at
or beyond the width (`const u32 Z = 1 << 200;`) folds to 0.

```zer
const u8 B = 200;
const u32 S = B << 4;           // 128 — a u8 shift, wrapped
const u32 Z = 1 << 200;         // 0 — over-width, folded at file scope
u32 main() {
    u8 a = 200;
    u32 x = a << 4;             // 128
    u32 y = (u32)a << 4;        // 3200 — widened first
    u32 n = 40;
    u32 z = 1 << n;             // 0 — count >= 32 at run time
    if (x != 128 || y != 3200 || z != 0 || S != 128 || Z != 0) { return 1; }
    return 0;
}
```

### Comparison
`==  !=  <  >  <=  >=` — Returns bool.

Not defined on AGGREGATES: struct, union, slice and array operands are rejected
for all six operators (compare the fields you care about individually). A value
OPTIONAL is rejected too — see `?T` above; only `opt == null` is allowed.

### Logical
`&&  ||  !` — Short-circuit evaluation.

The RIGHT operand of `&&` / `||` runs only when the left permits it, and the
bounds checker knows that: `if (i < 4 && a[i] > 0)` — the canonical guarded
access — compiles. The index is not PROVEN in range there (range narrowing does
not yet flow across a short-circuit), so it carries a runtime auto-guard rather
than being elided; write the guard as a nested `if` to get the check for free.
Putting the access on the LEFT (`a[i] > 0 && i < 4`) runs it unconditionally and
is still a hard error when the index is provably out of range.

### Assignment
`=  +=  -=  *=  /=  %=  &=  |=  ^=  <<=  >>=`

An assignment is an expression whose value is the stored value, as in C, and the
target is evaluated once:
```zer
u32 main() {
    u32 x = 0;
    u32 y = (x += 1) + 2;          // x = 1, y = 3
    if ((x += 1) > 3) { return 1; } // x = 2
    u32 z = (x = 7) + 1;           // x = 7, z = 8
    return y + z - 11;             // 0
}
```
`x /= 0` and `x %= 0` with a divisor that folds to zero are compile errors, exactly
like `x / 0`; a divisor the compiler cannot prove nonzero is one too (see "SAFETY GUARANTEES").

### Bit Extraction
```zer
reg[9..8]                  // Extract bits 9:8
reg[7..4] = 0x0F;          // Set bits 7:4
reg[7..0] += 3;            // Compound assign — read-modify-write of the field
```
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
- `,` — Comma operator
- Pointer arithmetic (`p + 1`) — index instead (`p[1]` on a `[*]T`)
- Implicit narrowing or sign conversion — `(T)x` / `@truncate` / `@saturate` are
  the explicit routes (C-style casts ARE supported — see "Casts")
- `(*U)p` between two different pointer types — `@pun(*U, p)` is the audit-visible form

(`goto` IS in ZER — see "goto + labels" above.)

---

## SAFETY RULES YOU WILL HIT

Rules that reject code most people expect to compile. Each is here because the
alternative is a wrong answer at run time rather than a message at compile time.

### Bounds: four verdicts, not two

An index gets one of four verdicts:

| Verdict | When | Result |
|---|---|---|
| PROVEN SAFE | the whole range is inside the bound | no check emitted — zero overhead |
| PROVABLY OUT OF BOUNDS | no value in the range can be valid | **compile error** |
| LOOP RUNS PAST THE END | the index is the counter of a counted loop that *will* take a value past the bound | **compile error** |
| UNKNOWN | the range straddles the bound, or is unknown | auto-guard inserted (early return) |

An index the compiler can prove is *always* wrong is an error, not a runtime
check — including when it is reached through a variable, and including a range
that is entirely negative.

<!-- audit: skip -->
```zer
u32 past_end() {
    u32[4] arr;
    u32 i = 10;
    return arr[i];      // ERROR — proven range [10,10], always out of bounds
}

u32 negative() {
    u32[4] arr;
    i32 i = -1;
    return arr[i];      // ERROR — proven range [-1,-1], no valid index exists
}
```

An **empty** range is not an error. A range whose max is below its min (a
zero-trip loop, or a contradictory guard) says the access is unreachable, so it
is never a compile error — the index gets the ordinary "not proven in range"
warning and an auto-guard, which never fires:

```zer
u32 main() {
    u32[4] arr;
    for (u32 i = 0; i < 0; i += 1) { arr[i] = 9; }   // warning only — the body never runs
    return arr[0];
}
```

The third verdict is the off-by-N loop bound. A *range* only says which values a
variable MAY hold, which is why a straddling range alone is not an error. But the
counter of a `for` whose init, bound and step are constants, with a body that
cannot skip an iteration or change the counter, provably TAKES every value of its
sequence — so if one of them indexes past the end, the loop provably performs an
out-of-bounds access:

<!-- audit: expect-error: the loop provably performs an out-of-bounds access -->
```zer
u32 main() {
    u32[4] arr;
    for (u32 i = 0; i < 8; i += 1) { arr[i] = i; }   // ERROR — counter runs 0..7
    return 0;
}
```

<!-- audit: skip -->
```zer
u32[4] arr;
for (u32 i = 0; i <= 4; i += 1) { arr[i] = i; }  // ERROR — off by one
for (u32 i = 0; i < 4; i += 1) { arr[i] = i; }   // OK — and no check is emitted
```

`while` and `do-while` get the same treatment when the counter is bumped by a
constant in the LAST statement of the body — that position is what makes the
values at the access `lo, lo+step, ...`; with the increment first they are
`lo+step, ...` instead, so no certainty is claimed there.

Break any premise and it falls back to the auto-guard: a `break`/`return`/`goto`
or an `orelse` in the body, an assignment to the counter, a nested loop, a
non-constant bound, or an access nested inside an `if` (the offending values may
never reach it). When the range is KNOWN and straddles the end but no certainty
holds, the warning says so and says what the guard does at runtime.

Proven-safe really does mean no code: `u32 i = 2; arr[i]` emits a bare `arr[i]`. An
unprovable index emits `if ((size_t)(i) >= 4u) { return 0; }` in front of the access.

**Every position a loop evaluates is checked under the value the counter holds
THERE**, not the value it had before the loop. That means the body, the
condition, and the STEP — all three run once per iteration under the
loop-carried value:

```zer
u32 main() {
    u32[4] a;
    a[0] = 1; a[1] = 1; a[2] = 1; a[3] = 1;

    // the index in the STEP is checked against k in [0,7], not k in [0,0]:
    // warns, auto-guard inserted, so the loop returns early at k == 4
    for (u32 k = 0; k < 8; k += a[k] + 1) { }

    // the loop's own bound proves this one, so nothing is emitted:
    for (u32 j = 0; j < 4; j += a[j]) { }

    return 0;
}
```

  The init is the exception, and only because it genuinely runs once before the
  loop exists, so the pre-loop range is the right one there.

**The LOWER bound of a counter is trusted only while the counter is monotone.**
`for (i32 i = 2; i < 4; ...)` proves `i >= 2` only if the step is the counter's
sole writer and is a non-negative constant increment (`i += C`, `i = i + C`,
`i = C + i`). A body that writes the counter, or a decrementing step, drops the
lower bound to "unknown" and the index is guarded (BUG-1034 — before this the
range stayed `[2,3]` while `i` went negative, and the store landed BELOW the
array). An UNSIGNED counter is unaffected: its minimum is 0 by type. The upper
bound never depends on this — the condition is re-tested before every body entry.

```zer
u32 main() {
    u32[4] a;
    u32 n = 0;
    // a SIGNED counter the body LOWERS: i = 2, then -2, -6 ... — [2,3] is not
    // true any more, so a[i] is guarded (warning + auto-guard, returns early)
    for (i32 i = 2; i < 4; i += 1) { a[i] = 1; i -= 5; n += 1; if (n > 3) { break; } }
    // only a non-negative constant step writes the counter: proven, no code
    for (i32 j = 0; j < 4; j = j + 1) { a[j] = 2; }
    return 0;
}
```

**A `return` in an `orelse { ... }` block counts wherever the block sits** —
in a statement, or in a CONDITION (`if`, `while`, the `switch` subject, the for
init/cond/step, an `await` condition, a `spawn` argument). The callee's
return-range summary unions it in, so `arr[pick(k)]` keeps its check when
`pick` can return 9 from inside `if ((mb(x) orelse { return 9; }) > 0)`
(BUG-1035 — the condition positions were skipped and the index was emitted
with no check at all).

### A negative literal in an unsigned destination

`u32 x = -1;` is a compile error — ZER has no implicit sign conversion — and the
rule looks at the whole constant expression, not just a lone literal.

There is a second, less obvious half. Bare integer literals are `u32`, so `/`,
`%` and `>>` on a negated operand compute a DIFFERENT value than the same
expression read as signed arithmetic:

<!-- audit: expect-error: reads differently signed and unsigned -->
```zer
u32 main() {
    u32 x = -4 / -2;      // ERROR — signed this is 2, unsigned it is 0
    return x;
}
```

  Signed, `-4 / -2` is 2. Unsigned, it is `0xFFFFFFFC / 0xFFFFFFFE`, which is 0.
  ZER will not pick one silently. Write the signed reading with a signed type
  (`i32 y = -4; i32 z = -2; u32 x = (u32)(y / z);`), or `@bitcast(u32, ...)` for
  the unsigned bit pattern.

  Only those three operators are affected — they are the only ones whose result
  depends on the signedness of the operands. `+ - * & | ^ <<` produce the same
  bits either way and are untouched:

<!-- audit: skip -->
```zer
u32 a = -4 * -2;      // 8 — fine, one unambiguous answer
u32 b = -4 ^ -2;      // 2 — fine
u32 c = -4 + -2;      // ERROR — folds to -6, a negative constant
```

---

### A plain access races a SCOPED thread too, not just a fire-and-forget one

Once a global is touched with `@atomic_*` anywhere it is an ATOMIC CELL, and every other
access to it in a concurrent context must be atomic as well. The concurrent context of a
scoped spawn is the window between the `spawn` and its `join` — not nothing:

<!-- audit: expect-error: plain access to 'g_ctr' in a concurrent context -->
```zer
u32 g_ctr;
void worker() { u32 v = @atomic_add(&g_ctr, 1); }

u32 main() {
    g_ctr = 0;                         // OK — before the spawn, one thread
    ThreadHandle t = spawn worker();
    g_ctr = 5;                         // ERROR — races the running worker
    t.join();
    g_ctr = g_ctr + 1;                 // OK — after the join, one thread again
    return 0;
}
```

With two scoped threads live, one `join` does not close the window — the other thread is
still running.

### An out-of-bounds access inside `@critical` TRAPS

The auto-guard normally returns early. Inside `@critical` an early `return` would
leak the interrupt-disable, so there the guard **traps** instead (`SIGTRAP`, message
`out-of-bounds access inside a held lock, @critical block or defer cleanup — cannot
return without leaking it`), aborting before both the access and the leak.

<!-- audit: expect-trap: out-of-bounds access inside a held lock, @critical block or defer cleanup -->
```zer
volatile u32 g_idx = 9;
u32 out;
u32 main() {
    u8[4] a;
    u32 i = g_idx;
    @critical { out = a[i]; }     // unprovable index inside @critical -> TRAPS at run time
    return 0;
}
```

A `shared struct` statement does NOT trap: its lock is taken per statement, so the
guard is placed BEFORE the lock and returns early as usual, with no lock held.
Either way, an explicit check removes the guard entirely:

```zer
shared struct S { u32 v; }
S g;
volatile u32 g_idx = 9;

u32 main() {
    u8[4] a;
    u32 i = g_idx;
    if (i >= 4) { return 0; }   // explicit check — no guard is emitted at all
    g.v = a[i];
    return 0;
}
```

### A pointer into a `packed` field may not be dereferenced

`&packed.field` is not naturally aligned. Dereferencing it is a hard fault on ARMv7-M and
RISC-V, a silent split access on Cortex-M0+, and merely slow on x86 — so a hosted test
will never show you the problem. FORMING the pointer is allowed; USING it is not, and the
fact travels through an alias:

<!-- audit: skip -->
```zer
packed struct P { u8 a; u32 b; }
P g;

void poke(*u32 p) { *p = 7; }

u32 packed_misuse() {
    *u32 q = &g.b;      // forming it is fine
    *q = 7;             // ERROR — dereferencing a pointer into a PACKED struct field
    poke(&g.b);         // ERROR — argument 1 points into a PACKED struct field
    return 0;
}
```

Read and write the field directly (`g.b = 7;`) — that path knows the layout and emits a
correct unaligned access.

The same holds for every spelling of "use the misaligned address":
- a FIELD access through such a pointer is a dereference too — `*In q = &gp.inner;
  q.x` is rejected ("it points into a PACKED struct field");
- a `switch` on a UNION that is a field of a packed struct is rejected (the switch
  works through the union's address);
- a mutable capture `if (gp.opt) |*v|` of an optional packed field is rejected;
- a SLICE view of a packed array field whose elements need alignment
  (`[*]u32 s = gp.w;`) is rejected — a `u8` array field is fine.

Copy to an aligned local instead:

```zer
struct In { u32 x; u32 y; }
union U { u8 b; In s; }
packed struct P { u8 a; In inner; U u; }
P gp;

u32 main() {
    gp.inner.x = 5;             // direct field access: the layout-aware path
    In copy = gp.inner;         // or copy to an aligned local
    U uc = gp.u;                // a union field: copy it, then switch on the copy
    u32 r = 0;
    switch (uc) {
        .b => |v| { r = v; }
        .s => |s| { r = s.x; }
    }
    return copy.x + r - 5;
}
```

### The dereference identity rule

Reading a value out of a pointer dereference makes a COPY. For a scalar or a plain struct
that is what you want. For three kinds of value it destroys the identity the compiler
tracks, so BINDING the result is a compile error:

<!-- audit: skip -->
```zer
struct Node { u32 v; }
move struct Tok { u32 k; }
Pool(Node, 4) pl;

u32 bad_alias() {
    Node n; *Node p = &n; **Node pp = &p;
    *Node k = *pp;              // ERROR — pointer ALIAS: which allocation is k?
    return k.v;
}

u32 bad_handle() {
    Handle(Node) h = pl.alloc() orelse return;
    *Handle(Node) hp = &h;
    Handle(Node) g = *hp;       // ERROR — Handle: which pool slot does g own?
    pl.free(g);
    return 0;
}

u32 bad_move() {
    Tok t; t.k = 1; *Tok tp = &t;
    Tok b = *tp;                // ERROR — move struct: b is a second owner
    return b.k;
}
```

All three report `cannot bind a value obtained by dereferencing a pointer to a pointer`. Recovering the
identity would need points-to analysis over the dereferenced pointer, which ZER
deliberately does not have — whole-program analysis is outside the architecture — so the
rule is "cannot prove, therefore reject".

Copies with no identity to lose are fine:

```zer
struct S { u32 v; }

u32 main() {
    u32 x = 5;
    *u32 p = &x;
    u32 v = *p;             // OK — a scalar copy has no identity

    S s; s.v = 3;
    *S sp = &s;
    S c = *sp;              // OK — a plain struct VALUE copy

    if (v != 5) { return 1; }
    if (c.v != 3) { return 2; }
    return 0;
}
```

The fix is always to bind the POINTER the compiler can follow (`*Node k = p;`) or to take
the `Handle` by value rather than a pointer to it.

---

## SAFETY GUARANTEES

| Bug Class | How ZER Prevents It |
|-----------|-------------------|
| Buffer overflow | Bounds check on every array/[*]T access. Proven-safe indices skip check. |
| Use-after-free | Handle generation counter + zercheck compile-time analysis |
| Null dereference | `*T` non-null by type. `?*T` forces unwrap. |
| Double free | zercheck: compile error |
| Memory leak | zercheck: compile ERROR (an allocation neither freed nor escaped) |
| Uninitialized memory | Everything auto-zeroed |
| Integer overflow | Wraps (defined), never UB |
| Silent truncation | Must use @truncate or @saturate explicitly |
| Missing switch case | Exhaustive check for enums, bools, unions |
| Dangling pointer | Scope escape analysis on return, assign, keep, orelse |
| Union type confusion | Cannot mutate union variant during switch capture |
| Arena pointer escape | Arena-derived pointers cannot be stored in globals |
| Division by zero | Forced guard — compile error if divisor not proven nonzero. A float divisor is proven by `if (y == 0.0) { return; }` / `if (y != 0.0) { ... }` (BUG-1067) — float division follows the integer rule, not IEEE `inf` |
| Invalid MMIO address | mmio range declarations + alignment check + boot probe |
| ISR data race | Globals shared ISR↔main (or between two ISRs) without volatile → compile error; an RMW of one outside `@critical` → compile error |
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
zerc source.zer --stack-limit 2048                # error when a stack budget is exceeded (below)
zerc source.zer --emit-ir                         # print the lowered IR (per-function CFG) and exit
zerc source.zer --track-cptrs -o out.c            # keep the C-interop pointer tracking (the
                                                  # malloc/free header wrappers) in emitted C;
                                                  # always on for --run
zerc source.zer --trace                           # compiler phase trace on stderr (debugging zerc)
zerc source.zer --trace-calls                     # the same; the full compiler call graph
                                                  # needs the instrumented `make zerc-trace` build
zerc source.zer --release                         # accepted, currently a NO-OP (prints a warning)
zerc -h                                           # usage (also --help)
```

- Options may come before or after the input: `zerc --target-bits 32 source.zer` and
  `zerc source.zer --target-bits 32` are the same. The input is the one argument that
  is not an option or an option's value; a second one is an error ("more than one
  input file" — zerc takes one entry file and finds its imports from it).
- `-o path.c` (or `--emit-c`) emits C and stops; any other `-o` path — `/dev/null`
  included — builds an executable through GCC.

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
u32 main() {
    g.value = 42;              // lock -> write -> unlock
    g.total = g.value + 1;     // a SEPARATE lock -> read+write -> unlock
    if (g.total != 43) { return 1; }
    return 0;
}
```
- Locking is **per statement**, not grouped: the lock is released between the two
  lines above, so a multi-statement check-then-act is NOT atomic against another
  thread. Put a read-modify-write that must be atomic in ONE statement
  (`g.value += 1;`). The per-statement model is also what makes cross-statement
  lock ordering deadlock-free (see Deadlock Detection).
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
- **`@barrier_init` is mandatory.** A `Barrier` zero-initializes to a target of 0,
  and a wait on that returns on the FIRST arrival — every thread sails through and
  it reports success while synchronizing nothing. Waiting on a barrier that no
  `@barrier_init` ever names is a compile error; a barrier reached through a
  pointer parameter (which no per-file analysis can see) traps at runtime instead.

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
u32 g_led;
void led_on()  { g_led = 1; }
void led_off() { g_led = 0; }

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
    for (u32 i = 0; i < 4; i += 1) {    // a real scheduler loops forever
        _zer_async_blink_poll(&task);   // advance one step: on, off, on, off
    }
    return g_led;                       // 0 — the fourth step turned it off
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

**`await <condition>;`** suspends the coroutine until the boolean condition is
true; the condition is re-evaluated on every poll. `poll` returns 0 while the
task is suspended and 1 once the function has finished.

```zer
volatile u32 g_ready;
u32 g_seen;

async void waiter() {
    await g_ready == 1;          // suspend until the condition holds; re-tested on every poll
    g_seen = g_ready;
}

u32 main() {
    _zer_async_waiter task;
    _zer_async_waiter_init(&task);
    if (_zer_async_waiter_poll(&task) != 0) { return 1; }   // still waiting
    g_ready = 1;
    if (_zer_async_waiter_poll(&task) != 1) { return 2; }   // condition met -> done
    if (g_seen != 1) { return 3; }
    return 0;
}
```

**RETURNING A VALUE FROM AN ASYNC FUNCTION**

An `async` function may return a value. The poll protocol stays an `int`
done-flag — "is it finished" and "what did it produce" are separate questions —
and the value is read with a third generated function,
`_zer_async_NAME_result(&task)`:

| generated | for | signature |
|---|---|---|
| `_zer_async_NAME_init`   | every async fn   | `void(*task, <original params>)` |
| `_zer_async_NAME_poll`   | every async fn   | `i32(*task)` — 0 = pending, 1 = done |
| `_zer_async_NAME_result` | NON-void only    | `<return type>(*task)` |

```zer
async ?u32 lookup(u32 k) {
    yield;                       // one step of work
    if (k > 10) { return null; }
    return k * 2;
}

u32 main() {
    _zer_async_lookup t;
    _zer_async_lookup_init(&t, 4);
    while (_zer_async_lookup_poll(&t) == 0) { }   // drive to completion

    ?u32 r = _zer_async_lookup_result(&t);
    if (r) |v| { if (v != 8) { return 1; } } else { return 2; }
    return 0;
}
```

Any return type works — scalar, struct, `?T`, `?*T`, `?void`. Reading the result
before the poll reports done gives the zeroed initial value, exactly like any
other field of a freshly `_init`ed task; the value is stable across further
polls of a finished task. A value-returning async function compiles with no
warning. A VOID async has no accessor at all, so
`_zer_async_blink_result` on one is an undefined identifier rather than a call
that yields nothing.

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
- No C-style cast between two DIFFERENT pointer types (`@pun` is the explicit
  route); value casts `(T)x` exist and are explicit, checked conversions
- No header files (use `import`)
- No preprocessor (use `comptime`)
- No pointer arithmetic

---

*ZER(C) — Zero Error Risk C Extension. Same syntax, same mental model. The compiler does the safety work.*
