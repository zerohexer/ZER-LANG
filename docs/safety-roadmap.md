# ZER-LANG Safety Roadmap — From Runtime Checks to Compile-Time Proofs

## Current State (v0.2.1)

ZER has 9 runtime safety checks in emitted C. Every check fires at runtime, every time the operation occurs.

### All Runtime Checks (Current)

| # | Check | Emitted C | Fires on |
|---|-------|-----------|----------|
| 1 | Bounds check | `_zer_bounds_check(idx, len)` | Every `arr[i]`, `slice[i]` |
| 2 | Handle gen (get) | `gen[idx] != h_gen` | Every `pool.get(h)` |
| 3 | Handle gen (free) | `gen[idx] != h_gen` | Every `pool.free(h)` |
| 4 | Division by zero | `if (_d == 0)` | Every `/` and `%` |
| 5 | Signed overflow div | `if (_d == -1 && x == MIN)` | Every signed `/` |
| 6 | *opaque type mismatch | `type_id != expected` | Every `@ptrcast` FROM `*opaque` |
| 7 | MMIO range (variable) | `if (!(addr >= start && ...))` | `@inttoptr` with variable addr |
| 8 | @cstr overflow | `if (src.len + 1 > buf_size)` | Every `@cstr(buf, slice)` |
| 9 | Arena alloc overflow | `__builtin_mul_overflow` | `arena.alloc_slice(T, n)` |

### Runtime Check Frequency in Typical Firmware

| Check | % of statements that hit it | Cost per hit |
|-------|---------------------------|-------------|
| Bounds | 30-50% | ~2 cycles |
| Handle gen | 5-10% | ~3 cycles |
| Division | 1-2% | ~2 cycles |
| Everything else | <1% each | ~3 cycles |

## Planned Features — Ordered by Priority

### Feature 1: zercheck Enhancements (Changes 1-4)

**Priority:** Immediate (v0.2.2)
**Language complexity added:** Zero. Same ZER code, compiler catches more.
**Runtime checks eliminated:** ~99% of handle gen checks
**Implementation:** ~400 lines in zercheck.c

#### Change 1: MAYBE_FREED State (~30 lines)

Current: `if (flag) { pool.free(h); }` then `pool.get(h)` — zercheck keeps `h` ALIVE (under-approximation). Misses the bug.

New: `HS_MAYBE_FREED` state. If one branch frees and the other doesn't, handle is MAYBE_FREED. Using a MAYBE_FREED handle is a compile error.

```
States: HS_UNKNOWN → HS_ALIVE → HS_FREED
                              → HS_MAYBE_FREED (new)

Merge rules:
  if/else: one frees, other doesn't → MAYBE_FREED (was: keep ALIVE)
  if/else: both free → FREED (unchanged)
  if-no-else: then frees → MAYBE_FREED (was: keep ALIVE)
```

#### Change 2: Handle Leak Detection (~40 lines)

Current: `h = pool.alloc(); h = pool.alloc()` — first handle leaked silently.

New: Overwriting an ALIVE handle → warning "handle leaked without free."
At scope exit, any ALIVE handle not returned → warning "potential leak."

#### Change 3: Loop Second Pass (~20 lines)

Current: zercheck does one pass through loop body. Misses conditional free patterns across iterations.

New: After first pass, if any handle state changed, run body once more. If still changing → widen to MAYBE_FREED.

#### Change 4: Cross-Function Analysis (~200-300 lines)

Current: `free_handle(pool, h)` wrapper function — zercheck doesn't follow the call.

New: For small functions (< 20 statements), inline the analysis — check if any parameter handle gets freed. Build function summaries: "this function frees parameter 1." Use summaries at call sites.

This is the biggest change. Requires a pre-scan of all functions to build summaries before the main analysis pass.

---

### Feature 2: Iterators (`for in`)

**Priority:** v0.3
**Language complexity added:** One new loop syntax
**Runtime checks eliminated:** ~80% of bounds checks
**Implementation:** ~200 lines (parser + checker + emitter)

#### Syntax

```zer
// Iterate over array — no bounds check, no index variable
for (x in arr) {
    x += 1;
}

// Iterate over slice — no bounds check
for (byte in data) {
    process(byte);
}

// With index (optional)
for (x, i in arr) {
    arr_b[i] = x * 2;  // i is guaranteed in range of arr, but arr_b still needs check
}
```

#### Emitted C

```c
// for (x in arr) { x += 1; }
for (size_t _i = 0; _i < sizeof(arr)/sizeof(arr[0]); _i++) {
    __typeof__(arr[0]) x = arr[_i];  // no bounds check — loop controls index
    x += 1;
}

// for (x in slice) { process(x); }
for (size_t _i = 0; _i < slice.len; _i++) {
    __typeof__(*slice.ptr) x = slice.ptr[_i];  // no bounds check
    process(x);
}
```

#### Why It Eliminates Bounds Checks

The loop itself controls the iteration variable. The compiler KNOWS `_i < len` because it emitted the condition. No bounds check needed on `arr[_i]` inside the loop — it's structurally impossible to be out of range.

The old `for (u32 i = 0; i < len; i += 1) { arr[i] }` still works and still has bounds checks — the compiler can't prove `i < arr.len` without range analysis because `i` is a user-controlled variable.

#### Mutable Iteration

```zer
// Mutable reference — modifies array in place
for (*x in arr) {
    *x += 1;
}

// Emitted C: x is a pointer to the element
for (size_t _i = 0; _i < ...; _i++) {
    __typeof__(arr[0]) *x = &arr[_i];
    *x += 1;
}
```

---

### Feature 3: Non-Zero Type (`nz_u32`)

**Priority:** v0.3
**Language complexity added:** One new type
**Runtime checks eliminated:** ~100% of division checks
**Implementation:** ~100 lines (checker + emitter)

#### Syntax

```zer
// Construction — checked once
nz_u32 divisor = nz(input) orelse return;  // if input == 0, returns

// Usage — no runtime check
u32 result = total / divisor;  // guaranteed nonzero, no check emitted

// Literals — always nonzero
nz_u32 four = 4;  // compile-time proven

// Regular division still works, still has runtime check
u32 result2 = total / maybe_zero;  // runtime check on maybe_zero
```

#### Type Rules

- `nz_u32` is a distinct type — cannot assign regular `u32` without `nz()` conversion
- `nz_u32 * nz_u32` → `nz_u32` (product of nonzeros is nonzero)
- `nz_u32 + u32` → `u32` (sum might be zero via overflow)
- `nz_u32` literal must be nonzero — `nz_u32 x = 0` is compile error
- Division: `u32 / nz_u32` emits plain division, no check
- Division: `u32 / u32` emits division with zero check (unchanged)

#### Variants

```
nz_u8, nz_u16, nz_u32, nz_u64
nz_i8, nz_i16, nz_i32, nz_i64
nz_usize
```

#### Emitted C

`nz_u32` emits as plain `uint32_t` — the nonzero guarantee is a compile-time property, no runtime representation needed. The only runtime check is at construction (`nz()` call).

---

### Feature 4: Ranged Integers (`u32(0..N)`)

**Priority:** v1.0 or v2.0
**Language complexity added:** New type syntax with range constraints
**Runtime checks eliminated:** ~remaining 19% of bounds checks (after iterators)
**Implementation:** ~500-800 lines (checker + emitter + type system)

#### Syntax

```zer
// Type carries range constraint
u32(0..256) index;

// Construction — checked once at boundary
u32(0..buf.len) idx = get_input() orelse return;

// Usage — no bounds check
buf[idx] = 5;      // compiler knows idx < buf.len
buf[idx] += 1;     // still in range

// Arithmetic narrows/widens ranges
u32(0..10) a = 5;
u32(0..10) b = 3;
u32(0..20) c = a + b;   // range widens
u32(0..100) d = a * b;  // range widens

// Narrowing requires proof
u32(0..5) e = a;  // COMPILE ERROR: a could be 6-10
u32(0..5) e = a orelse return;  // OK: runtime check, then proven
```

#### How It Eliminates Bounds Checks

```zer
u8[256] buf;
u32(0..256) idx = get_byte() orelse return;  // checked ONCE

// All of these are bounds-check-free:
buf[idx] = 1;
buf[idx] = 2;
buf[idx] = 3;
process(buf[idx]);
send(buf[idx]);
// 5 accesses, 0 bounds checks (was 5 bounds checks)
```

#### Range Propagation Through Control Flow

```zer
u32 i = get_value();
if (i < buf.len) {
    // Inside this block, compiler knows i is u32(0..buf.len)
    buf[i] = 5;  // no bounds check
}
// Outside: i is unconstrained, bounds check required
buf[i] = 5;  // runtime bounds check
```

This is the most complex feature — the checker must track range constraints through branches, loops, and arithmetic. But it's the same thing Ada/SPARK has done since 1983.

#### Emitted C

Ranged integers emit as plain C integers (`uint32_t`). The range is a compile-time-only property. No runtime representation. The construction check emits a regular comparison:

```c
uint32_t idx = get_byte();
if (idx >= 256) { return; }  // the ONE check
// everything below: no checks
buf[idx] = 1;
buf[idx] = 2;
```

---

### Feature: Generics (NOT planned — documented for reference)

**Status:** Not planned for ZER. Documented here for completeness.
**Reason skipped:** ZER uses `*opaque` with 3-layer provenance (compile-time Symbol + compound key + runtime type tag) instead of generics. This provides runtime safety for type-erased pointers without language complexity.

#### What Generics Would Look Like

```zer
// Generic function
fn register(T)(void (*handler)(*T), *T ctx) { ... }

// Generic struct
struct List(T) { *T data; u32 len; u32 cap; }

// Usage
List(Sensor) sensors;
register(Sensor)(on_sensor, &s);
```

#### Why Not

- `*opaque` with runtime type tags already catches wrong casts (100% coverage)
- Generics add ~1000+ lines of implementation (monomorphization in emitter)
- Generics interact with every other feature (optionals, comptime, distinct types)
- Template error messages are notoriously bad (C++ experience)
- Embedded firmware rarely needs user-defined generic containers
- ZER's builtins (Pool, Slab, Ring, Arena) already cover the common generic patterns
- "Same syntax as C" is a core value — generics break it

#### When Generics Might Be Reconsidered

If ZER gains adoption beyond embedded (server-side, application development), generics would reduce `*opaque` usage and improve type safety for library authors. This would be a v2.0+ consideration driven by user demand, not a proactive design decision.

---

## Safety Comparison After All Features

### ZER vs Rust (After Features 1-4)

| Category | ZER | Rust | Winner |
|----------|-----|------|--------|
| Buffer overflow | Iterators + ranged int = ~99% compile | Runtime bounds check | **ZER** |
| Use-after-free | zercheck 1-4 = ~99% compile + runtime gen | 100% compile (borrow checker) | **Rust** (slightly) |
| Null deref | `*T` non-null, `orelse` forced | `Option<T>`, `unwrap()` can panic | **ZER** (no unwrap panic) |
| Data races | Single-core + volatile + Ring barriers | `Send`/`Sync` compile-time | **Rust** (multi-core) |
| Division by zero | `nz_u32` = 100% compile (opt-in) | Runtime panic | **ZER** (with nz types) |
| Wrong type cast | 3-layer provenance | No `void*`, generics | **Tie** |
| Volatile/MMIO | Qualifier tracking + range validation | `unsafe` block | **ZER** |
| Integer overflow | Defined (wraps) | Debug: panic, Release: wraps | **Tie** |
| Memory leaks | Compile-time leak detection | `Drop` trait (leaks allowed) | **ZER** (with zercheck) |

### Runtime Check Frequency (After All Features)

| Scenario | Current | After features |
|----------|---------|---------------|
| `for (x in arr) { ... }` | Bounds check per iteration | **Zero** |
| `arr[ranged_idx]` | Bounds check per access | **Zero** (one check at construction) |
| `total / nz_divisor` | Div-zero check | **Zero** |
| `pool.get(h)` in simple path | Gen check | **Zero** (zercheck proves valid) |
| `@ptrcast(*Sensor, h.p)` | Type tag check | **Zero** (compile-time provenance map) |
| External input → index | Must check | **One check** (at boundary, unavoidable) |
| MMIO variable address | Must check | **Runtime** (inherently unknowable) |

### The SPARK/Ada Equivalence

After all four features, ZER achieves the same safety model as Ada/SPARK:
- **Proof in the core** — all operations between I/O boundaries are compile-time proven
- **Check at the edge** — runtime checks only where external data enters the system
- **Programmer chooses** — opt-in to stricter types (`nz_u32`, `u32(0..N)`, `for in`) for compile-time guarantees, or use regular types with runtime checks

This is the highest safety standard used in production systems (DO-178C Level A avionics, nuclear reactor control, military systems). No shipping language goes beyond this — only theorem provers (Coq, Lean, Agda) which are not programming languages.

---

## Implementation Timeline

### FINAL (revised)

| Feature | Target | Lines | Language Change | Breaks Existing Code? |
|---------|--------|-------|----------------|----------------------|
| zercheck changes 1-3 | v0.2.2 (now) | ~90 | **None** | No |
| zercheck change 4 (cross-func) | v0.3 | ~300 | **None** | No |
| Value range propagation | v0.3 | ~300-500 | **None** | No |
| Forced division guard | v0.3 | ~20 | **None** (new compile error only) | Existing unguarded divisions become errors |

Total: ~710-910 lines of compiler logic. Zero language changes. Zero new keywords. Zero new types. One new compile error (division requires proven nonzero divisor).

### Original (preserved for reference)

| Feature | Target | Lines | Breaks Existing Code? |
|---------|--------|-------|-----------------------|
| zercheck changes 1-3 | v0.2.2 (now) | ~90 | No |
| zercheck change 4 (cross-func) | v0.3 | ~300 | No |
| Iterators (`for in`) | v0.3 | ~200 | No |
| Non-zero type (`nz_u32`) | v0.3 | ~100 | No |
| Ranged integers (`u32(0..N)`) | v1.0-v2.0 | ~500-800 | No |

All features are additive. No existing ZER code breaks.

---

## Original Design Decisions (Preserved for Reference)

### Original Decision: Ranged Indexing — FORCED

**Status: SUPERSEDED by Value Range Propagation (see below) — no longer needed as explicit type.**

Array/slice indexing with `buf[i]` **requires** `i` to be a ranged type (`u32(0..buf.len)`) or a compile-time-provable literal. Bare `u32` as index is a compile error.

**Rationale:**
- Bounds checks are 30-50% of all runtime checks — the highest-frequency safety cost
- Buffer overflows are the #1 vulnerability class in systems code
- The boilerplate is minimal — one `orelse` per index source
- Same forcing pattern as `orelse` for optionals (already accepted by ZER programmers)
- Teaches good habit: handle the out-of-range case at the point you receive the value

```zer
// COMPILE ERROR: i is bare u32
u32 i = get_index();
buf[i] = 5;

// OK: ranged type, proven at construction
u32(0..buf.len) i = get_index() orelse return;
buf[i] = 5;  // zero runtime cost

// OK: literal, compiler proves in range
buf[0] = 5;
buf[7] = 5;

// OK: iterator, structurally bounded
for (x in buf) { x += 1; }
```

**Migration path (zer-convert / zer-upgrade):**
- `zer-convert` outputs `buf[i] = 5` (compat-level, runtime bounds check)
- `zer-upgrade` rewrites to `u32(0..buf.len) i = i orelse return; buf[i] = 5`
- Same pipeline as `malloc → compat → Slab`

### Original Decision: Non-Zero Division — OPTIONAL (not forced)

**Status: SUPERSEDED by Value Range Propagation (see below) — `nz_u32` no longer needed.**

`nz_u32` exists as an opt-in type. Regular `u32 / u32` keeps runtime div-zero check. Not forced.

**Rationale:**
- Division is only 1-2% of statements — low frequency
- Runtime div-zero check costs 2 cycles — negligible
- Forcing `nz()` on every divisor adds boilerplate with minimal safety return
- Division by zero is rarely a vulnerability (it crashes, doesn't corrupt memory)

```zer
// Both are valid:
u32 result = total / count;              // runtime check — SAFE, 2 cycles
nz_u32 safe_count = nz(count) orelse 1;
u32 result = total / safe_count;          // no check — programmer opted in
```

**Smart compiler inference (no boilerplate needed):**
- `total / 4` — literal, compiler proves nonzero
- `total / (a + 1)` — unsigned a+1 >= 1, compiler proves nonzero
- `total / nz_val` — nz type, proven
- `total / count` — unknown, runtime check

### Original Decision: Iterators — OPTIONAL (not forced)

**Status: UNCHANGED — iterators still planned as optional cleaner syntax.**

`for (x in arr)` is available alongside `for (u32 i = 0; ...)`. Not forced — both work.

**Rationale:**
- Some loops genuinely need an index variable (e.g., writing to a second array)
- The indexed loop still has runtime bounds checks — safe, just slower
- Iterators are the **better tool** but forcing them would break legitimate patterns

```zer
// Both valid:
for (x in buf) { process(x); }                          // no bounds check
for (u32 i = 0; i < buf.len; i += 1) { buf2[i] = buf[i]; }  // bounds checks on both
```

### Original Summary Table

| Feature | Enforcement | Reason |
|---------|------------|--------|
| Ranged indexing | **FORCED** | High frequency (30-50%), #1 vulnerability class, minimal boilerplate |
| Non-zero division | Optional | Low frequency (1-2%), negligible runtime cost, annoying to force |
| Iterators | Optional | Better tool, not the only valid tool |
| zercheck 1-4 | Automatic | Compiler improvement, no programmer involvement |

---

## REVISED DESIGN: Value Range Propagation (Replaces nz_u32 and Explicit Ranged Types)

### The Key Insight

`nz_u32` and `u32(0..N)` are unnecessary new types IF the compiler is smart enough to infer value constraints from existing code patterns. Programmers already write guard checks — the compiler just needs to notice them.

**One compiler pass — value range propagation — eliminates:**
- Bounds checks after `if (i < len)` guards
- Div-zero checks after `if (x != 0)` guards
- Both without ANY new syntax, types, or keywords

### How It Works

The compiler tracks what values a variable CAN hold based on prior conditionals:

```zer
// BOUNDS: compiler infers i < buf.len from guard
u32 i = get_index();
if (i >= buf.len) { return; }
buf[i] = 5;     // compiler: skip bounds check — proven i < buf.len

// DIVISION: compiler infers d != 0 from guard
u32 d = get_divisor();
if (d == 0) { return; }
total / d;       // compiler: skip div check — proven d != 0

// LOOPS: compiler infers i < buf.len from loop condition
for (u32 i = 0; i < buf.len; i += 1) {
    buf[i] = 5; // compiler: skip bounds check — i < buf.len by loop structure
}

// LITERALS: compiler knows the value
buf[0] = 5;      // 0 < any len — skip check
total / 4;        // 4 != 0 — skip check
```

**Zero language complexity added. Zero new types. Zero new keywords.**
The programmer writes normal C-style guards they'd write anyway. Same code ZER already accepts. Just smarter emission.

### What the Compiler Tracks Per Variable

At each program point, the compiler maintains:
```
Variable → { min_value, max_value, known_nonzero }
```

**Narrowing events (update constraints):**
- `if (i < N)` → inside the then-block: `i.max = N - 1`
- `if (i >= N)` then return → after the if: `i.max = N - 1`
- `if (x != 0)` → inside the then-block: `x.known_nonzero = true`
- `if (x == 0) { return; }` → after the if: `x.known_nonzero = true`
- `for (i = 0; i < N; ...)` → inside body: `i.min = 0, i.max = N - 1`
- Literal assignment: `x = 5` → `x.min = 5, x.max = 5, x.known_nonzero = true`

**Widening events (lose constraints):**
- Assignment from unknown: `x = get_value()` → constraints reset
- Function call might modify: only for globals/pointers
- Loop exit: loop variable constraints no longer valid

### Runtime Check Elimination Rules

```
Bounds:   arr[i]   → skip if i.max < arr.len (proven in range)
Division: a / b    → skip if b.known_nonzero (proven nonzero)
Division: a / b    → skip if b.min > 0 (proven positive)
```

### Examples: Before and After

```zer
// Example 1: sensor array processing
u32 idx = read_sensor_id();
if (idx >= 16) { idx = 0; }        // guard: idx now in 0..15
sensors[idx].value = read_adc();     // NO bounds check (was: runtime)

// Example 2: averaging with guard
u32 count = get_sample_count();
if (count == 0) { return; }         // guard: count now nonzero
u32 avg = total / count;             // NO div check (was: runtime)

// Example 3: standard for loop
for (u32 i = 0; i < data.len; i += 1) {
    data[i] = process(data[i]);      // NO bounds check per iteration (was: runtime)
}

// Example 4: no guard, unknown value
u32 i = get_index();
buf[i] = 5;                          // runtime bounds check (can't prove)
```

### This Replaces ALL Planned Language Features

| Original Feature | Status | Replaced By |
|-----------------|--------|-------------|
| `nz_u32` (non-zero type) | **REPLACED** | Range propagation infers nonzero from `if (x != 0)` guards |
| `u32(0..N)` (ranged integers) | **REPLACED** | Range propagation infers bounds from `if (i < N)` guards |
| Forced ranged indexing | **REPLACED** | Not needed — compiler auto-eliminates checks when guards exist |
| Iterators (`for in`) | **NOT NEEDED for safety** | Range propagation handles `for (i = 0; i < len; ...)` loops. Iterators may be added later as ergonomic improvement, not a safety feature. |

### FINAL PLAN: Two Compiler Features + One Forced Rule, Zero Language Complexity

| Feature | Type | Language change | Implementation |
|---------|------|----------------|----------------|
| zercheck 1-4 | Compiler improvement | **None** | ~400 lines in zercheck.c |
| Value range propagation | Compiler improvement | **None** | ~300-500 lines in checker.c |
| Forced division guard | Compiler rule | **None** (just a new error) | ~20 lines in checker.c |

**All three are invisible to the programmer's syntax.** Same ZER code. Same C syntax. The compiler gets smarter about what it can prove, and forces one more safety invariant (divisor nonzero). No new types, no new keywords, no new syntax.

**Forced division** is the same pattern as existing forced rules:
- `*T` requires initializer (already forced)
- `?T` requires `orelse` or `if |capture|` (already forced)
- Division requires proven nonzero divisor (NEW — closes last C UB gap)

**Iterators are NOT on the safety roadmap.** They may be added in the future as a syntax convenience (ergonomics), but they are not needed for compile-time safety — value range propagation covers the same loop patterns. If iterators are added, it's a separate decision driven by programmer ergonomics, not safety.

### Every C Undefined Behavior → ZER Compile Error

| C Undefined Behavior | ZER Prevention | Type |
|----------------------|----------------|------|
| Buffer overflow | Bounds check (runtime) + range propagation (compile-time) | Automatic |
| Null pointer deref | `*T` non-null, `?T` forced `orelse` | Forced |
| Use-after-free | Handle gen counter + zercheck | Automatic |
| Division by zero | **Forced nonzero proof** | **Forced** |
| Signed overflow | Defined as wrapping (`-fwrapv`) | Automatic |
| Uninitialized memory | Auto-zero everything | Automatic |
| Double free | Handle gen counter + zercheck | Automatic |
| Out-of-bounds pointer | Scope escape analysis | Automatic |
| Type confusion (void*) | `_zer_opaque` runtime type tag + compile-time provenance | Automatic |
| Volatile access reorder | Qualifier tracking on all casts | Automatic |

**Zero C undefined behaviors remain in ZER.** Every one is either prevented at compile time, trapped at runtime, or defined as safe behavior (wrapping).

### Coverage After Range Propagation + Forced Division Guard

| Category | Compile-time | Remaining runtime |
|----------|-------------|-------------------|
| Bounds (with guard or loop) | **100%** | Only unguarded bare `buf[unknown_i]` |
| Division | **100%** | **None — FORCED** (divisor must be proven nonzero) |
| Handle UAF (zercheck 1-4) | **~99%** | ISR + cinclude edge cases |
| Handle leaks | **100%** | None |
| Null deref | **100%** | None |
| *opaque cast | **~98%** | Variable index + function return |
| Scope escape | **~95%** | Deep indirection chains |
| Union type confusion | **100%** | None |
| Volatile stripping | **100%** | None |
| MMIO range | **~99%** | Computed variable addresses (rare) |
| @cstr overflow | **~50%** | Variable slice source |
| Arena overflow | **0%** | Inherently runtime |

**Overall: ~97% compile-time for guarded code, ~92% for unguarded code.**

### Forced: Division Guard (DECISION)

Division by zero is **undefined behavior in C** — can crash, corrupt memory, or do anything. ZER eliminates all C undefined behaviors at compile time. Division was the last gap. Now forced.

If the compiler cannot prove the divisor is nonzero, it's a **compile error**:

```zer
u32 x = total / count;  // COMPILE ERROR: divisor 'count' not proven nonzero
```

**How the programmer proves it (one of these):**

```zer
// Option 1: guard (most common)
if (count == 0) { return; }
u32 x = total / count;     // OK — compiler tracks count != 0 after guard

// Option 2: literal
u32 x = total / 4;          // OK — 4 is provably nonzero

// Option 3: loop variable starting > 0
for (u32 i = 1; i <= 10; i += 1) {
    total / i;               // OK — i starts at 1, always >= 1
}

// Option 4: orelse fallback (future, when orelse supports value exprs)
u32 x = total / (count orelse 1);  // OK — fallback guarantees nonzero
```

**Error message guides the fix:**
```
main.zer:5: error: divisor 'count' not proven nonzero —
    add 'if (count == 0) { return; }' before division,
    or use a literal divisor
```

**Why forced:**
- Closes the last C undefined behavior gap in ZER
- Consistent with existing forced patterns: `*T` requires init, `?T` requires `orelse`
- One `if` line per division — same guard C programmers should write anyway
- Division is 1-2% of statements — low boilerplate cost

### What Happens to Unguarded Bounds?

Bounds indexing is NOT forced (unlike division). Unguarded `buf[i]` keeps runtime check:

```zer
u32 i = get_index();
buf[i] = 5;  // no guard — runtime bounds check (safe, 2 cycles)
```

**Why not forced:** Bounds checks are 30-50% of statements. Forcing guards on every index access would be extremely verbose. The runtime check is cheap (2 cycles) and the common patterns (for loops, guarded access) are automatically optimized by range propagation. Unguarded access is safe via runtime — no UB, just overhead.

**Compiler warning (optional future enhancement):** "unguarded index access, consider adding bounds check." Nudges programmer toward guards without forcing them. The programmer sees the warning, adds the guard, the warning disappears, the runtime check is eliminated.

### Implementation

**Where it lives:** New pass in checker.c, between type checking and emitter. Or integrated into check_expr/check_stmt with a `VarRange` map on the Checker struct.

**Data structure:**
```c
typedef struct {
    const char *name;
    uint32_t name_len;
    int64_t min_val;     // INT64_MIN = unknown lower bound
    int64_t max_val;     // INT64_MAX = unknown upper bound
    bool known_nonzero;
} VarRange;
```

**Estimated size:** ~300-500 lines in checker.c. Handles:
- `if (x < N)` / `if (x >= N)` / `if (x == 0)` / `if (x != 0)` narrowing
- For loop condition inference
- Literal assignment
- Reset on unknown assignment
- Propagation through simple arithmetic (`x + 1` shifts range by 1)

**Does NOT handle (acceptable):**
- Cross-function range inference (would need function summaries)
- Complex arithmetic (`x * y` range = full product range, usually unknown)
- Pointer-derived values (ranges don't apply)

### The ZER Philosophy

**Same code a C programmer writes. Same guards they'd write anyway. The compiler just rewards them by skipping redundant checks.**

No new types. No new keywords. No new annotations. No new syntax. The programmer writes natural defensive code. The compiler notices and optimizes.

This is the opposite of Rust's approach (force new concepts on the programmer) and the opposite of Ada's approach (force ranged types). ZER's approach: **write C, get SPARK-level safety, because the compiler is smart enough to see what you already told it.**

### Debug/Release Flag (Future)

Not implemented yet. Future addition:
- `zerc --debug`: emit ALL runtime checks, even on proven paths (validates compiler reasoning)
- `zerc` or `zerc --release`: skip proven checks (production performance)

Default behavior until flag exists: proven paths skip runtime checks.
