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
Handle(Task) h;          u64: index(32) + generation(32), not a pointer
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

### Hardware Support
```
volatile *u32 reg = @inttoptr(*u32, 0x4002_0014);  // MMIO register
u32 bits = reg[9..8];                               // bit extraction
interrupt USART1 { handle_rx(); }                    // interrupt handler
packed struct Packet { u8 id; u16 val; u8 crc; }    // unaligned struct
```

### What ZER Does NOT Have
- No classes, inheritance, templates, generics
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

### Safety Guarantees
| Bug Class | Prevention |
|---|---|
| Buffer overflow | Inline bounds check on every array/slice access; proven-safe indices skip check (range propagation); unsafe indices get auto-guard (silent if-return inserted) |
| Use-after-free | Handle generation counter + ZER-CHECK (MAYBE_FREED + leak detection + loop pass + cross-function summaries) + Level 1-5 *opaque tracking (compile-time zercheck + poison-after-free + inline header + global malloc interception) |
| Null dereference | `*T` non-null by default, `?T` requires unwrapping |
| Uninitialized memory | Everything auto-zeroed |
| Integer overflow | Wraps (defined), never UB |
| Silent truncation | Must `@truncate` or `@saturate` explicitly |
| Missing switch case | Exhaustive check for enums and bools |
| Dangling pointer | Scope escape analysis (walks field/index chains, catches struct fields + globals + orelse fallbacks + @cstr buffers + array→slice coercion + struct wrapper returns) |
| Union type confusion | Cannot mutate union variant during mutable switch capture |
| Arena pointer escape | Arena-derived pointers cannot be stored in global/static variables (ALL arenas, including global — `is_from_arena` flag) |
| Division by zero | Forced guard (compile error if divisor not proven nonzero); struct fields via compound key range propagation |
| Invalid MMIO address | `mmio` declarations (compile-time) + alignment check + **MMIO index bounds from range** (compile-time) + startup @probe validation (boot-time) + `--no-strict-mmio` for unchecked access |
| Unsafe pointer indexing | Non-volatile `*T` indexing warns "use slice". Volatile `*T` from `@inttoptr` bounds-checked against `mmio` range. |
| Slab alloc in ISR | `slab.alloc()` in interrupt handler → compile error (calloc may deadlock). Use Pool instead. |
| Ghost handle (leaked alloc) | `pool.alloc()` / `slab.alloc()` as bare expression → compile error (handle discarded) |
| Wrong pointer cast | 4-layer: Symbol + compound key + array-level + whole-program param provenance. Runtime `_zer_opaque{ptr, type_id}` for cinclude only |
| Handle leak | zercheck: ALIVE/MAYBE_FREED at function exit = error. Overwrite alive handle = error |
| Wrong container_of | `@container` field validation + provenance tracking from `&struct.field` |
| Volatile/const strip | `@ptrcast`, `@bitcast`, `@cast` all check qualifier preservation |
| ISR data race | Shared global without volatile → error. Compound assign on shared volatile → error (non-atomic read-modify-write) |
| Stack overflow | Recursion detection via call graph DFS → warning. Stack frame size estimation per function |
| Misaligned MMIO | `@inttoptr` alignment check — address must match target type alignment (u32=4, u16=2, u64=8) |
| @critical escape | `return`/`break`/`continue`/`goto` inside `@critical` → compile error (would skip interrupt re-enable) |
| Interior pointer UAF | `*u32 p = &b.field; free(b); p[0]` → compile error. Field-derived pointers share alloc_id with parent. NODE_INDEX UAF check. |

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
| Task.new() / Task.delete() (auto-Slab) | Done | Done (auto global Slab per struct) |
| Task.new_ptr() / Task.delete_ptr() | Done | Done (auto-Slab + alloc_ptr) |
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
- **v0.2.1 (CURRENT):** comptime functions + comptime if, 4-layer MMIO safety (range + alignment + boot probe + signal() fault handler), @ptrcast/@container provenance tracking, safe intrinsics, zer-convert P0+P1 (volatile, MMIO casts, defined(), suffixes, include guards, stringify extraction), FULL SAFETY ROADMAP: value range propagation, bounds auto-guard, forced division guard (incl. struct fields), @cstr auto-orelse, auto-keep on fn ptr params, array-level + whole-program *opaque provenance, zercheck 1-4 (MAYBE_FREED + leaks + loops + cross-func), @probe safe MMIO reads, interrupt safety (ISR shared-state analysis), stack depth analysis (recursion detection), cross-platform portability warnings, 415+ bug fixes, 1,700+ tests, 11 audit rounds with convergence
- **v0.3:** better error messages, stdlib completion (io/fmt/conv)
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
- `Task.new()` / `Task.delete()` — auto-creates a global `Slab(T)` per struct type. No Slab declaration needed. Returns `?Handle(T)`. `Task.new_ptr()` returns `?*T`. `Task.delete_ptr()` type-checks argument. Auto-slabs emitted after struct declarations, before function bodies (two-pass emission). One auto-Slab per struct type, program-wide.
- **Emitter two-pass:** `emit_file` now emits struct/enum/union/typedef declarations first (pass 1), then auto-slab globals, then functions/globals (pass 2). This ensures auto-slabs can use `sizeof(StructType)`.
- **CRITICAL BUG (fixed):** orelse block path (`orelse { return; }`) emitted `0` as final statement expression value for null-sentinel `?*T`. This made ALL `*T = alloc_ptr() orelse { return; }` assign 0 instead of the pointer. The bare form (`orelse return`) was correct — only the block form was broken. Fixed: emit `_zer_tmp` for null-sentinel, `_zer_tmp.value` for struct optional.
- **Real code finds real bugs:** Writing a 60-line HTTP server in ZER found this orelse bug that 1,700+ tests missed. All tests used bare `orelse return`, none used block `orelse { return; }`. Lesson: write real programs in ZER, not just tests.
- zercheck now scans defer bodies for free/delete calls — `defer pool.free(h)` no longer triggers false "never freed" warning. Also: if-branch that always exits (return/break/continue) doesn't cause MAYBE_FREED — `if (err) { free(h); return; } use(h);` is correctly seen as safe.
- **CRITICAL BUG (fixed):** Auto-slab initializer used positional `{sizeof(T), 0, 0, ...}` which put sizeof into wrong field (`pages` instead of `slot_size`). Fix: designated initializer `{ .slot_size = sizeof(T) }`. Same pattern as normal Slab emission. All `Task.new()` was broken until this fix.
- **Lesson: always use designated initializers for emitted struct init.** Positional initializers are fragile — field order in `_zer_slab` doesn't match naive assumption. Future emitter code MUST use `.field = value` syntax, never positional.
- `const Handle(Task)` allows data mutation through auto-deref. Handle is a KEY (like `const int fd`), not a pointer. const key ≠ const data. Assignment checker sets `through_pointer = true` for TYPE_HANDLE in field chain. This also fixes if-unwrap `|t|` + Handle auto-deref (capture is const but data mutation is allowed).
- zercheck recognizes `Task.delete()` / `Task.delete_ptr()` as free calls (TYPE_STRUCT method detection). Also `Task.new()` / `Task.new_ptr()` as alloc calls. ISR ban applied to Task.new/new_ptr same as slab.alloc.
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
- Current tests: hash_map, ring_buffer, pool_handle, enum_switch, union_variant, defer_cleanup, extern_puts, hash_map_chained, tracked_malloc, arena_alloc, comptime_eval, bit_fields, optional_patterns, star_slice, goto_label, goto_switch_label, goto_defer, handle_autoderef, handle_autoderef_pool, alloc_ptr, alloc_ptr_pool, alloc_ptr_mixed, alloc_ptr_stress, handle_complex, handle_array, opaque_safe_patterns, task_new, task_new_ptr, task_new_complex, orelse_block_ptr, task_new_orelse, const_handle_ok, handle_if_unwrap, volatile_div, orelse_void_block, volatile_orelse, comptime_const_if, optional_null_init, comptime_if_call, opaque_level1, opaque_level2, opaque_level345, opaque_level9_complex, opaque_cross_func, opaque_mixed_features, handle_scalar_store, const_slice_return, distinct_funcptr_nested, void_optional_init, distinct_optional, distinct_optional_full, distinct_types, distinct_slice_ops, funcptr_array, defer_free, if_exit_free, optional_array, distinct_optional_ptr, volatile_field_array, comptime_negative, comptime_pool_size, const_slice_field, comptime_nested_call, funcptr_struct_reduce, alloc_ptr_func, multi_slab_cross, nested_struct_deref2, nullable_funcptr, recursive_functions, pool_exhaustion, comptime_signed, slice_subslice, bit_extract_set, packed_struct, atomic_ops, container_offset, bang_integer, forward_decl, critical_block, bitcast_int, array_3d, union_array_variant, comptime_const_arg, opaque_ptrcast_roundtrip, dyn_array_guard, autoguard_intrinsic, critical_handle, distinct_union_assign, goto_backward_safe, inline_call_range, inline_range_deep, guard_clamp_range, keep_store_global, driver_registry, defer_return_order, typecast_cstyle, defer_multi_free, super_defer_complex, super_plugin, super_ecs, super_interpreter, super_freelist, super_state_machine, super_hashmap, goto_spaghetti_safe, interior_ptr_safe, typecast_safe_complex
- Negative tests: uaf_handle, double_free, maybe_freed, bounds_oob, div_zero, null_ptr, dangling_return, isr_slab_alloc, ghost_handle, goto_bad_label, alloc_ptr_uaf, alloc_ptr_double_free, opaque_struct_uaf, opaque_return_freed, opaque_alias_uaf, opaque_maybe_freed, opaque_double_free, cross_func_free_ptr, free_ptr_wrong_type, handle_no_allocator, ghost_alloc_ptr, task_delete_double, task_delete_uaf, opaque_cross_func_uaf, opaque_task_delete_ptr_uaf, bitcast_width, array_return, float_switch, return_in_defer, asm_not_naked, ptrcast_strip_volatile, narrowing_coerce, dyn_array_loop_freed, critical_return, critical_break, goto_backward_uaf, nonkeep_store_global, goto_spaghetti_uaf, goto_spaghetti_swap, goto_circular_swap_uaf, goto_maybe_freed_branch, interior_ptr_uaf, interior_ptr_func, typecast_provenance, typecast_volatile_strip, typecast_const_strip, typecast_direct_ptr, typecast_int_to_ptr, typecast_ptr_to_int, arena_global_escape
- Module tests (`test_modules/`): main, app, diamond, use_types, use_defs, diamond2, collision_test, static_coll, gcoll, transitive, use_hal, opaque_wrap, opaque_wrap_df (negative), opaque_wrap_uaf (negative)
- Examples (not in automated tests): `examples/http_server.zer` — minimal HTTP server, needs network
- Add new tests by dropping `.zer` files in `tests/zer/` — runner picks them up automatically

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

## Implementation Workflow — Lessons Learned

These patterns were discovered through repeated mistakes. Follow them to avoid wasting turns.

### Adding New Builtin Methods (alloc_ptr, Task.new, etc.)
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

Also: update EVERY recursive AST walker that handles all node types. This session found that `collect_labels()` and `validate_gotos()` missed NODE_SWITCH/NODE_DEFER/NODE_CRITICAL. Zercheck's `zc_check_stmt()` may also need updating.

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

This session's proof: writing a 60-line HTTP server found the orelse block null-sentinel bug that 1,700+ unit tests missed. Testing `Task.new()` with both orelse forms found the auto-slab initializer bug.

**Bug patterns found ONLY by real code (not unit tests):**
- `orelse { return; }` (block form) emitting 0 instead of pointer — tests all used bare `orelse return`
- Auto-slab `{sizeof(T), 0, 0}` positional init putting sizeof in wrong field — tests used explicit Slab
- zercheck not recognizing `Task.delete()` as free — tests used `heap.free()` directly
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

## First Session Workflow

When starting a new session or lacking context:

1. Read `CLAUDE.md` (this file) — has FULL language reference above, rules, conventions
2. **MANDATORY — read `docs/compiler-internals.md` BEFORE modifying any compiler source file** (parser.c, checker.c, emitter.c, types.c, zercheck.c). It documents every emission pattern, optional handling, builtin method interception, scope system, type resolution flow, and common bug patterns. Skipping this and discovering patterns by reading source files wastes 20+ tool calls. The document exists specifically to prevent this.
3. Read `BUGS-FIXED.md` — 41 past bugs with root causes. Prevents re-introducing fixed bugs.
4. `ZER-LANG.md` — full language spec (only if CLAUDE.md quick reference is insufficient)
5. Read the relevant header files: `lexer.h` → `parser.h` → `ast.h` → `types.h` → `checker.h` → `emitter.h` → `zercheck.h`
4. Run `make docker-check` (preferred) or `make check` to verify everything passes before making changes
5. The compiler pipeline is: ZER source → Lexer → Parser → AST → Type Checker → ZER-CHECK → C Emitter → GCC

## Project Architecture

- **zerc** = the compiler binary (`zerc_main.c` + all lib sources)
- **zer-lsp** = LSP server (`zer_lsp.c` + all lib sources)
- Source files: `lexer.c/h`, `parser.c/h`, `ast.c/h`, `types.c/h`, `checker.c/h`, `emitter.c/h`, `zercheck.c/h`
- Test files: `test_lexer.c`, `test_parser.c`, `test_parser_edge.c`, `test_checker.c`, `test_checker_full.c`, `test_extra.c`, `test_gaps.c`, `test_emit.c`, `test_zercheck.c`, `test_firmware_patterns.c`, `test_fuzz.c`
- E2E tests in `test_emit.c`: ZER source → parse → check → emit C → GCC compile → run → verify exit code
- Cross-platform: `test_emit.c` uses `#ifdef _WIN32` macros (`TEST_EXE`, `TEST_RUN`, `GCC_COMPILE`) for `.exe` extension and path separators. Works on both Windows and Linux/Docker.
- Spec: `ZER-LANG.md` (full language spec), `zer-type-system.md` (type design), `zer-check-design.md` (ZER-CHECK design)
- **Default behavior:** `zerc main.zer` compiles to `main.exe` (or `main` on Linux) — the `.c` intermediate is temp, deleted after GCC. No `.c` visible to user. Looks native.
- Compiler flags: `--run` (compile+execute), `--emit-c` (keep `.c` output, old behavior), `--lib` (no preamble/runtime, for C interop), `--no-strict-mmio` (allow @inttoptr without mmio declarations), `--target-bits N` (usize width override), `--gcc PATH` (specify cross-compiler for auto-detect)
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
