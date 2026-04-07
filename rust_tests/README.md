# Rust Test Suite → ZER-LANG Safety Test Mapping

## Purpose
Map Rust's tests/ui/ safety tests to ZER equivalents. Not copy-paste —
translate the SAFETY INTENT to ZER's memory model.

## Coverage Summary

| Rust Category | Files | ZER Equivalent | Status |
|---|---|---|---|
| borrowck/ (UAF, aliasing) | 809 | zercheck ALIVE/FREED/MAYBE_FREED | Covered |
| nll/ (advanced lifetimes) | 399 | zercheck cross-func summaries | Covered |
| moves/ (use-after-move) | 130 | zercheck (no moves, tracks freed) | Covered |
| drop/ (destructor order) | 131 | defer + zercheck leak detection | Covered |
| dropck/ (dangling destructor) | 78 | scope escape analysis | Covered |
| regions/ (reference validity) | 343 | scope escape + auto-zero | Covered |
| lifetimes/ (borrow duration) | 209 | scope escape (no lifetime syntax) | Covered |
| sanitizer/ (ASan/TSan) | 44 | zercheck Levels 1-5 | Covered |
| uninhabited/ (null safety) | 29 | ?*T optional, *T non-null | Covered |
| precondition-checks/ | 37 | non-null + @inttoptr alignment | Covered |
| array-slice-vec/ (OOB) | 101 | bounds auto-guard + range prop | Covered |
| indexing/ (bounds check) | 18 | compile-time + runtime bounds | Covered |
| numbers-arithmetic/ (div0) | 75 | forced division guard | Covered |
| cast/ (type casts) | 127 | @truncate/@saturate/C-style cast | Covered |
| transmute/ (bit reinterpret) | 30 | @bitcast (width + qualifier check) | Covered |
| threads-sendsync/ (races) | 69 | shared struct + spawn safety | Covered |
| sync/ (atomics, mutex) | 14 | @atomic_* + shared struct | Covered |
| unsafe/ (raw pointers) | 62 | no unsafe — @ptrcast provenance | Covered |
| union/ (type confusion) | 91 | union variant lock | Covered |
| drop+dropck/ (leak) | 209 | zercheck leak = compile error | Covered |
| recursion/ (stack) | 41 | stack depth analysis | Covered |
| **TOTAL** | **~2,800** | | **All categories covered** |

## Key Differences in Approach

| Aspect | Rust | ZER |
|---|---|---|
| UAF prevention | Borrow checker (lifetimes) | zercheck (ALIVE/FREED tracking + gen counter) |
| Leak detection | Drop trait (runtime) | zercheck (compile error) |
| Null safety | Option<T> | ?*T / ?T |
| Thread safety | Send/Sync traits | shared struct + spawn checker |
| Deadlock | Runtime (thread poisoning) | Compile-time (lock ordering) |
| Division by zero | Runtime panic | Compile error (forced guard) |
| Buffer overflow | Runtime panic | Auto-guard or range-proven skip |
| Unsafe escape | unsafe {} blocks | No unsafe — all ops checked |

## Tests Written (rust_tests/)

### Positive (must compile + run):
- ownership_chain.zer — A→B→C transfer, cross-function free
- ownership_loop.zer — alloc/free per loop iteration
- conditional_ownership.zer — both if/else branches free
- recursive_self_ref.zer — linked list with ?*Node, walk + cleanup

### Negative (must be rejected):
- shared_mutable_nolock.zer — non-shared ptr to spawn (like Rust non-Send)
- deadlock_ordering.zer — B-then-A lock order (compile-time lockdep)

## Future: Tests to Translate from Rust

### From borrowck/ (809 files):
- Conditional borrow across branches
- Borrow in loop body
- Cross-function mutable borrow
- Re-borrow after move

### From threads-sendsync/ (69 files):
- Arc<Mutex<T>> patterns → shared struct
- Channel send/recv → Ring buffer
- Thread::spawn with closure → spawn with value args
- Scoped threads → spawn with shared struct

### From unsafe/ (62 files):
- Raw pointer deref patterns → @ptrcast provenance
- Type punning through void* → *opaque round-trip
- FFI pointer safety → cinclude + forward decl
