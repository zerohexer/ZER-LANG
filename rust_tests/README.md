# ZER Safety Test Suite

726 tests (499 positive, 227 negative), 0 failures. Updated 2026-04-12.
Runner: `run_tests.sh` — auto-detects negative tests via `reject` in name or `EXPECTED: compile error` in file.

## How to Use This File

**Adding tests:** Drop `.zer` files in this directory. Runner picks them up automatically.
**Negative tests:** Add `// EXPECTED: compile error` as first or second line.
**Naming:** `rt_<category>_<description>.zer` for Rust-translated, `gen_<category>_NNN.zer` for generated.
**Limitations:** Tests that expose unimplemented compiler checks go in `limitations/` (excluded from runner).

## Coverage by Safety Property

Each row = one safety property ZER enforces. Count = how many tests exercise it.

| Safety Property | Positive | Negative | Total | Key Test Prefixes |
|---|---|---|---|---|
| Use-after-free (handle) | 15 | 25 | 40 | gen_uaf_, rt_uaf_, rt_handle_, safety_uaf_ |
| Use-after-move (move struct) | 8 | 8 | 16 | rt_move_struct_ |
| Double free | 3 | 10 | 13 | gen_double_free_, rt_double_free_ |
| Maybe-freed (conditional) | 3 | 6 | 9 | rt_maybe_freed_, rt_conditional_ |
| Handle leak / overwrite | 2 | 8 | 10 | rt_handle_leak_, rt_ghost_, safety_handle_ |
| Scope escape (dangling ptr) | 2 | 10 | 12 | gen_scope_escape_, rt_scope_, rt_dangling_ |
| Interior pointer UAF | 2 | 3 | 5 | rt_interior_, rt_subpath_, safety_interior_ |
| Bounds / OOB | 8 | 8 | 16 | gen_bounds_, safety_bounds_, rt_const_oob |
| Division by zero | 3 | 9 | 12 | gen_div_, safety_div_, safety_mod_ |
| Null safety | 3 | 4 | 7 | gen_null_, safety_null_ |
| Narrowing / truncation | 1 | 9 | 10 | gen_narrowing_, rt_narrowing_, safety_narrowing_ |
| Overflow (wraps, no UB) | 4 | 0 | 4 | gen_overflow_, safety_overflow_, rt_overflow_ |
| *opaque provenance | 12 | 7 | 19 | rt_opaque_, safety_opaque_ |
| Shared struct / locking | 12 | 4 | 16 | gen_shared_, rt_shared_, conc_shared_ |
| Spawn / thread safety | 15 | 9 | 24 | rt_spawn_, conc_reject_, rt_send_ |
| Deadlock detection | 3 | 3 | 6 | rt_deadlock_, conc_reject_deadlock_ |
| Defer / cleanup ordering | 15 | 2 | 17 | gen_defer_, rt_defer_, rt_drop_, safety_defer_ |
| Pool / Slab / Handle ops | 20 | 6 | 26 | rt_pool_, rt_slab_, gen_handle_ |
| Arena (alloc/escape) | 6 | 4 | 10 | rt_arena_, gen_arena_, safety_arena_ |
| Async / coroutines | 14 | 2 | 16 | gen_async_, rc_async_, conc_async_ |
| Comptime | 8 | 2 | 10 | rt_comptime_, gen_comptime_, safety_comptime_ |
| Volatile / MMIO | 3 | 2 | 5 | rt_unsafe_mmio_, gen_volatile_ |
| Ring channel | 7 | 1 | 8 | gen_ring_, rc_ring_ |
| Enum / union / switch | 6 | 2 | 8 | gen_enum_, rt_union_, safety_enum_ |
| Funcptr / typedef | 5 | 0 | 5 | gen_funcptr_ |
| Goto / labels | 3 | 0 | 3 | gen_goto_ |
| Borrowck patterns (NLL) | 10 | 13 | 23 | rt_borrowck_, rt_borrow_, rt_nll_ |
| Drop ordering (Rust Drop) | 10 | 4 | 14 | rt_drop_ |
| Cross-function analysis | 5 | 5 | 10 | rt_cross_, rt_zercheck_, rt_slab_cross_ |
| Thread comms (Ring/shared) | 22 | 0 | 22 | rt_task_comm_ |

## Rust tests/ui/ Category Mapping

Status of translation from Rust's test suite directories:

| Rust Directory | Status | ZER Tests | Notes |
|---|---|---|---|
| threads-sendsync/ | **COMPLETE** | 51/67 | 16 not translatable (closures, HashMap, TLS destructors) |
| borrowck/ | Partial | ~23 | UAF, field sensitivity, scope escape, interior ptr |
| moves/ | Good | ~37 | move struct, conditional move, loop move, ownership chain |
| drop/ | Partial | ~14 | defer ordering, cleanup on all paths, struct-as-object |
| unsafe/ | Partial | ~8 | MMIO, @inttoptr, provenance |
| consts/ | Partial | ~16 | shift safety, div-by-zero, OOB, overflow wraps, comptime |
| nll/ | Minimal | ~4 | interior ptr, subpath invalidation, scope-limited borrow |

### Not-translatable Rust patterns (skip these)
Closures, traits (Send/Sync/Drop as trait), generics, iterators, HashMap/BTreeMap,
Debug formatting, lifetime annotations, async runtime (tokio), Pin/Unpin,
custom allocators, TLS destructors, stderr macros, subprocess testing.

### High-value gaps to fill next
1. **nll/** — two-phase borrows, polonius subsets, more subpath patterns
2. **borrowck/** — field-level borrow isolation, reborrow sequences
3. **moves/** — partial struct moves, move in match arms
4. **unsafe/** — more MMIO patterns, provenance chains through *opaque
5. **drop/** — conditional Drop, nested resource cleanup ordering

## Rust → ZER Pattern Translation

| Rust Pattern | ZER Equivalent |
|---|---|
| `Box<T>` | `Slab(T)` + `alloc_ptr` / `Pool(T,N)` + `alloc` |
| `Drop` trait | `defer` |
| `Rc<T>` / non-Send | non-shared `*T` to spawn → rejected |
| `Arc<Mutex<T>>` | `shared struct` |
| `RwLock<T>` | `shared(rw) struct` |
| `mpsc::channel` | `Ring(T, N)` |
| `thread::spawn` + join | `ThreadHandle th = spawn f(); th.join()` |
| `thread::spawn` fire-forget | `spawn f()` (shared ptr or value args only) |
| `Condvar` | `@cond_wait/signal/broadcast` |
| `unsafe { *raw_ptr }` | `@inttoptr / @ptrcast` (always checked) |
| `Box<dyn Any>` | `*opaque` + `@ptrcast` |
| `const fn` | `comptime` |
| `#[cfg]` | `comptime if` |
| `move \|\|` closure | function + value args to spawn |
| `mem::forget` | (no equivalent — ZER forces cleanup via zercheck) |

## Test Naming Convention

| Prefix | Source | Count | Description |
|---|---|---|---|
| `rt_` | Rust tests/ui/ | ~280 | Direct translations preserving safety intent |
| `gen_` | Generated | ~164 | Category coverage (UAF, bounds, defer, etc.) |
| `rc_` | Rust concurrency | ~43 | Concurrency: shared, spawn, ring, atomic, cond |
| `safety_` | Hand-written | ~27 | Core safety property demonstrations |
| `conc_` | Hand-written | ~22 | Concurrency edge cases + reject patterns |
| `ownership_` | Hand-written | ~3 | Ownership chain / loop / conditional |

## Known Limitations

Tests in `limitations/` are correct (should reject) but the compiler doesn't catch them yet.
Each file documents the root cause and fix direction.

Currently: **empty** (all limitations fixed: BUG-468 conditional move, BUG-469 nested move, BUG-470 return transfer, BUG-471 pool.free type check).

## File Counts by Category (for duplicate checking)

```
164 gen_*          50 rt_move_*      43 rc_*
 27 safety_*       26 rt_borrowck_*  24 rt_drop_*
 22 rt_task_*      19 rt_opaque_*    16 rt_nll_*
 15 rt_unsafe_*    11 rt_handle_*    10 rt_const_*
 10 rt_cfg_*        9 rt_defer_*      9 conc_reject_*
  7 rt_slab_*       7 rt_borrow_*    6 rt_pool_*
  6 rt_issue_*      5 rt_uaf_*       5 rt_comptime_*
  4 rt_shared_*     4 rt_scope_*     4 rt_spawn_*
  4 rt_arena_*      3 rt_send_*      3 rt_maybe_*
  3 rt_ghost_*      3 rt_double_*    3 rt_deadlock_*
```
