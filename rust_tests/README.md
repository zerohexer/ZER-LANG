# Rust Test Suite → ZER-LANG Safety Test Mapping

## Purpose
Translate Rust's tests/ui/ safety tests to ZER equivalents. Each test
preserves the SAFETY INTENT from the original Rust test, using ZER's
memory model (Pool/Slab/Handle/Arena instead of Box/Rc/Arc).

## Test Count: 350 tests, 0 failures

## Rust Directory Coverage

### tests/ui/threads-sendsync/ — COMPLETE (51/67 translated)

All 67 .rs files reviewed. 51 translated to ZER. 16 not translatable
(Rust-specific features with no ZER equivalent).

**Translated (51 files):**

| Rust File | ZER Test | Pattern |
|---|---|---|
| task-comm-0.rs | rt_task_comm_0 | 3 messages through Ring, verify FIFO |
| task-comm-1.rs | rt_task_comm_1 | Basic spawn + join |
| task-comm-3.rs | rt_task_comm_3 | 16 threads, shared counter, sum=480 |
| task-comm-4.rs | rt_task_comm_4 | Single-thread Ring: push 8, pop 8, sum=36 |
| task-comm-5.rs | rt_task_comm_5 | 100 messages through Ring, sum=4950 |
| task-comm-6.rs | rt_task_comm_6 | 4 senders, shared accumulator, sum=1998000 |
| task-comm-7.rs | rt_task_comm_7 | 4 threads with offsets, verify sum |
| task-comm-9.rs | rt_task_comm_9 | Thread sends 0..9, main sums=45 |
| task-comm-10.rs | rt_task_comm_10 | Bidirectional signaling via shared struct |
| task-comm-11.rs | rt_task_comm_11 | Channel-of-channels → shared condvar |
| task-comm-12.rs | rt_task_comm_12 | Spawn, yield, join finished thread |
| task-comm-13.rs | rt_task_comm_13 | Thread sends 10 msgs, join, no deadlock |
| task-comm-14.rs | rt_task_comm_14 | 10 threads send IDs, sum=45 |
| task-comm-15.rs | rt_task_comm_15 | Graceful receiver death handling |
| task-comm-16.rs | rt_task_comm_16 | Struct/Packet through Ring, FIFO verify |
| task-comm-17.rs | rt_task_comm_17 | Spawn temp closure (issue #922) |
| task-comm-chan-nil.rs | rt_task_comm_chan_nil | Send unit through Ring |
| issue-4446.rs | rt_issue_4446 | Send string → shared struct signal |
| issue-4448.rs | rt_issue_4448 | Thread receives message, asserts |
| issue-8827.rs | rt_issue_8827 | FizzBuzz with modulo logic |
| issue-9396.rs | rt_issue_9396 | try_recv polling → atomic flag poll |
| issue-29488.rs | rt_issue_29488 | TLS with Drop order → threadlocal |
| child-outlives-parent.rs | rt_child_outlives_parent | Thread takes owned data |
| clone-with-exterior.rs | rt_clone_with_exterior | Clone for thread, keep original |
| comm.rs | rt_comm | Basic channel: child sends 10 |
| mpsc_stress.rs | rt_mpsc_stress | 4 writers stress test |
| rc-is-not-send.rs | rt_rc_is_not_send | Non-Send → spawn rejects (NEGATIVE) |
| recursive-thread-spawn.rs | rt_recursive_thread_spawn | 10 sequential spawns |
| send-is-not-static-par-for.rs | rt_send_is_not_static | par_for → shared accumulator |
| send-resource.rs | rt_send_resource | Move resource to thread |
| send-type-inference.rs | rt_send_type_inference | Type inference with Ring |
| sendable-class.rs | rt_sendable_class | Struct through Ring |
| sendfn-spawn-with-fn-arg.rs | rt_sendfn_spawn_with_fn_arg | Function dispatch via value arg |
| spawn-fn.rs | rt_spawn_fn | Spawn bare function |
| spawn-types.rs | rt_spawn_types | Different parameter types |
| spawn.rs | rt_spawn | Basic spawn + join |
| spawn2.rs | rt_spawn2 | Spawn with captured value |
| std-sync-right-kind-impls.rs | rt_sync_send_in_std | Verify sync types compile |
| sync-send-atomics.rs | rt_sync_send_atomics | Atomics cross-thread |
| task-life-0.rs | rt_task_life_0 | Thread starts/finishes, main continues |
| task-spawn-barefn.rs | rt_task_spawn_barefn | Spawn bare fn |
| task-spawn-move-and-copy.rs | rt_task_spawn_move_and_copy | Value preserved across thread |
| thread-join-unwrap.rs | rt_thread_join_unwrap | Minimal join |
| threads.rs | rt_threads | Basic threading |
| tls-dont-move-after-init.rs | rt_tls_dont_move_after_init | TLS pointer stability |
| tls-init-on-init.rs | rt_tls_init_on_init | TLS re-entrant init |
| trivial-message.rs | rt_trivial_message | Send one trivial message |
| unwind-resource.rs | rt_unwind_resource | Drop on panic → defer cleanup |
| yield.rs | rt_yield | Thread yield → async yield |
| yield1.rs | rt_yield1 | Multiple yields |
| yield2.rs | rt_yield2 | Parent yields for child |

**Not translatable (16 files) — Rust-specific features:**

| Rust File | Why Not Translatable |
|---|---|
| sendfn-is-a-block.rs | Closures (ZER has no closures) |
| send_str_hashmap.rs | HashMap (ZER has no stdlib collections) |
| send_str_treemap.rs | BTreeMap (ZER has no stdlib collections) |
| spawning-with-debug.rs | Debug trait (no formatting equivalent) |
| sync-send-iterators-in-libcollections.rs | Iterator trait bounds |
| sync-send-iterators-in-libcore.rs | Iterator trait bounds |
| thread-local-syntax.rs | TLS macro syntax (ZER uses keyword) |
| thread-local-extern-static.rs | extern TLS |
| tls-dtors-are-run-in-a-static-binary.rs | TLS destructors (no Drop trait) |
| tls-in-global-alloc.rs | Custom global allocator + TLS |
| tls-try-with.rs | TLS try_with during Drop |
| eprint-on-tls-drop.rs | stderr + TLS drop |
| task-stderr.rs | stderr access |
| test-tasks-invalid-value.rs | Rust test harness env var parsing |
| issue-24313.rs | TLS destructor panic in subprocess |

### tests/ui/borrowck/ — Partial (13 patterns translated)

| Pattern | ZER Test | Safety Property |
|---|---|---|
| nested-calls-free | rt_borrowck_nested_calls_free | Cross-function UAF |
| borrow-from-owned-ptr | rt_borrowck_borrow_from_owned_ptr | Interior pointer UAF |
| assign-comp | rt_borrowck_assign_comp | Field pointer after free |
| borrow-from-temporary | rt_borrowck_borrow_from_temporary | Return ptr to local |
| block-uninit | rt_auto_zero_init | Auto-zero (ZER safer) |
| and-init | rt_borrowck_and_init | Short-circuit auto-zero |

### tests/ui/moves/ — Partial (6 patterns translated)

| Pattern | ZER Test | Safety Property |
|---|---|---|
| moves-based-on-type-exprs | rt_moves_based_on_type_exprs | UAF after store in struct |
| move-in-guard-1 | rt_move_in_guard_1 | MAYBE_FREED in branch |
| move-out-of-field | rt_move_out_of_field | Consume via function |
| no-reuse-move-arc | rt_no_reuse_move_arc | Shared struct safe after spawn |
| issue-72649-uninit-in-loop | rt_issue_72649_uninit_in_loop | UAF in loop iteration |
| move-3-unique | rt_move_3_unique | Conditional ownership |

### tests/ui/unsafe/ — Partial (1 pattern translated)

| Pattern | ZER Test | Safety Property |
|---|---|---|
| unsafe-fn-deref-ptr | rt_unsafe_fn_deref_ptr | @inttoptr without mmio decl |

## Other Test Categories

### Generated tests (gen_*, rc_*) — 161 tests
Category-based generation covering UAF, double-free, bounds, division,
null safety, narrowing, scope escape, defer, handles, shared struct,
async, arena, comptime, enum, union, funcptr, goto, overflow, Ring.

### Hand-written safety tests (safety_*) — 27 tests
Direct translations of Rust safety patterns: div-zero, mod-zero,
overflow wrap, shift width, bounds OOB, UAF, double-free, dangling
return, narrowing, null deref, union variant, exhaustive switch,
defer cleanup, comptime eval, opaque provenance, arena escape.

### Hand-written concurrency tests (conc_*) — 22 tests
Shared counter, value transfer, deadlock ordering, atomic flag,
multi-field shared, scoped pointer, async yield/multi-task,
condvar producer-consumer, rwlock readers, threadlocal isolation,
@once multithread, reject patterns (non-shared spawn, double join).

### Hand-written ZER-specific (rt_ extra) — 15 tests
Pool slot reuse, slab stress, handle array, shared counter 4-thread,
condvar ping-pong, atomic CAS spinlock, *opaque round-trip, defer
nested return, auto-zero init, alloc/free loop cycle.

## Key Differences: ZER vs Rust

| Aspect | Rust | ZER |
|---|---|---|
| UAF prevention | Borrow checker (lifetimes) | zercheck (ALIVE/FREED + gen counter) |
| Leak detection | Drop trait (runtime) | zercheck (compile error) |
| Null safety | Option<T> | ?*T / ?T |
| Thread safety | Send/Sync traits | shared struct + spawn checker |
| Deadlock | Runtime (thread poisoning) | Compile-time (lock ordering) |
| Division by zero | Runtime panic | Compile error (forced guard) |
| Buffer overflow | Runtime panic | Auto-guard or range-proven skip |
| Unsafe escape | unsafe {} blocks | No unsafe — all ops checked |
| Uninitialized vars | Compile error | Auto-zero (always 0) |
| Integer overflow | Panic (debug) / wrap (release) | Always wrap (defined) |
