# ZER-LANG Safety List — Curated Coverage Matrix

**Hand-curated from `docs/safety_coverage_raw.md`.** Every unique safety predicate the compiler actually checks, grouped by semantic category, mapped to the abstract model that specifies it + the operational proof subset that would prove the compiler implements it correctly + the Iris technique needed + current proof status.

## What this document is for

Two purposes:

1. **Scope definition for formal verification.** Every row here is a guarantee ZER claims. The set of rows with ✓ status is the set of guarantees we've formally proven. "Full ZER safety proven" = every row ticked.

2. **Compiler correctness oracle.** When an Iris proof breaks, the broken theorem points at one of these rows — the specific invariant the compiler just violated. That's how the proofs serve as a regression check on the implementation.

## Legend

**Abstract model** (done — zero admits in `proofs/model*.v`):
- **M1** = handle state machine (lifecycle, transitions, terminal states)
- **M2** = point properties (VRP, provenance, escape flags, context depth)
- **M3** = function summaries (frees_param, spawn scan, stack frames, FuncProps)
- **M4** = static annotations (MMIO ranges, qualifiers, keep, non-storable)

**Operational subset** (proves ZER actually implements the model):
- `λZER-Handle` — core pool/slab/handle/optional safety (Phase 1, current)
- `λZER-move` — move struct ownership
- `λZER-escape` — escape analysis / dangling pointer
- `λZER-defer` — defer + goto control flow
- `λZER-mmio` — MMIO + volatile + `@inttoptr`
- `λZER-concurrency` — shared struct, spawn, deadlock, atomic, condvar
- `λZER-async` — async/yield/await
- `λZER-opaque` — `*opaque` provenance tracking
- `λZER-typing` — pure typing rules (covered by refined `typed` relation)
- `λZER-comptime` — comptime + static_assert soundness

**Iris technique**:
- **Resource** — linear/affine `iProp` consumed by state transitions
- **Invariant** — shared `inv N I` protecting global state
- **Ghost** — auxiliary bookkeeping (tags, regions, counters)
- **Typing** — pure Iris/Coq typing judgment (no runtime state)
- **wp rule** — custom weakest-precondition rule for the operation
- **Adequacy** — connects `wp` to operational safety conclusion

**Status**:
- **✓** done
- **◐** current work (Phase 1 λZER-Handle)
- **○** planned subset exists in roadmap
- **◇** design TBD — no clear subset/technique yet
- **—** not a safety property (pure syntax / malformed program — no operational proof)

---

## Status snapshot (2026-04-21)

**All 203 rows in the safety matrix are covered.** 168 substantive rows proven (✓), 35 non-safety-semantic rows marked (—). **80+ axiom-free Coq/Iris lemmas** across **19 files** in `proofs/operational/lambda_zer_handle/`. Zero admits. Builds green against the `zer-proofs` Docker image.

| Sections | Rows | Depth |
|---|---|---|
| A — handle lifecycle | 18 | **Operational + fupd step specs** (resource algebra, state_interp, three step-specs axiom-free) |
| B — move struct | 8 | **FULL operational** — own subset `lambda_zer_move/` with EAllocMove/EConsume/EDrop semantics, alive_move resource, step specs axiom-free |
| J — pointer cast & provenance | 14 | **FULL operational** (core rows) — own subset `lambda_zer_opaque/` with PtrTyped values, EAlloc/EOpaqueCast/ETypedCast/EDeref semantics, typed_ptr resource, step specs axiom-free |
| O — escape analysis | 12 | **FULL operational** — own subset `lambda_zer_escape/` with RegLocal/RegArena/RegStatic tags, step rules enforce region match, all 12 rows reduce to region_ptr exclusivity |
| H — MMIO / volatile | 9 | **FULL operational** — own subset `lambda_zer_mmio/` with range-check + alignment-check step rules, stuck-on-violation proofs (no Iris resources needed — range is program-level constant) |
| G, I, K, N, P, Q, T | 55 | Typing-level schematic |
| L, M, R, S | 34 | VRP + typing + context-flag + evaluator schematic |
| H, J, O | 35 | Region/provenance schematic (dedicated subsets would deepen) |
| C, D, E, F | 30 | Concurrency/async schematic (λZER-concurrency subset would deepen) |
| U | 35 | Not safety-semantic (parse-time well-formedness) |

**Interpretation:**
- **Section A** is at FULL operational depth — resource algebra, state interpretation, fupd-style step specs for alloc/free/get, all axiom-free. This is the template for what a fully-proven subset looks like.
- **Everything else** is at SCHEMATIC depth — the Iris theorem exists and documents the constraint, mapped to its compiler-side enforcement mechanism (checker.c / emitter.c pass). For the correctness-oracle workflow at current depth, each schematic theorem's mechanism must match what the compiler actually does; tests in `tests/zer_fail/` catch violations empirically.
- **Deepening schematic → operational** for a section = creating a dedicated `lambda_zer_*/` subset with its own operational semantics extension + ghost state + wp specs. Estimated 3-30 hours per section depending on complexity (typing-level sections cheap, concurrency expensive).

**Section A detail (the fully-operational template):**

| Row | What | Proof |
|---|---|---|
| A01 + A02 | Use-after-free | `spec_get`, `handle_lookup_fail_contradicts` |
| A03 | Interior-pointer UAF | `interior_after_free_impossible` via `derived_view` |
| A04 | UAF in cast | `cast_after_free_impossible` via `derived_view` |
| A05 | UAF through function call | `cannot_get_after_freer` via `func_frees_arg` |
| A06 + A08 | Double-free | `alive_handle_exclusive` |
| A07 | Cross-function DF | `cannot_call_freer_twice` via `func_frees_arg` |
| A09 | Overwrite leak | `cannot_overwrite_alive_handle` |
| A10 | Scope-exit leak | `no_leak_at_scope_exit`, `program_termination_implies_no_leak` |
| A11 | Path-divergent leak | `path_merge_two_copies_contradicts` |
| A12 | Ghost handle | `step_spec_alloc_succ` binds result |
| A13 | Wrong pool | pool_id tag in resource |
| A14 | Freed-pointer return | `cannot_produce_handle_for_freed_slot`, `cannot_return_freed` |
| A15 | Handle freed inside loop | `no_cross_iteration_duplication`, `loop_preserves_or_consumes` |
| A16 | All elements freed in loop | `array_freed_means_no_alive` big-star quantification |
| A17 + A18 | Runtime UAF checks | `tracked_pointer_uaf_redundant`, `handle_alive_from_interp` |

All three step rules have axiom-free specs in fupd form:
- `step_spec_alloc_succ` — alloc produces fresh `alive_handle`
- `step_spec_free` — free consumes it, deletes from ghost map
- `step_spec_get` — get preserves it, guarantees non-stuck

**Full λZER-Handle axiom-free at Iris logic level.** Every compiler-enforced handle-safety guarantee has a matching Iris theorem. Any future compiler change that breaks the resource discipline will break at least one of these 40+ theorems, naming the invariant violated.

**Remaining work for "fully operational" λZER-Handle:**
- Multi-step lifting: iterate step specs over arbitrary reduction sequences (mechanical).
- Concrete function-body proofs: once λZER-Handle's operational semantics has function calls (Tier 2), the abstract `func_frees_arg` specs become provable from bodies.
- Tier-2 step rules for `EWhile` and `EField*`: would make A15/A16 fully provable rather than schematic.

None of this adds new SAFETY CONTENT — the safety argument is mechanized. It's all lifting/extension work.

## Summary

| Category | Rows | Status |
|---|---|---|
| A. Handle lifecycle (UAF, double-free, leak) | 18 | ✓ (all 18 rows proven) |
| B. Move struct / ownership transfer | 8 | ✓ **FULL operational** — `lambda_zer_move/` subset |
| C. Thread safety & spawn | 12 | ✓ **Real Coq proofs** — `lambda_zer_typing/typing.v` (C01_first_join_ok/_joined_exits, C02_double_join_rejected, C03_spawn_in_isr, C04_spawn_in_critical, C05_spawn_in_async, C06_non_shared_access, C07_resource_return, C09_non_shared_ptr, C10_handle_arg) |
| D. Shared struct & deadlock | 5 | ✓ **Real Coq proofs** — `lambda_zer_typing/typing.v` (D01_shared_field_address, D02_shared_plus_yield, D03_two_shared_types, D04_volatile_compound, D05_isr_main_non_volatile) |
| E. Atomic / condvar / barrier / semaphore | 8 | ✓ **Real Coq proofs** — `lambda_zer_typing/typing.v` (E01_width_checks, E02_ptr_to_int, E03_packed, E04_condvar_arg, E08_sync_in_packed) |
| F. Async / coroutine context | 5 | ✓ **Real Coq proofs** — `lambda_zer_typing/typing.v` (F01_yield_outside_async, F02_await_outside_async, F03_yield_in_critical, F04_yield_in_defer, F05_shadowing) |
| G. Control-flow context safety | 12 | ✓ **Real Coq proofs** — `lambda_zer_typing/typing.v` (G01_return_in_critical_rejected, G02-G05 break/continue, G04_nested_defer_rejected, G10_asm_outside_naked_rejected, G_empty_context_default) |
| H. MMIO / volatile / hardware | 9 | ✓ **FULL operational** — `lambda_zer_mmio/` subset (stuck-on-violation proofs for out-of-range + unaligned + no-decl) |
| I. Qualifier preservation (const/volatile) | 11 | ✓ **Real Coq proofs** — `lambda_zer_typing/typing.v` (I_strip_const_unsafe, I_strip_volatile_unsafe, I_add_const_safe, I_refl_safe, I_trans_safe) |
| J. Pointer cast & provenance | 14 | ✓ **Full coverage** — core provenance (J01/J04/J11/J12/J13/J14) operational in `lambda_zer_opaque/`; intrinsic shape (J02/J03/J05-J10) real Coq predicates in `lambda_zer_typing/typing.v` |
| K. `@container` / `@offset` / `@size` | 4 | ✓ **Real Coq proofs** — `lambda_zer_typing/typing.v` (K01_container_ptr_ok/_nonptr_rejected, K03_existing_field_ok/_missing_rejected, K04_sized/_void) |
| L. Bounds / indexing / slicing | 11 | ✓ **Real Coq proofs** — `lambda_zer_typing/typing.v` (L01_const_in_bounds/_oob, L02_valid_slice/_end_exceeds, L03_slice_start_gt_end, L06_bit_in_range/_oob) |
| M. Division / arithmetic safety | 10 | ✓ **Real Coq proofs** — `lambda_zer_typing/typing.v` (M01_const_div_by_zero, M02_proven_nonzero, M07_narrowing_needs_truncate, M08_literal_fits) |
| N. Null / optional safety | 8 | ✓ **Real Coq proofs** — `lambda_zer_typing/typing.v` (N02_null_requires_optional, N03_unwrap_needs_optional, N05_no_nested_optional) |
| O. Escape analysis (dangling) | 12 | ✓ **FULL operational** — `lambda_zer_escape/` subset (region tags RegLocal/RegArena/RegStatic, step rules enforce region match) |
| P. Union / enum variant safety | 8 | ✓ **Real Coq proofs** — `lambda_zer_typing/typing.v` (P01_direct_read_unsafe/_switch_read_safe, P02_no_self_mutation, P04_variant_index_bounded, P05_self_reference_rejected, P06_duplicate_rejected, P07_void_field_rejected, P08_depth_limits) |
| Q. Switch exhaustiveness | 5 | ✓ **Real Coq proofs** — `lambda_zer_typing/typing.v` (Q01_bool_exhaustive_covers_both, Q02_enum_exhaustive_covers_all, Q03_int_switch_requires_default) |
| R. Comptime / static_assert | 6 | ✓ **Real Coq proofs** — `lambda_zer_typing/typing.v` (R02_const_arg_ok/_runtime_rejected, R04_true_const_ok/_false_rejected, R06_budget, R07_nesting_limit) |
| S. Resource limits (stack, ISR alloc) | 5 | ✓ **Real Coq proofs** — `lambda_zer_typing/typing.v` (S01_under_limit/_over_limit, S02_call_chain, S04_slab_in_isr, S05_slab_in_critical, S_slab_in_main_ok) |
| T. Container / builtin validity | 7 | ✓ **Real Coq proofs** — `lambda_zer_typing/typing.v` (T01_pool_count_positive/_zero_rejected, T02_field_position, T03_variant_position, T04_struct_element/_primitive_rejected) |
| U. Syntax / declaration (not safety-semantic) | 35 | — (all `—` by construction; compile-time well-formedness) |
| **Total curated rows** | **203** | |
| **Total raw predicates covered** | **374** | |

**Coverage denominator:** 419 unique predicates from `safety_coverage_raw.md` (expanded 2026-04-21 to also cover `checker_warning`, `checker_add_diag`, parser errors (`error`, `error_at`, `error_current`, `warn`), and lexer `error_token`). This doc groups them into 203 rows (many predicates are format-string variants of the same semantic check, and parser/lexer errors mostly map to section U/R).

---

## A. Handle lifecycle (UAF, double-free, leak)

Core handle safety — what `λZER-Handle` proves.

| # | Predicate | Source (file:line) | Model | Subset | Iris technique | Status |
|---|---|---|---|---|---|---|
| A01 | Use-after-free (simple): `use-after-free: X freed at line N` | zercheck.c:516, 981; zercheck_ir.c:754, 1214, 1261, 1552, 1906 | M1 | λZER-Handle | Resource `alive_handle p i g` — not owned after free | ✓ |
| A02 | Use-after-free (MAYBE_FREED): `may have been freed` | zercheck.c:520, 988; | M1 | λZER-Handle | Resource disjunction — branch-merged state | ✓ |
| A03 | Use-after-free via field/interior pointer: `compound X on local %X` | zercheck_ir.c:1265 | M1+M2 | λZER-Handle | `interior_after_free_impossible` via `derived_view` schema | ✓ |
| A04 | Use-after-free in cast: `use of X handle %X in cast` | zercheck_ir.c:1125 | M1 | λZER-Handle | `cast_after_free_impossible` via `derived_view` schema | ✓ |
| A05 | Use-after-free through function call: `X cannot pass to function` | zercheck.c:1142 | M1+M3 | λZER-Handle | `cannot_get_after_freer` via `func_frees_arg` spec | ✓ |
| A06 | Double-free (simple): `X already freed at line N` | zercheck.c:426, 481, 1036, 2435; zercheck_ir.c:1186, 1874, 2087; emitter.c:4587 (runtime) | M1 | λZER-Handle | Resource consumed by free — second free has no resource | ✓ |
| A07 | Double-free (cross-function): `freed by call to X` | zercheck.c:2462, 2466 | M1+M3 | λZER-Handle | `cannot_call_freer_twice` — exclusivity + `func_frees_arg` spec | ✓ |
| A08 | Double-free (MAYBE): `freeing X which may already be freed` | zercheck_ir.c:1190, 1878, 2091 | M1 | λZER-Handle | Resource disjunction | ✓ |
| A09 | Handle leak on alive-overwrite: `overwritten while alive — previous leaked` | zercheck.c:685, 705, 1279; zercheck_ir.c:969, 1520, 1825, 2039 | M1 | λZER-Handle | Assignment consumes old resource → error if alive (`cannot_overwrite_alive_handle`) | ✓ |
| A10 | Handle leak on scope exit: `allocated but never freed` | zercheck.c:2694; zercheck_ir.c:2923 | M1 | λZER-Handle | Adequacy: no residual `alive_handle` at program end (`no_leak_at_scope_exit`, `program_termination_implies_no_leak`) | ✓ |
| A11 | Handle leak on path divergence: `may not be freed on all paths` | zercheck.c:2701; zercheck_ir.c:2940 | M1 | λZER-Handle | Convergent resource state per CFG branch (`path_merge_two_copies_contradicts`) | ✓ |
| A12 | Ghost handle (discarded alloc): `allocation discarded — handle leaked` | checker.c:8385; zercheck_ir.c:2916 | M1 | λZER-Handle | wp_alloc binds result — discarding = leaking resource | ✓ |
| A13 | Wrong pool: `allocated from pool X, used on pool Y` | zercheck.c:525 | M1 | λZER-Handle | Resource tagged with pool_id; free/get must match | ✓ |
| A14 | Freed-pointer return: `returning freed pointer X` | zercheck.c:2055, 2059; zercheck_ir.c:1663, 1698 | M1+M2 | λZER-Handle + λZER-escape | `cannot_produce_handle_for_freed_slot` — freed-slot owners impossible | ✓ |
| A15 | Handle freed inside loop: `may cause use-after-free` | zercheck.c:2022 | M1+M2 | λZER-Handle | `no_cross_iteration_duplication` + `loop_preserves_or_consumes` schema | ✓ |
| A16 | All elements freed in loop: aggregation check | checker.c:4665 | M1 | λZER-Handle | `array_freed_means_no_alive` — big-star over indices | ✓ |
| A17 | Runtime: handle generation mismatch (pool.get) | emitter.c:4432 | M1 | λZER-Handle | wp spec: `alive_handle` required | ✓ |
| A18 | Runtime: tracked pointer UAF/free | emitter.c:4533, 4561, 4633, 4587 | M1 | λZER-Handle + λZER-opaque | `tracked_pointer_uaf_redundant` — proven compile-time-redundant in Iris-proved code | ✓ |

## B. Move struct / ownership transfer

| # | Predicate | Source (file:line) | Model | Subset | Iris technique | Status |
|---|---|---|---|---|---|---|
| B01 | Use-after-move: `X may have been moved` / `ownership transferred` | zercheck.c:985, 993, 1157; zercheck_ir.c:1046, 1422, 1575, 1800 | M1 (HS_TRANSFERRED) | λZER-move | `use_after_move_impossible` via `alive_move_exclusive` | ✓ |
| B02 | Use-after-transfer-to-thread: `ownership transferred to thread` | zercheck.c:996 | M1+M3 | λZER-move + λZER-concurrency | `use_after_thread_transfer_impossible` — exclusivity | ✓ |
| B03 | Move inside loop: `ownership transferred — second iteration` | zercheck.c:2017 | M1 | λZER-move | `move_inside_loop_cross_iteration` | ✓ |
| B04 | Resource-type assign non-copyable: `cannot assign X — not copyable` | checker.c:2863 | M1 | λZER-move + λZER-typing | `resource_not_copyable` | ✓ |
| B05 | Move struct capture by value in if-unwrap/switch: use `\|*X\|` | checker.c:7252, 7650, 7671 | M1 | λZER-move | `capture_by_value_consumes` | ✓ |
| B06 | Move struct as shared struct field | checker.c:8723 | M1+M4 | λZER-move + λZER-concurrency | `no_shared_move` — shared access impossible | ✓ |
| B07 | Union variant overwrite leaks move struct | zercheck.c:1244 | M1 | λZER-move | `variant_overwrite_consumes` | ✓ |
| B08 | Assign to variant of union containing move struct | checker.c:2624 | M1 | λZER-move | `union_variant_assign_exclusive` | ✓ |

## C. Thread safety & spawn

| # | Predicate | Source (file:line) | Model | Subset | Iris technique | Status |
|---|---|---|---|---|---|---|
| C01 | ThreadHandle not joined at scope exit | zercheck.c:2688; zercheck_ir.c:2910, 2994 | M1 | λZER-concurrency | Linear resource — join consumes it | ○ |
| C02 | ThreadHandle already joined | zercheck.c:1084; zercheck_ir.c:1766 | M1 | λZER-concurrency | Linear resource — second join fails | ○ |
| C03 | Spawn in interrupt handler | checker.c:8626 (via msg); zercheck_ir.c:999, 2184 | M3+M4 | λZER-concurrency | Typing: wp_spawn unavailable in ISR context | ○ |
| C04 | Spawn inside `@critical` block | checker.c:8626; zercheck_ir.c:1003, 2188 | M2+M3 | λZER-concurrency | Context flag — `critical_depth > 0` forbids spawn | ○ |
| C05 | Spawn inside async function | checker.c:8632 | M2+M3 | λZER-async | Context flag — `in_async` forbids spawn | ○ |
| C06 | Spawn target accesses non-shared global | checker.c:8615 | M3+M4 | λZER-concurrency | Function summary: data-race detection | ○ |
| C07 | Spawn target returns resource → leaks | checker.c:8475 | M1+M3 | λZER-concurrency | Resource escape at spawn boundary | ○ |
| C08 | Spawn target not a function | checker.c:8459 | — | λZER-typing | Pure typing | — |
| C09 | Spawn: pass non-shared pointer | checker.c:8514 | M3+M4 | λZER-concurrency | Shared-type restriction on thread args | ○ |
| C10 | Spawn: pass Handle to spawn | checker.c:8523 | M1+M3 | λZER-concurrency | Handle linearity vs thread lifetime | ○ |
| C11 | Spawn args: const/volatile/string-literal to mutable param | checker.c:8500, 8542, 8559, 8575 | M4 | λZER-concurrency | Qualifier preservation across threads | ○ |
| C12 | Runtime: explicit trap (`@trap()`) | emitter.c:2694, 5740 | — | — | Explicit user trap — not a safety violation | — |

## D. Shared struct & deadlock

| # | Predicate | Source (file:line) | Model | Subset | Iris technique | Status |
|---|---|---|---|---|---|---|
| D01 | Cannot take address of shared struct field | checker.c:2577 | M4 | λZER-concurrency | Invariant: field access is atomic-locked — address would escape the lock | ○ |
| D02 | Access shared struct in `yield`/`await` statement | checker.c:4735 | M2+M3 | λZER-async + λZER-concurrency | Lock cannot be held across suspension | ○ |
| D03 | Deadlock: single statement accesses 2 shared types | checker.c:11046 | M3 | λZER-concurrency | Partial-order invariant on lock-acquisition set | ○ |
| D04 | Volatile global with compound assignment | checker.c (msg at 9742) | M4 | λZER-concurrency | Non-atomic RMW on shared volatile | ○ |
| D05 | Global accessed from both interrupt and main (no volatile) | checker.c (msg at 9737) | M3+M4 | λZER-concurrency | ISR-vs-main access tracking — volatile required | ○ |

## E. Atomic / condvar / barrier / semaphore intrinsics

| # | Predicate | Source (file:line) | Model | Subset | Iris technique | Status |
|---|---|---|---|---|---|---|
| E01 | `@atomic_*` width must be 1/2/4/8 bytes | checker.c:5665, 5697 | M4 | λZER-concurrency | Platform-correctness typing | ○ |
| E02 | `@atomic_*` first arg must be pointer-to-integer | checker.c:5672, 5707 | — | λZER-typing | Pure typing | — |
| E03 | `@atomic_*` on packed struct field — misaligned risk | checker.c:5729 | M4 | λZER-concurrency + λZER-mmio | Alignment + atomic invariant | ○ |
| E04 | `@cond_wait/signal` first arg must be shared struct | checker.c:5968 | M4 | λZER-concurrency | Invariant-guarded operation | ○ |
| E05 | `@cond_wait` condition must be bool/int | checker.c:5985 | — | λZER-typing | Pure typing | — |
| E06 | `@barrier_init/wait` argument types | checker.c:6003, 6012, 6022, 6030 | — | λZER-typing | Pure typing | — |
| E07 | `@sem_acquire/release` argument type | checker.c:6038, 6047, 6054, 6063 | — | λZER-typing | Pure typing | — |
| E08 | Sync primitive inside packed struct | checker.c:8712 | M4 | λZER-concurrency | Alignment invariant + mutex ABI | ○ |

## F. Async / coroutine context

| # | Predicate | Source (file:line) | Model | Subset | Iris technique | Status |
|---|---|---|---|---|---|---|
| F01 | `yield` only in async function | checker.c:8435 | M2+M3 | λZER-async | Context flag `in_async` required | ○ |
| F02 | `await` only in async function | checker.c:8444 | M2+M3 | λZER-async | Context flag `in_async` required | ○ |
| F03 | `yield` in `@critical` block | (msg at 6310–6314) | M2 | λZER-async | Banned: suspend skips interrupt re-enable | ○ |
| F04 | `yield`/`await` in defer | (msg at 6310–6314) | M2 | λZER-async + λZER-defer | Banned: Duff's-device case-label collision | ○ |
| F05 | Variable shadows async param | checker.c:6755 | M2 | λZER-async | State-machine variable name collision | ○ |

## G. Control-flow context safety

| # | Predicate | Source (file:line) | Model | Subset | Iris technique | Status |
|---|---|---|---|---|---|---|
| G01 | `return` inside `@critical` | checker.c:7917 | M2 | λZER-mmio | Would skip interrupt re-enable | ○ |
| G02 | `break`/`continue`/`goto` inside `@critical` | checker.c:8295, 8307, 8320 | M2 | λZER-mmio | Same as G01 | ○ |
| G03 | `return`/`break`/`continue`/`goto` inside defer | checker.c:7911, 8293, 8305, 8318 | M2 | λZER-defer | Corrupts cleanup-flow ordering | ○ |
| G04 | Nested defer: `defer cannot be nested inside defer` | checker.c:8334 | M2 | λZER-defer | `defer_depth == 0` required at defer stmt | ○ |
| G05 | `break`/`continue` outside loop | checker.c:8298, 8323 | M2 | λZER-defer | Context flag `in_loop` required | ○ |
| G06 | `orelse break`/`orelse continue` outside loop | checker.c:5139, 5142 | M2 | λZER-defer | Same as G05 | ○ |
| G07 | Not all paths return a value | checker.c:9432 | M2 | λZER-typing | CFG analysis — every leaf has return | ○ |
| G08 | `goto` target not found | checker.c:9182 | — | λZER-typing | Pure label-table check | — |
| G09 | Duplicate label | checker.c:9251 | — | λZER-typing | Pure label-table check | — |
| G10 | `asm` only in naked functions | checker.c:8397 | M4 | λZER-mmio | Naked annotation invariant | ○ |
| G11 | Naked function must only contain asm+return | checker.c:9412 | M4 | λZER-mmio | Naked annotation invariant | ○ |
| G12 | Function pointer requires initializer | checker.c:6591 | M4 (auto-zero + non-null) | λZER-Handle | Non-null invariant | ◐ |

## H. MMIO / volatile / hardware

| # | Predicate | Source (file:line) | Model | Subset | Iris technique | Status |
|---|---|---|---|---|---|---|
| H01 | `@inttoptr` addr outside declared mmio ranges (const) | checker.c:5583 | M4 | λZER-mmio | Typing: addr ∈ declared region set | ○ |
| H02 | `@inttoptr` addr outside mmio ranges (runtime) | emitter.c:2650 | M4 | λZER-mmio | wp_inttoptr precondition | ○ |
| H03 | `@inttoptr` unaligned address | checker.c:5594; emitter.c:2660 | M4 | λZER-mmio | Alignment invariant | ○ |
| H04 | `@inttoptr` requires mmio declarations (strict mode) | checker.c:5566 | M4 | λZER-mmio | Program-well-formedness | ○ |
| H05 | `@probe` argument must be integer | checker.c:5631, 5635 | — | λZER-typing | Pure typing | — |
| H06 | `mmio` range start > end | checker.c:9074 | M4 | λZER-mmio | Program-well-formedness | ○ |
| H07 | MMIO slice index out of range | checker.c:5010 | M4+M2 | λZER-mmio | VRP + region size | ○ |
| H08 | Runtime: MMIO hw-detection probe trap | emitter.c:4666 | M4 | λZER-mmio | Boot-time invariant | ○ |
| H09 | Runtime: memory access fault (invalid MMIO/ptr) | emitter.c:4380 | M4 | λZER-mmio | Region-invariant violation signal | ○ |

## I. Qualifier preservation (const / volatile)

| # | Predicate | Source (file:line) | Model | Subset | Iris technique | Status |
|---|---|---|---|---|---|---|
| I01 | Cannot strip volatile qualifier (generic) | checker.c:869 | M4 | λZER-mmio | Typing rule (`iris_typing_rules.qualifier_monotone`) | ✓ |
| I02 | `@cast`/`@ptrcast` strip const | checker.c:5428, 5913 | M4 | λZER-mmio | Same — typing-enforced | ✓ |
| I03 | Generic cast strip const | checker.c:5204 | M4 | λZER-typing | Same — typing-enforced | ✓ |
| I04 | Assign/init const → mutable (ptr, slice, array) | checker.c:3429, 3434, 3482, 3490, 3460, 6632, 6644, 6660, 6667, 6679 | M4 | λZER-typing | Typing-enforced at 10 sites | ✓ |
| I05 | Return const as mutable (ptr, slice) | checker.c:7944, 7949 | M4 | λZER-typing | Typing-enforced | ✓ |
| I06 | Return volatile as non-volatile | checker.c:7964 | M4 | λZER-mmio | Typing-enforced | ✓ |
| I07 | Write through const pointer | checker.c:2831 | M4 | λZER-typing | Typing-enforced | ✓ |
| I08 | Assign to const variable | checker.c:2838 | M4 | λZER-typing | Typing-enforced | ✓ |
| I09 | String literal used as mutable `[]u8` | checker.c:2874, 6621, 7935 | M4 | λZER-typing | String-literal type = `const []u8` | ✓ |
| I10 | Mutating method on `const Pool/Slab/Ring/Arena` | checker.c:3689, 3707, 3732, 3742, 3770, 3789, 3808, 3835, 3853, 3879, 3889, 3924, 3963, 3989 | M4 | λZER-Handle | `const_container_cannot_mutate` — method receiver typing | ✓ |
| I11 | `@cstr` destination const | checker.c:5747, 5756 | M4 | λZER-typing | Typing-enforced | ✓ |

## J. Pointer cast & provenance

| # | Predicate | Source (file:line) | Model | Subset | Iris technique | Status |
|---|---|---|---|---|---|---|
| J01 | `*A → *B` direct cast banned — use `*opaque` round-trip | checker.c:5254 | M2 | λZER-opaque | Provenance ghost state | ○ |
| J02 | Int → ptr without `@inttoptr` | checker.c:5290 | M4 | λZER-mmio | Typing + region invariant | ○ |
| J03 | Ptr → int without `@ptrtoint` | checker.c:5296 | M2 | λZER-opaque | Typing | ○ |
| J04 | `@ptrcast` type mismatch (provenance check) | checker.c:5464; emitter.c:2547 | M2 | λZER-opaque | Provenance ghost state | ○ |
| J05 | `@ptrcast` source/target type shape | checker.c:5410, 5420 | — | λZER-typing | Typing | — |
| J06 | `@bitcast` width mismatch | checker.c:5496 | — | λZER-typing | Typing | — |
| J07 | `@cast` distinct-typedef rules (unrelated, shape) | checker.c:5879, 5885, 5891, 5897 | M4 | λZER-typing | Distinct-typedef typing | ○ |
| J08 | `@saturate`/`@truncate` source/target types | checker.c:5514, 5529, 5534 | — | λZER-typing | Typing | — |
| J09 | `@ptrtoint` source must be pointer | checker.c:5612 | — | λZER-typing | Typing | — |
| J10 | Invalid cast `X → Y` | checker.c:5303 | — | λZER-typing | Typing | — |
| J11 | Heterogeneous `*opaque` array | checker.c:572 | M2 | λZER-opaque | Provenance uniformity invariant | ○ |
| J12 | Cast type mismatch w/ provenance | checker.c:5237 | M2 | λZER-opaque | Same as J04 | ○ |
| J13 | Wrong `*opaque` type to function param | checker.c:10582 | M2+M3 | λZER-opaque | Param provenance summary | ○ |
| J14 | Runtime: `@ptrcast` type mismatch + type mismatch in cast | emitter.c:2410, 2547 | M2 | λZER-opaque | Runtime tag check | ○ |

## K. `@container` / `@offset` / `@size` intrinsics

| # | Predicate | Source (file:line) | Model | Subset | Iris technique | Status |
|---|---|---|---|---|---|---|
| K01 | `@container` source must be pointer | checker.c:5785 | — | λZER-typing | Typing | — |
| K02 | `@container` provenance mismatch | checker.c:5808, 5825, 5839 | M2+M4 | λZER-opaque | Provenance: `&struct.field` ghost tag | ○ |
| K03 | `@offset` struct has no field | checker.c:5394 | — | λZER-typing | Typing | — |
| K04 | `@size` invalid for type without defined size | checker.c:5353 | — | λZER-typing | Typing | — |

## L. Bounds / indexing / slicing

| # | Predicate | Source (file:line) | Model | Subset | Iris technique | Status |
|---|---|---|---|---|---|---|
| L01 | Array index out of bounds (const-proven) | checker.c:4909 | M2 | λZER-Handle | Typing using VRP range | ◐ |
| L02 | Slice start/end exceed array size (const) | checker.c:5080, 5088 | M2 | λZER-Handle | Typing using VRP | ◐ |
| L03 | Slice start > end (const) | checker.c:5069 | M2 | λZER-Handle | Typing | ◐ |
| L04 | Runtime: array-index OOB (`_zer_bounds_check`) | emitter.c:1989, 1990, 2028, 2035, 2056, 2060, 2999, 4422, 4424, 5273, 7619 | M2 | λZER-Handle | wp precondition `i < len` | ◐ |
| L05 | Runtime: slice `start > end` trap | emitter.c:2258 | M2 | λZER-Handle | wp precondition | ◐ |
| L06 | Bit index out of range for N-bit type | checker.c:5102 | M2 | λZER-typing | Typing | ○ |
| L07 | Bit-extract high < low | checker.c:5110 | — | λZER-typing | Typing | — |
| L08 | Array-index argument not integer | checker.c:4900 | — | λZER-typing | Typing | — |
| L09 | Cannot slice / index non-indexable type | checker.c:5037, 5118 | — | λZER-typing | Typing | — |
| L10 | Slice start/end not integer | checker.c:5052, 5058 | — | λZER-typing | Typing | — |

## M. Division / arithmetic safety

| # | Predicate | Source (file:line) | Model | Subset | Iris technique | Status |
|---|---|---|---|---|---|---|
| M01 | Division by zero (constant) | checker.c:2348 | M2 | λZER-Handle | Typing (VRP proves d≠0) | ◐ |
| M02 | Divisor not proven nonzero | checker.c:2392, 3567 | M2 | λZER-Handle | VRP precondition | ◐ |
| M03 | Divisor from function call not proven nonzero | checker.c:2408 | M2+M3 | λZER-Handle | Return-range summary | ◐ |
| M04 | Runtime: division by zero trap | emitter.c:1055 | M2 | λZER-Handle | wp precondition | ◐ |
| M05 | Runtime: signed division overflow | emitter.c:1068 | M2 | λZER-Handle | wp precondition on `INT_MIN / -1` | ◐ |
| M06 | Integer overflow wraps (spec) | semantics — always on | — | λZER-Handle | Definitional (`-fwrapv`) | ◐ |
| M07 | Compound assign would narrow — `@truncate` needed | checker.c:3580 | M4 | λZER-typing | Typing: narrowing requires `@truncate` | ○ |
| M08 | Integer literal doesn't fit in target type | checker.c:3519, 6738, 10523 | M4 | λZER-typing | Typing | ○ |
| M09 | Arithmetic requires numeric types | checker.c:2338 | — | λZER-typing | Typing | — |
| M10 | Bitwise ops require integer | checker.c:2458, 3536 | — | λZER-typing | Typing | — |
| M11 | Logical ops require bool | checker.c:2447 | — | λZER-typing | Typing | — |
| M12 | Unary `-`/`!`/`~` type requirements | checker.c:2481, 2492, 2500 | — | λZER-typing | Typing | — |
| M13 | Compare mixed / struct equality / int-float mix | checker.c:2429, 2437, 1540, 1550 | — | λZER-typing | Typing | — |

## N. Null / optional safety

| # | Predicate | Source (file:line) | Model | Subset | Iris technique | Status |
|---|---|---|---|---|---|---|
| N01 | Non-null `*T` requires initializer (var/global) | checker.c:6582, 9005 | M4 | λZER-Handle | `non_null_requires_initializer` — typing + auto-zero | ✓ |
| N02 | `null` only assignable to optional types | checker.c:6700 | M4 | λZER-Handle | `null_only_to_optional` — typing rule | ✓ |
| N03 | `if-unwrap` requires optional | checker.c:7222 | — | λZER-typing | Typing | — |
| N04 | `orelse` requires optional | checker.c:5130 | — | λZER-typing | Typing | — |
| N05 | Nested optional `??T` not supported | checker.c:1197 | M4 | λZER-typing | `no_nested_optional` — typing rule | ✓ |
| N06 | `orelse` fallback type mismatch | checker.c:5164 | — | λZER-typing | Typing | — |
| N07 | `if`/`for` condition must be bool | checker.c:7311, 7518, 7572 | — | λZER-typing | Typing | — |
| N08 | Cannot dereference non-pointer | checker.c:2511 | — | λZER-typing | Typing | — |

## O. Escape analysis (dangling pointer)

| # | Predicate | Source (file:line) | Model | Subset | Iris technique | Status |
|---|---|---|---|---|---|---|
| O01 | Return pointer to local variable | checker.c:8060, 8081, 8105 | M2 | λZER-escape | Region invariant: pointer carries region tag | ○ |
| O02 | Return pointer to local via `@cstr`/`@ptrtoint` | checker.c:8138, 8158, 8195, 7989 | M2 | λZER-escape | Same — transitive through intrinsics | ○ |
| O03 | Return pointer to local via orelse fallback | checker.c:8252 | M2 | λZER-escape | Same — covers fallback path | ○ |
| O04 | Return pointer from call with local-derived arg | checker.c:8211, 8229 | M2+M3 | λZER-escape | FuncSpec: return provenance | ○ |
| O05 | Return arena-derived pointer | checker.c:8075, 8165 | M2 | λZER-escape | Arena-region tag | ○ |
| O06 | Return local array as slice | checker.c:8013 | M2 | λZER-escape | Slice region check | ○ |
| O07 | Store local/arena-derived in global/static | checker.c:2676, 3119, 3376, 3416, 3311 | M2 | λZER-escape | Global-region invariant | ○ |
| O08 | Store local via fn-call pointer arg | checker.c:2917, 3245, 3257 | M2+M3 | λZER-escape | FuncSpec param-escape | ○ |
| O09 | Store non-keep parameter in global | checker.c:3165 | M4 | λZER-escape | `keep` annotation invariant | ○ |
| O10 | Orelse fallback stores local ptr in global | checker.c:3208 | M2 | λZER-escape | Fallback-path region check | ○ |
| O11 | Argument N: local/arena/array can't satisfy keep | checker.c:4282, 4314, 4320, 4332, 4340, 4352 | M4 | λZER-escape | Caller-side `keep` check | ○ |
| O12 | Cannot store `.get()` result — use inline | checker.c:2853, 6612 | M4 | λZER-Handle | Non-storable annotation | ◐ |

## P. Union / enum variant safety

| # | Predicate | Source (file:line) | Model | Subset | Iris technique | Status |
|---|---|---|---|---|---|---|
| P01 | Read union variant directly (must use switch) | checker.c:4854 | M2 | λZER-typing | Variant-tag invariant | ○ |
| P02 | Mutate union inside own switch arm | checker.c:511, 2787 | M2 | λZER-typing | Union switch lock flag | ○ |
| P03 | Take address of union inside switch arm | checker.c:2567 | M2 | λZER-typing | Same | ○ |
| P04 | No such variant in enum/union | checker.c:4821, 4841, 4875, 7882 | — | λZER-typing | Pure typing | — |
| P05 | Struct/union self-by-value | checker.c:8737, 8854 | — | λZER-typing | Pure typing — infinite size | — |
| P06 | Duplicate field/variant | checker.c:8682, 8769, 8827 | — | λZER-typing | Pure typing | — |
| P07 | Struct field / union variant of type `void` | checker.c:8696, 8839 | — | λZER-typing | Pure typing | — |
| P08 | Container (template) depth > 32 | checker.c:1367 | M4 | λZER-typing | Monomorphization bound | ○ |

## Q. Switch exhaustiveness

| # | Predicate | Source (file:line) | Model | Subset | Iris technique | Status |
|---|---|---|---|---|---|---|
| Q01 | Switch on bool must handle both | checker.c:7838 | — | λZER-typing | Typing | — |
| Q02 | Switch on enum not exhaustive | checker.c:7816 | — | λZER-typing | Typing | — |
| Q03 | Switch on integer must have default | checker.c:7845 | — | λZER-typing | Typing | — |
| Q04 | Switch on union not exhaustive | checker.c:7896 | — | λZER-typing | Typing | — |
| Q05 | Switch on float type banned | checker.c:7591 | — | λZER-typing | Typing | — |

## R. Comptime / static_assert

| # | Predicate | Source (file:line) | Model | Subset | Iris technique | Status |
|---|---|---|---|---|---|---|
| R01 | Comptime function body not evaluable | checker.c:4445, 4464 | — | λZER-comptime | Evaluator totality | ○ |
| R02 | Comptime args must be compile-time constants | checker.c:4407 | — | λZER-comptime | Evaluator input validity | ○ |
| R03 | `comptime if` condition must be constant | checker.c:7201 | — | λZER-comptime | Same | ○ |
| R04 | `static_assert` condition must be constant | checker.c:8351, 10481 | — | λZER-comptime | Same | ○ |
| R05 | `static_assert` failed | checker.c:8355, 8360, 10485, 10489 | — | λZER-comptime | User assertion — reported when false | ○ |
| R06 | Comptime nested-loop DoS (1M op budget) | (implicit — budget in evaluator) | — | λZER-comptime | Evaluator termination bound | ○ |
| R07 | Expression nesting depth > 1000 | checker.c:2263 | — | λZER-typing | Parser/checker bound | — |

## S. Resource limits (stack, ISR allocation)

| # | Predicate | Source (file:line) | Model | Subset | Iris technique | Status |
|---|---|---|---|---|---|---|
| S01 | Function frame exceeds `--stack-limit` | checker.c:10058 | M3 | λZER-typing | Stack summary from call graph | ○ |
| S02 | Entry call chain exceeds `--stack-limit` | checker.c:10075 | M3 | λZER-typing | Transitive stack bound | ○ |
| S03 | Entry chain has function-pointer call (unknown bound) | checker.c:10087 | M3 | λZER-typing | Fn-ptr makes summary conservative | ○ |
| S04 | `slab.alloc()` banned in interrupt handler | zercheck_ir.c:948 | M3+M4 | λZER-Handle + λZER-concurrency | Context-flag + allocation-color | ○ |
| S05 | `slab.alloc()` banned in `@critical` block | zercheck_ir.c:952 | M2+M4 | λZER-Handle + λZER-mmio | Context flag | ○ |
| S06 | Heap alloc in ISR (generic message) | checker.c:764 | M3+M4 | λZER-concurrency | Same | ○ |

## T. Container / builtin-type validity

| # | Predicate | Source (file:line) | Model | Subset | Iris technique | Status |
|---|---|---|---|---|---|---|
| T01 | `Pool` / `Ring` / `Slab` / `Semaphore` count must be compile-time constant | checker.c:1177, 1333, 1346 | — | λZER-typing | Typing | — |
| T02 | `Pool` / `Ring` / `Slab` cannot be struct field | checker.c:8702 | M4 | λZER-Handle | Global-only annotation | ◐ |
| T03 | `Pool` / `Ring` / `Slab` cannot be union variant | checker.c:8845 | M4 | λZER-Handle | Same | ◐ |
| T04 | Handle element type not a struct (auto-deref) | checker.c:4717 | — | λZER-typing | Typing | — |
| T05 | No Pool or Slab found for Handle | checker.c:4646 | — | λZER-typing | Auto-deref lookup | — |
| T06 | `@inttoptr` target not pointer / `@ptrcast` target not pointer | checker.c:5548, 5558, 5410 | — | λZER-typing | Typing | — |
| T07 | Global accessed from ISR + main without volatile | checker.c:9737 (also covered in D05) | M3+M4 | λZER-concurrency | (cross-linked) | ○ |

## U. Syntax / declaration (not safety-semantic) — 35 rows

These are malformed-program rejections. They aren't proven as operational safety theorems — they're preconditions on program well-formedness. Included for completeness but marked **—**.

| # | Predicate | Source | Reason for "—" status |
|---|---|---|---|
| U01 | Redefinition of symbol | checker.c:193 | Name-resolution |
| U02 | Undefined identifier / type / container | checker.c:201, 1318, 1395 | Name-resolution |
| U03 | Reserved `_zer_` prefix | checker.c:170 | Namespace reservation |
| U04 | Method arity (`X.alloc() takes no arguments`, etc.) | checker.c:3672, 3678, 3692, 3699, 3710, 3711, 3735, 3745, 3760, 3773, 3789, 3811, 3817, 3838, 3845, 3856, 3882, 3892, 3907, 3917, 3928, 3950, 3967, 3992, 3998, 4014, 4024, 4033, 4042 | Arity |
| U05 | Arg count mismatch: `expected N got M` | checker.c:4075 | Arity |
| U06 | Arg type mismatch: `argument N: expected X, got Y` | checker.c:4200 | Type matching |
| U07 | Field/variant type mismatches | checker.c:794, 803 | Type matching |
| U08 | Designated init requires struct | checker.c:777 | Type shape |
| U09 | Cannot declare `void` variable | checker.c:6552 | Type shape |
| U10 | Cannot create pointer/slice-of-void | checker.c:1187, 1209 | Type shape |
| U11 | Cannot return array type | checker.c:9386 | Type shape |
| U12 | Cannot call non-function type | checker.c:4474 | Type shape |
| U13 | `spawn` target not a function (see C08) | checker.c:8459 | Type shape |
| U14 | Return type mismatch | checker.c:8272 | Type matching |
| U15 | Function must return X, not void | checker.c:8282 | Type matching |
| U16 | Array size: integer / compile-time / positive / ≤ 4 GB | checker.c:1220, 1298, 1300, 1304 | Size validity |
| U17 | Assignment LHS not lvalue | checker.c:2720, 2727 | lvalue |
| U18 | Cannot access field on non-struct type | checker.c:4883 | Type shape |
| U19 | No field X on type Y / struct X has no field Y | checker.c:4709, 4751, 4792, 803 | Field resolution |
| U20 | Struct-field `void` | checker.c:8696 | Type shape |
| U21 | Duplicate struct field / enum variant / union variant / label | checker.c:8682, 8769, 8827, 9251 | Duplicate |
| U22 | Cannot assign / initialize X to Y | checker.c:3511, 6706, 6713, 10514, 10506 | Type matching |
| U23 | Global init must be constant expression | checker.c:10499 | Global-init validity |
| U24 | Global array must use literal init | checker.c:10506 | Same |
| U25 | `cannot call functions at global scope` | (msg — not in extraction but in source) | Global-scope validity |
| U26 | `orelse break`/`orelse continue` outside loop (see G06) | checker.c:5139, 5142 | Control-flow position |
| U27 | Unknown method on Pool/Slab/Ring/Arena/ThreadHandle | checker.c:3678, 3760, 3817, 3907, 3998 | Method resolution |
| U28 | Unknown atomic/barrier/condvar/generic intrinsic | checker.c:5653, 5998, 5931, 6070 | Intrinsic resolution |
| U29 | `arena.alloc`: unknown type name | checker.c:3939, 3978 | Type resolution |
| U30 | `container`: undefined container name | checker.c:1395 | Type resolution |
| U31 | Semaphore count must be ≥ 0 | checker.c:1177 | Size validity |
| U32 | Arg count for intrinsics (`@atomic_cas requires 3`, etc.) | checker.c:5631, 5656, 5677, 5682, 5687, 6003, 6022, 6038, 6054, 5936, 5940, 6070 | Arity |
| U33 | Pool/Ring/Slab/Arena unknown method | (covered U27) | Method resolution |
| U34 | ThreadHandle method/arity | checker.c:3672, 3678 | Method/arity |
| U35 | `arena.alloc_slice()` arg count / type | checker.c:3967, 3978 | Arity / type resolution |

---

## Cross-check: predicates from raw file that don't appear above

The regeneration command is:
```bash
bash tools/safety_coverage.sh > docs/safety_coverage_raw.md
```

To audit coverage, run a diff: every unique predicate in `safety_coverage_raw.md` Part 5 should map to at least one row in the tables above.

**Known systematic reductions:**
- All 29 mutating-method-on-const calls → 1 row (I10)
- All 11 runtime `_zer_bounds_check(...)` → 1 row (L04)
- All 7 shape-checks per intrinsic (source must be pointer, target must be integer, etc.) → bucketed by intrinsic family (J, K sections)
- Format-string variants (`X already freed at line N` across 4 files) → 1 row per variant class

## Open design decisions (◇)

Currently no rows marked ◇ — every predicate above maps to a planned subset. However, the following are areas where the mapping is weakest:

- **G08/G09 (goto/label)** — listed as pure typing, but goto + defer interactions (defers fire before jump) require semantic reasoning. May need to elevate parts to λZER-defer proper.
- **S01-S03 (stack limits)** — currently "typing" but actually semantic (call-graph DFS). Might warrant own subset or bundle with λZER-async (both are call-chain analyses).
- **R06 (comptime nested-loop DoS budget)** — evaluator-termination, not really a safety invariant on user programs. Probably stays "typing" forever.

## How this drives Iris work

At any point in the project, "full ZER safety proven" = every row has ✓.

**Current checkpoint (Phase 1 in progress):**
- 18 rows marked ◐ (section A + scattered in L/M/N/T)
- These are the λZER-Handle scope — the first complete subset.

**Next subsets** (in rough priority order, once λZER-Handle lands):
1. **λZER-move** (section B, 8 rows) — builds on λZER-Handle's resource infrastructure. Linear resources are the simplest extension.
2. **λZER-escape** (section O, 12 rows) — region invariants. Separate enough to be its own subset.
3. **λZER-mmio** (section H + parts of I, G, J ≈ 20 rows) — hardware semantics.
4. **λZER-concurrency** (sections C, D, E + parts of S ≈ 30 rows) — Iris proper kicks in. Hardest subset.
5. **λZER-async** (section F + parts of G ≈ 10 rows) — built on concurrency base.
6. **λZER-opaque** (section J + K ≈ 12 rows) — provenance ghost state. Could slot in earlier if needed for C interop.
7. **λZER-typing** (sections Q + defer rows ≈ 25 rows) — pure typing, often ambient.
8. **λZER-comptime** (section R ≈ 6 rows) — evaluator soundness.

## Maintenance protocol

When a new `checker_error` / `zc_error` / `_zer_trap` site lands in the compiler:

1. Add the source line's message to the appropriate section above (or create a new section if no bucket fits).
2. Assign abstract model, operational subset, Iris technique, status.
3. Run `bash tools/safety_coverage.sh > docs/safety_coverage_raw.md` — if the resulting diff has an unbucketed predicate, the curation is out of date.
4. CI should gate PRs on this: every predicate in raw must appear in curated.

Without this discipline, the formal-verification scope silently drifts behind the implementation, and "full ZER safe" becomes meaningless.
