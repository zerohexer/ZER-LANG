# Rust Test Suite → ZER-LANG Safety Test Mapping

## Purpose
Identify safety-relevant tests from Rust's test suite (tests/ui/) and write
equivalent ZER tests. NOT copy-paste — translate the SAFETY INTENT to ZER's
memory model (Pool/Slab/Handle/Arena instead of Box/Rc/Arc).

## Rust Test Categories → ZER Equivalents

### 1. borrowck/ — Borrow Checker (UAF, dangling, aliasing)
Rust tests: use-after-move, dangling references, multiple mutable borrows
ZER equivalent: zercheck UAF, Handle aliasing, interior pointer tracking
**Status:** Already covered by tests/zer_fail/uaf_*, interior_ptr_*, alias_*

### 2. threads/ — Send/Sync/Thread Safety
Rust tests: non-Send type across thread boundary, data race detection
ZER equivalent: spawn non-shared pointer → error, shared struct
**Status:** Already covered by spawn_nonshared_ptr.zer, shared_struct.zer

### 3. use-after-free/ — UAF patterns
Rust tests: use heap allocation after free, dangling Box
ZER equivalent: pool.free(h) then pool.get(h), alloc_ptr then free_ptr
**Status:** Already covered by tests/zer_fail/uaf_handle.zer, alloc_ptr_uaf.zer

### 4. double-free/ — Double free patterns
Rust tests: Drop called twice
ZER equivalent: pool.free(h) twice
**Status:** Already covered by tests/zer_fail/double_free.zer

### 5. unsafe/ — Patterns requiring unsafe in Rust
Rust tests: raw pointer deref, type punning, ptr arithmetic
ZER equivalent: @ptrcast provenance, (*opaque) round-trips, @inttoptr
**Status:** ZER handles these at compile time WITHOUT unsafe. Covered by
typecast_provenance.zer, opaque_ptrcast_roundtrip.zer

### 6. consts/ — Compile-time evaluation
Rust tests: const fn, const generics, compile-time arithmetic
ZER equivalent: comptime functions, comptime if
**Status:** Already covered by comptime_eval.zer, comptime_const_arg.zer

### 7. array-slice-length-overflow/ — Buffer overflow
Rust tests: array index OOB, slice bounds
ZER equivalent: bounds check, auto-guard, range propagation
**Status:** Already covered by bounds_oob.zer, dyn_array_guard.zer

### 8. div-by-zero/ — Division safety
Rust tests: runtime panic on /0
ZER equivalent: compile-time error (forced guard)
**Status:** Already covered by div_zero.zer — ZER catches at compile time,
Rust only catches at runtime

### 9. never_type/ — Exhaustive matching
Rust tests: non-exhaustive match
ZER equivalent: exhaustive switch on enum
**Status:** Already covered by enum_switch.zer

### 10. leak/ — Memory leaks
Rust tests: mem::forget, Rc cycles
ZER equivalent: handle leak compile error
**Status:** Already covered by zercheck leak detection (alloc_id grouping)

## Tests to WRITE (patterns Rust tests but ZER doesn't yet)

### Priority 1: Cross-thread patterns
- [ ] Shared mutable state without lock (Rust: no Send, ZER: no shared struct)
- [ ] Thread join and result collection
- [ ] Channel-based message passing (Ring buffer equivalent)
- [ ] Mutex deadlock (lock ordering)

### Priority 2: Complex ownership
- [ ] Ownership transfer chain (A → B → C, A can't use after)
- [ ] Conditional ownership (if branch takes, else doesn't)
- [ ] Ownership in loop (alloc per iteration, free per iteration)

### Priority 3: Edge cases Rust found over 10 years
- [ ] Recursive struct with optional self-reference
- [ ] Function pointer storing dangling reference
- [ ] Arena-allocated data outliving scope
- [ ] Volatile pointer aliasing

## How to use this
1. Pick a category
2. Read the Rust test's INTENT (what safety property it tests)
3. Write equivalent ZER code that tests the SAME property
4. Add to tests/zer/ (positive) or tests/zer_fail/ (negative)
5. Add generator to semantic fuzzer if the pattern is combinatorial
