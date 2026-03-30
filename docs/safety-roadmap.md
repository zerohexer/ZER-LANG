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

| Feature | Target | Lines | Breaks Existing Code? |
|---------|--------|-------|-----------------------|
| zercheck changes 1-3 | v0.2.2 (now) | ~90 | No |
| zercheck change 4 (cross-func) | v0.3 | ~300 | No |
| Iterators (`for in`) | v0.3 | ~200 | No |
| Non-zero type (`nz_u32`) | v0.3 | ~100 | No |
| Ranged integers (`u32(0..N)`) | v1.0-v2.0 | ~500-800 | No |

All features are additive. No existing ZER code breaks. The programmer opts in to stricter types for compile-time guarantees. The old patterns continue to work with runtime checks.
