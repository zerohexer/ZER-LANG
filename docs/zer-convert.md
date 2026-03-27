# zer-convert — Automated C to ZER Migration Tool

## Overview

`zer-convert` is a two-phase tool that converts C source code to memory-safe ZER code with 100% automation. No manual rewriting. No guessing. The tool either converts fully or tells you exactly what to fix.

**The pitch:** Paste your C. Get memory safety. Zero rewrite.

No other language offers this. Rust requires learning a new paradigm. Zig requires understanding comptime. ZER says: same code, two commands, done.

---

## Architecture

```
Phase 1: zer-convert (syntax transform)
  legacy.c → legacy.zer
  100% automated, zero analysis needed
  Uses compat.zer scaffolding for unsafe patterns

Phase 2: zerc --safe-upgrade (semantic rewrite)
  legacy.zer → legacy_safe.zer
  Compiler analyzes compat builtins, replaces with safe ZER
  What can't be proven safe stays with a warning

Full pipeline:
  legacy.c → zer-convert → legacy.zer → zerc --safe-upgrade → legacy_safe.zer
                (phase 1)                     (phase 2)
```

---

## Phase 1: Syntax Transform (zer-convert)

Phase 1 is a mechanical, 1:1 translation. Every C construct maps to a ZER construct. No analysis. No decisions. The output compiles and runs identically to the C original.

### Complete Transform Table

```
C                                ZER (Phase 1 output)
───────────────────────────────  ─────────────────────────────────────────
// Types
int                           →  i32
unsigned int                  →  u32
uint8_t                       →  u8
uint16_t                      →  u16
uint32_t                      →  u32
uint64_t                      →  u64
int8_t                        →  i8
int16_t                       →  i16
int32_t                       →  i32
int64_t                       →  i64
size_t                        →  usize
float                         →  f32
double                        →  f64
char                          →  u8
_Bool                         →  bool
void                          →  void

// Pointers
Node *ptr                     →  *Node ptr
const Node *ptr               →  const *Node ptr
volatile uint32_t *reg        →  volatile *u32 reg
void *p                       →  *opaque p

// Arrays
int arr[10]                   →  i32[10] arr
char buf[256]                 →  u8[256] buf

// Operators
i++                           →  i += 1
i--                           →  i -= 1
(uint32_t)x                  →  @truncate(u32, x)
(Node *)p                    →  @ptrcast(*Node, p)
NULL                          →  null

// Memory
malloc(sizeof(Node))          →  zer_malloc(Node)
calloc(n, sizeof(Node))       →  zer_calloc(Node, n)
realloc(buf, new_size)        →  zer_realloc(buf, new_size)
free(ptr)                     →  zer_free(ptr)

// Pointer arithmetic
ptr + offset                  →  zer_ptr_add(ptr, offset)
ptr - offset                  →  zer_ptr_sub(ptr, offset)
ptr++                         →  ptr = zer_ptr_add(ptr, 1)
ptr--                         →  ptr = zer_ptr_sub(ptr, 1)
ptr[i] (via pointer)          →  zer_ptr_index(ptr, i)

// String functions
strlen(s)                     →  zer_strlen(s)
strcmp(a, b)                  →  zer_strcmp(a, b)
memcmp(a, b, n)              →  zer_memcmp(a, b, n)
memcpy(dst, src, n)           →  zer_memcpy(dst, src, n)
strdup(s)                     →  zer_strdup(s)

// Structs
struct Node { ... };          →  struct Node { ... }
struct Node var;              →  Node var;

// Control flow (mostly identical)
for (int i = 0; i < n; i++)  →  for (i32 i = 0; i < n; i += 1)
switch/case/break             →  switch/.arm => { } (ZER switch syntax)
```

### What Phase 1 does NOT do

- Does NOT analyze lifetimes
- Does NOT decide which allocator to use
- Does NOT remove unsafe patterns
- Does NOT rewrite logic

It's a syntax translator. The output uses `compat.zer` builtins for every unsafe C pattern. The output compiles and runs, but isn't memory-safe yet. That's Phase 2's job.

### Switch Statement Conversion

C switch requires special handling because ZER switch syntax differs:

```c
// C:
switch (state) {
    case STATE_IDLE:
        handle_idle();
        break;
    case STATE_RUNNING:
    case STATE_BUSY:
        handle_active();
        break;
    default:
        handle_unknown();
        break;
}

// ZER:
switch (state) {
    .idle => { handle_idle(); }
    .running, .busy => { handle_active(); }
    default => { handle_unknown(); }
}
```

For integer switches (not enums), the converter emits the integer values directly:
```c
// C:
switch (x) {
    case 0: return 1;
    case 1: return 2;
    default: return 0;
}

// ZER:
switch (x) {
    0 => { return 1; }
    1 => { return 2; }
    default => { return 0; }
}
```

### Preprocessor Handling

```c
// C preprocessor → ZER equivalents
#define SIZE 256             →  // const usize SIZE = 256; (if simple constant)
#define MAX(a,b) ((a)>(b)?(a):(b))  →  // inline function (manual review)
#include "header.h"          →  import module;  (if ZER module exists)
#include <stdio.h>           →  cinclude "<stdio.h>";
#ifdef _WIN32 ... #endif     →  // platform conditionals need manual review
```

Complex macros are flagged for manual review. Simple constant macros auto-convert.

---

## The Compat Library (compat.zer)

**This is NOT part of ZER.** It's scaffolding for conversion. It exists only during the migration process. The final safe output does not import it.

```zer
// lib/compat.zer — C compatibility scaffolding
// DO NOT use in new ZER code. This exists only for zer-convert output.
// zerc --safe-upgrade removes all compat imports.

cinclude "<stdlib.h>"
cinclude "<string.h>"

// Memory allocation wrappers
// Tagged so zerc --safe-upgrade can find and replace them

// zer_malloc(T) — allocates one T, returns ?*T
// Phase 2 replaces with: slab.alloc() or arena.alloc()
?*opaque zer_malloc_raw(usize size) {
    *opaque p = @inttoptr(*opaque, c_malloc(size));
    // returns null on failure
    return p;
}

// zer_free(ptr) — frees one allocation
// Phase 2 replaces with: slab.free(handle)
void zer_free(*opaque ptr) {
    c_free(ptr);
}

// zer_calloc(T, n) — allocates n zeroed Ts
// Phase 2 replaces with: arena.alloc_slice(T, n)
?*opaque zer_calloc_raw(usize count, usize size) {
    *opaque p = @inttoptr(*opaque, c_calloc(count, size));
    return p;
}

// zer_realloc(ptr, new_size) — resize allocation
// Phase 2 replaces with: new Slab alloc + copy + free old
?*opaque zer_realloc_raw(*opaque ptr, usize new_size) {
    *opaque p = @inttoptr(*opaque, c_realloc(@ptrtoint(ptr), new_size));
    return p;
}

// Pointer arithmetic wrappers
// Phase 2 replaces with: slice indexing

// zer_ptr_add(ptr, offset) — advance pointer by offset elements
*opaque zer_ptr_add(*opaque ptr, usize offset, usize elem_size) {
    return @inttoptr(*opaque, @ptrtoint(ptr) + offset * elem_size);
}

// zer_ptr_index(ptr, i) — index into pointer (like ptr[i])
// Phase 2 replaces with: slice[i] (bounds-checked)
*opaque zer_ptr_index(*opaque ptr, usize i, usize elem_size) {
    return @inttoptr(*opaque, @ptrtoint(ptr) + i * elem_size);
}

// String wrappers
// Phase 2 replaces with: []u8 slice operations

usize zer_strlen(const []u8 s) {
    usize i = 0;
    while (i < s.len) {
        if (s[i] == 0) { return i; }
        i += 1;
    }
    return i;
}

bool zer_strcmp(const []u8 a, const []u8 b) {
    if (a.len != b.len) { return false; }
    for (usize i = 0; i < a.len; i += 1) {
        if (a[i] != b[i]) { return false; }
    }
    return true;
}
```

### Why compat builtins are tagged

Every compat function has a known name prefix (`zer_`). Phase 2 searches for these by name. When it finds `zer_malloc(Node)`, it knows:
- The type is `Node`
- It needs a `Slab(Node)` or `Arena` allocation
- The corresponding `zer_free` tells it the lifetime pattern

This is why Phase 2 works — the compat builtins carry enough type information for the compiler to make safe replacement decisions.

---

## Phase 2: Safe Upgrade (zerc --safe-upgrade)

Phase 2 is a compiler pass that reads Phase 1 output, analyzes compat builtin usage, and replaces them with safe ZER constructs.

### The Algorithm

```
Step 1: Scan — find all zer_malloc/zer_free/zer_realloc calls
  Record: type, variable name, line number, scope

Step 2: Match — pair mallocs with frees
  Same function, same variable → matched pair
  Cross-function → flag for Slab (shared allocator)
  No free found → flag for Arena (bulk free)
  realloc found → flag for Slab (fixed slot, copy pattern)

Step 3: Group — collect by type
  All Node mallocs → static Slab(Node) nodes;
  All Token mallocs → static Slab(Token) tokens;
  Temporary buffers → Arena

Step 4: Rewrite — emit safe ZER
  zer_malloc(Node)     → nodes.alloc() orelse return
  zer_free(n)          → nodes.free(n_handle)
  n->kind              → nodes.get(n_handle).kind
  zer_ptr_add(p, i)    → slice[i]
  zer_realloc(buf, sz) → new alloc + copy + free old

Step 5: Clean — remove compat import
  Remove: import compat;
  Remove: any remaining zer_ prefixed calls (error if can't replace)
```

### Replacement Rules

```
Pattern                              Safe Replacement
──────────────────────────────────── ──────────────────────────────────
zer_malloc(T) + zer_free(ptr)     →  Slab(T) + alloc/free
  same scope, matched pair            (individual alloc + free)

zer_malloc(T), never freed         →  Arena.alloc(T)
  allocated once, lives forever       (bulk free on reset)

zer_calloc(T, n) + no free         →  Arena.alloc_slice(T, n)
  array allocation, lives forever     (bulk free on reset)

zer_realloc(buf, size)             →  new_slab.alloc() + copy + old_slab.free()
  resize = new alloc + copy            (Slab slots are fixed size)

zer_ptr_add(arr, i)                →  arr[i]
  pointer arithmetic                   (bounds-checked slice index)

zer_ptr_index(buf, i)              →  buf[i]
  pointer indexing                     (bounds-checked)

zer_strlen(s)                      →  s.len
  string length                        (slice carries length)

zer_strcmp(a, b)                    →  bytes_equal(a, b)
  string comparison                    (ZER stdlib helper)

zer_memcpy(dst, src, n)            →  copy_bytes(dst, src)
  memory copy                          (ZER stdlib helper, bounds-checked)
```

### What Phase 2 Cannot Auto-Upgrade

These cases emit a warning instead of failing:

```
WARNING: zer_realloc at line 42 — manual review needed
  Reason: realloc changes pointer identity. Safe replacement requires
  restructuring the data flow. Consider using Slab with fixed max size.

WARNING: zer_malloc(opaque) at line 87 — type unknown
  Reason: malloc(variable_size) has no static type. Specify the type
  in the C code before converting: malloc(sizeof(Node)) instead of malloc(n).

WARNING: zer_ptr_add at line 103 — stride unknown
  Reason: pointer arithmetic on void*. Cast to typed pointer in C first.
```

The file still compiles and runs (compat builtins are functional). The warnings tell you exactly what to fix for full safety.

---

## Usage

### Convert a single file

```bash
# Phase 1: C → ZER (with compat scaffolding)
zer-convert parser.c -o parser.zer

# Phase 2: unsafe ZER → safe ZER
zerc --safe-upgrade parser.zer -o parser_safe.zer

# Or both in one command:
zer-convert --full parser.c -o parser_safe.zer
```

### Convert an entire project

```bash
# Convert all .c files in a directory
zer-convert project/src/ -o project/zer-src/

# Upgrade all converted files
zerc --safe-upgrade project/zer-src/ -o project/zer-safe/

# Report: what was converted, what needs manual review
zer-convert --report project/src/
```

### Report output example

```
=== zer-convert report for project/src/ ===

parser.c (1500 lines):
  ✓ 47 type conversions (int→i32, uint8_t→u8, etc.)
  ✓ 23 operator conversions (i++→i+=1, casts→@truncate)
  ✓ 8 malloc/free pairs → Slab(Node) x3, Slab(Token) x2, Arena x3
  ✓ 12 pointer arithmetic → slice indexing
  ⚠ 2 realloc sites → manual review needed (lines 234, 567)
  ⚠ 1 void* cast chain → manual review (line 89)
  Result: 95% automated, 3 manual review items

checker.c (1950 lines):
  ✓ 62 type conversions
  ✓ 31 operator conversions
  ✓ 5 malloc/free pairs → Slab(Symbol) x2, Arena x3
  ✓ 0 pointer arithmetic
  ✓ 0 realloc
  Result: 100% automated

Total: 97% automated across 8 files, 5 manual review items
```

---

## Design Decisions

### Why two phases instead of one?

Phase 1 guarantees correctness — the output always compiles and runs identically to the C input. Phase 2 is an optimization pass that may not handle every case. Splitting them means you never get stuck: worst case, you have working (unsafe) ZER code that you can manually improve.

### Why compat builtins instead of keeping raw malloc?

ZER the language has no `malloc`. If we allowed raw `malloc` in ZER, it would be a permanent escape hatch that undermines the safety model. The compat builtins are:
1. Clearly marked as temporary (`zer_` prefix)
2. Searchable by the compiler (Phase 2 knows what to replace)
3. Removable (Phase 2 deletes the import)
4. Not part of the language spec

### Why not just pattern-match C directly into safe ZER?

Because it requires solving the aliasing problem (undecidable in general). Phase 1 avoids this entirely — it doesn't analyze, just translates. Phase 2 has more information because ZER's type system is stricter than C's, making the analysis tractable.

### Why refuse instead of guess?

If Phase 2 can't prove a replacement is safe, it leaves the compat builtin in place and warns. The code still works. This is better than guessing wrong and introducing a bug that didn't exist in the C original.

---

## Compat Library Lifecycle

```
Day 0:   C project with malloc/free everywhere
         ↓
Day 1:   zer-convert → ZER project with compat imports
         Code compiles and runs identically to C
         ↓
Day 2:   zerc --safe-upgrade → most compat calls replaced
         Manual review for warnings (realloc, void*, etc.)
         ↓
Day 3:   Fix remaining warnings manually
         Remove compat import
         ↓
Day 4:   Pure ZER. No compat. Full safety.
         compat.zer deleted from project.
```

The compat library is training wheels. You remove them when you can ride.

---

## Implementation Status

| Component | Status |
|-----------|--------|
| Phase 1: zer-convert | Not started (v0.3 scope) |
| Phase 2: zerc --safe-upgrade | Not started (v0.3 scope) |
| compat.zer library | Not started (v0.3 scope) |
| Report generator | Not started (v0.3 scope) |

### Implementation Order

1. `compat.zer` — write the scaffolding library first
2. Phase 1 converter — C lexer/parser that emits ZER syntax
3. Phase 2 safe-upgrade — compiler pass that replaces compat calls
4. Report generator — summary of conversion results
5. Test on zerc's own codebase — convert lexer.c → lexer.zer as proof

---

## Scope Limitations

`zer-convert` targets C code. NOT C++. NOT K&R-style C.

### True limitations (out of scope)

- **C++ code** — classes, templates, exceptions, RAII. Not C. Not convertible.
- **Computed gotos** (`goto *label_ptr`) — niche GCC extension, no ZER equivalent.

### Handled via cinclude (already works, zero conversion needed)

- **Assembly inline** (`__asm__`) — `cinclude` passes C headers through to GCC unchanged. Assembly blocks in C headers work as-is. No conversion needed.
- **Platform-specific headers** (`#include <windows.h>`) — `cinclude` handles all C headers.

### Handled via compat builtins (Phase 1 converts, Phase 2 replaces)

- **`setjmp`/`longjmp`** — Phase 1: `zer_setjmp()`/`zer_longjmp()` compat wrappers. Phase 2: replaces with ZER error handling (`?T` return types, `orelse` propagation). The C pattern "setjmp for error recovery" maps to "optional returns with orelse."
- **`alloca`** — Phase 1: `zer_alloca(T, n)` compat wrapper. Phase 2: replaces with `Arena.alloc_slice(T, n)`. Same concept — bump allocation on a buffer. Arena is stack-like but bounded and safe.

### Handled via syntax transform (Phase 1 converts directly)

- **Bit fields** — C `struct { uint8_t flag : 1; uint8_t mode : 3; }` converts to ZER bit extraction: `val[0..0]` for flag, `val[3..1]` for mode. ZER's bit syntax is more explicit and works with any integer, not just struct fields.
- **Simple macros** — `#define SIZE 256` → `const usize SIZE = 256;`. `#define MAX(a,b) ((a)>(b)?(a):(b))` → inline function. Auto-converted.
- **Conditional compilation** — `#ifdef _WIN32` → ZER doesn't have a preprocessor, but `cinclude` blocks can wrap platform-specific C headers. Platform detection moves to build system (Makefile), not source code.

### Handled via automated cinclude extraction

- **Complex preprocessor metaprogramming** — recursive macros, X-macros with `##` token pasting. The converter extracts them into a `.h` file and adds `cinclude "macros.h"` automatically. GCC expands the macros normally. Zero manual work.
- **Variadic functions** (`printf(fmt, ...)`) — ZER doesn't have varargs. The converter keeps variadic calls in a `.h` wrapper and adds `cinclude`. Calls work unchanged through GCC.
