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

#### Change 1: MAYBE_FREED State — DONE

`HS_MAYBE_FREED` state added. If one branch frees and the other doesn't, handle is MAYBE_FREED. Using or freeing a MAYBE_FREED handle is a compile error.

```
States: HS_UNKNOWN → HS_ALIVE → HS_FREED
                              → HS_MAYBE_FREED

Merge rules:
  if/else: one frees, other doesn't → MAYBE_FREED
  if/else: both free → FREED
  if-no-else: then frees → MAYBE_FREED
  switch: all arms free → FREED, some arms free → MAYBE_FREED
```

#### Change 2: Handle Leak Detection — DONE

Overwriting an ALIVE handle → error "handle leaked without free."
At function exit, any ALIVE or MAYBE_FREED handle not freed → error "handle leaked."
Parameter handles excluded — caller is responsible.

#### Change 3: Loop Second Pass — DONE

After first loop pass, if any handle state changed from pre-loop, run body once more. If state still unstable → widen to MAYBE_FREED. Catches conditional free patterns that span iterations.

#### Change 4: Cross-Function Analysis (~200-300 lines)

**DONE.** Pre-scan builds `FuncSummary` for each function with Handle params. Summary records which params are definitely freed vs conditionally freed. At call sites, summary effects are applied to caller's PathState. Reuses existing `zc_check_stmt` walker with error suppression during summary phase.

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
| zercheck changes 1-3 | **DONE** | ~90 | **None** | No |
| zercheck change 4 (cross-func) | **DONE** | ~130 | **None** | No |
| Value range propagation | **DONE** | ~150 | **None** | No |
| Forced division guard | **DONE** | ~10 | **None** (new compile error only) | Unguarded variable divisions become errors |
| Bounds auto-guard | **DONE** | ~100 | **None** (invisible auto-insert) | No — invisible to programmer |
| Auto keep on fn ptr pointer-params | **DONE** | ~15 | **None** (invisible, auto-inserted) | No — invisible to programmer |
| @cstr auto-orelse | **DONE** | ~15 | **None** (auto-inserted) | No — invisible |
| Array-level *opaque provenance | **DONE** | ~20 | **None** | Heterogeneous *opaque arrays become errors |
| Cross-function provenance summaries | **DONE** | ~50 | **None** | No — extends checker infrastructure |

Total: ~830-1030 lines of compiler logic. Zero new language syntax. Three new compile errors. One existing keyword (`keep`) extended. One auto-inserted `orelse`. Runtime checks kept as belt-and-suspenders backup on all paths.

### Remaining Runtime Checks (Belt-and-Suspenders Only)

After all planned features, these runtime checks remain in the emitted C as **backup safety nets**. The compile-time analysis catches them first — the runtime check is a redundant layer that should never fire in correct code.

| Check | Compile-time | Runtime (backup) | When runtime fires |
|-------|-------------|-----------------|-------------------|
| Bounds | Forced guard proves in range | `_zer_bounds_check` stays in emitted C | Never (guard already caught it) |
| Division | Forced guard proves nonzero | `if (_d == 0)` stays in emitted C | Never (guard already caught it) |
| Handle gen | zercheck proves valid | Gen counter check stays | Never (zercheck already caught it) |
| `*opaque` type | Compile-time provenance (all paths) | `type_id` check stays in emitted C | Never (provenance already caught it) |
| @cstr overflow | Auto-orelse handles failure | Length check inside @cstr stays | Triggers → auto-orelse handles it |
| MMIO range | Constant addresses proven | Variable address check stays | Only computed MMIO (rare) |
| Arena overflow | `?[]T` forces `orelse` — overflow → null → handled | `__builtin_mul_overflow` stays | **Already 100% safe** — `orelse` forces programmer to handle failure |

**Runtime checks are NEVER removed.** Even when the compiler proves safety at compile time, the runtime check remains as a second layer. Belt and suspenders. If the compiler has a bug in its range propagation, the runtime catches it.

**Future `--release` flag** may strip proven runtime checks for performance. But default always keeps both layers.

### *opaque 100% Compile-Time Coverage

Three mechanisms close all `*opaque` gaps:

**1. Array-level provenance (NEW — closes variable-index gap):**
When any element of a `*opaque` array is assigned, provenance is stored under BOTH the element key AND the array root key:
```
callbacks[0] = @ptrcast(*opaque, &sensor)
  → prov_map["callbacks[0]"] = *Sensor    // element-level
  → prov_map["callbacks"] = *Sensor       // array-level (NEW)

callbacks[1] = @ptrcast(*opaque, &motor)
  → prov_map["callbacks[1]"] = *Motor
  → prov_map["callbacks"] CONFLICT: *Sensor vs *Motor → COMPILE ERROR
```
All elements must have the same provenance. `@ptrcast(*Sensor, callbacks[i])` checks against array-level provenance — proven for ANY `i`.

Forces homogeneous `*opaque` arrays. Heterogeneous arrays (different types per element) get compile error — use separate arrays per type.

~10 lines in checker.c (extend `prov_map_set` to also set root key on array assignments).

**2. Cross-function provenance summaries (NEW — closes function-return gap):**
When a function returns `*opaque`, the compiler analyzes the function body and records what provenance the return value carries:
```zer
*opaque get_ctx() { return stored_ctx; }  // summary: returns provenance of stored_ctx

*Sensor s = @ptrcast(*Sensor, get_ctx());  // checks against function's provenance summary
```
Same infrastructure as zercheck change 4 (cross-function analysis). Applied to provenance instead of handle state.

~50 lines on top of zercheck change 4's function summary infrastructure.

**3. Existing mechanisms (unchanged):**
- Symbol-level provenance for simple idents
- Compound key `prov_map` for struct fields and constant indices

**Coverage after all three:**

| Path | Mechanism | Coverage |
|------|-----------|----------|
| `ctx` (simple ident) | Symbol provenance_type | **Compile-time** |
| `h.p` (struct field) | prov_map compound key | **Compile-time** |
| `arr[0]` (constant index) | prov_map compound key | **Compile-time** |
| `arr[i]` (variable index) | prov_map array-level key | **Compile-time** |
| `get_ctx()` (function return) | Cross-function summary | **Compile-time** |

**100% compile-time. Runtime type_id tag stays as belt-and-suspenders backup.**

### SPARK/Ada Comparison (Updated)

| Check | ZER | SPARK/Ada |
|-------|-----|-----------|
| `*opaque` type safety | **100% compile-time** + runtime backup | **N/A** (no void* — avoids the feature) |
| @cstr overflow | **100%** (auto-orelse) | **N/A** (no @cstr — avoids the feature) |
| ~~Arena overflow~~ | ~~Runtime (inherent)~~ | **ALREADY SAFE** — `?[]T` forces `orelse`, overflow returns null |
| MMIO computed address | Runtime (inherent) | Runtime (same — inherent) |

**ZER now matches SPARK's compile-time coverage while having features SPARK doesn't offer.** The only remaining runtime checks (arena overflow, MMIO computed address) are inherent — SPARK doesn't do better, it just doesn't have the feature.

**ZER's philosophy: have the feature, make it safe, prove it at compile time, AND keep runtime backup.**
**SPARK's philosophy: don't have the feature.**

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

### FINAL PLAN: Two Compiler Features + Three Forced Rules, Zero New Syntax

| Feature | Type | Language change | Implementation |
|---------|------|----------------|----------------|
| zercheck 1-4 | Compiler improvement | **None** | ~400 lines in zercheck.c |
| Value range propagation | Compiler improvement | **None** | ~300-500 lines in checker.c |
| Forced division guard | Compiler rule | **None** (new error only) | ~20 lines in checker.c |
| Bounds auto-guard | Compiler + Emitter | **None** (invisible auto-insert) | ~60-80 lines in checker.c + emitter.c |
| Auto keep on fn ptr pointer-params | Compiler rule | **None** (invisible, auto-inserted) | ~10 lines in checker.c |
| @cstr auto-orelse | Compiler + Emitter | **None** (auto-inserted) | ~30 lines in checker.c + emitter.c |

**All six are invisible to the programmer's syntax.** Same ZER code. Same C syntax. The compiler gets smarter about what it can prove, forces safety invariants, and makes @cstr physically unable to overflow. No new types, no new keywords, no new syntax.

**Forced rules** — same pattern as existing forced rules:
- `*T` requires initializer (already forced)
- `?T` requires `orelse` or `if |capture|` (already forced)
- Division requires proven nonzero divisor (NEW)
- Array/slice indexing requires proven in-range index (NEW)
- Function pointer pointer-params auto-`keep` — invisible, compiler inserts (NEW — no programmer action)
- `@cstr` auto-inserts `orelse { return <zero>; }` when no explicit `orelse` — never overflows (NEW — 100% invisible)

```zer
// Forced division: compiler error if divisor not proven nonzero
u32 avg = total / count;          // ERROR: 'count' not proven nonzero
if (count == 0) { return; }
u32 avg = total / count;          // OK — guard proves nonzero

// Forced bounds: compiler error if index not proven in range
buf[i] = 5;                        // ERROR: 'i' not proven < buf.len
if (i >= buf.len) { return; }
buf[i] = 5;                        // OK — guard proves in range

// Forced keep on function pointer returning pointer:
*u32 (*fn)(*u32) = f;              // ERROR: pointer param must be 'keep'
*u32 (*fn)(keep *u32) = f;         // OK — caller must pass global/static
```

**Iterators are NOT on the safety roadmap.** They may be added in the future as a syntax convenience (ergonomics), but they are not needed for compile-time safety — value range propagation covers the same loop patterns. If iterators are added, it's a separate decision driven by programmer ergonomics, not safety.

### Every C Undefined Behavior → ZER Compile Error

| C Undefined Behavior | ZER Prevention | Type |
|----------------------|----------------|------|
| Buffer overflow | **Forced in-range proof** + range propagation | **Forced** |
| Null pointer deref | `*T` non-null, `?T` forced `orelse` | Forced |
| Use-after-free | Handle gen counter + zercheck | Automatic |
| Division by zero | **Forced nonzero proof** | **Forced** |
| Signed overflow | Defined as wrapping (`-fwrapv`) | Automatic |
| Uninitialized memory | Auto-zero everything | Automatic |
| Double free | Handle gen counter + zercheck | Automatic |
| Out-of-bounds pointer | Scope escape analysis + **auto keep on fn ptr params** | Forced + Automatic |
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
| *opaque cast | **100%** | Array-level provenance + cross-function summaries (runtime tag stays as backup) |
| Scope escape | **100%** | Cross-function analysis + auto keep on fn ptr params |
| Union type confusion | **100%** | None |
| Volatile stripping | **100%** | None |
| MMIO range | **100%** | Range propagation proves guarded computed addresses (runtime backup stays) |
| @cstr overflow | **100%** | Auto-orelse for variable slices, compile error for constant strings |
| Arena overflow | **100%** | `?[]T` return forces `orelse` — overflow returns null, programmer handles it (already safe today) |

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

### Bounds: Auto-Guard System (FINAL DESIGN — decided 2026-03-31)

**Status: SUPERSEDES all previous bounds guard decisions (forced guard, warning, optimization-only).**

#### The Problem with Previous Approaches

| Approach tried | Why it failed |
|----------------|---------------|
| Forced guard (compile error) | 30-50% of statements are array access. Broke firmware patterns (ring buffers, lookup tables, globals). Beginners can't write guards. |
| Runtime trap only | Not compile-time. Untested code paths ship with bugs — runtime check only fires if the bad input actually occurs during testing. Bug hides in production. |
| Auto-orelse with return (silent) | Hides logic bugs. Motor gets wrong speed, sensor reads wrong calibration. Every serious language crashes on OOB — silent return is bad practice. |

**The key insight that changed the design:** runtime traps only catch bugs that GET HIT during testing. If a code path with a bounds bug is never tested, the runtime check exists in the binary but never fires. The bug ships to production and crashes a deployed device that needs physical reset.

#### The Design: Proven + Auto-Guard (Two Layers)

The compiler ALWAYS tries to prove bounds safety first. When it can't prove, it auto-inserts a guard. Both layers always present (belt and suspenders).

```
Compiler sees arr[i]:
  ├── CAN prove safe? (literal, loop, guard, known init)
  │   └── YES → mark proven. Emitter: plain arr[idx] + runtime check stays as backup
  │
  └── CANNOT prove?
      └── Auto-insert: if (i >= arr_size) { return <zero_value>; }
          Emitter: auto-guard + plain arr[idx] + runtime check stays as backup
```

#### What Gets Auto-Guarded (ALL unproven cases)

```zer
// Param — auto-guard
void f(u32 idx) {
    u32[8] buf;
    buf[idx] = 5;          // auto: if (idx >= 8) { return; }
}

// Global — auto-guard
u32 g_idx = 0;
u32[10] buffer;
void write(u32 val) {
    buffer[g_idx] = val;   // auto: if (g_idx >= 10) { return; }
}

// Volatile — auto-guard
volatile u32 hw_reg;
u32[4] table;
void read_hw() {
    table[hw_reg] = 5;     // auto: if (hw_reg >= 4) { return; }
}

// Computed — auto-guard
void f(u32 a, u32 b) {
    u32[8] buf;
    buf[a + b] = 5;        // auto: temp = a+b; if (temp >= 8) { return; }
}
```

#### What Gets Proven (zero overhead, zero auto-guard needed)

```zer
buf[3] = 5;                                          // literal — proven
for (u32 i = 0; i < 8; i += 1) { buf[i] = i; }     // loop — proven
if (idx >= 8) { return; } buf[idx] = 5;              // guard — proven
u32 x = 5; buf[x] = 10;                              // known init — proven
```

#### Emitted C (Both Layers)

```c
// Unproven: auto-guard + runtime check (belt and suspenders)
if (idx >= 8) { return 0; }                                    // auto-guard (layer 1)
(_zer_bounds_check(idx, 8, __FILE__, __LINE__), buf)[idx] = 5; // runtime (layer 2)

// Proven: runtime check only (backup — should never fire)
(_zer_bounds_check(idx, 8, __FILE__, __LINE__), buf)[idx] = 5; // runtime backup only
// Future --release flag: skip proven runtime checks
```

The auto-guard and runtime check are REDUNDANT for correct code. The runtime check is a backup in case the auto-guard insertion logic has a compiler bug.

#### Why Auto-Guard with Return (Not Trap)

For firmware deployed in the field:
- **Auto-guard returns** → device keeps running with default value → self-corrects on next cycle
- **Runtime trap** → device crashes → needs physical reset → operator must visit site

The auto-guard return is SAFER for deployed devices. The runtime trap stays as backup layer.

During development, the runtime trap catches the bug loudly (stderr output, crash). In production, the auto-guard prevents the crash from ever reaching the trap.

#### Programmer Override

The programmer can explicitly choose different OOB behavior:
```zer
u32 val = table[hw_reg] orelse @trap();        // explicit crash
u32 val = table[hw_reg] orelse 0xFF;           // explicit default
u32 val = table[hw_reg] orelse { log(); return; };  // custom
```

No explicit `orelse` → compiler auto-inserts `orelse { return <zero_value>; }`.

#### Auto-Guard Return Values (same logic as auto-orelse for @cstr)

| Function return type | Auto-guard inserts |
|---------------------|-------------------|
| `void` | `return;` |
| `u32` / `i32` / etc | `return 0;` |
| `bool` | `return false;` |
| `?*T` | `return null;` |
| `?T` | `return` (null optional) |

Uses `current_func_ret` — same infrastructure as auto-orelse for @cstr.

#### Why This Is Compile-Time

The compiler decides at compile time:
1. Analyzes every array access
2. Attempts to prove safety via range propagation
3. If proven → marks node as proven (zero overhead path)
4. If not proven → inserts auto-guard (compile-time code generation)
5. **Every code path is handled. No testing needed. 100% coverage.**

Contrast with runtime-only: the bounds check exists but only fires if the bad input occurs during testing. Untested paths ship with undetected bugs.

Auto-guard guarantees that EVERY unproven access is protected BEFORE the program runs. The guard is in the binary for every code path, not just tested ones.

#### Training Wheels Analogy

- **Beginner:** most accesses auto-guarded (training wheels on). Code compiles, runs safely.
- **Expert:** writes guards and loops naturally. Compiler proves safety. Auto-guard rarely needed (training wheels off, zero overhead).
- **Same safety level for both.** Expert gets better performance.

#### Implementation

**Checker (checker.c):**
- In `check_expr` NODE_INDEX: after range propagation check, if not proven AND object is TYPE_ARRAY:
  - Record the node as "needs auto-guard" (new array on Checker: `auto_guard_nodes`)
  - Store array size and index expression for the emitter

**Emitter (emitter.c):**
- In `emit_expr` NODE_INDEX: check if node is in `auto_guard_nodes`
  - If yes: emit `if (idx >= size) { return <zero>; }` before the access
  - The access itself still has `_zer_bounds_check` as backup (belt and suspenders)
- For auto-guard return value: use `e->current_func_ret` (same as auto-orelse @cstr)

**Estimated size:** ~60-80 lines (checker + emitter)

#### What This Means for the Coverage Table

| Category | Before | After auto-guard |
|----------|--------|-----------------|
| Bounds (proven: literal, loop, guard) | Compile-time | Compile-time (unchanged) |
| Bounds (unproven: param, global, volatile, computed) | Runtime only | **Compile-time** (auto-guard) + runtime backup |

**100% compile-time bounds safety for ALL cases.** Zero programmer effort. Invisible.

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
- `@cstr` overflow: if source slice length proven < buffer size via guard, skip runtime check (improves @cstr from ~50% to ~90% compile-time)

**@cstr auto-orelse (FINAL DESIGN — replaces truncation approach):**

`@cstr` returns `?void`. When no explicit `orelse` is written, the compiler auto-inserts `orelse { return <zero_value>; }` based on the function's return type. 100% invisible to the programmer.

```zer
// Programmer just writes:
@cstr(buf, data);

// Compiler auto-generates based on enclosing function return type:
//   void function:  @cstr(buf, data) orelse return;
//   u32 function:   @cstr(buf, data) orelse { return 0; };
//   bool function:  @cstr(buf, data) orelse { return false; };
//   ?*T function:   @cstr(buf, data) orelse { return null; };

// Explicit orelse still available for custom behavior:
@cstr(buf, data) orelse break;                     // exit loop
@cstr(buf, data) orelse { log("too long"); return; };  // log then bail
```

**Why not truncation:** Silent truncation hides logic bugs. A truncated command to a motor controller is worse than a clean failure. `orelse return` fails loud — the function stops, the caller knows something went wrong, the system keeps running (unlike a trap which crashes everything).

**Why auto-insert instead of forcing explicit `orelse`:** The programmer just writes `@cstr(buf, data);` and it works in ANY function. The compiler knows `current_func_ret`, knows the zero value for every type (same logic as auto-zero), and inserts the correct return. Zero boilerplate. Zero learning curve. If they want custom failure handling, they write `orelse` explicitly — same keyword they already know.

**Rules:**
- Constant string that doesn't fit → **compile error** (fix your sizes)
- Variable slice without explicit `orelse` → **auto-inserted** `orelse { return <zero>; }`
- Variable slice with explicit `orelse` → **programmer's choice**
- Buffer physically cannot overflow in any case
- **100% safe. 100% invisible. Zero boilerplate.**

**Can this auto-orelse pattern apply to other failable operations?**
Yes — any intrinsic or builtin that returns `?T` could auto-insert `orelse { return <zero>; }`. This is a general mechanism, not @cstr-specific. Potential future extension to `arena.alloc()`, etc. But start with `@cstr` first — it's the simplest case.

**Implementation:**
- Checker: when `@cstr` is used as expression-statement (NODE_EXPR_STMT) without explicit `orelse`, auto-wrap in orelse with zero-return
- Emitter: emit the orelse path with function return type's zero value
- ~30 lines total (checker + emitter)

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

---

## CONTEXT DUMP — Design Decisions, Reasoning, and Rejected Alternatives

**This section exists for fresh Claude sessions that have zero context of the discussions that led to these decisions. Read this section FULLY before proposing changes to the safety roadmap.**

### Core Philosophy (decided through extensive discussion)

1. **Zero language complexity.** Every safety feature is a compiler improvement, not a new syntax. The programmer writes C-style code. The compiler gets smarter. No new types, no new keywords, no annotations, no contracts, no proof obligations.

2. **Belt and suspenders.** Runtime checks are NEVER removed, even when compile-time proves safety. The runtime check is a backup in case the compile-time analysis has implementation bugs. The value range propagation is ~500 lines of new checker logic — it WILL have bugs initially. The runtime checks are ~5 lines of emitted C each — trivially correct.

3. **Force where it prevents UB, don't force where it's just overhead.** Division by zero = C undefined behavior → forced. Bounds = C undefined behavior → forced. @cstr overflow = ZER-defined trap → not forced (auto-orelse instead). The line is: if C has UB for this, ZER forces the guard.

4. **Auto-insert over explicit.** When the compiler can determine the correct behavior, do it automatically. `@cstr` auto-inserts `orelse { return <zero>; }`. Function pointer pointer-params auto-become `keep`. The programmer doesn't write these — the compiler inserts them invisibly.

5. **The programmer's guards are the proofs.** `if (i >= len) { return; }` is not boilerplate — it's the proof that `i < len`. The programmer already writes these guards as defensive practice. Value range propagation just notices them and skips the redundant runtime check. No new concept to learn.

### Evolution of Design (what was considered and why it was rejected)

**Attempt 1: Explicit ranged types `u32(0..N)`**
- Proposed: new type syntax, `u32(0..buf.len) i = get_index() orelse return;`
- Problem: adds language complexity, new type syntax to learn
- Status: **REJECTED** — replaced by value range propagation

**Attempt 2: Non-zero type `nz_u32`**
- Proposed: new type for proven-nonzero integers, eliminates div-zero checks
- Problem: new type to learn, forced `nz()` construction on every divisor
- Status: **REJECTED** — replaced by value range propagation (compiler infers nonzero from `if (d == 0) { return; }` guard)

**Attempt 3: Forced ranged indexing**
- Proposed: `buf[i]` requires `i` to be `u32(0..buf.len)` type, compile error otherwise
- Problem: 30-50% of statements are array access, massive boilerplate
- Status: **REJECTED** — replaced by auto-guard system

**Attempt 3b: Forced bounds guard (compile error for unproven)**
- Proposed: unproven `arr[i]` is a compile error, programmer must write `if (i >= N) { return; }` guard
- Problem: broke 30-50% of firmware tests. Globals, params, computed indices all errored. Beginners can't write guards.
- Status: **REJECTED** — replaced by auto-guard (compiler inserts guard invisibly)

**Attempt 3c: Bounds auto-orelse with return (silent)**
- Proposed: compiler auto-inserts `if (i >= N) { return 0; }` — invisible, no crash
- Concern: hides logic bugs (motor gets wrong speed, sensor reads wrong calibration)
- Counter-argument: runtime trap ALSO hides bugs — untested paths ship with bugs that crash in production
- Status: **ACCEPTED** — auto-guard with return is the final design. Runtime trap stays as belt-and-suspenders backup. See "Bounds: Auto-Guard System" section.

**Attempt 3d: Volatile-specific solutions (auto-modulo, ranged types, datasheet)**
- Auto-modulo `arr[val % N]`: wrong element on bad input, expensive on small MCUs
- Ranged volatile `volatile u32(0..7)`: requires datasheet knowledge
- Both rejected: auto-guard handles volatile the same as everything else
- Status: **REJECTED** — auto-guard covers all cases uniformly

**Attempt 4: Iterators `for (x in arr)` for bounds elimination**
- Proposed: new loop syntax, structurally bounded, no bounds check
- Problem: not needed for safety — value range propagation handles `for (i = 0; i < len; ...)` automatically
- Status: **REMOVED from safety roadmap** — may be added later as ergonomic feature, not safety feature

**Attempt 5: @cstr silent truncation**
- Proposed: @cstr always truncates to fit, never overflows, never crashes
- Problem: hides logic bugs. Truncated command to motor controller is worse than failed command.
- Status: **REJECTED** — replaced by auto-orelse

**Attempt 6: @cstr forced explicit `orelse`**
- Proposed: @cstr returns `?void`, programmer must write `orelse return`
- Problem: non-void functions need `orelse { return 0; }` — beginner doesn't know what value to return
- Status: **TRANSFORMED** — auto-orelse: compiler auto-inserts `orelse { return <zero_value>; }` based on function return type. 100% invisible in void functions, auto-correct in non-void.

**Attempt 7: Generics to replace `*opaque`**
- Proposed: `fn register(T)(void (*fn)(*T), *T ctx)` — type-safe callbacks without type erasure
- Problem: ~1000+ lines implementation, interacts with every feature, breaks "same syntax as C"
- Status: **REJECTED** — `*opaque` with 3-layer provenance (compile-time Symbol + compound key + runtime type_id tag) achieves 100% safety without generics. Generics may be reconsidered for v2.0 if ZER gains adoption beyond embedded.

**Attempt 8: Provenance on Type struct instead of Symbol**
- Proposed: move `provenance_type` from Symbol to Type for per-field tracking
- Problem: struct field Types are shared across all instances of a struct — can't have per-instance provenance on shared Types
- Status: **REJECTED** — compound key map on Checker struct achieves the same coverage. Runtime type_id tag covers the remaining paths.

### Key Mechanism: Value Range Propagation

**This is the single most important planned feature.** It eliminates ~80% of all runtime checks without any language change.

**How it works:** At each program point, the compiler tracks value constraints per variable:
```
Variable → { min_value, max_value, known_nonzero }
```

**Narrowing events:**
- `if (i < N)` → inside then-block: `i.max = N - 1`
- `if (i >= N)` then return → after the if: `i.max = N - 1`
- `if (x != 0)` → inside then-block: `x.known_nonzero = true`
- `if (x == 0) { return; }` → after the if: `x.known_nonzero = true`
- `for (i = 0; i < N; ...)` → inside body: `i.min = 0, i.max = N - 1`
- Literal assignment: `x = 5` → `x.min = 5, x.max = 5, x.known_nonzero = true`

**Widening events:**
- Assignment from unknown: `x = get_value()` → constraints reset
- Function call might modify: only for globals/pointers
- Loop exit: loop variable constraints no longer valid

**Check elimination rules:**
- `arr[i]` → skip bounds check if `i.max < arr.len`
- `a / b` → skip div check if `b.known_nonzero` or `b.min > 0`
- `@inttoptr(*T, base + offset)` → skip mmio check if `(base + offset.min)...(base + offset.max)` within mmio range
- `@cstr(buf, data[0..n])` → skip length check if `n.max + 1 <= buf.size`

**Implementation:** ~300-500 lines in checker.c. Data structure: `VarRange` array on Checker struct. Integrated into `check_expr`/`check_stmt`.

**Does NOT handle (acceptable):**
- Cross-function range inference (would need function summaries — future)
- Complex arithmetic (`x * y` range = full product range, usually unknown)
- Pointer-derived values (ranges don't apply to pointers)

### Key Mechanism: Auto-Orelse for @cstr

When `@cstr(buf, data)` is used as an expression statement without explicit `orelse`, the compiler auto-inserts `orelse { return <zero_value>; }` based on the enclosing function's return type:

- `void` function → `orelse return`
- `u32` function → `orelse { return 0; }`
- `bool` function → `orelse { return false; }`
- `?*T` function → `orelse { return null; }`

The compiler already knows `current_func_ret` (used for return type checking). It already knows zero values for all types (used for auto-zero). Auto-orelse just combines both.

**This pattern could extend to other failable operations in the future** — any intrinsic returning `?T` could auto-insert orelse. But start with @cstr — it's the simplest case and the only one where the programmer has no natural reason to write `orelse` explicitly.

### Key Mechanism: Auto-Keep on Function Pointer Params

ALL pointer parameters in function pointer types are automatically treated as `keep`. The programmer never writes `keep` on function pointer declarations — the compiler inserts it invisibly.

**Why:** The compiler can't see inside a function pointer call. It must assume the worst — the function might store the pointer. `keep` prevents passing `&local` through function pointers.

**Regular functions are different:** The compiler CAN see the body. `keep` is explicit and only required when the function actually stores the pointer. This is the existing behavior — unchanged.

### Key Mechanism: Array-Level *opaque Provenance

When assigning to any element of a `*opaque` array, provenance is stored under BOTH the element key AND the array root key in `prov_map`:

```
callbacks[0] = @ptrcast(*opaque, &sensor)
  → prov_map["callbacks[0]"] = *Sensor    // element-level
  → prov_map["callbacks"] = *Sensor       // array-level
```

Subsequent assignments to ANY element must match the array-level provenance. This forces homogeneous `*opaque` arrays — all elements must be the same type.

**Consequence:** Heterogeneous `*opaque` arrays (different types per element) produce compile errors. The programmer must use separate arrays per type. This is cleaner code anyway — a `*opaque[4]` mixing Sensor and Motor pointers is a code smell.

**Variable index:** `@ptrcast(*Sensor, callbacks[i])` checks against the array-level key `"callbacks"`. Since all elements are proven to be Sensor, any `i` is valid. 100% compile-time.

### Key Mechanism: Cross-Function Provenance Summaries

When a function returns `*opaque`, the compiler analyzes the function body once and records what provenance the return value carries. This summary is used at call sites.

**Depends on:** zercheck change 4 (cross-function analysis infrastructure). The provenance summaries reuse the same function summary mechanism — analyze once, use at every call site.

**Implementation:** ~50 lines on top of change 4's infrastructure.

### Forced vs Not-Forced Decision Table

| Rule | Forced? | Why |
|------|---------|-----|
| `*T` requires init | **Yes** | Prevents null deref (C UB) |
| `?T` requires `orelse` | **Yes** | Prevents unhandled None |
| Division requires guard | **Yes** | Prevents div-by-zero (C UB) |
| Bounds requires guard | **Auto** | Auto-guard inserted, invisible. Programmer CAN override with explicit `orelse`. |
| @cstr requires handling | **Auto** | Auto-orelse inserted, invisible |
| Fn ptr params = keep | **Auto** | Auto-inserted, invisible |
| *opaque array homogeneous | **Yes** | Prevents type confusion at compile time |
| Non-zero type | **No** | Removed — range propagation handles it |
| Iterators | **No** | Removed from safety roadmap — ergonomic only |
| MMIO guard | **No** | Range propagation handles it naturally |

### Comparison: ZER vs Rust vs SPARK (Post-Roadmap)

| | ZER | Rust | SPARK |
|---|-----|------|-------|
| Compile-time safety | **100%** (13/13 categories) | ~80% (no bounds proof, no div proof) | **100%** |
| Runtime backup | **Always on** | Panics (no backup, IS the safety) | Optional |
| `unwrap()` panic | **Impossible** (no unwrap exists) | Possible (common source of crashes) | N/A |
| Volatile/MMIO | **100% tracked** | `unsafe` block, untracked | Partial |
| Dynamic allocation | Pool/Slab/Ring/Arena — all safe | Vec/Box — safe via borrow checker | **None** (stack only) |
| void*/type erasure | `*opaque` with runtime type tag | No void* (generics instead) | No void* |
| Language complexity | **C syntax** | Lifetimes, traits, borrowing, Send/Sync | Contracts, loop invariants, pre/post |
| Programmer effort for safety | **Zero** (compiler does everything) | Medium (fight borrow checker) | High (write proofs) |
| Prover/toolchain cost | **Free** (GCC) | Free (rustc) | $10K-50K/year |
| LLM can write it | **Yes** (C syntax) | Partially (lifetimes trip LLMs) | No (no training data) |
| Data races | Not addressed (single-core target) | Compile-time (Send/Sync) | Compile-time (Ravenscar) |

### Why Not Borrow Checker

Discussed and rejected. A borrow checker would:
- Eliminate the handle gen counter runtime check (the ONE check it solves)
- Add move semantics, lifetime annotations, borrowing rules
- Break "same syntax as C"
- Cost: massive language complexity for one check that costs 3 cycles

ZER's zercheck (changes 1-4) achieves ~100% compile-time handle tracking via typestate analysis — same result, zero language change.

### Why Not Dependent Types

Discussed and rejected. Dependent types (`u32 where i < N`) would:
- Eliminate ALL runtime checks (100% compile-time proof)
- Require the programmer to write proofs for every variable
- Turn every function signature into a contract
- Make ZER into Idris/F*/ATS — theorem provers, not programming languages
- Nobody writes firmware in theorem provers

Value range propagation achieves the same result for the common patterns (guards, loop conditions, literals) without any programmer effort.

### Why Not Generics

Discussed and rejected for now. Generics would:
- Eliminate `*opaque` entirely — no type erasure needed
- Add ~1000+ lines of implementation (monomorphization)
- Interact with every other feature (optionals, comptime, distinct types)
- Produce template-style error messages (C++ nightmare)
- Break "same syntax as C"

`*opaque` with 3-layer provenance (Symbol + compound key + runtime type_id) achieves 100% safety. Generics solve a problem that's already solved. May reconsider for v2.0 if user demand exists.

### Implementation Dependencies

```
zercheck 1-3 (MAYBE_FREED, leak detection, loop pass)
    ↓ (no dependency)
zercheck 4 (cross-function analysis)
    ↓ (provides function summary infrastructure)
Cross-function provenance summaries (reuses change 4 summaries)

Value range propagation (independent — no dependency on zercheck)
    ↓ (enables)
Forced division guard (requires range propagation to prove guards)
Bounds auto-guard (requires range propagation to know what's already proven)
MMIO guard elimination (range propagation proves computed addresses)
@cstr check elimination (range propagation proves slice fits)

Auto-orelse for @cstr (independent — pure emitter change)
Auto-keep on fn ptr params (independent — pure checker change)
Array-level *opaque provenance (independent — extends existing prov_map)
```

**ALL DONE.** Every feature on the safety roadmap is implemented.
- zercheck 1-4, value range propagation, forced division guard (incl. struct fields), bounds auto-guard
- Auto-keep on fn ptr params, @cstr auto-orelse, array-level provenance, cross-function provenance
- 49 zercheck + 229 E2E + 512 checker tests passing

### Post-Roadmap: Struct Field Range Propagation (2026-03-31)

Extended range propagation and forced division guard to handle struct fields via `build_expr_key`:
- `if (cfg.divisor == 0) { return; } ... total / cfg.divisor` → **proven nonzero**
- `if (s.idx >= N) { return; } ... arr[s.idx]` → **proven in range**

Both NODE_IF condition extraction and NODE_BINARY division check now handle NODE_FIELD via compound keys.

**Known limitation:** struct field ranges in `?T` return functions have a subtle interaction with optional return checking. Workaround: copy struct field to local variable before guard (`u32 d = s.field; if (d == 0) return;`). This is actually better practice — snapshots the value.

### Remaining Runtime Cases (FINAL — absolute minimum)

| Case | Why | What ZER does |
|------|-----|---------------|
| `*opaque` from `cinclude` (C code) | External C code — ZER can't see it | Runtime type_id check |
| Signed `INT_MIN / -1` | Rare math edge case, 2 cycles per signed div | Runtime trap |

**Only 2 runtime cases in the entire language.** Both are correct as runtime:
- `cinclude` is external code — not ZER's scope, runtime type_id is the safety net
- `INT_MIN / -1` fires instantly when hit, 2-cycle cost, astronomically rare

**Everything else is compile-time** — including `*opaque` within ZER code (whole-program provenance, see below).

MMIO variable addresses are compile-time via range propagation (bitmask, modulo, guard patterns) + auto-guard fallback. Unknown hardware discovered via @probe at boot.

### Future: Whole-Program *opaque Provenance (ZER-to-ZER)

**Status:** Planned. Not yet implemented.
**Priority:** v0.3
**Language complexity:** Zero — compiler improvement only
**Implementation:** ~50-100 lines in checker.c

#### The Problem (currently)

```zer
// library.zer
void process(*opaque ctx) {
    *Sensor s = @ptrcast(*Sensor, ctx);  // is ctx Sensor? Motor? Unknown.
}
```

Currently, `ctx` from a parameter has no provenance — runtime type_id catches wrong casts. But the compiler sees ALL ZER files when compiling. It COULD trace provenance through call arguments.

#### The Solution: Whole-Program Call-Site Provenance

At each call site where `*opaque` is passed as argument, trace the provenance to the callee's parameter:

```zer
// app.zer
Motor m;
process(@ptrcast(*opaque, &m));  // call site: passing *Motor provenance

// library.zer
void process(*opaque ctx) {          // ctx gets provenance *Motor from call site
    *Sensor s = @ptrcast(*Sensor, ctx);  // *Motor ≠ *Sensor → COMPILE ERROR
}
```

#### Implementation

Same infrastructure as cross-function provenance summaries (already done for return values), extended to parameters:

1. **Build param provenance summaries:** at each call site, record what provenance each `*opaque` argument carries
2. **Propagate to callee:** when checking the callee's body, set parameter symbol's `provenance_type` from call-site summary
3. **Multiple callers:** if different callers pass different types → compile error "function called with conflicting *opaque types"

```c
// New: ParamProvSummary
struct ParamProvSummary {
    const char *func_name;
    uint32_t func_name_len;
    Type **param_provenance;  // provenance per *opaque param
    int param_count;
};
```

**Pre-scan phase (like zercheck cross-function):**
1. Scan all call sites in all modules
2. For each call to a function with `*opaque` params, record argument provenance
3. If multiple callers disagree on type → compile error at the CALLER site
4. If all callers agree → set parameter provenance → callee's @ptrcast is proven

#### Coverage After Implementation

| `*opaque` source | Compile-time? |
|---|:---:|
| Same function | **Yes** (Symbol provenance) |
| Struct field | **Yes** (compound key prov_map) |
| Array element | **Yes** (array-level provenance) |
| Function return | **Yes** (cross-func return summary) |
| ZER-to-ZER param | **Yes** (whole-program param provenance) |
| C via `cinclude` | Runtime (external code — type_id check) |

**100% compile-time for all ZER code.** Runtime only for external C code, which is not ZER's scope.

### Future: @probe Intrinsic — Safe Hardware Discovery

**Status:** Planned. Not yet implemented.
**Priority:** v0.3 — important for real embedded workflow
**Language complexity:** One new intrinsic (@probe)
**Implementation:** ~50-100 lines (platform-specific fault handler)

#### The Problem

Every embedded developer starts by probing hardware registers. Before writing a driver, you poke addresses to see what responds. Currently done in C with zero safety — invalid address → HardFault → board crashes → physical reset.

No language offers safe hardware probing. Ada crashes (Constraint_Error). Rust requires unsafe. C is undefined behavior. JTAG debuggers can probe safely but that's a separate tool, not in-language.

#### The Design

```zer
// @probe: try reading an MMIO address. Returns ?u32 — null if address faults.
?u32 val = @probe(0x40020000);

if (val) |v| {
    // address exists — v is the register value
    // now we know 0x40020000 is valid hardware
} else {
    // address invalid — no crash, no reset, no damage
}

// Scan a range to discover peripherals:
for (u32 addr = 0x40000000; addr < 0x40100000; addr += 0x1000) {
    ?u32 v = @probe(addr);
    if (v) |reg_val| {
        // found a peripheral at this address!
    }
}
```

#### How It Works (ARM Cortex-M)

1. Install a temporary HardFault/BusFault handler that sets a "fault occurred" flag and skips the faulting instruction
2. Try the read: `volatile uint32_t val = *(volatile uint32_t *)addr;`
3. If fault → handler catches it, flag set, return null (`?u32` with has_value=0)
4. If success → return value (`?u32` with has_value=1, value=val)
5. Restore original fault handler

```c
// Emitted C (ARM Cortex-M):
static volatile bool _zer_probe_fault;
static uint32_t _zer_probe_saved_pc;

void _zer_probe_handler(void) {
    _zer_probe_fault = true;
    // Skip faulting instruction (adjust return address)
    __asm volatile("mov r0, lr; add r0, #4; mov lr, r0");
}

_zer_opt_u32 _zer_probe(uint32_t addr) {
    _zer_probe_fault = false;
    // Install handler, try read, restore handler
    volatile uint32_t *p = (volatile uint32_t *)addr;
    uint32_t val = *p;  // may fault
    if (_zer_probe_fault) return (_zer_opt_u32){ 0, 0 };
    return (_zer_opt_u32){ val, 1 };
}
```

#### Platform Support

| Target | How fault is caught | Difficulty |
|--------|-------------------|-----------|
| ARM Cortex-M3/M4/M7 | BusFault handler | Easy (~30 lines) |
| ARM Cortex-M0/M0+ | HardFault handler (no BusFault) | Medium (~40 lines) |
| RISC-V | Access fault exception handler | Medium (~40 lines) |
| AVR | No fault mechanism — reads garbage | N/A (always "succeeds") |
| x86 | Signal handler (SIGSEGV/SIGBUS) | Easy (~20 lines, OS-dependent) |

#### Startup MMIO Verification (auto-generated)

The compiler collects ALL `@inttoptr` constant addresses used in the program and auto-generates a verification function at the top of `main()`:

```c
// Auto-generated — zero programmer effort:
void _zer_mmio_verify(void) {
    if (!_zer_probe(0x40020000)) _zer_trap("MMIO 0x40020000 not responding");
    if (!_zer_probe(0x40020014)) _zer_trap("MMIO 0x40020014 not responding");
    if (!_zer_probe(0x40011000)) _zer_trap("MMIO 0x40011000 not responding");
}

int main() {
    _zer_mmio_verify();  // fires FIRST — before any code runs
    // ... rest of program
}
```

**Performance:** 1-2 cycles per address. 100 registers = ~3 microseconds at 72 MHz. Runs ONCE at boot. Invisible compared to clock/PLL setup time.

**Why this matters:** wrong MMIO address currently crashes when that specific code LINE executes — might be hours after boot, might be in an untested path, might be in production at 2 AM. Startup verification catches it at FIRST POWER-ON, in the lab, within microseconds.

#### Auto-Discovery Mode: Zero-Knowledge Hardware Scanning (FINAL DESIGN)

**Trigger:** the compiler detects `@inttoptr` in the code AND no `mmio` declarations exist. The emitter auto-generates the discovery preamble. No programmer input. No `main()` modification. The discovery runs BEFORE `main()` via GCC `__attribute__((constructor))`.

**Programmer writes:**
```zer
// No mmio. No imports. No SDK. Just @inttoptr.
volatile *u32 reg = @inttoptr(*u32, 0x40020014);
*reg = 100;
```

**Compiler detects:** "@inttoptr used, no mmio declared → emit auto-discovery."

**Emitted C:**
```c
// Auto-generated in preamble — runs BEFORE main():
static uint32_t _zer_discovered[256][2];  // [start, end] pairs
static int _zer_disc_count = 0;

// Phase 1: detect architecture + initial scan
// Phase 2: brute-force clock controller
// Phase 3: rescan with clocks enabled
// Runs via __attribute__((constructor)) — before main()
__attribute__((constructor))
void _zer_mmio_discover(void) {
    uint32_t scan_start, scan_end, scan_step;

    // Detect architecture by probing known addresses
    if (_zer_probe(0xE000ED00)) {
        // ARM Cortex-M — CPUID found
        scan_start = 0x40000000; scan_end = 0x5FFFFFFF; scan_step = 0x1000;
    } else if (_zer_probe(0x10000000)) {
        // RISC-V typical peripheral base
        scan_start = 0x10000000; scan_end = 0x1FFFFFFF; scan_step = 0x1000;
    } else {
        // Unknown — full 32-bit scan at 64KB granularity
        scan_start = 0x00000000; scan_end = 0xFFFFFFFF; scan_step = 0x10000;
    }

    // Phase 1: initial scan — find all responding blocks
    for (uint32_t base = scan_start; base <= scan_end && _zer_disc_count < 256; base += scan_step) {
        if (_zer_probe(base)) {
            _zer_discovered[_zer_disc_count][0] = base;
            _zer_discovered[_zer_disc_count][1] = base + scan_step - 1;
            _zer_disc_count++;
        }
        if (base > base + scan_step) break; // overflow guard
    }

    // Phase 2: brute-force clock controller discovery
    // Write 0xFFFFFFFF to each discovered block's first 16 registers.
    // If NEW blocks appear → that block was the clock controller.
    int baseline = _zer_disc_count;
    for (int i = 0; i < baseline; i++) {
        volatile uint32_t *regs = (volatile uint32_t *)_zer_discovered[i][0];
        uint32_t saved[16];
        for (int r = 0; r < 16; r++) saved[r] = regs[r];  // save
        for (int r = 0; r < 16; r++) regs[r] = 0xFFFFFFFF; // enable all

        // Quick check: probe 4 addresses that didn't respond before
        bool new_found = false;
        for (uint32_t test = scan_start; test <= scan_end && !new_found; test += scan_step * 8) {
            if (!_zer_in_discovered(test) && _zer_probe(test)) new_found = true;
        }

        if (new_found) {
            // Found clock controller — leave clocks enabled, full rescan
            break;
        }
        for (int r = 0; r < 16; r++) regs[r] = saved[r]; // restore
    }

    // Phase 3: rescan — find newly clock-enabled peripherals
    for (uint32_t base = scan_start; base <= scan_end && _zer_disc_count < 256; base += scan_step) {
        if (!_zer_in_discovered(base) && _zer_probe(base)) {
            _zer_discovered[_zer_disc_count][0] = base;
            _zer_discovered[_zer_disc_count][1] = base + scan_step - 1;
            _zer_disc_count++;
        }
        if (base > base + scan_step) break;
    }

    // Phase 4: brute-force power controller (same pattern as RCC)
    // Some chips have power domains — peripherals powered off by default.
    // Power controller (PMC/PWR) is always powered on.
    // Write to each block, check if NEW blocks appear in powered-off regions.
    int after_clock = _zer_disc_count;
    for (int i = 0; i < after_clock; i++) {
        volatile uint32_t *regs = (volatile uint32_t *)_zer_discovered[i][0];
        uint32_t saved[16];
        for (int r = 0; r < 16; r++) saved[r] = regs[r];
        for (int r = 0; r < 16; r++) regs[r] = 0xFFFFFFFF;

        bool new_found = false;
        for (uint32_t test = scan_start; test <= scan_end && !new_found; test += scan_step * 8) {
            if (!_zer_in_discovered(test) && _zer_probe(test)) new_found = true;
        }

        if (new_found) break;  // power controller found — domains enabled
        for (int r = 0; r < 16; r++) regs[r] = saved[r];
    }

    // Phase 5: final rescan — find power-gated peripherals now enabled
    for (uint32_t base = scan_start; base <= scan_end && _zer_disc_count < 256; base += scan_step) {
        if (!_zer_in_discovered(base) && _zer_probe(base)) {
            _zer_discovered[_zer_disc_count][0] = base;
            _zer_discovered[_zer_disc_count][1] = base + scan_step - 1;
            _zer_disc_count++;
        }
        if (base > base + scan_step) break;
    }

    // TrustZone detection: if running non-secure, secure peripherals
    // correctly fault — this is security, not a gap.
    // All accessible peripherals are discovered. 100%.
}

// Validation: called from emitted @inttoptr code
bool _zer_mmio_valid(uint32_t addr) {
    for (int i = 0; i < _zer_disc_count; i++) {
        if (addr >= _zer_discovered[i][0] && addr <= _zer_discovered[i][1]) return true;
    }
    return false;
}
```

**Full boot sequence — 5 phases:**

```
Phase 1: Initial scan (find always-on peripherals)         ~3.5ms
Phase 2: Brute-force clock controller (enable all clocks)  ~0.5ms
Phase 3: Rescan (find clock-gated peripherals)             ~3.5ms
Phase 4: Brute-force power controller (enable all domains) ~0.5ms
Phase 5: Final rescan (find power-gated peripherals)       ~3.5ms
                                                    Total: ~12ms
```

**Timing per architecture:**

| Architecture | All 5 phases | Context |
|---|---|---|
| ARM Cortex-M (72MHz) | **~12ms** | PLL lock takes ~2ms, board power-up ~50ms |
| RISC-V (100MHz) | **~5ms** | Invisible in boot |
| Unknown full-scan (72MHz) | **~8ms** | Full 32-bit at 64KB granularity |
| AVR (16MHz) | **~0.05ms** | Tiny address space |

**12ms worst case. Board power stabilization alone takes 50ms. The probe is invisible.**

**100% coverage guarantee:**
- Phase 1: finds always-on peripherals
- Phase 2+3: finds clock-gated peripherals (RCC brute-forced)
- Phase 4+5: finds power-gated peripherals (PMC brute-forced)
- TrustZone: secure peripherals correctly hidden (security, not a gap)
- Result: every physically accessible peripheral discovered

**Compiler trigger logic (in emitter):**
1. During emission, track if ANY `@inttoptr` node exists
2. If yes AND `checker.mmio_range_count == 0` (no mmio declared) AND `--no-strict-mmio`:
   - Emit `_zer_probe()` fault handler in preamble
   - Emit `_zer_mmio_discover()` with `__attribute__((constructor))`
   - Emit `_zer_mmio_valid()` helper
   - At each `@inttoptr` with variable address: emit `if (!_zer_mmio_valid(addr)) return <zero>;`
3. If `mmio` IS declared: use compile-time validation (current behavior, no probe)

**The programmer writes NOTHING. The compiler handles everything.**

**100% accurate because:**
1. Pass 1 finds all always-on peripherals
2. Brute-force finds clock controller (writes to blocks, checks if new blocks appear)
3. Pass 3 finds all clock-gated peripherals (now enabled)
4. A peripheral either responds or doesn't. No hidden state. Silicon can't lie.

**Fault handler per architecture (~20 lines each):**

| Architecture | Handler | How it works |
|---|---|---|
| ARM Cortex-M3/M4/M7 | BusFault | Set flag in handler, skip faulting instruction |
| ARM Cortex-M0/M0+ | HardFault | Same but HardFault (no BusFault on M0) |
| RISC-V | Exception handler | mcause=5 (load fault), skip instruction via mepc |
| AVR | No faults | All reads "succeed" (reads 0xFF from unmapped) |
| x86 (hosted) | SIGSEGV handler | Signal handler with setjmp/longjmp |

**Hierarchy (compiler picks the best available):**

| Priority | Source | When used |
|---|---|---|
| 1 | Programmer `mmio` declaration | Always preferred if present — compile-time |
| 2 | GCC auto-detect (architecture spec) | When GCC target detected, no manual declaration |
| 3 | **@probe auto-discovery** | **`--no-strict-mmio` — scans actual hardware at boot** |

The compiler automatically picks the highest available source. Programmer writes nothing → gets the best validation possible for their setup.

**Why this is unique:** no other language probes hardware at boot. C has no concept of MMIO validation. Ada requires type declarations. SPARK requires contracts. Rust requires unsafe. ZER scans the silicon and builds its own safety map. The hardware IS the specification.

#### Flag Behavior (REVISED — decided 2026-03-31)

| Flag | Who | What happens |
|---|---|---|
| (none) strict default | Production | `mmio` required. `@inttoptr` without declaration = compile error |
| `--no-strict-mmio` | **Noob / bring-up** | **Auto-probe at boot.** `@inttoptr` compiles. Hardware scanned. All addresses validated against discovered map. SAFE. |
| `--discover` | Learning → production | Same as `--no-strict-mmio` but ALSO prints discovered ranges to console so programmer can copy `mmio` lines into code for strict mode |
| `--mmio-unchecked` | Expert | No validation at all. Programmer accepts full responsibility. Dangerous. |

**`--no-strict-mmio` is NO LONGER an unsafe escape hatch.** It means "let the hardware tell us what's valid." Full safety via auto-probe. The noob path:

```
1. Writes @inttoptr → compile error: "add mmio or use --no-strict-mmio"
2. Adds --no-strict-mmio → compiles
3. Runs on board → probe discovers hardware → all @inttoptr validated
4. Done. Safe. Zero knowledge needed.
```

**When ready for production:**

```
5. Changes to --discover → prints mmio ranges on console
6. Copies mmio lines into code
7. Removes flag → strict mode → full compile-time validation
8. Production-ready.
```

#### Recommended Usage: @probe in ALL MMIO Code

`@probe` is encouraged in BOTH strict and non-strict modes:

**Strict mode (production):** `mmio` declarations validate addresses at compile time. Startup `@probe` adds a runtime hardware verification layer — catches wrong datasheet, dead peripheral, broken board. Belt and suspenders.

**Non-strict mode (development/discovery):** `@probe` auto-discovery is the PRIMARY safety mechanism. Hardware scanned at boot. Every `@inttoptr` validated against discovered map.

```zer
// RECOMMENDED PATTERN — works in both modes:
mmio 0x40020000..0x40020FFF;  // compile-time validation (strict)

u32 main() {
    // @probe runs at startup automatically (compiler-generated)
    // OR manually for conditional init:
    ?u32 id = @probe(0x40020000);
    if (id) |chip_id| {
        // peripheral exists — initialize it
    } else {
        // peripheral missing — skip init, no crash
    }
    return 0;
}
```

**The layered approach:**
1. **Compile-time:** `mmio` declaration validates constant addresses (strict mode)
2. **Boot-time:** auto-generated `_zer_mmio_verify()` probes all addresses before main runs
3. **Runtime:** manual `@probe` for conditional/dynamic hardware discovery

All three layers are invisible to the programmer. Compiler handles everything. Hardware bugs caught at earliest possible moment.

#### Why This Matters

1. **Hardware bring-up:** new board, unknown peripherals → scan and discover
2. **Reverse engineering:** unknown chip → probe address space safely
3. **Manufacturing test:** verify all peripherals respond without crashing the test jig
4. **Education:** beginners explore hardware without fear of bricking
5. **Production safety:** wrong datasheet, dead peripheral, board revision mismatch → caught at boot, not in the field

No other language offers this. C crashes. Ada crashes. Rust requires unsafe + platform-specific code. ZER: `?u32 val = @probe(addr);` — one line, safe, portable across ARM/RISC-V.

#### Auto-Detect MMIO from GCC Target (companion feature)

When GCC target is detected, ZER auto-declares architecture-level MMIO ranges:

```c
// In zerc_main.c, after GCC probe:
if (target_is_arm_cortex_m) {
    add_mmio_range(checker, 0x40000000, 0x5FFFFFFF);   // peripherals
    add_mmio_range(checker, 0xE0000000, 0xE00FFFFF);   // system
}
if (target_is_avr) {
    add_mmio_range(checker, 0x0000, 0x00FF);            // I/O
}
```

Programmer writes zero `mmio` declarations. Constant @inttoptr validated against architecture ranges. Variable @inttoptr auto-guarded. @probe for discovery on unknown hardware.

#### Extended Range Propagation Patterns (for MMIO proof)

| Pattern | Range result | Proves MMIO? |
|---------|-------------|:---:|
| `x & 0xFFF` | `{0, 0xFFF}` | Yes — base + masked offset in range |
| `x % 16` | `{0, 15}` | Yes — bounded offset |
| `a + b` (both ranged) | `{a.min+b.min, a.max+b.max}` | Yes — sum in range |
| Function returns masked value | Cross-func range summary | Yes — caller proven |

### Code Location Guide (WHERE to implement each feature)

**Read `docs/compiler-internals.md` first — it documents every pattern in the codebase.**

#### zercheck changes 1-3 — DONE

**Implemented.** MAYBE_FREED state, leak detection, loop second pass. 43 zercheck tests passing. See BUGS-FIXED.md for details.

#### zercheck change 4 — DONE

**Implemented.** `FuncSummary` struct with `frees_param[]` + `maybe_frees_param[]`. Pre-scan via `zc_build_summary()` reuses `zc_check_stmt` with error suppression. `zc_apply_summary()` at call sites. 49 zercheck tests passing.

#### Value range propagation — DONE

**Implemented.** `VarRange` stack on Checker tracks `{min, max, known_nonzero}` per variable. Stack-based save/restore for scoped narrowing. `proven_safe` array tracks nodes where runtime checks can be skipped. Emitter calls `checker_is_proven()` to skip `_zer_bounds_check` and division trap.

Patterns covered: literal indices, for-loop variables, guard patterns (`if (i >= N) return`), literal divisors, nonzero-guard divisors. 224 E2E tests passing.

#### Forced division guard — DONE

**Implemented.** After range propagation check, if divisor is `NODE_IDENT` and not proven nonzero → compile error with fix suggestion. Complex expressions (struct fields, function calls) fall back to runtime check. 7 new checker tests.

#### Bounds auto-guard

**Files:** `checker.c` + `emitter.c`

- **Checker:** In `check_expr` NODE_INDEX, after range propagation check: if not proven AND object is TYPE_ARRAY, record node in `auto_guard_nodes` array on Checker with array size info. Mark node as proven (subsequent uses skip bounds check).
- **Emitter:** In `emit_expr` NODE_INDEX, check if node is in `auto_guard_nodes`. If yes: emit `if (idx >= size) { return <zero>; }` before the access. Use `e->current_func_ret` for zero value (same as auto-orelse @cstr). Runtime `_zer_bounds_check` stays as belt-and-suspenders backup.
- **For slices:** auto-guard uses `.len` instead of constant size: `if (idx >= slice.len) { return <zero>; }`
- **For computed indices:** hoist to temp: `size_t _idx = (a+b); if (_idx >= N) return; arr[_idx]`
- **Depends on** value range propagation (DONE) and `current_func_ret` in emitter (exists)

#### Auto-orelse for @cstr

**Files:** `checker.c` + `emitter.c`

- **Checker** → in `check_expr` NODE_INTRINSIC, @cstr handler (`checker.c:~3080`): when @cstr is used as `NODE_EXPR_STMT` (expression statement) without being wrapped in `NODE_ORELSE`, auto-wrap. OR: do it in `check_stmt` NODE_EXPR_STMT — detect @cstr call without orelse, insert synthetic orelse node.
- **Emitter** → in `emit_expr` NODE_INTRINSIC, @cstr handler (`emitter.c:~1830`): if the @cstr has auto-orelse flag, emit the orelse path with function return type's zero value. Use `e->current_func_ret` (already available on Emitter struct).
- **Zero value emission** → already exists in emitter for auto-zero global vars and return-null paths. Reuse: `TYPE_VOID → return`, `TYPE_U32 → return 0`, `TYPE_BOOL → return 0`, `TYPE_POINTER → return NULL`, `TYPE_OPTIONAL → return {0}`.

#### Auto-keep on function pointer pointer-params

**File:** `checker.c`

- **Location** → NODE_CALL handler (`checker.c:~2054`), keep parameter validation section
- **Logic** → when callee is a function pointer (resolved via typemap → TYPE_FUNC_PTR), treat ALL pointer params as `keep` regardless of the type's `param_keeps` array. Add check: if `effective_callee` from typemap has `kind == TYPE_FUNC_PTR`, force keep validation on every pointer-typed argument.
- **Existing keep validation** → `checker.c:~2060-2170` — already handles keep checks for direct calls and function pointer calls via `param_keeps`. Just need to override: if callee is func ptr, all pointer params are keep.

#### Array-level *opaque provenance

**File:** `checker.c`

- **Location** → `prov_map_set` calls in NODE_ASSIGN (`checker.c:~1193`) and NODE_VAR_DECL (`checker.c:~3655`)
- **Logic** → when storing provenance under a compound key like `"callbacks[0]"`, ALSO extract the array root name (everything before `[`) and store provenance under that root key. On subsequent assignments to any element, check if root key already has provenance — if different, compile error.
- **Helper** → extract root from key: scan backwards from key end for `[`, take substring. E.g., `"callbacks[0]"` → `"callbacks"`, `"s.arr[2]"` → `"s.arr"`.
- **Existing infrastructure** → `prov_map_set` at `checker.c:306`, `prov_map_get` at `checker.c:335`, `build_expr_key` at `checker.c:264`.

#### Cross-function provenance summaries

**File:** `checker.c`

- **Depends on** zercheck change 4's function summary infrastructure. Same pre-scan, same summary storage.
- **Summary** → for each function, record: "return value has provenance X" if the function returns `*opaque` and the return expression's provenance is known.
- **Usage** → in `check_expr` NODE_CALL, when callee returns `*opaque`: look up function summary, get return provenance. Store in prov_map for the call site's target.
- **Location** → near existing provenance check in @ptrcast handler (`checker.c:~3000`)

#### Existing code patterns to follow

- **Compound key pattern** → `build_expr_key()` in checker.c:264 and `handle_key_from_expr()` in zercheck.c:126 — same pattern, both build string keys from AST expressions
- **Provenance map** → `prov_map_set/get` in checker.c:306-340 — dynamic array with arena-allocated keys
- **Type ID assignment** → checker.c register_decl, `c->next_type_id++` for struct/enum/union
- **Runtime type tag** → emitter.c:1700-1740, @ptrcast handler — wraps with `(_zer_opaque){ptr, tid}`, unwraps with `({ check type_id; (T*)tmp.ptr; })`
- **Auto-zero values** → emitter uses `current_func_ret` to determine zero values for return paths. Same logic reusable for auto-orelse.
- **Union switch key** → `union_switch_key` on Checker struct, built via `build_expr_key` — same pattern for array-level provenance

### What "100% Compile-Time" Actually Means

It does NOT mean "zero runtime checks in the emitted C." Runtime checks stay as backup.

It means: **for every safety category, the compiler catches 100% of violations before the code runs.** The programmer gets a compile error, not a runtime crash. The error message tells them exactly how to fix it.

The runtime checks remain as a second layer. They should never fire in correct code — the compiler already caught the problem. But if the compiler's analysis has a bug (it will — it's ~1000 lines of new logic), the runtime check catches what the compiler missed.

This is unique to ZER. SPARK doesn't keep runtime backup. Rust's runtime panics ARE the safety (no compile-time backup for bounds/div). ZER has BOTH layers on every path.

### Target Audience Context

ZER is NOT competing for web developers, application programmers, or systems programmers who already chose Rust. ZER is competing for **embedded firmware engineers who currently write C** and:

- Have been told to "learn Rust" and resisted
- Had a senior engineer say "we've been doing C for 20 years, it works"
- Had a production bug caused by a buffer overflow that took a week to find
- Work at companies where SPARK/Ada licensing costs more than a developer's salary
- Use GCC cross-compilers for ARM/RISC-V/AVR and can't change toolchains

These engineers will NOT learn new syntax. They will NOT write proofs. They will NOT change their build system. They WILL accept "same code, fewer bugs, same compiler" if the evidence is compelling.

The safety roadmap's value proposition: **"Your code, but the compiler catches your bugs before they reach hardware."**

---

## Design Decision: Remove 5-Phase Auto-Discovery (decided 2026-04-01)

**Decision:** Remove the 5-phase brute-force auto-discovery boot scan (Phases 1-5 above) and `_zer_mmio_valid()` runtime gate. Keep `@probe` as a standalone intrinsic. Keep `mmio` declarations with compile-time validation.

**What is removed:**
- 5-phase boot scan (`_zer_mmio_discover` constructor): initial scan, RCC brute-force, rescan, power controller brute-force, final rescan
- `_zer_mmio_valid()` — runtime validation of `@inttoptr` against discovered map
- `_zer_in_disc()` / `_zer_disc_scan()` / `_zer_disc_brute_enable()` helpers
- `has_inttoptr()` AST scan that triggered discovery emission
- `--no-strict-mmio` no longer emits any discovery infrastructure

**What is kept:**
- `mmio` declarations — compile-time `@inttoptr` address validation (100% correct)
- `@probe(addr)` intrinsic — safe read returning `?u32`, no crash on fault (100% accurate)
- `@probe` fault handler preamble (ARM HardFault, RISC-V exception, x86 SIGSEGV) — needed by @probe
- `--no-strict-mmio` flag — allows `@inttoptr` without `mmio` declarations (no validation, like C)

**Why removed:**
1. **False negatives → false blocking.** Auto-discovery can't find locked peripherals (watchdog behind KEY register), TrustZone-gated peripherals, write-only registers, or peripherals behind vendor-specific unlock sequences. `_zer_mmio_valid()` blocks access to these valid addresses — the "safety" mechanism becomes a bug source.
2. **Chip-family-specific.** RCC brute-forcing is STM32-centric. NXP, TI, Microchip, Nordic have different clock architectures. The code pretends to be universal but isn't.
3. **Overpromises.** "100% coverage guarantee" in the original design is false. @probe can only find hardware that responds to reads. Locked/gated/write-only hardware exists but doesn't respond. ~80% coverage presented as 100%.
4. **Complexity vs value.** ~150 lines of platform-specific emitted C, boot-time overhead (~12ms), and runtime validation on every `@inttoptr` — all for a heuristic that can't be trusted.

**What replaces it — mmio declaration startup validation (simpler, correct):**
Instead of discovering unknown hardware, validate known declarations. At boot, @probe each declared mmio range start address. If the hardware doesn't respond, trap immediately with a clear message. Wrong datasheet address caught at first power-on, not hours later in an untested code path.

```c
// Emitted C — runs before main()
__attribute__((constructor))
static void _zer_mmio_validate(void) {
    if (!_zer_probe(0x40020000).has_value)
        _zer_trap("mmio 0x40020000..0x40020FFF: no hardware detected");
    if (!_zer_probe(0x40011000).has_value)
        _zer_trap("mmio 0x40011000..0x4001103F: no hardware detected");
}
```

- Probes only declared ranges (2-5 addresses, not thousands)
- Microseconds, not milliseconds
- No false positives — validates what the programmer declared
- Wrong address from datasheet → instant trap at boot

**@probe remains useful standalone** — programmers who want to discover hardware write their own scan loop:
```zer
for (u32 base = 0x40000000; base < 0x50000000; base += 0x1000) {
    ?u32 val = @probe(base);
    if (val) |v| { /* found peripheral at base */ }
}
```

**Revised MMIO safety layers:**
1. **Compile-time:** `mmio` declarations validate `@inttoptr` addresses (100%)
2. **Boot-time:** @probe validates declared ranges have real hardware (catches wrong datasheet)
3. **Manual:** @probe available for programmer-driven discovery
4. **`--no-strict-mmio`:** allows @inttoptr without declarations — no validation (programmer's choice)
