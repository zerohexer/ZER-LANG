# ZER Language Specification

**Full name:** ZEROHEXER (ZER)
**Status:** v0.1 complete — all compiler passes implemented and tested (851+ tests). Compiles multi-file ZER programs to C. LSP server available.
**Author:** ZEROHEXER
**Date:** 2026-03-19
**Goal:** Memory-safe C. No runtime. No LLVM. No excuses. Zero silent memory corruption on every board GCC supports. The compiler does the safety work — the developer writes C-style code.

---

## Table of Contents

1. [What ZER Is](#1-what-zer-is)
2. [Design Principles](#2-design-principles)
3. [What ZER Takes From C](#3-what-zer-takes-from-c)
4. [What ZER Changes From C](#4-what-zer-changes-from-c)
5. [Type System](#5-type-system)
6. [Pointer Model](#6-pointer-model)
7. [Pointer Arithmetic — The char* Problem Solved](#7-pointer-arithmetic--the-char-problem-solved)
8. [Safety Model — Prevention Not Detection](#8-safety-model--prevention-not-detection)
9. [Memory Model — Stack + Static + Builtins](#9-memory-model--stack--static--builtins)
10. [Strings — []u8 Is The String](#10-strings--u8-is-the-string)
11. [Error Handling — ?T + orelse](#11-error-handling--t--orelse)
12. [Structs, Enums, Unions](#12-structs-enums-unions)
13. [Function Pointers — Like C](#13-function-pointers--like-c)
14. [Hardware Support — Embedded First](#14-hardware-support--embedded-first)
15. [Modules — No Header Files](#15-modules--no-header-files)
16. [Intrinsics — @ Builtins](#16-intrinsics---builtins)
17. [Control Flow](#17-control-flow)
18. [Operators](#18-operators)
19. [Defer — Cleanup Without Goto](#19-defer--cleanup-without-goto)
20. [C Interop](#20-c-interop)
21. [Bounds Check — Safety vs Optimization](#21-bounds-check--safety-vs-optimization)
22. [Pool, Slab, Ring, Arena — Builtin Types](#22-pool-slab-ring-arena--builtin-types)
23. [What ZER Does NOT Have](#23-what-zer-does-not-have)
24. [Complete Keyword Reference](#24-complete-keyword-reference)
25. [Compiler Architecture](#25-compiler-architecture)
26. [Compiler Build Order](#26-compiler-build-order)
27. [Competitive Landscape](#27-competitive-landscape)
28. [Design Justifications](#28-design-justifications)
29. [Open Decisions](#29-open-decisions)
30. [Full Example — UART Driver](#30-full-example--uart-driver)
31. [Appendix A: ZER vs C Quick Reference](#appendix-a-zer-vs-c-quick-reference)
32. [Appendix B: Memory Layout on Cortex-M](#appendix-b-memory-layout-on-cortex-m)
33. [Appendix C: Why Not These Languages?](#appendix-c-why-not-these-languages)
34. [Appendix D: How Emit-C Actually Works](#appendix-d-how-emit-c-actually-works--concrete-examples)
35. [Appendix E: The char* Mathematical Proof](#appendix-e-the-char-mathematical-proof--full-exploration)
36. [Appendix F: *opaque Debug Tag Mechanism](#appendix-f-opaque-debug-tag-mechanism--full-detail)
37. [Appendix G: Implementation Reality](#appendix-g-implementation-reality--honest-assessment)

---

## 1. What ZER Is

ZER is a systems programming language designed to replace C for embedded and systems programming. It uses C syntax so C developers can read and write it immediately. The safety is underneath — the compiler enforces rules that C leaves to developer discipline.

ZER is NOT:
- A new paradigm (it's procedural, like C)
- A Rust competitor (no borrow checker, no ownership model)
- A C++ replacement (no classes, no inheritance, no templates)
- An academic exercise (it targets real embedded hardware)
- LLVM-dependent (own compiler, written in C, emits C or native)

ZER IS:
- C syntax with safety enforced by the compiler
- Embedded-first, freestanding from day one
- A research study by ZEROHEXER, used on real projects
- Open for anyone to use and contribute to

```
Safety spectrum:

  silent corruption          zero silent corruption
  simple                     complex

  C ----------- ZER -------------------- Rust
  |             |                         |
  silent bugs   compile error             compile error
  UB everywhere or runtime trap           always compile-time
                never silent              borrow checker

  ~32 keywords  ~50 keywords              ~53 keywords + concepts

  bugs ship     bugs caught               bugs caught
  silently      before shipping           before compiling
```

---

## 2. Design Principles

**Principle 1: C-inspired syntax, ZER safety.**
A C developer can READ ZER code in 5 minutes — type-first declarations, braces, semicolons, pointers, structs, same operators. WRITING ZER requires learning a few new constructs (slices `[]T`, optionals `?T`, `orelse`, `=> |val|` captures, `@` intrinsics). This is the lowest adoption barrier of any safe language, but it is not identical to C. The new constructs exist because they carry the safety.

**Principle 2: Prevention, not detection.**
The compiler does not detect bugs and ask the developer to rewrite. The language design makes bugs impossible to write in the first place. Arrays always carry length. Variables are always initialized. Overflow is always defined. The developer writes normal code — it's already safe.

**Principle 3: Freestanding first.**
ZER works on bare metal with no OS, no heap, no runtime. Stack and static memory only. Dynamic allocation (Pool, Slab, Ring, Arena) is opt-in via builtin types. Pool/Ring use compile-time-sized static memory. Slab grows dynamically via page allocation. No malloc. No free. No garbage collector.

**Principle 4: No LLVM. Own compiler.**
The ZER compiler is written in C. It is a single self-contained binary. No external dependencies. Targets: ARM Cortex-M (Thumb-2), RISC-V (RV32IM), x86_64, AVR. Initial strategy: emit C, let GCC handle backends. Later: own native backends.

**Principle 5: Explicit everything.**
No hidden behavior. No implicit conversions that lose data. No undefined behavior. Every operation does exactly what it says. If something is dangerous, you must spell it explicitly (@truncate, @bitcast, @ptrcast).

**Principle 6: Keep the compiler small.**
Target: ~13,500 lines of C. Smaller than most web apps. Every feature must justify its compiler cost. If a feature adds 5,000 lines of compiler complexity, it doesn't go in. This keeps the language maintainable by one person.

**Principle 7: C developers feel at home.**
A C developer should read ZER code and understand it in 5 minutes. Same mental model — pointers, structs, functions, manual memory. Writing ZER requires learning ~10 new constructs (slices, optionals, orelse, captures, intrinsics, defer, tagged unions, packed, distinct typedef, switch arrows). This is less than one afternoon of learning. No "fighting the compiler." No paradigm shift.

---

## 3. What ZER Takes From C

ZER uses C syntax for everything that C got right:

```
// Type declarations — type first, name second
u32 x = 5;
const u32 y = 10;
u8[256] buf;

// Functions — return type first, C-style parameters
u32 add(u32 a, u32 b) {
    return a + b;
}

// Structs — same layout as C
struct Task {
    u32 pid;
    u32 priority;
}

// Function pointers — C syntax
void (*callback)(i32 event, *opaque ctx);

// Control flow — identical to C
if (x > 5) { ... }
else { ... }
for (u32 i = 0; i < 10; i += 1) { ... }
while (running) { ... }
switch (state) { ... }

// Operators — same as C
+  -  *  /  %  &  |  ^  ~  <<  >>
==  !=  <  >  <=  >=

// Pointers — same concept, & and * operators
*Task t = &my_task;
u32 val = *ptr;

// Dot operator — struct field access AND UFCS function calls
t.priority = 5;              // field access (same as C)
t.run();                     // UFCS — desugars to run(&t)
                             // resolution:
                             //   1. is 'run' BOTH a field AND a function? → COMPILE ERROR
                             //   2. is 'run' a field of Task? → field access
                             //   3. is there fn run(*Task)? → rewrite to run(&t)
                             //   4. neither? → compile error
                             // ambiguity = error, not silent resolution.
                             // type of t disambiguates when multiple
                             // functions named 'run' exist across modules.

// Semicolons — statements end with ;
// Braces — blocks delimited by { }
// Comments — // and /* */
```

---

## 4. What ZER Changes From C

Same spelling, different rules underneath:

```
C code                          C behavior           ZER behavior
---------------------------------------------------------------------------
u8[10] arr;                     garbage values        all zeroed
arr[i] = 5;                     no check, may crash   bounds-checked
u32 x;                          uninitialized garbage  x = 0, always
*Task t = get_task();           might be null          can never be null
u32 y = x + 1; (at MAX)        undefined behavior     wraps (defined)
i64 big = 100000;
i16 small = big;                silent truncation      COMPILE ERROR — must use
                                                       @truncate or @saturate
switch(state) { case A: ... }   missing case = silent  must handle all cases
case A: ... case B:             implicit fallthrough   no fallthrough
```

---

## 5. Type System

### Primitive Types

```
u8   u16  u32  u64            unsigned integers
i8   i16  i32  i64            signed integers
usize                         unsigned, pointer-width (u32 on 32-bit, u64 on 64-bit)
f32  f64                      floating point
bool                          true / false
void                          return type only. cannot declare void variables.
```

### Compound Types

```
u8[256] buf;                  fixed array — size known at compile time
[]u8 data;                    slice — pointer + length, always bounded
*Task ptr;                    pointer — guaranteed non-null
?*Task maybe;                 optional pointer — might be null
?u32 result;                  optional value — might be null (any type)
*opaque raw;                  type-erased pointer (C's void*)
```

### Builtin Container Types

```
Pool(Task, 8) tasks;          fixed-slot allocator, 8 Task slots (compile-time bound)
Slab(Task) tasks;             dynamic slab allocator, grows on demand (no bound)
Ring(u8, 256) rx_buf;         circular buffer, 256 bytes
Arena scratch;                bump allocator over developer-owned memory
Handle(Task) h;               index + generation counter, not a pointer
```

### Integer Behavior

```
// Overflow: DEFINED. wraps by default. never undefined.
u32 x = 4294967295;          // u32 max
x = x + 1;                   // x = 0. wraps. defined. always.

// Small-to-big: IMPLICIT. always safe, never loses data.
u8 small = 42;
u32 big = small;              // big = 42. automatic. safe.

// Big-to-small: EXPLICIT. no default. developer states intent.
u32 big = 100000;
u16 small = big;              // COMPILE ERROR: narrowing conversion.
                              // must choose:
u16 a = @truncate(big);      // truncate (keep low bits). for protocol parsing,
                              // ADC downsampling, bit masking.
u16 b = @saturate(big);      // clamp to max (65535). for safe clamping.

// Signed/unsigned conversion: EXPLICIT required.
i32 x = -5;
u32 y = x;                   // COMPILE ERROR
u32 y = @bitcast(u32, x);    // OK — explicit. reinterprets bits.

// Shift by >= width: DEFINED as zero.
u32 x = 1 << 32;             // x = 0. always. every architecture.
```

### Type Coercions and Layout

```
ARRAY TO SLICE — implicit coercion:
  u8[256] buf;
  void process([]u8 data) { ... }
  process(buf);               // buf coerces to []u8 { .ptr = &buf[0], .len = 256 }

  Rule: T[N] implicitly coerces to []T. Length set to N.
  const T[N] coerces to const []T (immutable slice).

OPTIONAL TYPE LAYOUT:
  ?*T:     same size as pointer. null is the sentinel. zero overhead.
  ?u32:    struct { u32 value; u8 has_value; } = 5 bytes, padded to 8.
  ?u8:     struct { u8 value; u8 has_value; } = 2 bytes.
  ?bool:   2 bytes.
  ?void:   1 byte (has_value flag only, no value field). presence/absence only.

  In emitted C: typedef struct { T value; uint8_t has_value; } __zer_opt_T;
  For ?void:    typedef struct { uint8_t has_value; } __zer_opt_void;

@size AND @offset RETURN TYPE:
  usize — matches pointer width.
  32-bit target: usize = u32.
  64-bit target: usize = u64.
  Array .len is usize. Slice index is usize.

TYPEDEF — type alias:
  typedef u32 Milliseconds;           // alias. interchangeable with u32.

  distinct typedef u32 Celsius;       // distinct. NOT interchangeable.
  distinct typedef u32 Fahrenheit;    // distinct. NOT interchangeable.
  Fahrenheit f = celsius_val;         // COMPILE ERROR: distinct types.
  Fahrenheit f = @cast(Fahrenheit, celsius_val);  // explicit. same bits, different type.

  // @cast on non-distinct typedef (alias): no-op. compiles. does nothing.
  // @cast on unrelated types: COMPILE ERROR.
  // @cast only valid between distinct typedefs of same underlying type.
```

### Scoping Rules

```
FOR LOOP VARIABLE — scoped to loop body (C99 semantics):
  for (u32 i = 0; i < 10; i += 1) {
      process(i);
  }
  process(i);                 // COMPILE ERROR: i not in scope.

STATIC LOCAL VARIABLES — supported, same as C:
  void isr_counter() {
      static u32 count;      // auto-zeroed ONCE at program start.
      count += 1;             // retains value across calls.
  }
```

### Const Coercion Rules

```
SAFE (allowed):
  u8[256] buf;                            // mutable array
  void read_only(const []u8 data) { ... } // takes const slice
  read_only(buf);                          // OK: mutable → const (safe)

UNSAFE (compile error):
  const u8[256] buf;                       // const array
  void process([]u8 data) { ... }          // takes mutable slice
  process(buf);                            // COMPILE ERROR: cannot drop const
                                           // same as C++ const correctness
```

### Named Capture Syntax — |val| and |*val|

Used with optional unwrapping and tagged union switching.

```
IMMUTABLE CAPTURE — |val| (copy, read-only):
  ?*Task maybe;
  if (maybe) |val| { ... }          // val is *Task (pointer copy, can deref to mutate target)

  ?u32 maybe_num;
  if (maybe_num) |val| { ... }      // val is u32 (value copy, immutable)

  switch (msg) {
      .sensor => |data| { ... }     // data is SensorData (copy, immutable)
  }

MUTABLE CAPTURE — |*val| (pointer to original, read-write):
  switch (msg) {
      .sensor => |*data| { ... }    // data is *SensorData (pointer to variant)
      // data.value = 5;            // OK — mutates the original union variant
  }

  // For optionals — |*val| gives mutable reference to the unwrapped value:
  ?u32 maybe_num = 5;
  if (maybe_num) |*val| {
      *val = 10;                    // mutates the original optional's value
  }

RULES:
  |val|  = copy. immutable. safe. default choice.
  |*val| = pointer to original. mutable. for when you need to modify in-place.
  For ?*T with |val|: val is *T. you can already mutate through the pointer.
  For ?*T with |*val|: val is **T. rarely needed.
```

### orelse Targets

```
u32 n = read(buf) orelse return;       // propagate failure
u32 n = read(buf) orelse break;        // exit loop
u32 n = read(buf) orelse continue;     // skip iteration
u32 n = read(buf) orelse 0;            // default value
queue.push_checked(cmd) orelse {       // block — multiple statements
    log_error("queue full");
    report_backpressure();
};

// Rule: orelse accepts:
//   - flow control keyword (return, break, continue)
//   - single expression (value)
//   - block { ... } (multiple statements)
```

### ?void — Presence-Only Optional

`?void` carries only presence/absence — no value payload. Used when a function can succeed or fail but has no return value.

```
?void push_checked(Ring *self, u8 val) {
    if (full) return null;     // failed — no space
    // ... push logic ...
    return;                    // success — void "value"
}

queue.push_checked(cmd) orelse report();    // orelse handles the null case

// Layout: ?void = 1 byte (has_value flag only). no value field.
// This does NOT violate "cannot declare void variables" —
// ?void is an optional, not a void variable. The ? wrapper makes it a valid type.
```

### Module Rules

```
CIRCULAR IMPORTS — rejected.
  // uart.zer imports gpio → OK
  // gpio.zer imports uart → COMPILE ERROR: circular import detected
  // Compiler builds dependency graph and rejects cycles.

STATIC — two meanings, determined by context (same as C):
  // Top-level: visibility. function/variable is module-private.
  static void configure_pins() { ... }    // not visible to importers

  // Inside function: storage duration. persists across calls.
  void isr_counter() {
      static u32 count;                    // auto-zeroed once. retains value.
      count += 1;
  }
  // Parser knows from position: top-level = visibility, inside function = storage.

OPAQUE — pointer qualifier, not standalone type.
  *opaque ptr;        // valid — type-erased pointer
  opaque x;           // COMPILE ERROR — opaque only valid as *opaque
```

---

## 6. Pointer Model

### keep — Pointer Parameter Storage Control

By default, pointer parameters CANNOT be stored, returned, or assigned to anything that outlives the function call. This is safe by default — the developer writes nothing, the compiler prevents dangling pointers across function boundaries.

`keep` explicitly marks a pointer parameter as storable. The compiler then checks the CALLER side — only static, global, or Pool-backed memory can satisfy a `keep` parameter. Both sides checked. No silent bugs.

```
DEFAULT — pointer parameters are non-storable:
  void process(*Task t) {
      t.priority = 5;         // OK — write THROUGH pointer. always fine.
      global_task = t;        // COMPILE ERROR: can't store parameter.
      return t;               // COMPILE ERROR: can't return parameter.
  }
  // 99% of code. no keyword. safe by default.

KEEP — parameter can be stored/returned:
  static *opaque saved_ctx;
  void register_cb(void (*cb)(*opaque), keep *opaque ctx) {
      global_cb = cb;          // function pointer is a value — storable.
      saved_ctx = ctx;         // OK — keep allows storage.
  }

CALLER SIDE — compiler checks what you pass to keep:
  void bad() {
      u32 local = 5;
      register_cb(handler, @ptrcast(*opaque, &local));
      // COMPILE ERROR: local cannot satisfy keep.
  }

  static u32 global_data = 5;
  void good() {
      register_cb(handler, @ptrcast(*opaque, &global_data));
      // OK — static satisfies keep.
  }

WHAT DOESN'T NEED KEEP:
  local pointers (not params):  compiler sees both sides directly.
  function pointers:            addresses in .text, live forever.
  vtable structs:               static const, assigned directly.
  Pool handles:                 values (u32/u64), not pointers.

WHAT NEEDS KEEP:
  callback context:             register(handler, keep *opaque ctx)
  that's the main case. ~1% of function parameters.

RULES SUMMARY:
  *T val          non-storable. use it, read it, write through it. default.
  keep *T val     storable. caller must pass static/global/pool memory.
  local pointers  no restriction. compiler sees both sides.
```

`keep` ships in v0.1. You can't write a driver without callback registration. You can't register callbacks without storing a context pointer. `keep` is not optional — it's required for real firmware from day one.

### Non-Null by Default

```
*Task t = get_task();         // t ALWAYS holds a valid address
*t.priority = 5;              // always safe. can never be null.

?*Task maybe = find_task();   // might be null. ? means optional.
*maybe.priority = 5;          // COMPILE ERROR: maybe might be null

// must unwrap optional pointer:
if (maybe) |t| {
    t.priority = 5;           // inside here, t is guaranteed non-null
}
```

### Type-Erased Pointer (*opaque)

Replaces C's void*. Cannot be dereferenced. Must be explicitly cast back.

```
*opaque raw = @ptrcast(*opaque, &my_task);    // erase type

// cannot dereference *opaque:
*raw = 5;                     // COMPILE ERROR: *opaque can't be dereferenced

// must cast back explicitly:
*Task t = @ptrcast(*Task, raw);   // explicit, visible, grep-able

// debug build: @ptrcast checks type tag.
//   stored *Task, cast to *Task → OK
//   stored *Task, cast to *Inode → trap with message
// release build: zero cost. same as C's (Task *)raw.
```

---

## 7. Pointer Arithmetic — The char* Problem Solved

### The Problem in C

C scales pointer arithmetic by sizeof(*ptr):

```
// C: ptr + N actually adds N * sizeof(*ptr) bytes
int *p = some_address;
p + 1;                        // adds 4 bytes (sizeof(int)), not 1 byte
p + 5;                        // adds 20 bytes, not 5

// To add ACTUAL bytes, C forces you to use char*:
(char *)p + 5;                // adds 5 bytes (sizeof(char) == 1)

// container_of REQUIRES char* because of this:
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
```

This exists because C merged two operations (pointer arithmetic and array indexing) into one syntax, then needed char* as an escape hatch where the scale factor is 1. sizeof(char) == 1 is the ONLY value that divides every possible offset — proven by mathematical necessity, not convention.

### Why char* Cannot Be Avoided in C

Every alternative was explored and all collapse back to char*:

- **Integer division to undo scaling**: fails when offset % sizeof(type) != 0 (e.g., offset 7 / sizeof(int) 4 = 1, gives 4 bytes not 7)
- **Padding to force divisibility**: wastes massive memory
- **Bitfield struct with sizeof==1**: `struct byte { unsigned b:8; }` → sizeof is 4 (storage unit of unsigned int), not 1. Using `char b:8` works but that's char again
- **Union with uintptr_t**: only works where sizeof(int) == sizeof(void*) — breaks on 64-bit (8-byte pointer, 4-byte int)
- **Inline assembly**: works but not portable across architectures

char* is not a workaround. It's a mathematical inevitability given C's pointer scaling rules. Only sizeof==1 divides every possible byte offset, and char is the only type the C standard guarantees to be size 1.

### ZER's Solution: Don't Scale

ZER separates pointer arithmetic from array indexing. The hardware never scales — ADD 24 means 24 bytes on every CPU ever made. C added scaling as an abstraction. ZER doesn't.

```
// ZER: pointer arithmetic is ALWAYS in bytes.
// Because that's what the hardware does.

*Task p = get_task();
*Task q = p + 24;            // adds 24 BYTES. not 24 * sizeof(Task).
                              // same as ADD 24 on ARM, RISC-V, x86, AVR.

// Array indexing is SEPARATE — compiler scales for you.
Task[10] arr;
Task third = arr[3];          // compiler reads at arr + 3 * sizeof(Task)
                              // this is indexing, not pointer math.

// These are two different operations:
p + 24                        // byte offset (what the CPU does)
arr[3]                        // element index (compiler scales)
```

### container_of Is a Builtin

```
// C: macro using char* subtraction
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

// ZER: compiler builtin. no char*. no macro. no offset math.
*Task task = @container(*Task, node_ptr, list);

// Compiler knows Task.list is at offset 20.
// Emits: SUB R0, R0, #20
// Same instruction on every architecture. No char*. No sizeof.
```

### Compilation Output — Every Architecture

```
ZER source:
    *Task task = @container(*Task, node_ptr, list);

Compiler knows: Task.list is at offset 20.

ARM Cortex-M:    SUB R0, R0, #20
RISC-V:          addi x10, x10, -20
x86_64:          sub rax, 20
AVR:             sbiw r26, 20

Same instruction. Same meaning. No char*.
Just: subtract 20. That's what the hardware always did.
```

---

## 8. Safety Model — Prevention Not Detection

ZER's safety is NOT a static analyzer. NOT a linter. NOT warnings you can ignore. The language itself makes bugs impossible to write. The developer writes normal code — it's already safe.

### Detection vs Prevention

```
DETECTION (what ZER does NOT do):
  developer writes bug → compiler says "fix it" → developer rewrites
  this is just a strict linter. developer does extra work.

PREVENTION (what ZER does):
  the bug CANNOT BE WRITTEN in the first place.
  the language design makes it impossible.
  developer writes normal code → it's already safe → done.
```

### Safety Feature 1: Slices Always Carry Length

```
// C: array decays to pointer. length is lost.
void process(int *data, int len) {    // hope len is correct
    data[5] = 42;                      // hope 5 < len
}

// ZER: array IS pointer + length. inseparable.
void process([]u32 data) {            // length travels with data
    data[5] = 42;                      // compiler inserts bounds check
}                                      // developer writes NOTHING extra
```

The developer didn't "add safety." They just wrote normal code. The language handles it.

### Safety Feature 2: No Uninitialized Memory

```
// C: uninitialized = garbage
int x;                                // garbage bits
int arr[100];                         // 100 garbage values

// ZER: everything is zero. always.
u32 x;                                // x = 0. automatically.
u32[100] arr;                         // all zeroed. automatically.
Task t;                               // all fields zeroed. automatically.

// There is no uninitialized state in ZER.
// The concept doesn't exist. Can't happen.
```

### Safety Feature 3: Non-Null Pointers

```
// C: any pointer can be null. surprise.
int *p = get_value();
*p = 5;                               // maybe null. maybe crash.

// ZER: *T can never be null. ?*T might be null.
*i32 p = get_value();                  // always valid. compiler guarantees.
*p = 5;                                // always safe.

?*i32 maybe = find_value();            // ? means might be null
*maybe = 5;                            // COMPILE ERROR: might be null

if (maybe) |val| {
    *val = 5;                          // safe. val guaranteed non-null.
}
```

### Safety Feature 4: Defined Integer Overflow

```
// C: signed overflow is undefined behavior.
// compiler can LITERALLY do anything. can delete your security check.
int x = INT_MAX;
x = x + 1;                            // C: undefined.

// ZER: overflow wraps. always. defined. every architecture.
u32 x = 4294967295;
x = x + 1;                            // x = 0. wraps. defined.
```

### Safety Feature 5: Explicit Narrowing Casts

```
// C: silently truncates. developer may not intend this.
long x = 100000;
short y = (short)x;                    // truncated. silent. maybe a bug.

// ZER: narrowing requires explicit choice. no default.
i32 x = 100000;
i16 y = x;                            // COMPILE ERROR: narrowing conversion
i16 y = @truncate(x);                 // truncate (keep low bits). explicit.
i16 y = @saturate(x);                 // clamp to i16 max/min. explicit.

// Developer states intent. Compiler enforces.
// No silent truncation. No surprising saturation.
// Embedded protocol parsing uses @truncate (intentional bit-width reduction).
// Safe clamping uses @saturate (prevent overflow).
```

### Safety Feature 6: Exhaustive Switch

```
// C: missing case is silent.
enum state { QUEUED, RUNNING, DONE };
switch(s) {
    case QUEUED: ...; break;
    case RUNNING: ...; break;
    // forgot DONE. C: "fine."
}

// ZER: must handle every case.
switch (s) {
    QUEUED => ...,
    RUNNING => ...,
}
// COMPILE ERROR: switch does not handle DONE
```

### Safety Feature 7: No Implicit Fallthrough

```
// C: fallthrough is silent.
switch(x) {
    case 1: do_thing();      // forgot break. falls into case 2.
    case 2: other_thing();
}

// ZER: each case is isolated. no fallthrough. ever.
switch (x) {
    1 => do_thing(),
    2 => other_thing(),
}
```

### Switch Rules by Type

```
switch on enum:      EXHAUSTIVE. must handle every variant. no default needed.
switch on bool:      EXHAUSTIVE. must handle true and false.
switch on integer:   MUST have default arm. cannot enumerate 4 billion cases.
switch on float:     NOT ALLOWED. compile error. use if/else chains.

// Integer switch example:
switch (error_code) {
    0 => ok(),
    1 => retry(),
    2 => abort(),
    default => unknown_error(error_code),   // required for integer switch
}

// Bool switch example:
switch (ready) {
    true => proceed(),
    false => wait(),       // exhaustive: both cases handled
}

// Multi-value arms — multiple values share one handler:
switch (reg_value & 0x07) {
    0, 1, 2 => idle(),        // any of these → idle
    3, 4 => active(),         // 3 or 4 → active
    default => error(),
}
```

### CVE Coverage

```
Bug class                   % of CVEs    ZER prevents it?
-----------------------------------------------------------
Buffer overflow             ~25%         YES — bounds checking
Use of uninitialized mem    ~8%          YES — auto-zero
Integer overflow            ~10%         YES — defined behavior
Null dereference            ~7%          YES — non-null types
Type confusion              ~5%          YES — no implicit casts
Format string               ~2%          YES — checked at compile
Off-by-one                  ~5%          YES — bounds checking

ZER SAFETY GUARANTEE: no silent memory corruption. ever.

  Every memory bug is either:
    COMPILE ERROR — developer fixes before running.
    RUNTIME TRAP  — program stops, tells you where and why.

  Never:
    SILENT CORRUPTION — data wrong, nobody knows, ships broken.
    UNDEFINED BEHAVIOR — compiler deletes your security check.

  Compile errors catch:
    Buffer overflow, null deref, type confusion, uninitialized memory,
    integer overflow, use-after-free (local handles), double free,
    scope escape, return pointer to local, store pointer to expired scope.

  Runtime traps catch:
    Use-after-free (handles in arrays/structs) — generation counter.
    Traps are loud. program stops. diagnostic message with file and line.
    Even untested code paths cannot silently corrupt — they trap.

  Automatic (builtins handle it):
    Data races on Ring/Pool ISR patterns — internal barriers.

  Manual (developer responsibility):
    Data races on raw shared variables — @barrier intrinsic.

  Cannot catch (no language can):
    Logic bugs.
```

---

## 9. Memory Model — Stack + Static + Builtins

### Freestanding Memory — No Heap Required

ZER's core language uses only stack and static memory. No malloc. No free. No heap. No garbage collector. Same as freestanding C.

```
LANGUAGE LEVEL (always available, no opt-in):
  stack variables — auto-zeroed, bounds-checked
  static globals — auto-zeroed
  fixed arrays — size known at compile time
  slices — pointer + length view into existing memory

BUILTIN TYPES (always available, use static memory):
  Pool(T, N) — fixed-slot allocator over static RAM
  Ring(T, N) — circular buffer over static RAM
  Arena — bump allocator over developer-declared memory
  Handle(T) — index + generation counter, prevents use-after-free

These builtins are NOT a library that needs an OS.
They are source files that compile to plain instructions.
No malloc. No heap. No syscall. No runtime.
The memory is INSIDE the struct — static RAM.
Same as Linux kernel's list.h — freestanding code.
```

### Typical Embedded Memory Layout

```
STATIC (lives forever):
  Pool(Task, 8) tasks;           // 8 Task slots in .bss
  Ring(u8, 256) uart_rx;         // 256 byte ring in .bss
  u32 tick_count;                // counter in .bss

STACK (per function call, dies on return):
  u8[64] scratch;                // temporary buffer
  Task t;                        // local struct
  []u8 msg;                      // slice into scratch

ALL ZEROED. ALL BOUNDED. ALL FREESTANDING.
No heap. No malloc. No free. No leak possible.
```

### platform_mem — The Universal Memory Plug

Every platform provides ONE function: `platform_mem(u32 size) -> ?[]u8`. This is the only thing that changes per platform. Pool, Ring, and Arena don't care where memory comes from — they manage whatever `[]u8` they're given.

```
BARE METAL:    platform_mem returns a slice of static RAM
RTOS:          platform_mem wraps pvPortMalloc / k_malloc
LINUX KERNEL:  platform_mem wraps the kernel page allocator
LINUX USER:    platform_mem wraps mmap
WINDOWS:       platform_mem wraps VirtualAlloc
```

Each platform implementation is ~5-20 lines. Ships with the ZER compiler per `--platform` target. Developer never writes it.

```
// Pool and Ring are ALWAYS static .bss. They never call platform_mem.
// platform_mem is for Arena when you need OS-backed memory.

Pool(Task, 8) tasks;              // static .bss. always. no platform_mem.
Ring(u8, 256) rx;                 // static .bss. always. no platform_mem.
Arena.over(static_buf);           // over developer-declared static memory. no platform_mem.

// platform_mem is for when YOU want dynamic Arena backing:
?[]u8 pages = platform_mem(65536);
Arena heap = Arena.over(pages orelse panic("oom"));
// heap Arena uses OS-provided pages. YOU called platform_mem.
// Pool and Ring never do this. They are always static.
```

```
COMPARISON:

  C:   5 different allocator APIs across platforms
       malloc, kmalloc, pvPortMalloc, VirtualAlloc, mmap
       each with different semantics, different free functions
       developer must learn each platform's allocator

  ZER: 1 API everywhere
       Pool and Arena
       platform_mem is the ONLY thing that changes
       and it ships pre-written per target
```

---

## 10. Strings — []u8 Is The String

ZER has no string type. A "string" is `[]u8` — a slice of bytes. Same bytes as C, same memory locations, but with length always known and bounds always checked.

```
// C string: pointer to bytes, null-terminated, no length.
char *s = "hello";             // must scan for \0 to find length
s[100];                        // reads garbage. no check.

// ZER string: slice of bytes. length known. bounds-checked.
[]u8 s = "hello";             // s.ptr = address, s.len = 5
s[100];                        // trap. bounds-checked.
s[0..3];                       // "hel" — sub-slice. no copy.
```

### String Patterns

```
// String literal — stored in .rodata (flash on embedded)
const []u8 msg = "hello";     // in flash. zero RAM used. immutable.

// String in stack buffer
u8[128] buf;
[]u8 msg = format(buf, "temp={} pressure={}", temp, pres);
uart_write(msg);               // msg is a slice into buf. no heap.

// String table in flash
const []u8 ERRORS[4] = { "OK", "TIMEOUT", "CRC_FAIL", "OVERFLOW" };
uart_write(ERRORS[code]);     // bounded. checked.

// C interop — when you need null terminator
const []u8 name = "config.txt";
u8[64] cbuf;
*u8 cname = @cstr(cbuf, name);   // copies + adds \0, bounds-checked
c_fopen(cname, "r");              // pass to C function
```

### Encoding

ZER doesn't know or care about encoding. Just bytes. Same as C. Embedded doesn't need Unicode. `[]u8` is sufficient.

---

## 11. Error Handling — ?T + orelse

ZER extends `?` from optional pointers to any type. A function that might fail returns `?T`. The caller MUST handle the null case — the compiler enforces this.

```
// Function that might fail:
?u32 uart_read([]u8 buf) {
    if (no_data()) return null;
    return bytes_read;
}

// Caller MUST handle null:
u32 n = uart_read(buf) orelse return;    // propagate failure up
u32 n = uart_read(buf) orelse 0;         // default value

if (uart_read(buf)) |n| {
    process(buf[0..n]);                   // n guaranteed valid
}

// CANNOT ignore:
u32 n = uart_read(buf);     // COMPILE ERROR: ?u32 is not u32. must unwrap.
```

### Error Reason

```
// When you need to know WHY something failed:
enum UartError {
    none,
    timeout,
    framing,
    overrun,
}

?u32 uart_read([]u8 buf, *UartError err) {
    if (timeout()) { *err = UartError.timeout; return null; }
    if (framing()) { *err = UartError.framing; return null; }
    *err = UartError.none;
    return bytes_read;
}

// Caller:
UartError err;
if (uart_read(buf, &err)) |n| {
    process(buf[0..n]);
} else {
    switch (err) {
        .timeout => retry(),
        .framing => reset_uart(),
        .overrun => clear_fifo(),
        .none => {},
    }
}
```

---

## 12. Structs, Enums, Unions

### Structs

```
struct Task {
    u32 pid;
    []u8 name;
    TaskState state;
    u32 priority;
}

// Usage:
Task t;                       // all fields zeroed
t.pid = 42;
t.name = "sensor_read";
t.priority = 3;
```

### Packed Structs

For protocol parsing and hardware registers. No padding between fields. ZER guarantees safe access on all architectures — the compiler emits byte-by-byte reads/writes for unaligned fields. No alignment fault on any ARM variant (M0, M3, M4, M7), even with CCR.UNALIGN_TRP set.

```
packed struct SensorPacket {
    u8 id;
    u16 temperature;          // at byte offset 1 — unaligned. ZER handles it.
    u16 pressure;
    u8 checksum;
}   // exactly 6 bytes. guaranteed. safe on all architectures.

// Parse bytes from UART:
*SensorPacket pkt = @ptrcast(*SensorPacket, raw_bytes.ptr);
u16 temp = pkt.temperature;
```

### Enums

```
enum TaskState {
    idle,
    running,
    blocked,
    done,
}

// Switch must be exhaustive:
switch (state) {
    .idle => start(),
    .running => continue(),
    .blocked => wait(),
    .done => cleanup(),
}
// Missing a case = COMPILE ERROR.
```

### Tagged Unions

For variant types. Compiler enforces you check which variant before accessing.

```
union Message {
    SensorData sensor;
    Command command;
    Ack ack;
}

// CONSTRUCTION — assign to variant field, compiler sets tag automatically:
Message msg;
msg.sensor = read_sensor();   // compiler inserts: msg._tag = 0

// Cannot READ wrong variant:
msg.sensor.temperature;       // COMPILE ERROR: must switch first

// Must switch to read:
switch (msg) {
    .sensor => |data| { process_sensor(data); },
    .command => |cmd| { execute(cmd); },
    .ack => |a| { confirm(a); },
}

// WRITE is allowed — it changes the active variant:
msg.command = parse_cmd();    // compiler inserts: msg._tag = 1
// Now msg is a Command, not a SensorData.
```

---

## 13. Function Pointers — Like C

No methods. No self. No this. No virtual. No classes. Just structs with function pointers — same as C, same as the Linux kernel.

### Basic Function Pointer

```
void (*callback)(i32 event, *opaque ctx);

// Assign:
callback = my_handler;

// Call:
callback(42, @ptrcast(*opaque, &my_data));
```

### Vtable Pattern (Linux kernel style)

```
struct FileOps {
    ?i32 (*open)(*File);
    ?u32 (*read)(*File, []u8);
    ?u32 (*write)(*File, []u8);
    void (*close)(*File);
}

// UART implementation:
?i32 uart_open(*File f) { ... }
?u32 uart_read(*File f, []u8 buf) { ... }
?u32 uart_write(*File f, []u8 buf) { ... }
void uart_close(*File f) { ... }

const FileOps uart_ops = {
    .open  = uart_open,
    .read  = uart_read,
    .write = uart_write,
    .close = uart_close,
};

// SPI implementation:
const FileOps spi_ops = {
    .open  = spi_open,
    .read  = spi_read,
    .write = spi_write,
    .close = spi_close,
};

// Polymorphism:
void transfer(*FileOps ops, *File f, []u8 data) {
    ops.write(f, data);       // calls uart_write or spi_write
}                              // one instruction: BLX R3 (ARM)
                               // no runtime. no class. no magic.
```

This is NOT C++ virtual methods. It's a struct with function pointers. Set them. Call them. The lowest-level polymorphism possible. Same pattern used in Linux kernel's `file_operations`, `proto_ops`, `block_device_operations`, etc.

---

## 14. Hardware Support — Embedded First

### Volatile

```
// Hardware registers are memory-mapped. Must not be optimized away.
volatile *u32 reg = @inttoptr(*u32, 0x4002_0014);

*reg = 0xFF;                  // compiler NEVER removes this write
u32 val = *reg;               // compiler NEVER caches this read
```

### Interrupt Handlers

```
// Compiler generates correct prologue/epilogue (save/restore regs).
// Default: compiler looks up platform table for handler symbol name.
interrupt SysTick {
    tick_count += 1;
}

interrupt UART_RX {
    uart_rx.push(UART_DR);
}

// Explicit rename: when platform table doesn't have your name,
// or when you want a custom mapping:
interrupt UART_1 as "USART1_IRQHandler" {
    uart_rx.push(UART_DR);
}

// If name not in platform table AND no 'as' clause → COMPILE ERROR.
```

### Inline Assembly

```
// When you need direct hardware control:
asm("cpsid i");               // disable interrupts (ARM)
asm("wfi");                   // wait for interrupt (ARM)
asm("nop");                   // no operation
```

### Bit Manipulation

```
// Extract bits from a register:
u32 val = reg[7..4];          // extract bits 7 down to 4

// Set bits in a register:
reg[9..8] = mode;             // set bits 9:8 to mode
                              // compiler generates shift + mask
```

### Pointer/Integer Conversion (MMIO)

```
// Convert integer address to pointer (memory-mapped I/O):
volatile *u32 reg = @inttoptr(*u32, 0x4002_0014);

// Convert pointer to integer:
u32 addr = @ptrtoint(reg);
```

---

## 15. Modules and Conditional Compilation

### Modules — No Header Files

```
// No #include. No header files. No include guards. No forward declarations.

import uart;                  // imports uart.zer module
import gpio;                  // imports gpio.zer module
import spi;                   // imports spi.zer module

// The compiler resolves modules from the source tree.
// Each .zer file is a module.
```

### Conditional Compilation — @config + Platform Files

No preprocessor. No `#ifdef`. Instead: build-time constants via `@config` and platform-specific source files selected by `--platform`.

```
// Build flags:
// zerc main.zer --config DEBUG=true --config TARGET=stm32f4 --platform=stm32f4

// Access build config as compile-time constants:
const bool DEBUG = @config("DEBUG", false);       // default: false
const []u8 TARGET = @config("TARGET", "generic"); // default: "generic"

// Dead code elimination — compiler removes false branches entirely:
if (DEBUG) {
    uart_println("debug: entering main loop");  // gone in release. zero cost.
}

// Platform-specific code — use separate module files:
//   platform/stm32f4.zer   — STM32F4 registers, clocks, pin mappings
//   platform/nrf52.zer     — nRF52 registers, clocks, pin mappings
//   platform/generic.zer   — stub/simulator
//
// zerc --platform=stm32f4 selects platform/stm32f4.zer
// import platform;  imports the selected platform module
//
// No #ifdef. Platform differences are in separate files.
// Same pattern as CMake/Makefile selecting source files per target.
```

---

## 16. Intrinsics — @ Builtins

All intrinsics start with @ — visible, grep-able, explicit.

```
@ptrcast(*T, ptr)             type-erase or type-recover (*opaque <-> *T)
@bitcast(T, val)              reinterpret bits (signed/unsigned conversion)
@truncate(T, val)             explicit narrowing — keep low bits (protocol parsing, ADC)
@saturate(T, val)             explicit narrowing — clamp to T max/min (safe clamping)
@cast(T, val)                 convert between distinct typedef types (same underlying bits)
                              NOT @bitcast — @cast is for same-representation types,
                              @bitcast is for reinterpreting bits (signed ↔ unsigned)
@inttoptr(*T, addr)           integer to pointer (MMIO addresses)
@ptrtoint(ptr)                pointer to integer
@cstr(buf, slice)             copy slice into buffer + null terminator (C interop)
@size(T)                      sizeof
@offset(T, field)             offsetof
@container(*T, ptr, field)    container_of — compiler computes offset, emits SUB
@barrier()                    full memory barrier (DMB on ARM, fence on RISC-V)
@barrier_store()              store barrier only — ensure writes visible before proceeding
@barrier_load()               load barrier only — ensure reads complete before proceeding
@config(key, default)         build-time constant from --config flags (conditional compilation)
```

---

## 17. Control Flow

```
// If/else — same as C
if (x > 5) {
    do_thing();
} else {
    do_other();
}

// For loop — same as C
for (u32 i = 0; i < 10; i += 1) {
    process(i);
}

// While loop — same as C
while (running) {
    poll();
}

// Switch — exhaustive, no fallthrough
switch (state) {
    .idle => start(),
    .running => continue_work(),
    .blocked => wait(),
    .done => finish(),
}

// break, continue, return — same as C
```

### Logical Operators — Same as C

```
// Logical operators — identical to C:
if (x > 0 && y > 0) { ... }       // logical AND
if (x > 0 || y > 0) { ... }       // logical OR
if (!ready) { ... }                // logical NOT

// Bitwise operators — identical to C:
u32 val = a & b;                   // bitwise AND
u32 val = a | b;                   // bitwise OR
u32 val = ~a;                      // bitwise NOT
```

---

## 18. Operators

### Arithmetic

```
+    addition (wraps on overflow, defined)
-    subtraction (wraps on underflow, defined)
*    multiplication (wraps, defined)
/    division
%    modulo
```

### Bitwise

```
&    AND
|    OR
^    XOR
~    NOT (complement)
<<   left shift (shift by >= width defined as zero)
>>   right shift (shift by >= width defined as zero)
```

### Comparison

```
==   equal
!=   not equal
<    less than
>    greater than
<=   less or equal
>=   greater or equal
```

### Logical

```
&&   logical AND (same as C)
||   logical OR (same as C)
!    logical NOT (same as C)
```

### Assignment

```
=    assign
+=   add-assign
-=   subtract-assign
*=   multiply-assign
/=   divide-assign
%=   modulo-assign
&=   AND-assign
|=   OR-assign
^=   XOR-assign
<<=  left-shift-assign
>>=  right-shift-assign
```

### Deliberately Removed from C

```
++   NOT in ZER. use i += 1.
--   NOT in ZER. use i -= 1.

// Why: ++ and -- have confusing pre/post increment semantics in C.
// a = b++ vs a = ++b — different behavior, same symbol.
// i += 1 is explicit. one meaning. no ambiguity.
// Every loop in ZER: for (u32 i = 0; i < n; i += 1)
```

---

## 19. Defer — Cleanup Without Goto

Defer runs a statement when the current scope exits — regardless of how it exits (return, break, error).

```
// C cleanup pattern (ugly, error-prone):
void transfer(void) {
    mutex_lock(&spi_lock);
    cs_low();

    if (error1) goto cleanup;
    // ... do work ...
    if (error2) goto cleanup;

cleanup:
    cs_high();
    mutex_unlock(&spi_lock);
}

// ZER cleanup pattern:
void transfer() {
    mutex_lock(&spi_lock);
    defer mutex_unlock(&spi_lock);

    cs_low();
    defer cs_high();

    if (error1) return;           // defers run automatically. clean.
    // ... do work ...
    if (error2) return;           // defers run automatically. clean.
}

// Defers run in REVERSE order (last defer runs first).
// Same as Go and Zig. Proven pattern.
```

---

## 20. C Interop

ZER can call C functions and C can call ZER functions. Same ABI, same calling convention.

### Calling C From ZER

```
// C function (in a .c file or linked library):
// FILE *fopen(const char *path, const char *mode);

// ZER calling it:
const []u8 path = "data.bin";
u8[64] cbuf;
?*opaque f = c_fopen(@cstr(cbuf, path), "rb");
if (f) |file| {
    // use file
}
```

### Conversions at Boundary

```
ZER type       C type            Conversion needed
--------------------------------------------------
[]u8           char* + len       @cstr adds \0 for C
*T             T*                same (non-null both sides)
?*T            T* (nullable)     ZER wraps C return as optional
u32            uint32_t          same
*opaque        void*             same
```

### Key Friction Point

ZER `[]u8` (pointer + length) ≠ C `char*` (pointer + null terminator). Every C string boundary needs `@cstr`. This is the tradeoff for having safe strings. Annoying but necessary.

---

## 21. Bounds Check — Safety vs Optimization

### Safety Is Not Optional

Every array access is bounds-checked. This is the LANGUAGE RULE. It's what makes ZER safe. Without it, ZER is just C with different syntax.

```
[]u8 data;
data[i] = 5;

// Compiler ALWAYS inserts:
if (i >= data.len) trap();
data[i] = 5;

// This is the safety. It's not optional. Not a flag. Not configurable.
```

### Optimization Removes Redundant Checks

The compiler removes checks it can PROVE are unnecessary. This is optimization — affects speed, not safety.

```
// EASY CASE — compiler eliminates check:
for (u32 i = 0; i < arr.len; i += 1) {
    arr[i] = 0;              // compiler KNOWS i < arr.len. no check.
}
// Cost: zero.

// HARD CASE — compiler cannot prove, inserts check:
void process([]u8 data, []u32 indices) {
    for (u32 i = 0; i < indices.len; i += 1) {
        data[indices[i]] = 0; // indices[i] could be anything. check inserted.
    }
}
// Cost: 1 compare + 1 branch per access. ~2 nanoseconds.
```

### Compiler Pointer Safety — No Annotations, No Borrow Checker

The compiler performs two analyses silently within each function (intraprocedural only). The developer writes C-style code. The compiler rejects dangerous patterns. No lifetime annotations. No ownership syntax. No new concepts.

```
SCOPE ESCAPE — pointer to local cannot be returned or stored in longer-lived location:
  *u32 bad() {
      u32 local = 42;
      return &local;          // COMPILE ERROR: pointer to local escapes scope
  }
  // Same check GCC does as WARNING (-Wreturn-local-addr). ZER makes it an ERROR.

  *Task good() {
      static Task t;
      return &t;              // OK — static lives forever
  }
  *Task also_good(*Task t) {
      return t;               // OK — caller's pointer, outlives this function
  }

STORE-THROUGH — pointer stored in struct must outlive struct (same function only):
  static Holder global_h;
  void bad() {
      u32 local = 5;
      global_h.ptr = &local;  // COMPILE ERROR — local dies, global_h survives
  }
  void ok() {
      u32 local = 5;
      Holder h;
      h.ptr = &local;         // OK — h and local are same scope, die together
  }

// SCOPE: intraprocedural only (same function).
// The compiler tracks storage class (stack/static/pool) of every variable
// and validates pointer relationships within each function.
//
// Cross-function analysis (passing &holder to another function that stores
// into it) is NOT checked — that would require interprocedural lifetime
// analysis (borrow checker territory). Deferred to future versions.
//
// Compiler cost: ~100-200 lines. No new keywords. No new syntax.
```

### Every Case Is Safe

```
              SAFE?     FAST?
easy case:    YES       YES (check eliminated)
hard case:    YES       ~2ns slower (check runs)

Never unsafe. Never unchecked.
Safety is v0.1. Optimization improves over time.
```

### Optimization Levels (compiler evolution)

```
v0.1 — no optimization. every access checked. safe. ~2% slower than C.
v0.2 — loop bound proof. i < arr.len → skip check. ~200 lines.
v0.3 — hoist checks before loop. ~200 lines.
v0.4 — constant index proof. arr[0] → one check. ~50 lines.
v0.5 — mask proof. idx & 0xFF → always < 256. ~50 lines.

Total optimization code: ~500 lines. Covers 95% of embedded patterns.
Remaining 5% get a 2-nanosecond check. Invisible on hardware that
waits 87 microseconds per UART byte.
```

---

## 22. Pool, Slab, Ring, Arena — Builtin Types

These are PART OF THE LANGUAGE. Not a library. Not an import. Builtin. They compile to plain instructions and work freestanding. No malloc. No heap. No garbage collector.

Method-style calls on builtins (e.g., `tasks.alloc()`, `rx.push()`) are compiler-generated — not UFCS. The compiler has special knowledge of Pool, Slab, Ring, Arena, and Handle operations. These are not free functions that UFCS discovers; they are intrinsic operations the compiler emits directly.

### Pool(T, N) — Fixed-Slot Allocator

```
// 8 Task slots in .bss (static RAM). Auto-zeroed.
Pool(Task, 8) tasks;

// Allocate — returns Handle, not pointer
Handle(Task) h = tasks.alloc() orelse return;

// Access — checked via generation counter
tasks.get(h).priority = 5;

// Free — generation incremented, old handles invalidated
tasks.free(h);

// Use-after-free CAUGHT:
tasks.get(h);
// debug: trap — "handle generation mismatch"
// release: returns zeroed slot (1 compare)
```

#### How Handles Work

```
Handle(T) — size matches platform pointer width:
  32-bit target: u32 [generation: 16 bits] [index: 16 bits]
  64-bit target: u64 [generation: 32 bits] [index: 32 bits]

  Generation wraps after 65,535 (32-bit) or ~4 billion (64-bit)
  alloc/free cycles per slot. For long-running industrial systems
  on 32-bit targets, monitor generation counts or schedule
  maintenance resets. 64-bit targets are practically infinite.

Pool(Task, 8) in memory:
  slots:       [Task, Task, Task, Task, Task, Task, Task, Task]
  generations: [  0,    0,    0,    0,    0,    0,    0,    0 ]
  used:        [  0,    0,    0,    0,    0,    0,    0,    0 ]

alloc():
  find first unused slot (e.g., index 2)
  used[2] = 1
  return Handle { generation: 0, index: 2 }

get(handle):
  check: generations[handle.index] == handle.generation
  if match: return &slots[handle.index]
  if mismatch: trap (use-after-free!)

free(handle):
  used[handle.index] = 0
  generations[handle.index] += 1   // ← THIS invalidates old handles
```

Handles are NOT pointers. They're 4-byte indices with generation counters. Cannot dangle in the pointer sense. Use-after-free detected by generation mismatch.

#### Compile-Time Safety Rules for Pool and Arena

Three rules eliminate use-after-free at compile time for ~95% of cases. No borrow checker. No lifetime annotations. ~350 lines of compiler code total.

**Rule 1: Handle consumption.** `free(h)` consumes the handle variable. Any use after free = compile error.

```
Handle(Task) h = tasks.alloc() orelse return;
tasks.get(h).priority = 5;     // OK — h is alive
tasks.free(h);                  // h is CONSUMED
tasks.get(h);                   // COMPILE ERROR: h consumed at line above

// Branching:
if (condition) { tasks.free(h); }
tasks.get(h);                   // COMPILE ERROR: h MIGHT be consumed

// Passed to function:
some_function(h);               // h POTENTIALLY consumed
tasks.get(h);                   // COMPILE ERROR: h potentially consumed
```

Compiler cost: ~300 lines. Same mechanism as tracking variable initialization.

**Rule 2: get() result is not storable.** Must use inline. Prevents holding raw pointer across free.

```
tasks.get(h).priority = 5;     // OK — used immediately
tasks.get(h).name = "hello";   // OK — used immediately

*Task ptr = tasks.get(h);      // COMPILE ERROR: cannot store get() result
```

Each get() checks the generation counter (~2 nanoseconds). Five field accesses = five checks. On a 72 MHz Cortex-M, that's ~70 nanoseconds total — 0.08% of one UART byte. Unmeasurable. The tradeoff is ergonomics (repetitive syntax) for safety (no dangling pointers). This is the correct tradeoff for ZER's target.

Compiler cost: ~30 lines. Check that get() return is not assigned to a variable.

**Rule 3: Arena.reset() warns outside defer.** Prevents dangling arena pointers. Warning, not error — developer can override with `unsafe_reset()`.

```
Arena scratch = Arena.over(buf);
defer scratch.reset();                // recommended. no warning.
*Task t = scratch.alloc(Task) orelse return;
t.priority = 5;
// scratch.reset() runs when scope exits
// t dies when scope exits — same time. no dangling.

scratch.reset();          // WARNING: reset() outside defer may cause
                          //   dangling pointers. use defer or unsafe_reset().

scratch.unsafe_reset();   // no warning. developer acknowledged the risk.
```

Compiler cost: ~20 lines. Check if reset() call is inside a defer. Warn if not.

**Rule 4: ZER-CHECK — Path-sensitive handle verification.** A read-only compiler pass that runs after type checking, before C emission. Catches the ~5% of handle bugs that Rules 1-3 cannot catch at compile time — specifically handles stored in arrays/structs and cross-pool usage.

Based on Facebook Infer's Pulse analyzer (Incorrectness Separation Logic, O'Hearn 2019). Proven at scale on millions of lines of production code at Meta. Adapted to ZER's specific handle patterns — simpler because ZER only tracks typed handles with fixed-size pools, not arbitrary heap memory.

```
TECHNIQUE: Typestate tracking with disjunctive path analysis.

Each handle has a state:  UNINITIALIZED → ALIVE → FREED
Each state tracks:        which pool allocated it, at which line

At branches (if/else):
  DON'T merge states. Keep both paths separate.
  Each path represents a REAL execution.
  Report bug only when a concrete path proves it.

At loops:
  Bounded unrolling to pool capacity.
  Pool(Task, 8) → unroll up to 8 iterations.
  Cross-iteration bugs caught by analyzing iteration N and N+1.

ZERO FALSE POSITIVE GUARANTEE:
  Under-approximation — only report what you can prove.
  Every reported bug has a concrete execution path.
  If analyzer can't determine state → stay silent.
  Runtime generation counter is the fallback.

ZERO COST TO COMPILED BINARY:
  ZER-CHECK is a compiler pass, not runtime code.
  It reads the typed AST, reports errors, doesn't modify anything.
  If it finds nothing, compilation continues normally.
  Can be skipped with --no-check for fast iteration.
```

**What ZER-CHECK catches that Rules 1-3 miss:**

```
// Bug 1: Handle in array, freed, then accessed
Handle(Task) handles[4];
handles[0] = tasks.alloc() orelse return;
tasks.free(handles[0]);
tasks.get(handles[0]);        // Rules 1-3: can't track array elements
                               // ZER-CHECK: traces alloc → free → use. ERROR.

// Bug 2: Wrong pool
Pool(Task, 8) pool_a;
Pool(Task, 4) pool_b;
Handle(Task) h = pool_a.alloc() orelse return;
pool_b.get(h);                // Rules 1-3: type matches Handle(Task). passes.
                               // ZER-CHECK: h.pool_id = pool_a, used on pool_b. ERROR.

// Bug 3: Freed in loop, used next iteration
while (condition) {
    tasks.get(h);             // iteration 2+: h already freed. BUG.
    tasks.free(h);
}                              // ZER-CHECK: unrolls 2 iterations, catches it.
```

**How ZER-CHECK avoids false positives:**

```
SITUATION                           WHAT ANALYZER DOES
------------------------------------------------------------------------
Freed on all branches, then used    Report error (definite bug)
Freed on one branch, then used      Report error WITH the specific path
Can't determine state               Stay silent (runtime trap catches it)
Handle passed to unknown function   Assume alive (optimistic, never false)
Loop with unknown bound             Unroll to pool capacity (bounded)
Different pools on different paths  Mark pool as unknown, don't report
```

Compiler cost: ~470 lines. Runs in milliseconds on embedded-sized codebases.

```
Compiler pipeline position:

  source.zer → LEXER → PARSER → TYPE CHECKER → ZER-CHECK → C EMITTER
                                                    ↑
                                              read-only pass
                                              ~470 lines
```

**Coverage summary (updated with ZER-CHECK):**

```
SCENARIO                        CAUGHT AT
---------------------------------------------------
Handle used after free          compile time (Rule 1)
Raw pointer from get() stored   compile time (Rule 2)
Arena pointer after reset       compile time (Rule 3)
Handle in array after free      compile time (ZER-CHECK)
Wrong pool usage                compile time (ZER-CHECK)
Cross-iteration handle reuse    compile time (ZER-CHECK)
Silent corruption               never. impossible.
```

With ZER-CHECK, compile-time safety coverage increases from ~95% to ~99%.
The remaining ~1% (deeply indirect handle aliasing through multiple function
calls) falls to runtime generation counter traps. Still zero silent corruption.

#### What Pool Compiles To

```
// ARM Cortex-M output for pool.alloc():

pool_alloc:
    mov     r1, #0                // i = 0
.scan:
    cmp     r1, #8                // i < 8?
    bge     .full
    ldrb    r2, [r0, r1]          // used[i]
    cbnz    r2, .next             // skip if used
    mov     r2, #1
    strb    r2, [r0, r1]          // used[i] = 1
    ldrh    r3, [r4, r1, lsl #1]  // generations[i]
    orr     r0, r3, r1, lsl #16   // handle = gen | (index << 16)
    bx      lr
.next:
    add     r1, #1
    b       .scan
.full:
    mov     r0, #0                // null (orelse catches this)
    bx      lr

// ~15 instructions. No OS. No heap. Runs on $2 chip.
```

### Slab(T) — Dynamic Growable Pool

```
// No compile-time count — grows on demand
Slab(Connection) conns;

// Same Handle API as Pool
Handle(Connection) h = conns.alloc() orelse return;
conns.get(h).fd = new_fd;
conns.free(h);

// Slab vs Pool:
//   Pool(T, 8)  — 8 slots, compile-time, static RAM, embedded
//   Slab(T)     — unlimited, grows by allocating pages, x86_64/servers
//
// Same safety: generation counters, use-after-free detection,
// non-storable get() results, ZER-CHECK handle tracking.
//
// Slab allocates pages of 64 slots each. When full, a new page
// is allocated. Handle encoding: (gen << 16) | flat_index.
// Max 65,536 total slots (1024 pages).
```

Rules:
- Must be `static` or global — not on the stack
- Cannot be assigned (not copyable) — same as Pool
- Cannot be a struct field — same as Pool
- `get()` returns non-storable `*T` — use inline
- `alloc()` returns `?Handle(T)` — handle OOM with `orelse`

### Ring(T, N) — Circular Buffer

```
// 256-byte ring buffer in .bss. Auto-zeroed.
Ring(u8, 256) uart_rx;

// Push — overwrites oldest if full (for ISR receive buffers):
interrupt UART_RX {
    uart_rx.push(UART_DR);       // always succeeds. overwrites oldest.
}

// push_checked — returns ?void, null if full (for command/work queues):
cmd_queue.push_checked(cmd) orelse {
    report_backpressure();        // queue full. consistent with orelse pattern.
}

// Pop (returns optional):
if (uart_rx.pop()) |byte| {
    process(byte);
}
// Returns null if empty. Must handle via if or orelse.
```

### Arena — Bump Allocator

```
// Arena operates over memory YOU declare. No heap.
u8[4096] scratch_mem;
Arena scratch = Arena.over(scratch_mem);

// Allocate — bump pointer forward
?*Task t = scratch.alloc(Task) orelse return;
?[]u8 buf = scratch.alloc_slice(u8, 64) orelse return;

// Use...
t.priority = 5;

// Free everything at once — one instruction
scratch.reset();
// All allocations gone. Zero leaks. Zero individual frees.
```

### Container Size on Embedded

```
Cortex-M, 32KB RAM, 64KB flash:

  Pool(Task, 8):     8 * sizeof(Task) + 8 * 2 + 8  bytes in .bss
  Ring(u8, 256):     256 + 2 bytes in .bss
  Arena:             developer-chosen size

  Total overhead vs C:
    .bss:   same (static arrays either way)
    .text:  +3-5% (bounds checks, generation checks)
    RAM:    +2 bytes per pool slot (generation counter)
    speed:  ~1-2 extra instructions per access
```

---

## 23. What ZER Does NOT Have

These are deliberate omissions, not missing features.

```
FEATURE             WHY ZER DOESN'T HAVE IT
-------------------------------------------------------------------
User-defined         *opaque + @ptrcast for v0.1. same pattern C uses.
generics             Pool(T,N) and Ring(T,N) are compiler-known
                    parameterized builtins. The developer cannot create
                    their own parameterized types IN V0.1. The compiler
                    has the machinery — opening it to users is a future
                    possibility, not a permanent ban. v0.1 constraint
                    to keep the type checker simple.

Borrow checker      too complex. ZER targets ~75% of CVEs with
                    ~10% of Rust's complexity. Handles catch
                    use-after-free in Pool. Arena catches leaks.

Ownership/move      copy semantics like C. explicit everywhere.

Classes             structs + function pointer structs. like C.
                    like Linux kernel. lowest-level polymorphism.

Inheritance         not needed. vtable structs compose, not inherit.

Methods             no method declarations. UFCS provides t.run()
                    syntax but it desugars to task_run(&t).
                    not OOP. just a syntactic rewrite.

Templates           no. C never had them. conquered the world.

Exceptions          ?T + orelse. explicit. cannot be ignored.

malloc / free       not provided. not needed.
                    Pool(T,N) + Handle(T) = typed alloc with free.
                    Arena = bulk allocation with reset.
                    platform_mem() = one platform abstraction underneath.
                    C interop: call C's free at interop boundary only.
                    The capability exists. The footgun doesn't.
                    malloc is not a language feature in C either —
                    it's libc. ZER replaces libc's allocator with
                    a safer one. Same capability, no raw pointers.

Garbage collector   stack + static + Pool/Ring/Arena. no heap.

LLVM               own compiler. written in C. ~13,500 lines.
                    emit C initially, own backends later.

Closures/lambdas    function pointer + *opaque context. like C.

RTTI                no runtime type information. types are
                    compile-time only. @ptrcast debug tag is
                    stripped in release.

Operator overload   no. operators do what they say. always.

Namespaces          modules (import). one .zer file = one module.

Preprocessor        no #define, no #ifdef. const and modules instead.

Header files        no. import replaces #include entirely.
```

---

## 24. Complete Keyword Reference

```
TYPES (14):
  u8  u16  u32  u64  i8  i16  i32  i64  usize
  f32  f64  bool  void  opaque

DECLARATIONS (7):
  struct  packed  enum  union  const  typedef  distinct

CONTROL FLOW (8):
  if  else  for  while  switch  break  continue  return

LOGICAL (3 — operators, same as C):
  &&  ||  !

ERROR HANDLING (2):
  orelse  null

MEMORY (4 builtins):
  Pool  Ring  Arena  Handle

SPECIAL (7):
  defer  import  volatile  interrupt  asm  static  keep

INTRINSICS (15):
  @ptrcast  @bitcast  @truncate  @saturate  @cast
  @inttoptr  @ptrtoint  @cstr
  @size  @offset  @container
  @barrier  @barrier_store  @barrier_load
@config

DEFAULT (1):
  default  (required in integer switch, globally reserved — cannot be used as identifier)

TOTAL: ~50 keywords + 13 intrinsics

Compare:
  C:     ~32 keywords (unsafe)
  C++:   ~97 keywords (complex)
  Rust:  ~53 keywords + borrow checker concepts
  Zig:   ~47 keywords + comptime concepts
  ZER:   ~47 keywords. C-like. safe. no new paradigms.
```

---

## 25. Compiler Architecture

### Strategy — The Bootstrap Path

ZER follows the same bootstrap path as C itself. C was first written in B, then C wrote its own compiler, then B was no longer needed. ZER does the same: written in C first, then ZER writes its own compiler, then C is no longer needed.

The key insight: **phase 1 is 95% of the work.** The language, the type checker, the safety rules, the error messages, the edge cases, the testing — all phase 1. Phase 2 is mechanical — same logic, different output format. Phase 3 is translation — same compiler, different source language.

**IMPORTANT: Phase 1 is NOT a stepping stone that gets thrown away.** It is a permanent backend. When phase 2 adds native backends, both paths coexist. The developer chooses per target:

```
zerc main.zer --target=c          → main.c → GCC → any board ever
zerc main.zer --target=arm        → firmware.elf directly (no GCC)
zerc main.zer --target=riscv      → firmware.elf directly (no GCC)

Same main.zer. Same safety rules. Same behavior.
One flag picks the backend.

--target=c      Use when: certified toolchain, exotic board, any GCC target.
                Covers EVERY architecture GCC supports (50+).
                Permanent. Not deprecated. Not replaced.

--target=arm    Use when: no GCC available, want smallest toolchain,
                full independence from external tools.
                Covers ONLY architectures with ZER native backends.
```

This is the same architecture Zig uses (LLVM backend + self-hosted backend, user picks per target). ZER just replaces LLVM with GCC-via-C-emission — simpler, wider reach, and the emitted C works with GCC versions from 2009.

```
PHASE 1: PROVE THE LANGUAGE (emit C, GCC dependency)
  source.zer → zerc (written in C) → output.c → GCC → binary

  This is where the real work lives:
    - Design and implement the type checker
    - Get every safety rule working (bounds, zero, null, overflow)
    - Handle every edge case (nested optionals, defer in loops, etc.)
    - Error messages that tell the developer what went wrong
    - Test suite proving safety guarantees hold

  ZER is a safety-enforcing frontend for GCC in this phase.
  Same relationship as TypeScript : V8 — TypeScript type-checks,
  enforces rules, emits JavaScript, V8 executes it.

  GCC handles: register allocation, instruction encoding,
  optimization, ELF output, every architecture ever.
  ZER handles: type checking, safety insertion, code expansion.

  GCC dependency is NOT a weakness. For embedded targets where
  GCC is the certified toolchain (arm-none-eabi-gcc), this IS
  the final architecture. The 2009 GCC in a certified toolchain
  works fine — ZER emits standard C.

  All architectures GCC supports work on day one: ARM, RISC-V,
  x86, AVR, MIPS, PowerPC, MSP430, and 50+ more.

  Precedent: Nim uses this strategy and ships production code.

  When phase 1 is done: ZER is a COMPLETE language. Every feature
  works. Every safety rule is enforced. The language is proven.
  It just happens to emit C.

  Timeline: 1-2 years.


PHASE 2: REPLACE THE FLOOR (own backends, no GCC for target)
  source.zer → zerc (still written in C) → native binary

  Same language. Same rules. Same safety. Same everything.
  The ZER developer notices NOTHING different. Same source code
  compiles to the same binary. GCC is just no longer in the middle.

  This is mechanical work, not design work. You already know what
  every ZER construct compiles to — you wrote the C emission.
  Instead of emitting if (i >= len) __zer_trap() as C text,
  you emit CMP R1, R2; BGE trap as machine instructions.
  Same logic. Different output format.

  Per architecture backend:
    x86_64                     ~10,000 lines (start here — your dev machine)
    ARM Cortex-M (Thumb-2)     ~8,000 lines  (most embedded boards)
    RISC-V (RV32IM)            ~5,000 lines  (growing embedded, cleanest ISA)
    AVR                        ~4,000 lines  (if ever needed)

  Note: GCC is still needed to compile zerc itself (it's C code).
  The ZER compiler is written in C. But the OUTPUT no longer
  goes through GCC.

  Timeline: +1 year after phase 1.


PHASE 3: THE THOMPSON MOMENT (self-hosting, full independence)
  Rewrite zerc in ZER.
  source.zer → zerc (written in ZER) → native binary

  ZER compiles itself. C is no longer needed. GCC is no longer
  needed. ZER is a fully independent language, same as how C
  became independent from B.

  The phase 1 C code becomes the reference implementation and
  translation guide. Every C struct → ZER struct. Every C
  function pointer vtable → ZER function pointer pattern.
  Every malloc for AST nodes → Pool. The compiler becomes
  the first serious ZER program using every feature ZER has.

  This is why phase 1 must be written in CLEAN C — not C++,
  not clever macros, not complex abstractions. The cleaner
  the C, the more mechanical the ZER translation.

  When phase 3 completes: you can hand zerc.zer to someone
  with zero GCC installation and they build the compiler from
  source using only ZER. Full independence.

  Timeline: +6-12 months after phase 2.

  Historical precedent:
    B → C written in B → C compiler written in C → B no longer needed
    Same path. Same outcome. Not novel — proven.


FULL ARC:
  Phase 1 (prove language, emit C):    1-2 years
  Phase 2 (own backends):             +1 year
  Phase 3 (self-hosting):             +6-12 months
  ──────────────────────────────────────────────
  Full independence:                   3-4 years total
```

### Compiler Pipeline

```
source.zer
    |
    v
[LEXER]          tokens          ~560 lines   ✅ DONE
    |
    v
[PARSER]         AST             ~1,200 lines ✅ DONE
    |
    v
[TYPE CHECKER]   typed AST       ~2,500 lines
    |
    v
[ZER-CHECK]      verified AST    ~470 lines
  path-sensitive handle verification
  wrong-pool detection
  cross-iteration use-after-free
  zero false positives (under-approximation)
    |
    v
[SAFETY]         checked AST     ~980 lines
  bounds insertion
  zero insertion
  null enforcement
  overflow definition
  cast saturation
  switch exhaustiveness
    |
    v
[?T + orelse]    error handling   ~100 lines
    |
    v
[BUILTINS]       Pool/Ring/       ~200 lines
                 Arena/Handle
    |
    v
[INTRINSICS]     @ builtins       ~200 lines
    |
    v
[DEFER]          cleanup codegen  ~100 lines
    |
    v
[UNION]          tagged union     ~200 lines
    |
    v
[PACKED]         layout           ~50 lines
    |
    v
[BIT EXTRACT]    shift+mask       ~80 lines
    |
    v
[IR]             intermediate     ~2,000 lines
                 representation
    |
    v
[C EMITTER]      output.c        ~2,000 lines
    |
    v
gcc/clang        native binary
```

### Total Compiler Size

```
Component                        Lines    Status
-----------------------------------------------------
Lexer (lexer.c/h)                  743    ✅ v0.1
Parser + AST (parser/ast .c/h)   2,207    ✅ v0.1
Type system (types.c/h)            704    ✅ v0.1
Type checker (checker.c/h)       1,769    ✅ v0.1
ZER-CHECK (zercheck.c/h)          457    ✅ v0.1
C emitter (emitter.c/h)         1,662    ✅ v0.1
  (includes defer, tagged union, packed struct,
   bit extraction, Pool/Ring/Arena, intrinsics,
   orelse, bounds checks — all in one pass)
Compiler driver (zerc_main.c)      378    ✅ v0.1
LSP server (zer_lsp.c)          1,371    ✅ v0.1
-----------------------------------------------------
TOTAL                            9,291 lines

Bounds check optimization (v0.2): +500 lines
IR generation (v0.3):           ~2,000 lines
-----------------------------------------------------
PROJECTED v0.3 TOTAL            ~11,800 lines
```

Compare:
- LLVM: ~30,000,000 lines
- GCC: ~15,000,000 lines
- Rust compiler: ~600,000 lines
- Go compiler: ~500,000 lines
- Zig compiler: ~100,000 lines
- TCC (Tiny C Compiler): ~25,000 lines
- **ZER compiler: ~13,900 lines**

---

## 26. Compiler Build Order

### Phase 1: Prove the Language (emit C)

```
MILESTONE ZERO — first successful end-to-end compile:
  Goal: u32 x = 5; return x; → compiles through GCC → runs.
  Needs: lexer, parser (minimal), type checker (minimal), C emitter.
  This is the "it works" moment. Everything after is expanding coverage.

VERSION 0.1 — ZER runs. safe. complete feature set. ✅ SHIPPED
  1. Lexer (tokens)                                              ✅ DONE (218 tests)
  2. Parser (AST — functions, structs, variables, control flow)  ✅ DONE (158 tests)
  3. Type checker (coercion, scope escape, exhaustive switch)    ✅ DONE (265 tests)
  4. ZER-CHECK (path-sensitive handle verification)              ✅ DONE (8 tests)
     → Zero false positives via under-approximation (Pulse/ISL technique).
  5. Safety insertion (bounds, zero, null, overflow, casts)      ✅ DONE
  6. C emitter (all constructs → GCC-compilable C)              ✅ DONE (76 E2E tests)
  7. Defer codegen (reverse order, works with break/continue)    ✅ DONE
  8. Tagged union (construction + switch routing)                ✅ DONE
  9. Packed struct (__attribute__((packed)))                      ✅ DONE
  10. Bit extraction (reg[7..4])                                 ✅ DONE
  11. Module imports (diamond deps, topological sort)            ✅ DONE (6 patterns)
  12. LSP server (diagnostics, hover, go-to-def, completion)    ✅ DONE
  13. Test suite (851+ tests, all passing)                       ✅ DONE

  ZER is a COMPLETE language. Every feature works.
  Phase 1 is done. The language is proven.

VERSION 0.2 — faster (bounds check optimization)
  14. Loop bound proof (i < arr.len → skip check)        ~200 lines
  15. Hoist checks before loops                           ~200 lines
  16. Constant index proof (arr[0] → one check)          ~50 lines
  17. Mask proof (idx & 0xFF → always < 256)             ~50 lines
```

### Phase 2: Own Backends (no GCC for target)

```
VERSION 0.3 — first native backend
  18. IR generation (architecture-independent intermediate form)
  19. x86_64 backend (your dev machine — dogfood the language here)
  20. DWARF debug info (gdb support)

VERSION 0.4 — embedded targets
  21. ARM Cortex-M backend (Thumb-2) — most embedded boards
  22. RISC-V backend (RV32IM) — growing embedded, cleanest ISA

When 0.4 is done: ZER compiles to native binaries for 3 architectures
without GCC. The compiler itself is still written in C.
The --target=c path remains for all other architectures.
```

### Phase 3: Self-Hosting (full independence)

```
VERSION 1.0 — the Thompson moment
  23. Rewrite zerc in ZER (zerc.zer replaces zerc.c)
  24. Compile zerc.zer with phase 2 zerc → new zerc binary
  25. New zerc compiles itself → verified self-hosting

When 1.0 is done: ZER is fully independent. No C. No GCC.
ZER compiles ZER. The bootstrap chain is complete.
C was needed to get here. C is no longer needed.
```

---

## 27. Competitive Landscape

### Comparison Table

```
Feature          C      ZER     Zig     Rust    C++
---------------------------------------------------
C syntax         YES    YES     no      no      partial
Own backend      YES    YES*    partial no      no
No LLVM          YES    YES     NO      NO      NO
Freestanding     YES    YES     YES     YES     partial
Auto-zero        NO     YES     NO      N/A     NO
Bounds check     NO     YES     YES     YES     partial
Non-null ptr     NO     YES     YES     YES     NO
Defined overflow NO     YES     YES     YES     NO
No UB            NO     YES     YES     YES     NO
Borrow checker   NO     NO      NO      YES     NO
Generics         NO     NO      YES**   YES     YES
Simple           YES    YES     medium  NO      NO
Embedded-first   YES    YES     YES     partial NO
Compiler size    25K    13.5K   100K    600K    15M

* emit C initially, own backends later
** Zig uses comptime, not traditional generics
```

### Closest Existing Languages

```
ZIG: 80% of ZER's goals.
  Same: slices, optionals, defined overflow, embedded, no borrow checker.
  Different: uses LLVM, has comptime (adds complexity), doesn't auto-zero,
  different syntax. ZER is essentially "simpler Zig with C syntax and no LLVM."

HARE: 70% of ZER's goals.
  Same: own backend (QBE), simple, C-like.
  Different: not really safer than C, limited to 2 architectures,
  small community.

C3: 60% of ZER's goals.
  Same: C evolution, embedded support, simpler than C++.
  Different: uses LLVM.
```

### Why ZER Doesn't Exist Yet

1. **Writing backends is tedious grunt work.** Everyone uses LLVM to avoid it.
2. **"Just use Rust" shuts down the conversation.** People assume safety requires Rust's complexity.
3. **Embedded engineers don't design languages.** PL researchers don't write firmware. The intersection is nearly empty.
4. **The design is "too obvious" for academia.** "Remove C's footguns" isn't publishable. Novel type theory is.
5. **Ecosystem matters more than design.** A language without libraries, tooling, and community is academic. ZER mitigates this by using C's ABI — every existing C library works with ZER on day one.

---

## 28. Design Justifications

### Why No Generics

C never had generics. Linux kernel, every OS, every microcontroller firmware — all written without generics. For 52 years. C uses void* and cast. ZER uses *opaque and @ptrcast. Same pattern, with debug-mode type checking.

Generics add ~3,000-5,000 lines of compiler complexity (type parameter tracking, monomorphization, error messages). They cause binary bloat on embedded (every instantiation generates separate code). 64KB flash budget doesn't tolerate template bloat.

Pool(T, N) and Ring(T, N) look like generics but are builtins — the compiler has special knowledge of them, not a general generic system.

### Why No Borrow Checker

ZER and Rust achieve the SAME guarantee: zero silent memory corruption. The difference is mechanism:
- Rust catches 100% at compile time via borrow checker + lifetime annotations
- ZER catches ~99% at compile time (type checker + ZER-CHECK), ~1% at runtime trap (generation counter)
- Both prevent shipping corrupted firmware

That 1% compile-time difference costs Rust: ~50,000 lines of borrow checker code, lifetime annotations on functions, ownership model, LLVM dependency, 6-12 month learning curve, "fighting the borrow checker." ZER-CHECK achieves near-parity with zero false positives using ~470 lines of path-sensitive analysis (based on Facebook Infer's Pulse technique). ZER achieves zero silent corruption without any of that complexity.

Additionally, ZER's compiler performs intraprocedural scope escape and store-through validation — catching the most common use-after-free patterns (returning pointer to local, storing local pointer in global struct) without annotations. Same checks GCC does as warnings, ZER makes them errors. ~100-200 lines of compiler code. Cross-function analysis deferred — that's borrow checker territory.

### Why C Syntax

Adoption barrier. New syntax means developers must learn a new language before getting any benefit. C syntax means developers write what they already know. Safety is free, no learning step.

Compiler barrier. C's grammar is known. Every edge case documented for 52 years. Parser references the C spec. Less compiler code, fewer bugs.

C interop barrier. C headers almost read as ZER. Copy a struct definition, minor edits, works.

Languages that succeeded as "better X" used X's syntax: TypeScript (JavaScript), Kotlin (Java), Objective-C (C).

### Why Prevention Not Detection

A compiler that rejects code and asks the developer to rewrite is functionally equivalent to a linter. The developer does extra work. That's not safety — that's a tax.

True safety means the developer writes normal code and it's ALREADY safe. Arrays carry length (developer doesn't add checks). Variables are zeroed (developer doesn't add initialization). Pointers are non-null (developer doesn't add null checks). The language handles it.

### Why *opaque Instead of void*

Same concept, different rules. void* in C can be dereferenced (undefined behavior) and cast to anything silently. *opaque cannot be dereferenced (compile error) and requires @ptrcast (explicit, grep-able). Debug builds carry a type tag that @ptrcast verifies. Release builds strip the tag — zero cost, same as C.

### Why No Regions

Regions as a language feature require ~1,000 lines of escape analysis in the compiler, create edge cases with callbacks and nested lifetimes, and introduce a new concept C developers don't know. Pool handles (index + generation counter) catch use-after-free with zero compiler complexity — it's a runtime check in a library type, not a language rule. Arena provides bulk deallocation without individual free(). Both work freestanding.

### Why Pointer Arithmetic Is Byte-Level

C merged pointer arithmetic and array indexing into one syntax, then needed char* as an escape hatch. ZER separates them: `ptr + 24` is always 24 bytes (what the CPU does), `arr[3]` scales by element size (what the programmer expects). container_of becomes a compiler builtin (@container) instead of a char* macro hack. The hardware never scaled — C added an abstraction that created a problem, then needed a workaround.

### Why Defined Overflow (Not UB)

C's undefined behavior on signed overflow lets the compiler delete security checks (documented real-world cases: Linux kernel CVEs). ZER defines overflow as wrapping — same as what the hardware does on every architecture. This costs zero performance (the CPU already wraps) and eliminates an entire class of compiler-introduced bugs.

### Why Auto-Zero (Not Undefined)

Zig defaults to `undefined` and requires explicit initialization. This means the compiler must track initialization state (dataflow analysis) and reject uninitialized use — the developer must add `= 0` everywhere. ZER just zeros everything. Simpler for the developer (nothing to add), simpler for the compiler (no dataflow analysis needed, just emit zero on declaration).

**Acknowledged tradeoff:** Auto-zero hides "forgot to initialize" bugs. A sensor reading that should have been set but wasn't will quietly produce `temperature = 0` instead of trapping on uninitialized use. Zig's approach catches this class of bug; ZER's approach prevents reading garbage memory. Both are valid — ZER chose predictable zeros over trap-on-undefined. For embedded, 0 is often a valid sentinel ("not yet read") and range-checking application logic catches the error downstream.

### Why Defer

Embedded code constantly acquires resources (mutexes, chip-select pins, interrupt masks) that must be released on every exit path. C uses goto-cleanup patterns which are error-prone (forget a goto = resource leak). Defer guarantees cleanup runs on EVERY exit path — return, break, error. Same as Go and Zig. ~100 lines of compiler code. Massive improvement in correctness.

---

## 29. Resolved and Open Decisions

### RESOLVED: Format Strings — Tagged Argument Array

Embedded logging is ~30% of firmware code. This must be decided before v0.1.

Decision: **tagged argument array with compiler sugar.** Not variadic (unsafe). Not generics. The fmt module is a standard ZER module using tagged unions. The compiler provides syntactic sugar to avoid manual array construction.

```
// WITH COMPILER SUGAR (preferred — developer writes this):
u8[128] buf;
u32 len = fmt_write(buf, "temp={} pressure={}", temp, pres);
uart_write(buf[0..len]);

// Compiler desugars to:
FmtArg __args[2] = { fmt_u32(temp), fmt_f32(pres) };
u32 len = fmt_write(buf, "temp={} pressure={}", __args);

// The compiler sees: fmt_write(buf, format_literal, extra_args...)
// Knows types of temp (u32) and pres (f32) at compile time.
// Counts args, checks count matches {} count, builds typed array.
// NOT variadic — compiler generates fixed-size array.
// ~100 lines of compiler code for the sugar.

// WITHOUT SUGAR (also valid — manual array construction):
FmtArg args[2] = { fmt_u32(temp), fmt_f32(pres) };
u32 len = fmt_write(buf, "temp={} pressure={}", args);

// Both forms compile to identical code.
// Sugar is convenience, not a different mechanism.
```

The fmt module is standard ZER code using ZER's own tagged union:

```
// fmt.zer — ships with ZER standard modules

// FmtArg is a tagged union. The compiler automatically
// inserts and tracks the tag — no separate enum needed.
// Same mechanism as union Message in Section 12.
union FmtArg {
    u32  u;
    i32  i;
    f32  f;
    u8   c;
    []u8 s;
}

// Constructors set the active variant.
// Compiler tracks which variant was initialized → sets tag.
FmtArg fmt_u32(u32 v) { return FmtArg { .u = v }; }
FmtArg fmt_i32(i32 v) { return FmtArg { .i = v }; }
FmtArg fmt_f32(f32 v) { return FmtArg { .f = v }; }
FmtArg fmt_u8(u8 v)   { return FmtArg { .c = v }; }
FmtArg fmt_str([]u8 v) { return FmtArg { .s = v }; }

// Helper functions (also in fmt.zer):
// write_u32([]u8 buf, u32 val) -> u32  — writes decimal digits, returns bytes written
// write_i32([]u8 buf, i32 val) -> u32  — writes signed decimal, returns bytes written
// write_f32([]u8 buf, f32 val) -> u32  — writes float, returns bytes written
// copy([]u8 dst, []u8 src) -> u32      — copies bytes, returns bytes written

// Format function — walks pattern string, replaces {}
// with next arg, dispatches on tagged union variant.
u32 fmt_write([]u8 buf, []u8 pattern, []FmtArg args) {
    u32 pos = 0;
    u32 arg_idx = 0;

    for (u32 i = 0; i < pattern.len; i += 1) {
        if (pattern[i] == '{' && i + 1 < pattern.len
            && pattern[i + 1] == '}') {
            if (arg_idx < args.len) {
                switch (args[arg_idx]) {
                    .u => |val| { pos += write_u32(buf[pos..], val); },
                    .i => |val| { pos += write_i32(buf[pos..], val); },
                    .f => |val| { pos += write_f32(buf[pos..], val); },
                    .c => |val| { buf[pos] = val; pos += 1; },
                    .s => |val| { pos += copy(buf[pos..], val); },
                }
                arg_idx += 1;
            }
            i += 1;  // skip '}'
        } else {
            buf[pos] = pattern[i];
            pos += 1;
        }
    }
    return pos;
}
```

Multiple typed write calls (`uart_write_u32`, etc.) remain available as the simplest option for developers who prefer them. The fmt module is opt-in via `import fmt`.

### RESOLVED: Pool/Ring/Arena Nesting — Restricted

Builtins take concrete types with compile-time constant sizes only. No nesting of builtins inside builtins. No runtime-determined sizes.

```
Pool(Task, 8);                // YES — concrete type, constant size
Ring(u8, 256);                // YES — concrete type, constant size
Arena scratch;                // YES — size set by Arena.over()

Pool(Pool(Task, 4), 8);      // COMPILE ERROR: Pool is not a user type
Ring(u8, config.buf_size);    // COMPILE ERROR: size must be compile-time constant

// Rationale: embedded doesn't do runtime-sized allocations.
// Buffer sizes are always known at compile time.
// If runtime flexibility is needed, use Arena with a static buffer.
// This restriction prevents creeping toward generics.
```

### RESOLVED: UFCS — Uniform Function Call Syntax — DROPPED

<!-- UFCS was spec'd but never implemented in the emitter. Decision: DROP IT.

Reasons:
1. ZER's target audience is C developers. They already write uart_write(&port, data)
   and prefer explicit function calls. UFCS adds implicit behavior they don't want.
2. The important method-call syntax (pool.alloc(), ring.push(), arena.alloc()) already
   works because these are compiler-known builtins — not UFCS. The cases that matter
   are already covered.
3. UFCS creates ambiguity: is t.foo() a struct field access or a function call? The
   resolution rules add complexity for zero safety benefit.
4. Embedded/safety-critical code favors explicit over implicit. Knowing exactly what
   function is being called matters more than saving a few characters.
5. C-style calls (func(&obj, args)) work everywhere, are unambiguous, and match what
   every C developer already knows.

If method syntax is ever needed for user types, function pointers in structs (vtables)
already provide it — and that pattern is well-understood by C developers.
-->

### RESOLVED: Visibility — static for File Scope, Module Qualification for Conflicts

Follow C. Everything visible by default. `static` scopes to current module.

```
// uart.zer
void uart_init(u32 baud) { ... }          // visible to importers
static void configure_pins() { ... }      // internal, not visible

// main.zer
import uart;
uart_init(9600);              // works
configure_pins();             // COMPILE ERROR: not visible (static)
```

Name conflicts across modules resolved by module qualification:

```
// uart.zer
void init(u32 baud) { ... }

// spi.zer
void init(u32 speed) { ... }

// main.zer
import uart;
import spi;
init(9600);           // COMPILE ERROR: ambiguous — init in uart and spi
uart.init(9600);      // OK — qualified with module name
spi.init(8000000);    // OK — qualified with module name
```

UFCS follows the same rule — if multiple modules define `run(*Task)`, compile error, developer qualifies with module name.

### RESOLVED: Slice Range Syntax — Exclusive End

```
buf[0..3]     // elements 0, 1, 2. three elements. end EXCLUSIVE.
buf[2..]      // element 2 through end. end = buf.len.
buf[..5]      // elements 0 through 4. start = 0.
buf[..]       // all elements. same as buf.

// Exclusive end: buf[0..buf.len] = whole slice.
// Same as Python, Rust, Go, Zig. Proven. No off-by-one.

// Runtime sub-slicing bounds — TWO checks:
buf[i..j]
// check 1: j <= buf.len   (end within bounds)
// check 2: i <= j          (start before end)
// both checked. both trap on failure. compiler inserts automatically.
```

### RESOLVED: Interrupt Emit-C Strategy — Platform-Specific Annotation

The developer writes `interrupt NAME { ... }`. The C emitter generates the correct platform-specific annotation.

```
// ZER source (same on all platforms):
interrupt USART1 { ... }

// Emitted C — varies by --platform flag:
// ARM:    void __attribute__((interrupt)) USART1_IRQHandler(void) { ... }
// RISC-V: void __attribute__((interrupt("machine"))) USART1_Handler(void) { ... }
// AVR:    ISR(USART1_vect) { ... }
```

Interrupt handler naming convention (e.g., `_IRQHandler` suffix for ARM CMSIS) is handled by the platform target. Developer writes the peripheral name only.

### RESOLVED: Memory Barriers — @barrier Intrinsic

`volatile` prevents compiler reordering. It does NOT prevent CPU reordering on out-of-order architectures (ARM, RISC-V). ZER adds memory barrier intrinsics:

```
@barrier()              // full memory barrier (DMB on ARM, fence on RISC-V)
@barrier_store()        // store barrier only (ensure stores visible before proceeding)
@barrier_load()         // load barrier only (ensure loads complete before proceeding)

// Ring builtin handles barriers INTERNALLY.
// Developer writes .push() — Ring emits the barrier for them.
// This is why Ring is a builtin, not a library struct.

// Developer writes:
interrupt UART_RX {
    rx_buf.push(UART_DR);    // safe. Ring handles ordering.
}

// Ring.push() internally does:
//   data[head] = byte;
//   @barrier_store();          ← Ring inserts this
//   head = (head + 1) & mask;

// @barrier is for developer's OWN shared variables,
// not for builtins. Example — raw shared flag between
// ISR and main loop:

volatile u32 data_ready = 0;
volatile u32 sensor_value = 0;

interrupt ADC_DONE {
    sensor_value = ADC_DR;     // write data first
    @barrier_store();           // ensure data visible
    data_ready = 1;             // then set flag
}

void main_loop() {
    if (data_ready) {
        @barrier_load();        // ensure we read fresh sensor_value
        process(sensor_value);
        data_ready = 0;
    }
}

// Emitted C (GCC):
// __atomic_thread_fence(__ATOMIC_RELEASE)  for @barrier_store
// __atomic_thread_fence(__ATOMIC_ACQUIRE)  for @barrier_load
```

### OPEN: Inline Functions

```
// Does ZER have inline hints?
// C: inline keyword (suggestion, not guarantee)
// ZER: TBD. Likely same — compiler decides, hint available.
```

---

## 30. Full Example — UART Driver

Complete ZER firmware example showing all features together.

```
// uart.zer — UART driver for STM32F4

import gpio;

// Hardware registers (packed, memory-mapped)
packed struct UART_Regs {
    u32 SR;           // status register
    u32 DR;           // data register
    u32 BRR;          // baud rate register
    u32 CR1;          // control register 1
    u32 CR2;          // control register 2
    u32 CR3;          // control register 3
}

// MMIO base address
volatile *UART_Regs UART1 = @inttoptr(*UART_Regs, 0x4001_1000);

// Receive buffer — builtin Ring, static memory, freestanding
Ring(u8, 256) rx_buf;

// Transmit buffer
Ring(u8, 256) tx_buf;

// Error tracking
enum UartError {
    none,
    timeout,
    framing,
    overrun,
}

// Initialize UART
void uart_init(u32 baud) {
    // Enable GPIO for UART pins
    gpio.configure(gpio.PA9, gpio.AF7);     // TX
    gpio.configure(gpio.PA10, gpio.AF7);    // RX

    // Set baud rate (assuming 16 MHz clock)
    UART1.BRR = 16000000 / baud;

    // Enable UART, TX, RX, RX interrupt
    UART1.CR1 = (1 << 13) | (1 << 3) | (1 << 2) | (1 << 5);
}

// Interrupt handler — compiler generates correct prologue/epilogue
interrupt USART1 {
    u32 sr = UART1.SR;

    // Receive
    if (sr & (1 << 5)) {                   // RXNE bit
        u8 byte = @truncate(UART1.DR);     // read DR clears RXNE
        rx_buf.push(byte);                  // ring buffer, overwrites oldest
    }

    // Transmit complete
    if (sr & (1 << 7)) {                   // TXE bit
        if (tx_buf.pop()) |byte| {
            UART1.DR = byte;
        } else {
            UART1.CR1 &= ~(1 << 7);       // disable TXE interrupt
        }
    }
}

// Read bytes from UART — non-blocking
?u32 uart_read([]u8 buf) {
    u32 count = 0;
    while (count < buf.len) {              // bounds: count < buf.len
        if (rx_buf.pop()) |byte| {
            buf[count] = byte;             // safe: count < buf.len proven
            count += 1;
        } else {
            break;
        }
    }
    if (count == 0) return null;
    return count;
}

// Write bytes to UART — non-blocking
u32 uart_write([]u8 data) {
    u32 count = 0;
    for (u32 i = 0; i < data.len; i += 1) {
        tx_buf.push(data[i]);              // safe: i < data.len
        count += 1;
    }
    // Enable TXE interrupt to start transmission
    UART1.CR1 |= (1 << 7);
    return count;
}

// Read with timeout
?u32 uart_read_timeout([]u8 buf, u32 timeout_ms, *UartError err) {
    u32 start = tick_count;
    u32 count = 0;

    while (count < buf.len) {
        if (rx_buf.pop()) |byte| {
            buf[count] = byte;
            count += 1;
        } else if (tick_count - start > timeout_ms) {
            *err = UartError.timeout;
            if (count == 0) return null;
            return count;
        }
    }

    *err = UartError.none;
    return count;
}

// Send a string (convenience)
void uart_print([]u8 msg) {
    uart_write(msg);
}

// Send a string with newline
void uart_println([]u8 msg) {
    uart_write(msg);
    uart_write("\r\n");
}
```

### What This Example Demonstrates

```
FEATURE                     WHERE IT APPEARS
-------------------------------------------------------------
packed struct               UART_Regs — hardware register layout
volatile MMIO               UART1 — memory-mapped registers
@inttoptr                   UART1 base address
Ring(T, N) builtin          rx_buf, tx_buf — ISR-safe buffers
interrupt                   USART1 handler
[]u8 slices                 buf, data, msg — bounded everywhere
?T optional return          uart_read returns null if no data
orelse                      (available to callers)
bounds checking             buf[count] — count < buf.len proven by loop
auto-zero                   all variables initialized
defined overflow            tick_count - start wraps correctly
bit manipulation            SR & (1 << 5), CR1 |= (1 << 7)
@truncate                   explicit narrowing from u32 DR to u8
enum                        UartError
function pointers           (shown in FileOps example above)
no malloc                   everything is static/stack
freestanding                no OS, no heap, no runtime
```

---

## Appendix A: ZER vs C Quick Reference

```
C                                     ZER
---------------------------------------------------------------------------
uint8_t id;                           u8 id;
int x = 5;                           i32 x = 5;
const int y = 10;                    const i32 y = 10;
char buf[256];                       u8[256] buf;
char *str = "hello";                 []u8 str = "hello";
void *ptr;                           *opaque ptr;
(Task *)ptr                          @ptrcast(*Task, ptr)
sizeof(Task)                         @size(Task)
offsetof(Task, field)                @offset(Task, field)
container_of(ptr, Task, list)        @container(*Task, ptr, list)
NULL                                 null
int *p = malloc(sizeof(int));        // no malloc. use Pool or Arena.
free(p);                             // no free. Pool.free(handle).
#include "uart.h"                    import uart;
printf("x=%d\n", x);                // format TBD. no variadic.
(volatile uint32_t *)0x40020014      @inttoptr(*u32, 0x4002_0014)
if (p != NULL)                       if (p) |val| { ... }
switch ... case ... break            switch ... => ...
&&  ||  !                            &&  ||  !  (same as C)
```

---

## Appendix B: Memory Layout on Cortex-M

```
FLASH (64KB):
  0x0800_0000  [interrupt vector table]
  0x0800_00C0  [.text — ZER compiled code]
  0x0800_XXXX  [.rodata — string literals, const tables]

RAM (32KB):
  0x2000_0000  [.data — initialized globals]
  0x2000_XXXX  [.bss — zeroed globals (Pool, Ring, Arena buffers)]
  0x2000_XXXX  [heap — NOT USED by ZER]
  0x2000_7FFF  [stack — grows downward]
               [stack pointer initialized here]

ZER binary size estimate:
  .text:   ~30-32 KB (same as C + 3-5% bounds checks)
  .rodata: ~1-2 KB (string literals)
  .bss:    depends on Pool/Ring/Arena declarations
  Total:   fits in 64KB flash easily
```

---

## Appendix C: Why Not These Languages?

```
"Why not Rust?"
  Both guarantee zero silent memory corruption.
  Rust catches everything at compile time via borrow checker.
  ZER catches most at compile time, remainder as runtime traps.
  Both prevent shipping corrupted firmware.
  The difference: Rust requires LLVM, borrow checker, lifetime annotations,
  6-12 month learning curve. ZER requires none of that.
  Show me Rust without LLVM on a $2 chip with 32KB flash.

"Why not Zig?"
  Closest to ZER's goals. But: LLVM required for production,
  comptime adds complexity, doesn't auto-zero, different syntax.
  If Zig dropped LLVM and comptime and used C syntax, it would be ZER.

"Why not C++?"
  97 keywords. Exception tables. RTTI. Virtual dispatch.
  Template metaprogramming. 15 million lines of compiler.
  Embedded engineers use a tiny subset and fight the rest.

"Why not C with sanitizers?"
  Sanitizers are test-time only. Production binary has no checks.
  Bug that doesn't trigger in testing → crash in production.
  ZER's checks are in the production binary.

"Why not C with static analysis?"
  Static analysis is advisory. Warnings can be ignored.
  ZER's safety is the language — can't ignore, can't skip,
  no binary produced until code is safe.
```

---

## Appendix D: How Emit-C Actually Works — Concrete Examples

ZER's phase 1 compiler is a transpiler. It reads `.zer` source and writes `.c` files. GCC compiles those `.c` files into native binaries. ZER's compiler is a FRONTEND ONLY — it does NOT generate machine code, do register allocation, encode instructions, or produce ELF binaries. GCC does all of that.

This is fundamentally different from TCC (which IS a full compiler generating native code directly in ~25K lines). ZER's compiler is closer to TypeScript emitting JavaScript — it translates syntax and inserts safety checks, then hands off to an existing compiler.

### ZER vs TCC — Not Comparable

```
TCC (Tiny C Compiler):
  source.c → [LEXER → PARSER → CODEGEN → REGISTER ALLOC →
               INSTRUCTION ENCODING → ELF GENERATION → LINKER]
  TCC does EVERYTHING. 25,000 lines because it includes
  machine code generation for x86, ARM, etc.

ZER compiler (phase 1):
  source.zer → [LEXER → PARSER → TYPE CHECK → SAFETY → EMIT C TEXT]
  Output: a .c file. fprintf statements writing C code.
  Then: gcc output.c -o binary

  ZER does NOT: register allocation, instruction encoding,
  ELF output, linking, optimization.
  GCC does all of that.

  13,500 lines is for the FRONTEND ONLY.
  Comparing to TCC's 25K (full compiler) is apples to oranges.
```

### Concrete Emit-C Examples

Every ZER feature has a direct C emission pattern. None require exotic C features.

**Slices and bounds checks:**

```
// ZER source:
void process([]u8 data) {
    data[5] = 42;
}

// Emitted C:
void process(uint8_t *data_ptr, uint32_t data_len) {
    if (5 >= data_len) { __zer_trap("main.zer", 2); }
    data_ptr[5] = 42;
}

// Slice []u8 becomes two parameters: pointer + length.
// Bounds check becomes an if + trap call.
// __zer_trap is __attribute__((noreturn)) so GCC never removes it.
```

**Auto-zero:**

```
// ZER source:
u32 x;
Task t;
u8[100] arr;

// Emitted C:
uint32_t x = 0;
Task t = {0};
uint8_t arr[100] = {0};

// Or for larger arrays/structs:
uint8_t arr[100];
memset(arr, 0, sizeof(arr));
```

**Optional types and orelse:**

```
// ZER source:
?u32 uart_read([]u8 buf) {
    if (no_data()) return null;
    return bytes_read;
}
u32 n = uart_read(buf) orelse return;

// Emitted C:
typedef struct { uint32_t value; uint8_t has_value; } __zer_opt_u32;

__zer_opt_u32 uart_read(uint8_t *buf_ptr, uint32_t buf_len) {
    if (no_data()) return (__zer_opt_u32){0, 0};
    return (__zer_opt_u32){bytes_read, 1};
}

// orelse return:
__zer_opt_u32 __tmp_1 = uart_read(buf_ptr, buf_len);
if (!__tmp_1.has_value) return;
uint32_t n = __tmp_1.value;
```

**Pool(Task, 8):**

```
// ZER source:
Pool(Task, 8) tasks;
Handle(Task) h = tasks.alloc() orelse return;
tasks.get(h).priority = 5;
tasks.free(h);

// Emitted C:
static Task __pool_tasks_slots[8] = {0};
static uint16_t __pool_tasks_gen[8] = {0};
static uint8_t __pool_tasks_used[8] = {0};

// alloc:
uint32_t h;
{
    int __found = 0;
    for (int __i = 0; __i < 8; __i++) {
        if (!__pool_tasks_used[__i]) {
            __pool_tasks_used[__i] = 1;
            h = ((uint32_t)__pool_tasks_gen[__i] << 16) | (uint32_t)__i;
            __found = 1;
            break;
        }
    }
    if (!__found) return; /* orelse return */
}

// get (with generation check):
{
    uint16_t __idx = (uint16_t)(h & 0xFFFF);
    uint16_t __gen = (uint16_t)(h >> 16);
    if (__idx >= 8 || __pool_tasks_gen[__idx] != __gen) {
        __zer_trap("main.zer", 5);
    }
    __pool_tasks_slots[__idx].priority = 5;
}

// free:
{
    uint16_t __idx = (uint16_t)(h & 0xFFFF);
    __pool_tasks_used[__idx] = 0;
    __pool_tasks_gen[__idx]++;
}
```

**Defer:**

```
// ZER source:
void transfer() {
    mutex_lock(&spi_lock);
    defer mutex_unlock(&spi_lock);
    cs_low();
    defer cs_high();
    if (error) return;
    do_work();
}

// Emitted C:
void transfer(void) {
    mutex_lock(&spi_lock);
    cs_low();
    if (error) goto __defer_0;
    do_work();
__defer_0:
    cs_high();
    mutex_unlock(&spi_lock);
}

// Every return becomes goto __defer_N.
// Defers emitted in reverse order at the label.
// ~100 lines of emitter code.
```

**Tagged union:**

```
// ZER source:
union Message {
    SensorData sensor;
    Command command;
}

switch (msg) {
    .sensor => |data| { process_sensor(data); },
    .command => |cmd| { execute(cmd); },
}

// Emitted C:
typedef struct {
    uint8_t __tag;  // 0 = sensor, 1 = command
    union {
        SensorData sensor;
        Command command;
    } __data;
} Message;

switch (msg.__tag) {
    case 0: {
        SensorData *data = &msg.__data.sensor;
        process_sensor(data);
        break;
    }
    case 1: {
        Command *cmd = &msg.__data.command;
        execute(cmd);
        break;
    }
}
```

**@container (container_of):**

```
// ZER source:
*Task task = @container(*Task, node_ptr, list);

// Emitted C:
Task *task = (Task *)((char *)(node_ptr) - offsetof(Task, list));

// Yes — the emitted C uses char*. ZER doesn't.
// The developer never writes or sees char*.
// The compiler generates the correct C idiom.
// ZER abstracts the char* hack away — it's an implementation detail
// of the C emission, not part of the ZER language.
```

**@ptrcast with debug tag:**

```
// ZER source:
*opaque raw = @ptrcast(*opaque, &my_task);
*Task t = @ptrcast(*Task, raw);

// Emitted C — RELEASE build:
void *raw = (void *)(&my_task);
Task *t = (Task *)raw;
// zero cost. identical to handwritten C.

// Emitted C — DEBUG build:
typedef struct { void *ptr; uint32_t type_id; } __zer_opaque;

__zer_opaque raw = { (void *)(&my_task), __ZER_TYPE_Task };
if (raw.type_id != __ZER_TYPE_Task) {
    __zer_trap_cast("main.zer", 2, "Task", raw.type_id);
}
Task *t = (Task *)raw.ptr;
// catches wrong cast at runtime with clear error message.
// stripped entirely in release. zero-cost.
```

### Why GCC Won't Break ZER's Safety

Concern: "Will GCC optimize away ZER's bounds checks?"

No. The trap function is marked `__attribute__((noreturn))`:

```c
__attribute__((noreturn))
void __zer_trap(const char *file, int line) {
    // embedded: trigger hardware breakpoint or reset
    __builtin_trap();
}
```

GCC never removes a branch to a `noreturn` function because removing it would change observable behavior (program would continue past a fatal error). This is a well-understood pattern — Zig, Go, and Rust all use the same technique.

### What the Emitted C Looks Like — Full File

```c
/* Generated by zerc from main.zer — DO NOT EDIT */
#include <stdint.h>
#include <string.h>

/* ZER runtime — ~20 lines */
__attribute__((noreturn))
void __zer_trap(const char *file, int line) {
    __builtin_trap();
}

/* Pool support */
static Task __pool_tasks_slots[8];
static uint16_t __pool_tasks_gen[8];
static uint8_t __pool_tasks_used[8];

/* Ring support */
static uint8_t __ring_rx_data[256];
static uint8_t __ring_rx_head;
static uint8_t __ring_rx_tail;

/* User code */
void process(uint8_t *data_ptr, uint32_t data_len) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < data_len; i++) {
        /* bounds check eliminated — i < data_len proven by loop */
        sum = sum + data_ptr[i]; /* wraps on overflow — defined */
    }
}

/* Compiles with: arm-none-eabi-gcc -O2 -mcpu=cortex-m4 output.c -o firmware.elf */
```

Standard C. GCC compiles it. Every architecture GCC supports works on day one.

---

## Appendix E: The char* Mathematical Proof — Full Exploration

This section documents the complete exploration of whether byte-level pointer arithmetic is possible in C without `char*`. Every path was tried. Every path failed. This is the proof that ZER's approach (byte-level pointer arithmetic as a language primitive) is the correct design.

### The Problem

C scales pointer arithmetic by `sizeof(*ptr)`. When `int *p` is incremented by 1, it moves 4 bytes (not 1). To move exactly N bytes, C requires casting to a type with `sizeof == 1`. The only type the C standard guarantees to be size 1 is `char`. Therefore, `char*` is the only portable mechanism for byte-level addressing.

### Every Alternative Tried

**Attempt 1: Integer division to reverse scaling.**

```c
// Idea: ptr arithmetic scales by sizeof, so divide to undo it
(int *)member_ptr - (24 / sizeof(int))   // subtract 6 * 4 = 24 bytes

// Problem: fails when offset isn't divisible by sizeof(type)
// offset 7, sizeof(int) = 4: 7 / 4 = 1 (truncated), gives 4 bytes not 7
```

**Attempt 2: Force padding to make offsets divisible.**

Works but wastes massive memory. A struct with odd-offset fields would need padding to make every offset divisible by every possible type size. Impractical.

**Attempt 3: Bitfield struct with sizeof == 1.**

```c
struct byte { unsigned b:8; };
sizeof(struct byte)  // = 4, NOT 1. Storage unit of unsigned int.

// Using char b:8 gives sizeof == 1, but that's char again.
```

**Attempt 4: Union with uintptr_t (before C99).**

```c
union pointer_stealth { void *raw; unsigned int addr; };
// PDP-11 (both 16-bit): works
// x86_64 (pointer=8, int=4): truncates. BROKEN.
// Only works when sizeof(int) == sizeof(void*) — not guaranteed.
```

**Attempt 5: uintptr_t (C99+).**

```c
uintptr_t addr = (uintptr_t)ptr;
addr -= 24;
ptr = (type *)addr;
// Works. But this IS the char* alternative — escaping the type system into integers.
// Only available since C99. char* has been available since K&R.
```

**Attempt 6: Inline assembly.**

```asm
; ARM: SUB R0, R0, #24
; x86: sub rax, 24
; RISC-V: addi x10, x10, -24
```

Works on every architecture. But assembly isn't portable across architectures. Each CPU has different register names and instruction mnemonics. `char*` gives the same result portably.

**Attempt 7: PDP-11 era integer cast.**

```c
(struct task_struct *)((int)member_ptr - 24)
```

Worked on PDP-11 (sizeof(int) == sizeof(void*) == 2). Broke on every subsequent architecture where int and pointer widths diverged.

### The Mathematical Inevitability

C's pointer scaling means `ptr + N` adds `N * sizeof(*ptr)` bytes. To add exactly `N` bytes, you need a type where `sizeof == 1`. For any byte offset `K`, you need `K / sizeof(type)` to be exact (no truncation from integer division). The only integer that divides EVERY integer exactly is 1. Therefore, the only type that works for EVERY offset is one with `sizeof == 1`. The C standard defines only one such type: `char`.

This is number theory, not convention. `char*` isn't a style choice — it's the only mathematically correct option in C's type system.

### ZER's Resolution

ZER avoids the entire problem by not scaling pointer arithmetic. `ptr + 24` always means 24 bytes. Array indexing `arr[3]` is a separate operation where the compiler scales. Two operations, two semantics, no ambiguity, no `char*` needed. The emitted C code still uses `char*` internally (the C backend must), but the ZER developer never writes or sees it.

### Historical Note: How Linked Lists Worked Before container_of

Before the `char*` / `container_of` pattern was invented, C programmers used typed embedding:

```c
// Each struct type had its own next/prev pointers:
struct task { int pid; struct task *next; };
struct inode { int ino; struct inode *next; };

// Or external nodes with void*:
struct node { void *data; struct node *next; };
```

Typed embedding meant every struct type needed its own list functions (insert_task, insert_inode, etc.) — massive code duplication. External nodes with `void*` required extra malloc per node and cache-destroying pointer chases (`node->data` is in different memory than the node).

The intrusive list pattern (`list_head` embedded in any struct, recovered via `container_of`) solved both problems: zero extra allocations, one set of list functions for all types. But it required byte-level pointer arithmetic — which required `char*`.

ZER makes `@container` a builtin, so the developer gets the benefits of intrusive lists without ever touching `char*` or understanding the underlying arithmetic.

---

## Appendix F: *opaque Debug Tag Mechanism — Full Detail

### Release Build (--release flag)

`*opaque` is a raw pointer. 4 bytes on 32-bit, 8 bytes on 64-bit. Identical to C's `void*`. Zero overhead. Zero type information. `@ptrcast` is a raw cast.

```
*opaque in memory (release):
  [pointer: 4/8 bytes]

  identical to void*. same size. same layout. same performance.
```

### Debug Build (default)

`*opaque` carries a hidden type tag. The tag is stored alongside the pointer. `@ptrcast` checks the tag before casting. Wrong cast = trap with diagnostic message.

```
*opaque in memory (debug):
  [pointer: 4/8 bytes] [type_id: 4 bytes]

  type_id is a compile-time constant assigned per type:
    __ZER_TYPE_Task    = 1
    __ZER_TYPE_Inode   = 2
    __ZER_TYPE_File    = 3
    ...
```

### Tag Assignment

The compiler assigns type IDs automatically. Every struct, enum, and union gets a unique ID.

```
// ZER compiler generates:
enum {
    __ZER_TYPE_UNKNOWN = 0,
    __ZER_TYPE_Task = 1,
    __ZER_TYPE_Inode = 2,
    __ZER_TYPE_SensorPacket = 3,
    // ... one per type in the program
};
```

### Storing a Tag

When a pointer is cast to `*opaque`, the compiler inserts the type tag:

```c
// ZER: *opaque raw = @ptrcast(*opaque, &my_task);

// Debug C:
__zer_opaque raw;
raw.ptr = (void *)(&my_task);
raw.type_id = __ZER_TYPE_Task;

// Release C:
void *raw = (void *)(&my_task);
```

### Checking a Tag

When `*opaque` is cast back to a concrete type, the compiler checks:

```c
// ZER: *Task t = @ptrcast(*Task, raw);

// Debug C:
if (raw.type_id != __ZER_TYPE_Task) {
    __zer_trap_cast("main.zer", 42, "Task", raw.type_id);
}
Task *t = (Task *)raw.ptr;

// Release C:
Task *t = (Task *)raw;
```

### Trap Message

`__zer_trap_cast` provides a clear diagnostic:

```
ZER CAST ERROR at main.zer:42
  Expected type: Task (id=1)
  Actual type:   Inode (id=2)

  The *opaque was created from an Inode but cast to *Task.
```

This catches the exact class of bug where C's `void*` silently corrupts: casting to the wrong type. In release builds, all of this is stripped — same binary as C.

### Memory Overhead in Debug

```
Debug build:
  Every *opaque: +4 bytes (type_id)
  Every @ptrcast: +1 comparison + 1 branch

  On Cortex-M with 32KB RAM: acceptable for development.
  Stripped entirely in release. Zero production cost.
```

---

## Appendix G: Implementation Reality — Honest Assessment

### What's Genuinely Hard

```
COMPONENT                   DIFFICULTY    WHY
---------------------------------------------------------------
Lexer                       Easy          State machine. Well-understood.
                                          Every language's lexer is similar.

Parser                      Medium        Recursive descent for C-like grammar.
                                          Known technique. Edge cases in
                                          declaration syntax (function pointers,
                                          arrays of pointers, etc.)

Type checker                HARD          Deceptively complex. Must handle:
                                          - slice/array interconversion
                                          - optional type propagation
                                          - signed/unsigned rules
                                          - struct field access through pointers
                                          - function pointer type compatibility
                                          - tagged union variant tracking
                                          This is where most time goes.
                                          3-4 months minimum.

Error messages              HARD          Not mentioned enough. Good error
                                          messages are 30-40% of compiler work.
                                          "line 42: type mismatch" is functional
                                          but unusable. Need: "expected *Task
                                          but got *Inode when calling process()
                                          at line 42." This requires tracking
                                          source locations through every
                                          compiler phase.

C emission                  Medium        Straightforward pattern matching.
                                          Each ZER construct maps to known C.
                                          Edge cases: nested optionals,
                                          defer in loops, Pool in structs.

Bounds check emission       Easy          if (i >= len) trap().
                                          GCC won't remove it (__noreturn).

Safety insertion            Easy-Medium   Auto-zero: emit = {0}. Simple.
                                          Non-null: type system tracks it.
                                          Overflow: just don't emit UB.
                                          Saturation: emit clamp code.
```

### Realistic Timeline

```
Phase 1 — emit C, basic safety (MINIMUM VIABLE):
  Lexer + parser + type checker + safety + C emitter.
  Can compile simple ZER programs to working C.
  6-12 months of evenings/weekends.

Phase 2 — optimization + polish:
  Bounds check optimization. Better error messages.
  Defer, tagged unions, packed structs.
  Another 6-12 months.

Phase 3 — own backends (OPTIONAL):
  RISC-V backend: 3-6 months.
  ARM Cortex-M backend: 3-6 months.
  x86_64 backend: 3-6 months.
  DWARF debug info: 2-3 months.
  Total: 1-2 years.

Phase 4 — self-hosting (OPTIONAL):
  Rewrite compiler in ZER.
  3-6 months.

TOTAL to production quality: 2-4 years.
This is a marathon, not a sprint.
```

### The 13,500 Line Estimate

The estimate is for the FRONTEND ONLY (emit-C strategy). It does not include:
- Native code generation backends (~8K per architecture)
- DWARF debug info generation
- Linker script handling
- Extensive error message formatting

Realistic final size for emit-C phase: 15,000-20,000 lines. Still one-person scale. Still smaller than TCC (25K for a FULL compiler including native codegen). The comparison is important: ZER at 20K lines does LESS than TCC (no native codegen) but does it DIFFERENTLY (type checking, safety insertion, optional types, tagged unions).

### Known Dunning-Kruger Risks

```
RISK                              MITIGATION
---------------------------------------------------------------
"Spec looks complete,             Implementation will surface
 must be easy to build"           hundreds of edge cases not in
                                  the spec. This is normal.

"Type checker estimated           Could easily be 4,000-5,000.
 at 2,500 lines"                  Allow 2x the estimate.

"Error messages not in            Budget 30% of total effort
 the spec"                        for error messages alone.

"C emission sounds simple"        Nested constructs (defer inside
                                  loop with orelse) create
                                  complex emission patterns.

"13,500 lines total"              Probably 18,000-22,000 when done.
                                  Still manageable. Just honest.
```

### Why Build It Anyway

ZER is a research study. Not a product. Not a startup. Not competing with Zig for market share. If it helps one project — the author's own embedded work — it justified itself. If others find it useful, that's a bonus. The value is in the exploration: understanding why C works the way it does, why char* is mathematically necessary, why safety doesn't require a borrow checker, and whether a safer C can be built in under 20K lines.

---

---

## Appendix H: Spec Freeze Notice

This specification is **FROZEN** as of 2026-03-17. The language design is complete.

A language spec is a fractal — every decision creates edge cases, every edge case creates questions. Iterating on the spec forever produces a perfect document and zero compiler code. At some point you stop specifying and start building.

**Remaining edge cases go in `zer-type-system.md`** — written before the type checker, when the lexer and parser exist and you know what AST the type checker receives. That document captures implementation decisions. This document captures design intent.

**What this spec is:** a complete language design that a compiler author can implement from. Every major feature is specified. Every safety rule is defined. Every deliberate omission is justified. Every acknowledged tradeoff is documented.

**What this spec is NOT:** a formal language standard. Edge cases in type coercion, operator precedence tables, exact grammar productions — these surface during implementation and get resolved in `zer-type-system.md` and eventually a formal grammar file.

**The next documents in order:**
1. Read Crafting Interpreters Chapter 16-17 (lexer and parser patterns)
2. Write the lexer (`zerc` — tokenizer with trie keyword detection)
3. Write the parser (recursive descent, produces AST)
4. Write `zer-type-system.md` (internal type representation, coercion rules, edge cases)
5. Write the type checker
6. Write the C emitter
7. Milestone zero: `u32 x = 5; return x;` compiles end-to-end through GCC

---

*ZER is a research study by ZEROHEXER. The language design documented here is complete. Implementation (compiler in C) is the next step. The honest timeline is 2-4 years for production quality. The design is sound. The spec is frozen. The work is ahead.*
