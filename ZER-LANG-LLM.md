# ZER-LANG — LLM Reference

ZER is C with compile-time safety. Same types, same control flow, same mental model. The differences from C are small but strict — the compiler rejects invalid syntax with no recovery.

## CRITICAL: These are NOT C

```
WRONG (C syntax)          → RIGHT (ZER syntax)
─────────────────────────────────────────────
i++                       → i += 1
i--                       → i -= 1
(u32)x                    → @truncate(u32, x)
(int8_t)x                 → @saturate(i8, x)
struct Task t;            → Task t;
int arr[10];              → i32[10] arr;
int *ptr;                 → *i32 ptr;
NULL                      → null
if (x > 5) return 1;     → if (x > 5) { return 1; }
case 1: break;            → .value => { }
switch(x) { case 1: }    → switch (x) { .value => { } default => { } }
```

## Types

```
Unsigned:  u8  u16  u32  u64  usize
Signed:    i8  i16  i32  i64
Float:     f32  f64
Other:     bool  void
Pointer:   *T          (non-null guaranteed)
Optional:  ?T          (value: struct {T value; bool has_value})
           ?*T         (nullable pointer, null sentinel)
           ?void       (struct {bool has_value}, NO .value field)
Array:     T[N] name   (size between type and name)
Slice:     []T         (struct {*T ptr; usize len})
Opaque:    *opaque     (type-erased pointer, like void*)
Handle:    Handle(T)   (u64: index + generation counter)
```

## Declarations

```zer
// Variables — type before name, no struct keyword in usage
u32 count = 0;
*Task ptr = &task;
?*Task maybe = null;
u8[256] buffer;
[]u8 slice = "hello";
const u32 MAX = 100;
volatile *u32 reg = @inttoptr(*u32, 0x40020000);

// Structs — keyword only in declaration, not usage
struct Task {
    u32 id;
    u32 priority;
}
Task t;              // NOT: struct Task t;
*Task p = &t;

// Enums — qualified access with dot
enum State { idle, running, done }
State s = State.idle;

// Unions — tagged, switch to access
union Message {
    u32 integer;
    bool flag;
}

// Functions
u32 add(u32 a, u32 b) {
    return a + b;
}

// Forward declaration (no body)
u32 external_func(u32 x);

// Function pointers
u32 (*callback)(u32, u32) = add;
```

## Control Flow

```zer
// if — braces ALWAYS required
if (x > 5) {
    return 1;
}

// else if — supported
if (a) {
    // ...
} else if (b) {
    // ...
} else {
    // ...
}

// for — no ++, use += 1
for (u32 i = 0; i < 10; i += 1) {
    // body
}

// while
while (running) {
    // body
}

// switch — => arrows, no case keyword, no fallthrough
switch (state) {
    .idle => { start(); }
    .running => { poll(); }
    .done => { cleanup(); }
}

// switch on integers — needs default
switch (code) {
    1 => { handle_one(); }
    2, 3 => { handle_two_three(); }
    default => { handle_other(); }
}
```

## Optionals

```zer
// Optional value
?u32 result = 42;       // has value
?u32 empty = null;      // no value

// Unwrap with orelse
u32 val = get_value() orelse return;     // bare return, no value after return
u32 val = get_value() orelse 0;          // default value
u32 val = get_value() orelse { cleanup(); return; };  // block

// If-unwrap with capture
if (maybe_val) |v| {
    use(v);              // v is the unwrapped value
}

// Mutable capture
if (maybe_val) |*v| {
    v.field = 5;         // modify through pointer
}

// WRONG: orelse return with value
u32 val = get() orelse return 1;   // PARSE ERROR — bare return only
```

## Intrinsics (@ builtins)

```zer
@size(T)                    // sizeof
@truncate(T, val)           // narrow: keep low bits
@saturate(T, val)           // clamp to T's range
@bitcast(T, val)            // reinterpret bits (same width)
@cast(T, val)               // distinct typedef conversion
@inttoptr(*T, addr)         // integer to MMIO pointer
@ptrtoint(ptr)              // pointer to usize
@ptrcast(*T, ptr)           // pointer type cast
@offset(T, field)           // offsetof
@container(*T, ptr, field)  // container_of
@trap()                     // crash
@probe(addr)                // safe MMIO read → ?u32
@barrier()                  // full memory barrier
@cstr(buf, slice)           // copy slice to null-terminated buffer
```

## Builtin Containers

```zer
// Pool — fixed-size slot allocator, MUST be global
Pool(Task, 8) tasks;
Handle(Task) h = tasks.alloc() orelse return;
tasks.get(h).id = 1;        // inline access only, cannot store result
tasks.free(h);

// Slab — dynamic growable pool, MUST be global
Slab(Task) store;
Handle(Task) h = store.alloc() orelse return;
store.get(h).id = 1;        // same API as Pool
store.free(h);

// Ring — circular buffer, MUST be global
Ring(u8, 256) rx_buf;
rx_buf.push(byte);
u8 val = rx_buf.pop() orelse return;

// Arena — bump allocator
u8[4096] backing;
Arena scratch = Arena.over(backing);
*Task t = scratch.alloc(Task) orelse return;  // T must be struct name, NOT primitive
scratch.reset();

// WRONG — these are common mistakes:
*Task t = tasks.get(h);     // ERROR: get() result is non-storable, use inline
Pool(Task, 8) local_pool;   // ERROR: Pool/Slab/Ring must be global/static
arena.alloc(u32);            // ERROR: primitive keyword, use struct wrapper
```

## Defer

```zer
void process() {
    *Resource r = acquire();
    defer release(r);        // runs on ALL exit paths
    if (error) { return; }   // defer fires here
    // defer fires here too
}
```

## Comptime

```zer
// Compile-time functions — zero runtime cost
comptime u32 BIT(u32 n) {
    return 1 << n;
}
u32 mask = BIT(3);           // → 8 at compile time

// Conditional compilation
comptime if (1) {
    // compiled
} else {
    // stripped entirely
}
```

## C Interop

```zer
// Include C header (makes it available to GCC, NOT to ZER checker)
cinclude "my_header.h";

// Declare C functions with ZER types — checker needs these
i32 printf(const *u8 fmt);
i32 puts(const *u8 s);
void exit(i32 code);

// Call with auto .ptr coercion (extern functions only)
puts("hello");               // []u8 auto-coerces to *u8 for extern

// For ZER functions WITH bodies, .ptr is explicit
void my_func(*u8 s) { /* ... */ }
my_func("hello".ptr);        // must use .ptr
```

## Module System

```zer
// file: math.zer
u32 add(u32 a, u32 b) {
    return a + b;
}

// file: main.zer
import math;
u32 main() {
    return add(1, 2);
}
```

## MMIO and Hardware

```zer
// Declare valid MMIO address ranges (MANDATORY for @inttoptr)
mmio 0x40020000..0x40020FFF;
mmio 0x40011000..0x4001103F;

// MMIO register access
volatile *u32 gpio_odr = @inttoptr(*u32, 0x40020014);  // must be aligned
*gpio_odr = 0x01;                                       // write register
u32 val = *gpio_odr;                                    // read register

// Bit extraction and bit-set
u32 bits = gpio_odr[9..8];         // extract bits [9:8]
gpio_odr[3..0] = 0xF;              // set bits [3:0]

// Safe MMIO probe — returns ?u32, null if address faults
?u32 probe_result = @probe(0x40020000);
if (probe_result) |val| {
    // address is readable
}

// @inttoptr rules:
//   - address must be inside a declared mmio range (or --no-strict-mmio flag)
//   - address must be aligned to type (u32 = 4-byte aligned)
//   - variable addresses get runtime range check + trap
```

## Interrupts

```zer
// Interrupt handler — no params, no return
interrupt USART1 {
    u8 byte = *rx_reg;
    rx_buf.push(byte);
}

// Shared globals between ISR and main code:
//   - MUST be volatile (compile error otherwise)
//   - compound assign (|=, +=) in ISR on shared volatile → compile error (non-atomic RMW)
volatile u32 tick_count;
interrupt SysTick {
    tick_count += 1;        // ERROR if also accessed from non-ISR code with compound assign
}
```

## ASM and Low-Level Primitives

```zer
// @critical — disable interrupts for a block (per-architecture)
@critical {
    shared_counter += 1;    // safe: interrupts disabled
}

// @atomic operations — GCC __atomic builtins
@atomic_add(&counter, 1);
@atomic_sub(&counter, 1);
@atomic_or(&flags, 0x01);
@atomic_and(&flags, 0xFE);
@atomic_xor(&flags, 0x80);
u32 old = @atomic_load(&value);
@atomic_store(&value, 42);
bool swapped = @atomic_cas(&value, expected, desired);

// Extended inline asm — raw GCC syntax, only in naked functions
naked void isr_entry() {
    asm("push {r0-r3, lr}");
    asm("bl handler");
    asm("pop {r0-r3, pc}");
}

// naked — function with no prologue/epilogue
naked void _start() {
    asm("ldr sp, =_stack_top");
    asm("bl main");
}

// section — place function/variable in specific linker section
section(".isr_vector") u32[16] vector_table;
section(".ramfunc") void fast_copy() { }

// packed structs — no alignment padding
packed struct Packet {
    u8 id;
    u16 length;
    u8 checksum;
}

// Memory barriers
@barrier();              // full fence
@barrier_store();        // store fence
@barrier_load();         // load fence

// MISRA Dir 4.3: asm() only allowed inside naked functions
// Regular function with asm() → compile error
```

## Distinct Typedefs

```zer
// Distinct types — same underlying type, not interchangeable
distinct typedef u32 Celsius;
distinct typedef u32 Fahrenheit;

Celsius temp = @cast(Celsius, 100);     // wrap
u32 raw = @cast(u32, temp);             // unwrap
Fahrenheit f = @cast(Fahrenheit, temp); // ERROR: cross-distinct

// Distinct function pointers
distinct typedef u32 (*SafeOp)(u32, u32);
```

## What Does NOT Exist

- No `++` / `--`
- No `malloc` / `free` (use Pool, Slab, Ring, Arena)
- No `goto`
- No comma operator
- No C-style casts `(type)expr`
- No generics / templates (use `*opaque` + `@ptrcast`)
- No exceptions / try-catch
- No garbage collector
- No classes / inheritance
- No `#define` / `#ifdef` (use `comptime`)
- No header files (use `import`)
- No `char` type (use `u8`)
- No implicit narrowing or sign conversion
- No `NULL` (use `null`)
- No braceless if/else/for/while

## Common Patterns

```zer
// Hash map with open addressing
const u32 SIZE = 16;
Entry[16] table;
u32 slot = hash(key) % SIZE;   // slot proven [0, SIZE-1] by range propagation
table[slot].value = val;         // no bounds warning

// Linked list with Slab
struct Node {
    u32 data;
    ?Handle(Node) next;
}
Slab(Node) nodes;
struct List {
    ?Handle(Node) head;
}

// State machine
enum State { init, running, done }
State state = State.init;
switch (state) {
    .init => { state = State.running; }
    .running => {
        if (finished) { state = State.done; }
    }
    .done => { }
}

// Error propagation
?u32 parse_header([]u8 data) {
    u32 magic = read_u32(data) orelse return;
    if (magic != 0xDEAD) { return null; }
    u32 version = read_u32(data) orelse return;
    return version;
}

// MMIO register access (embedded)
mmio 0x40020000..0x40020FFF;
volatile *u32 gpio = @inttoptr(*u32, 0x40020014);
u32 bits = gpio[9..8];          // bit extraction
```

## Function Signature Rules for C Interop

```zer
// String parameter: const *u8 (C's const char *)
i32 puts(const *u8 s);

// Buffer parameter: []u8 or const []u8
void process(const []u8 data);

// Output pointer: *T
void get_result(*u32 out);

// Optional/nullable: ?*T
?*Task find_task(u32 id);
```

## Type Coercion Rules

```
Allowed implicit:
  u8 → u16 → u32 → u64        (widening)
  i8 → i16 → i32 → i64        (widening)
  T → ?T                       (wrap in optional)
  T[N] → []T                   (array to slice)
  mut → const (slice/pointer)  (add const)
  []T → *T                     (extern function calls ONLY, auto .ptr)

NOT allowed (compile error):
  u32 → u8                     (use @truncate or @saturate)
  i32 → u32                    (use @bitcast)
  *T → bool                    (use if (opt) |v| { })
  []T → *T                     (ZER functions — use .ptr explicitly)
  ?*T → *T                     (use orelse or if-unwrap)
```
