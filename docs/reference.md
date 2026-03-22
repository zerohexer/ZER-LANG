# ZER Language Reference

**Version:** 0.1 | **Compiler:** `zerc` | **Target:** Any platform GCC supports
**905 tests + 491 fuzz inputs, all passing**

---

## Types

### Primitive Types

| Type | Size | Description |
|------|------|-------------|
| `u8` | 1 byte | Unsigned 8-bit integer |
| `u16` | 2 bytes | Unsigned 16-bit integer |
| `u32` | 4 bytes | Unsigned 32-bit integer |
| `u64` | 8 bytes | Unsigned 64-bit integer |
| `i8` | 1 byte | Signed 8-bit integer |
| `i16` | 2 bytes | Signed 16-bit integer |
| `i32` | 4 bytes | Signed 32-bit integer |
| `i64` | 8 bytes | Signed 64-bit integer |
| `usize` | platform | Pointer-width unsigned |
| `f32` | 4 bytes | 32-bit float |
| `f64` | 8 bytes | 64-bit float |
| `bool` | 1 byte | `true` or `false` (not an integer) |
| `void` | 0 | Return type only |

All integers auto-zero on declaration. Overflow wraps (defined behavior). Shift by >= width returns 0.

### Arrays

Size goes between type and name:

```zer
u8[256] buf;        // 256-byte buffer
u32[4] values;      // 4 u32s
```

Every index is bounds-checked at runtime. Out-of-bounds traps.

### Slices — `[]T`

A `{ptr, len}` pair. Always bounded.

```zer
[]u8 data;                   // slice of bytes
void process([]u8 input) {   // function parameter
    u8 first = input[0];     // bounds-checked
    usize n = input.len;     // .len field
}
```

String literals are `[]u8`:

```zer
[]u8 msg = "hello";          // .ptr and .len (len=5, no NUL)
```

Sub-slicing:

```zer
buf[0..3]    // elements 0,1,2 (exclusive end)
buf[2..]     // element 2 through end
buf[..5]     // elements 0-4
```

Array-to-slice coercion happens at function call boundaries:

```zer
u8[256] buf;
void process([]u8 data) { }
process(buf);                // auto-coerces: { .ptr=buf, .len=256 }
```

### Pointers — `*T`

Non-null by default. The compiler guarantees `*T` is never null.

```zer
*Task t = get_task();        // always valid
t.priority = 5;              // safe — guaranteed non-null
```

Pointer arithmetic is **byte-level** (not scaled like C):

```zer
*u8 p = get_ptr();
*u8 q = p + 10;             // adds exactly 10 bytes
```

### Optional Pointers — `?*T`

Might be null. Must unwrap before use.

```zer
?*Task maybe = find_task();
maybe.priority = 5;                    // COMPILE ERROR
if (maybe) |t| { t.priority = 5; }    // safe — unwrapped
```

Zero overhead — represented as a plain C pointer where NULL = none.

### Optional Values — `?T`

Carries a value or nothing.

```zer
?u32 result;                 // struct { u32 value; u8 has_value; }
?bool flag;                  // struct { u8 value; u8 has_value; }
?void status;                // struct { u8 has_value; } — NO value field
```

### Type-Erased Pointer — `*opaque`

Equivalent to C's `void*`. Cannot be dereferenced without casting back.

```zer
*opaque raw = @ptrcast(*opaque, &my_task);
*Task t = @ptrcast(*Task, raw);          // explicit cast back
```

### Const

```zer
const u32 MAX = 100;
const []u8 NAME = "ZER";    // in .rodata (flash on embedded)
```

---

## Declarations

### Variables

```zer
u32 x;                      // auto-zeroed to 0
u32 y = 42;                 // explicit init
static u32 count;           // persists across calls
```

### Functions

```zer
u32 add(u32 a, u32 b) {
    return a + b;
}

?u32 safe_divide(u32 a, u32 b) {
    if (b == 0) { return null; }
    return a / b;
}

void no_return() {
    // ...
}
```

### Structs

```zer
struct Task {
    u32 pid;
    []u8 name;
    u32 priority;
}

Task t;                      // no 'struct' prefix in usage
t.pid = 42;
```

### Packed Structs

```zer
packed struct SensorPacket {
    u8  id;
    u16 temperature;         // unaligned — ZER handles safely
    u8  checksum;
}   // exactly 4 bytes, no padding
```

### Enums

```zer
enum State { idle, running, blocked, done }

State s = State.idle;        // qualified access

switch (s) {
    .idle    => { start(); }
    .running => { work(); }
    .blocked => { wait(); }
    .done    => { finish(); }
}
```

Explicit values and gaps (like C):

```zer
enum ErrorCode { ok = 0, warn = 100, err, fatal }
// ok=0, warn=100, err=101 (auto), fatal=102 (auto)
```

Enum switches must be exhaustive — missing a variant is a compile error.

### Tagged Unions

```zer
union Message {
    SensorData sensor;
    Command    command;
    Ack        ack;
}

Message msg;
msg.sensor = read_sensor();  // sets tag automatically

switch (msg) {
    .sensor  => |data| { process(data); }     // immutable capture
    .command => |*cmd| { cmd.x = 5; }         // mutable capture
    .ack     => |a|    { confirm(a); }
}

msg.sensor.temperature;      // COMPILE ERROR — must switch first
```

### Function Pointers

Same syntax as C. Optional function pointers use null sentinel (zero overhead).

```zer
u32 (*fn)(u32, u32) = add;                    // local variable
void (*callback)(u32 event);                   // global variable
struct Ops { u32 (*compute)(u32); }            // struct field
u32 apply(u32 (*op)(u32, u32), u32 x, u32 y); // parameter
?void (*on_event)(u32) = null;                 // optional — null = not set
typedef u32 (*BinOp)(u32, u32);                // function pointer typedef
```

Optional function pointers:

```zer
?void (*callback)(u32) = null;
callback = my_handler;           // set it
if (callback) |cb| { cb(42); }  // safe — unwrap before calling
```

### Distinct Types

```zer
typedef u32 Milliseconds;                // alias — interchangeable with u32
distinct typedef u32 Celsius;            // distinct — NOT interchangeable
distinct typedef u32 Fahrenheit;

Fahrenheit f = celsius_val;              // COMPILE ERROR
Fahrenheit f = @cast(Fahrenheit, c);     // explicit conversion
```

---

## Control Flow

### if / else

Braces always required. `else if` works like C.

```zer
if (x > 5) {
    do_thing();
}

if (a) {
    handle_a();
} else if (b) {
    handle_b();
} else {
    handle_neither();
    }
}
```

### for

No `++` or `--`. Use `+= 1`.

```zer
for (u32 i = 0; i < 10; i += 1) {
    process(i);
}
```

Loop variable `i` is scoped to the loop body.

### while

```zer
while (running) {
    poll();
}
```

### switch

Uses `=>` arrows. No `case` keyword. No fallthrough. No `break` needed.

```zer
// Enum — exhaustive, no default needed
switch (state) {
    .idle    => { start(); }
    .running => { work(); }
    .done    => { finish(); }
}

// Integer — default required
switch (code) {
    0 => { ok(); }
    1 => { retry(); }
    default => { error(); }
}

// Bool — exhaustive
switch (ready) {
    true  => { go(); }
    false => { wait(); }
}

// Multi-value arms
switch (val) {
    0, 1, 2 => { low(); }
    3, 4    => { high(); }
    default => { other(); }
}
```

### defer

Runs at scope exit, in reverse order. Fires on all exit paths.

```zer
void transfer() {
    mutex_lock(&lock);
    defer mutex_unlock(&lock);    // runs last
    cs_low();
    defer cs_high();              // runs first (reverse order)

    if (error) { return; }        // both defers fire
    do_work();
}
```

---

## Optional Unwrapping

### orelse

```zer
u32 val = get_value() orelse 0;          // default value
u32 val = get_value() orelse return;     // bare return (no value!)
u32 val = get_value() orelse break;      // exit loop
u32 val = get_value() orelse continue;   // skip iteration
u32 val = get_value() orelse {           // block fallback
    log_error();
    return;
};
```

**Critical:** `orelse return` is bare. `orelse return 1` is a parse error.

### if-unwrap

```zer
if (maybe_val) |v| {
    use(v);                              // v is the unwrapped value
}

if (maybe_ptr) |*p| {
    p.field = 5;                         // mutable capture
}
```

---

## Intrinsics

All intrinsics start with `@`.

### Type Conversion

| Intrinsic | Description | Example |
|-----------|-------------|---------|
| `@truncate(T, val)` | Keep low bits (big → small) | `@truncate(u8, big_u32)` |
| `@saturate(T, val)` | Clamp to T's min/max | `@saturate(i8, 200)` → 127 |
| `@bitcast(T, val)` | Reinterpret bits (same width) | `@bitcast(u32, signed_i32)` |
| `@cast(T, val)` | Distinct typedef conversion | `@cast(Fahrenheit, celsius)` |

### Pointer Operations

| Intrinsic | Description | Example |
|-----------|-------------|---------|
| `@inttoptr(*T, addr)` | Integer → pointer | `@inttoptr(*u32, 0x4000_0000)` |
| `@ptrtoint(ptr)` | Pointer → usize | `@ptrtoint(my_ptr)` |
| `@ptrcast(*T, ptr)` | Pointer type cast | `@ptrcast(*Task, opaque_ptr)` |

### Struct Operations

| Intrinsic | Description | Example |
|-----------|-------------|---------|
| `@size(T)` | sizeof → usize | `@size(Task)` |
| `@offset(T, field)` | offsetof → usize | `@offset(Task, priority)` |
| `@container(*T, ptr, field)` | container_of | `@container(*Task, field_ptr, name)` |

### Memory Barriers

| Intrinsic | Description | Emitted C |
|-----------|-------------|-----------|
| `@barrier()` | Full memory barrier | `__atomic_thread_fence(__ATOMIC_SEQ_CST)` |
| `@barrier_store()` | Store barrier | `__atomic_thread_fence(__ATOMIC_RELEASE)` |
| `@barrier_load()` | Load barrier | `__atomic_thread_fence(__ATOMIC_ACQUIRE)` |

### Other

| Intrinsic | Description |
|-----------|-------------|
| `@cstr(buf, slice)` | Copy `[]u8` into buffer + NUL terminator for C interop |
| `@config(key, default)` | Build-time constant from `--config` flag |
| `@trap()` | Intentional crash — calls `_zer_trap` |

---

## Builtin Types

### Pool(T, N) — Fixed-Slot Allocator

Must be global. Pre-allocated array of N slots with generation counters.

```zer
Pool(Task, 8) tasks;

// Allocate — returns Handle or null
Handle(Task) h = tasks.alloc() orelse return;

// Access — must use inline, cannot store result
tasks.get(h).priority = 5;

// Free — handle consumed, use-after-free caught by ZER-CHECK
tasks.free(h);
```

| Method | Return | Description |
|--------|--------|-------------|
| `.alloc()` | `?Handle(T)` | Allocate a slot |
| `.get(h)` | `*T` | Access by handle (generation checked) |
| `.free(h)` | `void` | Release slot, increment generation |

### Ring(T, N) — Circular Buffer

Must be global. ISR-safe with memory barriers.

```zer
Ring(u8, 256) rx_buf;

rx_buf.push(byte);                          // always succeeds, overwrites oldest
rx_buf.push_checked(byte) orelse { ... };   // returns ?void, null if full
if (rx_buf.pop()) |byte| { process(byte); } // returns ?T, null if empty
```

| Method | Return | Description |
|--------|--------|-------------|
| `.push(val)` | `void` | Push, overwrite oldest if full |
| `.push_checked(val)` | `?void` | Push, return null if full |
| `.pop()` | `?T` | Pop oldest, return null if empty |

### Arena — Bump Allocator

Uses developer-owned memory. No heap.

```zer
u8[4096] mem;
Arena ar = Arena.over(mem);

// Allocate struct (T must be struct/enum name, NOT primitive)
*Task t = ar.alloc(Task) orelse return;

// Allocate slice (T must be struct/enum name)
struct Elem { u32 val; }
?[]Elem items = ar.alloc_slice(Elem, 16);

// Reset
defer ar.reset();           // recommended — in defer
ar.unsafe_reset();          // no warning variant
```

| Method | Return | Description |
|--------|--------|-------------|
| `Arena.over(buf)` | `Arena` | Create arena over array or slice |
| `.alloc(T)` | `?*T` | Allocate one T (aligned) |
| `.alloc_slice(T, n)` | `?[]T` | Allocate n elements |
| `.reset()` | `void` | Reset offset to 0 |
| `.unsafe_reset()` | `void` | Reset without warning |

---

## Hardware Support

### Volatile / MMIO

```zer
volatile *u32 reg = @inttoptr(*u32, 0x4002_0014);
*reg = 0xFF;             // never optimized away
u32 val = *reg;          // never cached
```

### Interrupt Handlers

```zer
interrupt USART1 {
    u8 byte = @truncate(u8, UART1.DR);
    rx_buf.push(byte);
}

interrupt UART_1 as "USART1_IRQHandler" {   // explicit symbol name
    // ...
}
```

Emitted C: `void __attribute__((interrupt)) USART1_IRQHandler(void) { ... }`

### Bit Extraction / Set

```zer
u32 mode = reg[9..8];       // extract bits 9:8
reg[7..4] = 0x0F;           // set bits 7:4
u8 low_bit = val[0..0];     // single bit
```

### Inline Assembly

```zer
asm("cpsid i");              // disable interrupts
asm("wfi");                  // wait for interrupt
```

### Packed Structs

```zer
packed struct UART_Regs {
    u32 SR; u32 DR; u32 BRR; u32 CR1; u32 CR2; u32 CR3;
}
```

Emitted C: `__attribute__((packed))`. ZER handles unaligned access safely.

---

## Modules

```zer
import uart;                 // imports uart.zer from same directory
import gpio;

uart_init(9600);             // direct access to exported functions
```

- Functions are visible to importers by default
- `static` functions are internal only
- Circular imports are a compile error
- No header files, no forward declarations needed

---

## C Interop

### Calling C from ZER

```zer
const []u8 path = "data.bin";
u8[64] cbuf;
?*opaque f = c_fopen(@cstr(cbuf, path), "rb");
if (f) |file| { use(file); }
```

### Type Mapping

| ZER | C | Notes |
|-----|---|-------|
| `u8, u16, u32, u64` | `uint8_t, uint16_t, uint32_t, uint64_t` | Identical |
| `i8, i16, i32, i64` | `int8_t, int16_t, int32_t, int64_t` | Identical |
| `*T` | `T*` | Non-null both sides |
| `?*T` | `T*` (nullable) | ZER wraps as optional |
| `*opaque` | `void*` | Same |
| `[]u8` | `struct { uint8_t* ptr; size_t len; }` | Slice |
| `bool` | `uint8_t` | NOT an integer in ZER |

### --lib Flag

```bash
zerc module.zer --lib        # no preamble/runtime, for C interop
```

---

## Operators

### Arithmetic

`+` `-` `*` `/` `%` — all integer overflow wraps (defined, never UB).

### Bitwise

`&` `|` `^` `~` `<<` `>>` — shift by >= width returns 0 (defined).

### Comparison

`==` `!=` `<` `>` `<=` `>=`

### Logical

`&&` `||` `!`

### Assignment

`=` `+=` `-=` `*=` `/=` `%=` `&=` `|=` `^=` `<<=` `>>=`

### NOT in ZER

`++` `--` — use `+= 1` / `-= 1` instead.

---

## Safety Guarantees

| Bug Class | How ZER Prevents It |
|-----------|-------------------|
| Buffer overflow | Bounds check on every array/slice access |
| Use-after-free | Handle generation counter + ZER-CHECK |
| Null dereference | `*T` is non-null by type, `?*T` forces unwrap |
| Uninitialized memory | Everything auto-zeroed |
| Integer overflow | Wraps (defined), never UB |
| Silent truncation | Must `@truncate` or `@saturate` explicitly |
| Missing switch case | Exhaustive check for enums and bools |
| Dangling pointer | Scope escape analysis |

---

## Compiler

### Build

```bash
make            # build zerc compiler
make zer-lsp    # build language server
make check      # run all 905 tests + 491 fuzz
make release    # release binaries in release/
```

### Usage

```bash
zerc source.zer              # compile to C
zerc source.zer --run        # compile + run
zerc source.zer --lib        # library mode (no preamble)
zerc source.zer --config DEBUG=true
```

### Pipeline

```
source.zer → Lexer → Parser → AST → Checker → ZER-CHECK → Emitter → .c → GCC → binary
```

### Keywords (50)

**Types:** `u8` `u16` `u32` `u64` `i8` `i16` `i32` `i64` `usize` `f32` `f64` `bool` `void` `opaque`

**Declarations:** `struct` `packed` `enum` `union` `const` `typedef` `distinct`

**Control:** `if` `else` `for` `while` `switch` `break` `continue` `return` `default`

**Error handling:** `orelse` `null` `true` `false`

**Memory:** `Pool` `Ring` `Arena` `Handle`

**Special:** `defer` `import` `volatile` `interrupt` `asm` `static` `keep`

---

## What ZER Does NOT Have

- No classes, inheritance, templates, generics
- No exceptions, try/catch
- No garbage collector
- No heap / malloc / free
- No implicit narrowing or sign conversion
- No undefined behavior
- No `++` / `--`, no comma operator, no `goto`
- No C-style casts
- No header files (use `import`)
- No preprocessor (#define, #ifdef)
- No `else if` (nest instead)
- No `float` switch

---

## Full Example — UART Driver

```zer
import gpio;

packed struct UART_Regs {
    u32 SR; u32 DR; u32 BRR; u32 CR1; u32 CR2; u32 CR3;
}

volatile *UART_Regs UART1 = @inttoptr(*UART_Regs, 0x4001_1000);
Ring(u8, 256) rx_buf;
Ring(u8, 256) tx_buf;

void uart_init(u32 baud) {
    gpio.configure(gpio.PA9, gpio.AF7);
    gpio.configure(gpio.PA10, gpio.AF7);
    UART1.BRR = 16000000 / baud;
    UART1.CR1 = (1 << 13) | (1 << 3) | (1 << 2) | (1 << 5);
}

interrupt USART1 {
    u32 sr = UART1.SR;
    if (sr & (1 << 5)) {
        u8 byte = @truncate(u8, UART1.DR);
        rx_buf.push(byte);
    }
    if (sr & (1 << 7)) {
        if (tx_buf.pop()) |byte| {
            UART1.DR = byte;
        } else {
            UART1.CR1 &= ~(1 << 7);
        }
    }
}

?u32 uart_read([]u8 buf) {
    u32 count = 0;
    while (count < buf.len) {
        if (rx_buf.pop()) |byte| {
            buf[count] = byte;
            count += 1;
        } else {
            break;
        }
    }
    if (count == 0) { return null; }
    return count;
}

u32 uart_write([]u8 data) {
    for (u32 i = 0; i < data.len; i += 1) {
        tx_buf.push(data[i]);
    }
    UART1.CR1 |= (1 << 7);
    return @truncate(u32, data.len);
}
```

---

*ZER — Memory-safe C. Same syntax, same mental model. The compiler does the safety work.*
