# Phase 1 Predicate Catalog — Exhaustive Reference

**Purpose:** the single source of truth for every predicate in Phase 1 scope. Covers: extracted, to-extract, deferred, code-driven (no oracle), and runtime trap predicates. Each predicate includes signature, typing.v line reference, oracle theorem names, target file, call sites in checker.c/zercheck*.c, C implementation template, Coq VST spec template, and call-site code showing what must be delegated.

**Methodology:** derived 2026-04-22 by reading full content of `checker.c` (11,196 lines), `zercheck_ir.c` (3,044), `types.c` (542), all 14 `src/safety/*.h`, `typing.v` (1,334 — the oracle), all existing `src/safety/*.c` extractions, and `docs/safety_list.md` (203 curated rows).

**Counts (definitive):**
- Current extracted: **48**
- Phase 1 strict target: **71** (operational + strong-oracle, exclude concurrency schematic)
- Phase 1 full target: **85** (+14 concurrency with schematic oracle)
- Phase 4 deferred: **+7** (list/struct predicates — need VST separation logic)
- Code-driven (no typing.v oracle, but extractable): **+15** (bonus category)
- Runtime traps (mirror compile predicates, Category F): **16** (already covered by compile-time predicates)

---

## Legend

- **⚫ EXTRACTED** — exists in `src/safety/`, linked into `zerc`, VST-verified via `make check-vst`
- **🟢 READY** — typing.v has predicate + real Coq theorem; extract now with strong backing
- **🟡 SCHEMATIC** — typing.v has predicate + theorem, but no operational subset yet (Phase 7 upgrades)
- **🔴 DEFER** — requires list/record — Phase 4 (verified state APIs with separation logic)
- **⚪ CODE-DRIVEN** — compiler enforces, no typing.v oracle yet; extract with weak backing

**Oracle strength tiers:**
1. **Operational** (strongest) — own subset (λZER-Handle, λZER-move, λZER-opaque, λZER-escape, λZER-mmio) with step rules + adequacy
2. **Predicate-depth** — typing.v has `Definition X : bool` + `Theorem X_ok` / `_rejected` with real proof body
3. **Schematic** — typing.v has predicate + theorem body, but no operational semantics grounding it
4. **Code-driven** — no oracle, Coq spec written from C code; VST proves code matches itself (tautology)

Strict Phase 1 = tiers 1 + 2. Full Phase 1 adds tier 3. Phase 7 upgrades remaining tier 3 to tier 1.

---

## Operational subset catalog

Each λZER-* subset lives in `proofs/operational/<name>/` and provides the **strongest** oracle type — step rules + adequacy + resource algebra.

| Subset | What it proves | Used by predicates |
|---|---|---|
| **λZER-Handle** | Handle lifecycle (alloc→alive, free→freed, use after free = stuck). Resource `alive_handle`. Adequacy: no leaked `alive_handle` at program end. | handle_state.c (4) |
| **λZER-move** | Move struct ownership transfer. Resource `alive_move`. `step_spec_consume` proves transferred cannot be used. | move_rules.c (2) |
| **λZER-opaque** | `*opaque` provenance. Resource `typed_ptr γ id t`. `typed_ptr_agree` proves tag mismatch = stuck. | provenance_rules.c (3) |
| **λZER-escape** | Region invariants RegLocal/RegArena/RegStatic. `step_spec_store_global_static` proves only RegStatic can escape. | escape_rules.c (3) |
| **λZER-mmio** | MMIO range + alignment. Step rule `step_inttoptr_ok` requires BOTH range AND alignment. | mmio_rules.c (2) |
| **λZER-typing** | Pure typing predicates in `typing.v` — no ghost state, just bool functions + decidability. | ~22 predicates sourced from typing.v sections Q/I/N/T/P/G/K/L/M/R/S/C/D/E/F |

**Subsets NOT yet built (Phase 7 work):**
- **λZER-concurrency** — would deepen C/D/E schematic rows to operational. Requires Iris invariants for shared structs + atomic semantics. ~150 hrs.
- **λZER-async** — would deepen F rows. Requires coroutine state machine + suspend semantics. ~80 hrs.
- **λZER-comptime** — evaluator totality for R. ~40 hrs.

Current Phase 1 guarantee depends on tier:
- Tier 1 predicates (operational): 19/48 = 40% — strongest
- Tier 2 predicates (typing.v real theorem): 23/48 = 48% — strong
- Tier 3 (operational subset TODO): 14 concurrency — schematic only until Phase 7

---

## 1. Currently extracted (48)

Source: 14 files in `src/safety/`, total ~400 lines of pure predicate C code.

### 1.A Handle state — `src/safety/handle_state.c` (4)

Oracle: λZER-Handle operational.

```c
/* Constants (MUST match zercheck.h HS_* and zercheck_ir.c IR_HS_*) */
#define ZER_HS_UNKNOWN      0
#define ZER_HS_ALIVE        1
#define ZER_HS_FREED        2
#define ZER_HS_MAYBE_FREED  3
#define ZER_HS_TRANSFERRED  4

int zer_handle_state_is_invalid(int state);      /* 1 iff FREED|MAYBE_FREED|TRANSFERRED */
int zer_handle_state_is_alive(int state);        /* 1 iff state == ZER_HS_ALIVE */
int zer_handle_state_is_freed(int state);        /* 1 iff state == ZER_HS_FREED */
int zer_handle_state_is_transferred(int state);  /* 1 iff state == ZER_HS_TRANSFERRED */
```

**Call sites:**
- zercheck.c `is_handle_invalid` (dispatches to `zer_handle_state_is_invalid`)
- zercheck.c `is_handle_consumed` (same — different semantic label)
- zercheck_ir.c:251 `ir_is_invalid`
- Scope-exit leak checks at zercheck.c:2694, zercheck_ir.c:2923

### 1.B Range checks — `src/safety/range_checks.c` (3)

| Predicate | Oracle (typing.v line) | Call sites |
|---|---|---|
| `zer_count_is_positive(int n)` | T01 `pool_count_valid` (238) | checker.c:1353, 1366 (Pool/Ring count validation) |
| `zer_index_in_bounds(int size, int idx)` | L01 `array_index_valid` (542) | emitter.c runtime bounds check emission |
| `zer_variant_in_range(int n_variants, int variant_idx)` | P04 `variant_index_valid` (257) | checker.c union variant dispatch |

### 1.C Type kind classification — `src/safety/type_kind.c` (7)

No typing.v oracle (structural classification from `TypeKind` enum in types.h).

Constants `ZER_TK_VOID=0` through `ZER_TK_DISTINCT=29` — MUST match `TypeKind` enum order.

- `zer_type_kind_is_integer(int kind)` — u8/u16/u32/u64/usize/i8/i16/i32/i64 + enum
- `zer_type_kind_is_signed(int kind)` — i8/i16/i32/i64 + enum
- `zer_type_kind_is_unsigned(int kind)` — u8/u16/u32/u64/usize
- `zer_type_kind_is_float(int kind)` — f32, f64
- `zer_type_kind_is_numeric(int kind)` — integer or float
- `zer_type_kind_is_pointer(int kind)` — POINTER or OPAQUE
- `zer_type_kind_has_fields(int kind)` — STRUCT or UNION

Call sites: `types.c:type_is_*` (after `type_unwrap_distinct`). See types.c:168-193.

### 1.D Coerce rules — `src/safety/coerce_rules.c` (5)

Oracle: typing.v Section I `cast_safe` + `qual_le` (125).

```c
int zer_coerce_int_widening_allowed(int from_signed, int to_signed, int from_width, int to_width);
int zer_coerce_usize_same_width_allowed(int from_is_usize, int to_is_usize, int from_signed, int to_signed);
int zer_coerce_float_widening_allowed(int from_is_f32, int to_is_f64);
int zer_coerce_preserves_volatile(int from_volatile, int to_volatile);
int zer_coerce_preserves_const(int from_const, int to_const);
```

Call site: `types.c:326` (`can_implicit_coerce`).

### 1.E Context bans — `src/safety/context_bans.c` (6)

Oracle: typing.v Section G (`return_safe` 417, `break_safe` 425, `defer_safe` 446, `asm_safe` 459) + 2 derived (continue/goto — same shape as break/return).

- `zer_return_allowed_in_context(defer_depth, critical_depth)` — G01, G03
- `zer_break_allowed_in_context(defer_depth, critical_depth, in_loop)` — G02, G03, G05
- `zer_continue_allowed_in_context(defer_depth, critical_depth, in_loop)` — derived from break
- `zer_goto_allowed_in_context(defer_depth, critical_depth)` — derived from return
- `zer_defer_allowed_in_context(defer_depth)` — G04 (nested defer)
- `zer_asm_allowed_in_context(in_naked)` — G10

Call sites:
- NODE_RETURN: checker.c:7987
- NODE_BREAK: checker.c:8377
- NODE_CONTINUE: checker.c:8409
- NODE_GOTO: checker.c:8393
- NODE_DEFER: checker.c:8429
- NODE_ASM: checker.c:8493

### 1.F Escape rules — `src/safety/escape_rules.c` (3)

Oracle: λZER-escape operational.

```c
#define ZER_REGION_STATIC  0
#define ZER_REGION_LOCAL   1
#define ZER_REGION_ARENA   2

int zer_region_can_escape(int region);    /* 1 iff STATIC */
int zer_region_is_local(int region);      /* 1 iff LOCAL */
int zer_region_is_arena(int region);      /* 1 iff ARENA */
```

Call site: checker.c:8156 (NODE_RETURN escape check), via helper `zer_sym_region_tag(is_local, is_arena)` at checker.c:16.

### 1.G Provenance rules — `src/safety/provenance_rules.c` (3)

Oracle: λZER-opaque `typed_ptr_agree` lemma.

- `zer_provenance_check_required(src_prov_unknown, dst_is_opaque)` — 1 iff known source AND concrete dest
- `zer_provenance_type_ids_compatible(actual_id, expected_id)` — 1 iff same id OR one is 0 (unknown, C interop)
- `zer_provenance_opaque_upcast_allowed(void)` — always 1 (step_spec_opaque_cast)

Call site: checker.c:5519 (`@ptrcast` provenance check).

### 1.H MMIO rules — `src/safety/mmio_rules.c` (2)

Oracle: λZER-mmio `step_inttoptr_ok`.

- `zer_mmio_addr_in_range(addr, start, end)` — iteratively called over declared ranges
- `zer_mmio_inttoptr_allowed(in_any_range, aligned)` — 1 iff BOTH gates pass

Call site: checker.c:5661 (`@inttoptr` constant address check).

### 1.I Optional rules — `src/safety/optional_rules.c` (2)

Oracle: typing.v Section N (`permits_null` 191, `has_nested_optional` 218).

- `zer_type_permits_null(type_kind)` — N01/N02/N03 reduce to `kind == TYPE_OPTIONAL` (ZER_TK_OPTIONAL=14)
- `zer_type_is_nested_optional(outer_kind, inner_kind)` — N05: both OPTIONAL

Call site: checker.c:1217 (`resolve_type_inner` TYNODE_OPTIONAL case).

### 1.J Move rules — `src/safety/move_rules.c` (2)

Oracle: λZER-move `alive_move` resource, B01/B02 theorems.

- `zer_type_kind_is_move_struct(type_kind, is_move_flag)` — kind==STRUCT AND is_move set
- `zer_move_should_track(is_move_struct_direct, contains_move_field)` — direct OR contains move field/variant

Call sites: zercheck.c `is_move_struct_type`, zercheck_ir.c:277 `ir_is_move_struct_type`.

### 1.K Atomic rules — `src/safety/atomic_rules.c` (2)

Oracle: typing.v Section E (E01 `atomic_width_valid` 1126, E02 `atomic_arg_valid` 1146).

- `zer_atomic_width_valid(bytes)` — 1 iff 1, 2, 4, or 8
- `zer_atomic_arg_is_ptr_to_int(is_ptr_to_int)` — pass-through

Call site: checker.c:5772 (`@atomic_*` dispatch).

### 1.L Container rules — `src/safety/container_rules.c` (3)

Oracle: typing.v T/P/K.

- `zer_container_depth_valid(depth)` — P08 (381): `depth <= 32`
- `zer_field_type_valid(is_void)` — P07 (366): `!is_void`
- `zer_type_has_size(is_void)` — K04 (522): `!is_void`

Call sites:
- `zer_container_depth_valid`: checker.c:1386 (TYNODE_CONTAINER monomorphization depth)
- `zer_field_type_valid`: checker.c:8792 (struct field void), 8935 (union variant void)
- `zer_type_has_size`: checker.c:5409 (`@size(T)` for void/opaque rejection)

### 1.M Misc rules — `src/safety/misc_rules.c` (2)

Oracle: typing.v Section Q.

- `zer_int_switch_has_default(has_default_flag)` — Q03 (106)
- `zer_bool_switch_covers_both(has_default, has_true, has_false)` — Q01 (29)

Call sites: checker.c:7920, 7913 (switch exhaustiveness).

### 1.N ISR rules — `src/safety/isr_rules.c` (4)

Oracle: typing.v S04/S05/C03/C04 + CLAUDE.md Ban Decision Framework.

- `zer_alloc_allowed_in_isr(in_interrupt)` — 1 iff `in_interrupt == 0`
- `zer_alloc_allowed_in_critical(critical_depth)` — 1 iff `critical_depth <= 0`
- `zer_spawn_allowed_in_isr(in_interrupt)`
- `zer_spawn_allowed_in_critical(critical_depth)`

Call sites:
- `zer_alloc_allowed_in_isr`: checker.c:779 `check_isr_ban` helper
- Slab alloc bans: zercheck_ir.c:947/951 (IR_SLAB_ALLOC)
- Spawn bans: checker.c:8626/8632 (direct + via `check_body_effects`), zercheck_ir.c:998/1003

**Gap:** typing.v's `spawn_context_valid` is a 3-arg predicate covering `in_isr`, `in_critical`, AND `in_async`. Current split covers first 2. Either add `zer_spawn_allowed_in_async(in_async)` OR full 3-arg form — listed below under C section.

---

## 2. Phase 1 strict — 23 remaining

All have real Coq theorems in typing.v. Extract for immediate strong VST guarantee.

### 2.M Arithmetic — typing.v Section M (4 predicates)

**Target file:** `src/safety/arith_rules.c` (NEW)

#### M1. `zer_div_valid(int divisor)` → M01

```c
/* Oracle: typing.v:604 div_valid. M01_const_div_by_zero_rejected. */
int zer_div_valid(int divisor) {
    if (divisor == 0) return 0;
    return 1;
}
```

**Coq VST spec (proofs/vst/verif_arith_rules.v):**
```coq
Definition zer_div_valid_coq (divisor : Z) : Z :=
  if Z.eq_dec divisor 0 then 0 else 1.

Definition zer_div_valid_spec : ident * funspec :=
 DECLARE _zer_div_valid
  WITH divisor : Z
  PRE [ tint ]
    PROP (Int.min_signed <= divisor <= Int.max_signed)
    PARAMS (Vint (Int.repr divisor))
    SEP ()
  POST [ tint ]
    PROP () RETURN (Vint (Int.repr (zer_div_valid_coq divisor))) SEP ().
```

**Current inline check (delegate):**
- checker.c:2367-2369 `if (div_val == 0) { checker_error(...); }` → wrap with `if (zer_div_valid(...) == 0)`
- emitter.c:1055 runtime `_zer_trap("division by zero")` — already emits, no change

#### M2. `zer_divisor_proven_nonzero(int has_proof)` → M02

```c
/* Oracle: typing.v:619 divisor_proven_nonzero. Pass-through predicate —
   caller passes 1 if VRP has proven nonzero, 0 otherwise. */
int zer_divisor_proven_nonzero(int has_proof) {
    if (has_proof == 0) return 0;
    return 1;
}
```

**Call sites:** checker.c:2392, 2408, 3567 (variants of "divisor not proven nonzero" error).

**Note:** trivial but documents the decision point for VST.

#### M3. `zer_narrowing_valid(int src_width, int dst_width, int has_truncate)` → M07

```c
/* Oracle: typing.v:631 narrowing_valid. Valid iff src<=dst OR has_truncate. */
int zer_narrowing_valid(int src_width, int dst_width, int has_truncate) {
    if (src_width <= dst_width) return 1;
    if (has_truncate != 0) return 1;
    return 0;
}
```

**Call site:** checker.c:3620 compound assignment narrowing check.

#### M4. `zer_literal_fits(int width, uint64_t lit)` → M08

```c
/* Oracle: typing.v:650 literal_fits. Valid iff lit < 2^width. */
int zer_literal_fits(int width, uint64_t lit) {
    if (width >= 64) return 1;  /* always fits in 64-bit */
    if (lit < ((uint64_t)1 << width)) return 1;
    return 0;
}
```

**Call sites:** checker.c:3519, 6738, 10523 (integer literal range check via `is_literal_compatible`).

---

### 2.L Bounds extras — typing.v Section L (2 predicates)

**Target file:** extend `src/safety/range_checks.c`

#### L1. `zer_slice_bounds_valid(int size, int start, int end)` → L02/L03

```c
/* Oracle: typing.v:554 slice_bounds_valid.
   Valid iff start <= end AND end <= size. */
int zer_slice_bounds_valid(int size, int start, int end) {
    if (start > end) return 0;
    if (end > size) return 0;
    return 1;
}
```

**Call sites:** checker.c:5080, 5088 (slice bounds), 5069 (start > end).

#### L2. `zer_bit_index_valid(int width, int idx)` → L06

```c
/* Oracle: typing.v:584 bit_index_valid. Valid iff idx < width. */
int zer_bit_index_valid(int width, int idx) {
    if (idx < 0) return 0;
    if (idx >= width) return 0;
    return 1;
}
```

**Call sites:** checker.c:5158 (bit index OOB), 5164 (hi < lo for extract).

---

### 2.R Comptime — typing.v Section R (4 predicates)

**Target file:** `src/safety/comptime_rules.c` (NEW)

#### R1. `zer_comptime_arg_valid(int is_constant)` → R02

```c
/* Oracle: typing.v:674 comptime_arg_valid.
   Comptime function args must be CTConst, not CTRuntime. */
int zer_comptime_arg_valid(int is_constant) {
    if (is_constant == 0) return 0;
    return 1;
}
```

**Call site:** checker.c:4407 (comptime function call all-const check).

#### R2. `zer_static_assert_holds(int is_constant, int value)` → R04

```c
/* Oracle: typing.v:686 static_assert_holds.
   Valid iff condition is constant AND value is nonzero. */
int zer_static_assert_holds(int is_constant, int value) {
    if (is_constant == 0) return 0;
    if (value == 0) return 0;
    return 1;
}
```

**Call sites:** checker.c:8351 (NODE_STATIC_ASSERT statement form), 10481 (top-level static_assert).

#### R3. `zer_comptime_ops_valid(int ops_count)` → R06

```c
/* Oracle: typing.v:706 comptime_ops_valid. Budget = 1,000,000 operations. */
int zer_comptime_ops_valid(int ops_count) {
    if (ops_count < 1000000) return 1;
    return 0;
}
```

**Call site:** checker.c:1956 `if (++_comptime_ops > 1000000)` inside for loop, also line 1989, 1956 — multiple sites where budget exhausted triggers `CONST_EVAL_FAIL`.

#### R4. `zer_expr_nesting_valid(int depth)` → R07

```c
/* Oracle: typing.v:720 expr_nesting_valid. Limit = 1000. */
int zer_expr_nesting_valid(int depth) {
    if (depth < 1000) return 1;
    return 0;
}
```

**Call site:** checker.c:2283 `if (++c->expr_depth > 1000)` in `check_expr`.

---

### 2.S Stack limits — typing.v Section S (1 predicate)

**Target file:** `src/safety/stack_rules.c` (NEW)

#### S1. `zer_stack_frame_valid(int limit, int frame)` → S01/S02

```c
/* Oracle: typing.v:740 stack_frame_valid. Valid iff frame <= limit. */
int zer_stack_frame_valid(int limit, int frame) {
    if (frame <= limit) return 1;
    return 0;
}
```

**Call sites:**
- Per-function frame: checker.c:10154 `if (f->frame_size > c->stack_limit)`
- Call chain: checker.c:10161 `if (!f->is_recursive && max_depth > c->stack_limit)`

Same predicate used for both — delegate both sites.

---

### 2.P Variant safety — typing.v Section P (2 predicates)

**Target file:** `src/safety/variant_rules.c` (NEW)

#### P1. `zer_union_read_mode_safe(int mode)` → P01

```c
/* Oracle: typing.v:277 read_mode_safe.
   0 = DirectRead (unsafe), 1 = SwitchRead (safe). */
int zer_union_read_mode_safe(int mode) {
    if (mode == 1) return 1;  /* SwitchRead */
    return 0;                  /* DirectRead or invalid */
}
```

**Call site:** checker.c:4911 (union field access outside switch).

#### P2. `zer_union_arm_op_safe(int self_access)` → P02

```c
/* Oracle: typing.v:298 arm_safe_op.
   self_access=1 → unsafe (mutating union inside its switch arm).
   Returns 1 iff !self_access. */
int zer_union_arm_op_safe(int self_access) {
    if (self_access != 0) return 0;
    return 1;
}
```

**Call sites:** checker.c:511, 2787, 2567 (union switch mutation ban — via `check_union_switch_mutation` helper at checker.c:491).

---

### 2.T Container position — typing.v Section T (2 more predicates)

**Target file:** extend `src/safety/container_rules.c`

#### T1. `zer_container_position_valid(int decl_position)` → T02/T03

```c
/* Oracle: typing.v:787 container_position_valid.
   0 = DeclGlobal (OK), 1 = DeclField (rejected), 2 = DeclVariant (rejected). */
#define ZER_DP_GLOBAL   0
#define ZER_DP_FIELD    1
#define ZER_DP_VARIANT  2

int zer_container_position_valid(int decl_position) {
    if (decl_position == ZER_DP_GLOBAL) return 1;
    return 0;
}
```

**Call sites:** checker.c:8798 (struct field Pool/Slab/Ring), 8941 (union variant).

#### T2. `zer_handle_element_valid(int element_kind)` → T04

```c
/* Oracle: typing.v:808 handle_element_valid.
   Handle(T) element must be struct. element_kind 0=ElemStruct OK, else rejected. */
int zer_handle_element_valid(int element_kind) {
    if (element_kind == 0) return 1;  /* ElemStruct */
    return 0;
}
```

**Call site:** checker.c:4774 ("Handle element type is not a struct — cannot auto-deref").

---

### 2.K Container source — typing.v Section K (1 more predicate)

**Target file:** extend `src/safety/container_rules.c`

#### K1. `zer_container_source_valid(int type_category)` → K01

```c
/* Oracle: typing.v:490 container_source_valid.
   0 = CatPrim (rejected), 1 = CatPtr (OK), 2 = CatSlice/CatStruct (rejected). */
#define ZER_TCAT_PRIM    0
#define ZER_TCAT_PTR     1
#define ZER_TCAT_SLICE   2
#define ZER_TCAT_STRUCT  3

int zer_container_source_valid(int type_category) {
    if (type_category == ZER_TCAT_PTR) return 1;
    return 0;
}
```

**Call site:** checker.c:5860 (`@container` source must be pointer).

---

### 2.J-ext Cast intrinsics — typing.v Section J-extended (7 predicates)

**Target file:** `src/safety/cast_rules.c` (NEW) or extend `coerce_rules.c`

#### J1. `zer_conversion_safe(int kind)` → J02/J03

```c
/* Oracle: typing.v:833 conversion_safe.
   0 = ConvExplicitIntToPtr (OK), 1 = ConvExplicitPtrToInt (OK), 2 = ConvCStyleCast (rejected). */
#define ZER_CONV_INT_TO_PTR   0
#define ZER_CONV_PTR_TO_INT   1
#define ZER_CONV_CSTYLE       2

int zer_conversion_safe(int kind) {
    if (kind == ZER_CONV_CSTYLE) return 0;
    return 1;
}
```

**Call sites:** checker.c:5346 ("cannot cast integer to pointer — use @inttoptr"), 5352 ("cannot cast pointer to integer — use @ptrtoint").

#### J2. `zer_bitcast_width_valid(int src_width, int dst_width)` → J05

```c
/* Oracle: typing.v:852 bitcast_valid. Widths must match. */
int zer_bitcast_width_valid(int src_width, int dst_width) {
    if (src_width == dst_width) return 1;
    return 0;
}
```

**Call site:** checker.c:5558 (`@bitcast` width check).

#### J3. `zer_bitcast_operand_valid(int is_primitive)` → J06

```c
/* Oracle: typing.v:864 bitcast_operand_valid. Pass-through. */
int zer_bitcast_operand_valid(int is_primitive) {
    if (is_primitive != 0) return 1;
    return 0;
}
```

**Call site:** checker.c:5541 (@bitcast operand check — currently implicit in width check).

#### J4. `zer_cast_distinct_valid(int src_is_distinct, int dst_is_distinct)` → J07

```c
/* Oracle: typing.v:878 cast_distinct_valid.
   @cast requires at least one end to be distinct typedef. */
int zer_cast_distinct_valid(int src_is_distinct, int dst_is_distinct) {
    if (src_is_distinct != 0) return 1;
    if (dst_is_distinct != 0) return 1;
    return 0;
}
```

**Call site:** checker.c:5954 (`@cast` requires at least one distinct).

#### J5. `zer_saturate_operand_valid(int is_numeric)` → J08

```c
/* Oracle: typing.v:892 saturate_operand_valid. */
int zer_saturate_operand_valid(int is_numeric) {
    if (is_numeric != 0) return 1;
    return 0;
}
```

**Call sites:** checker.c:5576 (`@truncate`), 5590 (`@saturate`).

#### J6. `zer_ptrtoint_source_valid(int is_pointer)` → J09

```c
/* Oracle: typing.v:904 ptrtoint_source_valid. */
int zer_ptrtoint_source_valid(int is_pointer) {
    if (is_pointer != 0) return 1;
    return 0;
}
```

**Call site:** checker.c:5684 (`@ptrtoint` source check).

#### J7. `zer_cast_types_compatible(int src_tag, int dst_tag)` → J10

```c
/* Oracle: typing.v:916 cast_types_compatible.
   Simple equality check — used for generic invalid-cast rejection. */
int zer_cast_types_compatible(int src_tag, int dst_tag) {
    if (src_tag == dst_tag) return 1;
    return 0;
}
```

**Call site:** checker.c:5359 ("invalid cast from X to Y" catch-all).

**Subtotal strict remaining: 23. Hits 48 + 23 = 71 ← Phase 1 strict complete.**

---

## 3. Phase 1 full — 14 concurrency predicates (schematic oracle)

typing.v has real Coq theorems. Operational Iris subset missing. Extract for predicate-depth guarantee now; Phase 7 upgrades to operational.

### 3.C Thread lifecycle — typing.v Section C (6 predicates)

**Target file:** `src/safety/thread_rules.c` (NEW)

#### C1. `zer_thread_op_valid(int state, int joining)` → C01/C02

```c
/* Oracle: typing.v:941 thread_op_valid.
   state 0=Alive, 1=Joined. joining=1 means attempting join.
   Alive + join = OK (first join). Joined + join = double join rejected. */
#define ZER_THR_ALIVE   0
#define ZER_THR_JOINED  1

int zer_thread_op_valid(int state, int joining) {
    if (joining == 0) return 1;        /* no-op always OK */
    if (state == ZER_THR_ALIVE) return 1;  /* first join */
    return 0;                           /* double join */
}
```

**Call sites:** zercheck.c:1084, zercheck_ir.c:1766 ("ThreadHandle already joined").

#### C2. `zer_thread_cleanup_valid(int state)` → C01 (scope exit)

```c
/* Oracle: typing.v:957. At scope exit, thread must be Joined (not Alive). */
int zer_thread_cleanup_valid(int state) {
    if (state == ZER_THR_JOINED) return 1;
    return 0;
}
```

**Call sites:** zercheck.c:2688, zercheck_ir.c:2910 ("ThreadHandle not joined before function exit").

#### C3. `zer_spawn_context_valid(int in_isr, int in_critical, int in_async)` → C03/C04/C05

```c
/* Oracle: typing.v:970 spawn_context_valid.
   Valid iff NOT in ISR AND NOT in critical AND NOT in async. */
int zer_spawn_context_valid(int in_isr, int in_critical, int in_async) {
    if (in_isr != 0) return 0;
    if (in_critical != 0) return 0;
    if (in_async != 0) return 0;
    return 1;
}
```

**Call sites:** checker.c:8626 (in_critical), 8632 (in_async), zercheck_ir.c:998 (in_interrupt), 1003 (critical_depth > 0).

**Note:** supersedes isr_rules.c `zer_spawn_allowed_in_isr` + `zer_spawn_allowed_in_critical`. Keep the split forms for granular error messages; add this as the combined-form check.

#### C4. `zer_spawn_return_safe(int returns_resource)` → C07

```c
/* Oracle: typing.v:1005. Spawned function cannot return Handle or move struct. */
int zer_spawn_return_safe(int returns_resource) {
    if (returns_resource != 0) return 0;
    return 1;
}
```

**Call site:** checker.c:8572 ("spawn target returns Handle/move struct — resource would leak").

#### C5. `zer_spawn_arg_valid(int is_shared_ptr, int is_value)` → C09

```c
/* Oracle: typing.v:1017. Spawn arg must be shared_ptr OR value (not non-shared ptr). */
int zer_spawn_arg_valid(int is_shared_ptr, int is_value) {
    if (is_shared_ptr != 0) return 1;
    if (is_value != 0) return 1;
    return 0;
}
```

**Call site:** checker.c:8611 ("cannot pass non-shared pointer to spawn").

#### C6. `zer_spawn_arg_is_handle_rejected(int is_handle)` → C10

```c
/* Oracle: typing.v:1033. Handle cannot be spawn arg. Valid iff !is_handle. */
int zer_spawn_arg_is_handle_rejected(int is_handle) {
    if (is_handle != 0) return 0;
    return 1;
}
```

**Call site:** checker.c:8620 ("cannot pass Handle to spawn").

---

### 3.D Shared struct & deadlock — typing.v Section D (4 predicates)

**Target file:** `src/safety/shared_rules.c` (NEW)

#### D1. `zer_address_of_shared_valid(int is_shared_field)` → D01

```c
/* Oracle: typing.v:1049. Cannot take &(shared field) — bypasses lock. */
int zer_address_of_shared_valid(int is_shared_field) {
    if (is_shared_field != 0) return 0;
    return 1;
}
```

**Call site:** checker.c:2598 ("cannot take address of shared struct field").

#### D2. `zer_shared_in_suspend_valid(int accesses_shared, int has_yield)` → D02

```c
/* Oracle: typing.v:1061. Lock cannot be held across yield/await. */
int zer_shared_in_suspend_valid(int accesses_shared, int has_yield) {
    if (accesses_shared != 0 && has_yield != 0) return 0;
    return 1;
}
```

**Call site:** checker.c:4792 ("cannot access shared struct in statement containing yield/await").

#### D3. `zer_volatile_compound_valid(int is_volatile, int is_compound_op)` → D04

```c
/* Oracle: typing.v:1094. Volatile + compound RMW = non-atomic race. */
int zer_volatile_compound_valid(int is_volatile, int is_compound_op) {
    if (is_volatile != 0 && is_compound_op != 0) return 0;
    return 1;
}
```

**Call site:** checker.c (near line 9840) in `check_interrupt_safety`.

#### D4. `zer_isr_main_access_valid(int accessed_in_isr, int accessed_in_main, int is_volatile)` → D05

```c
/* Oracle: typing.v:1106. Globals shared between ISR+main MUST be volatile. */
int zer_isr_main_access_valid(int accessed_in_isr, int accessed_in_main, int is_volatile) {
    if (is_volatile != 0) return 1;
    if (accessed_in_isr == 0 || accessed_in_main == 0) return 1;
    return 0;
}
```

**Call site:** checker.c:9833 (`check_interrupt_safety`).

---

### 3.E Atomic extras — typing.v Section E (3 more)

**Target file:** extend `src/safety/atomic_rules.c`

#### E1. `zer_atomic_on_packed_valid(int is_packed_field)` → E03

```c
/* Oracle: typing.v:1158. Atomic on packed field = misaligned hard fault. */
int zer_atomic_on_packed_valid(int is_packed_field) {
    if (is_packed_field != 0) return 0;
    return 1;
}
```

**Call site:** checker.c:5804 (`@atomic_*` on packed struct field).

#### E2. `zer_condvar_arg_valid(int is_shared_struct)` → E04

```c
/* Oracle: typing.v:1166. @cond_wait/signal first arg must be shared struct. */
int zer_condvar_arg_valid(int is_shared_struct) {
    if (is_shared_struct != 0) return 1;
    return 0;
}
```

**Call site:** checker.c:6043 (`@cond_*` shared arg check).

#### E3. `zer_sync_in_packed_valid(int is_packed_container)` → E08

```c
/* Oracle: typing.v:1178. Semaphore/Barrier/mutex in packed struct = misaligned. */
int zer_sync_in_packed_valid(int is_packed_container) {
    if (is_packed_container != 0) return 0;
    return 1;
}
```

**Call site:** checker.c:8806 ("synchronization primitive cannot be inside packed struct").

---

### 3.F Async — typing.v Section F (1 predicate)

**Target file:** `src/safety/async_rules.c` (NEW) or extend `context_bans.c`

#### F1. `zer_yield_context_valid(int in_async, int in_critical, int in_defer)` → F01/F02/F03/F04

```c
/* Oracle: typing.v:1198 yield_context_valid.
   Valid iff in_async AND !in_critical AND !in_defer. */
int zer_yield_context_valid(int in_async, int in_critical, int in_defer) {
    if (in_async == 0) return 0;
    if (in_critical != 0) return 0;
    if (in_defer != 0) return 0;
    return 1;
}
```

**Call sites:** checker.c:8531 (yield outside async), 8540 (await outside async), + transitive via `check_body_effects` for defer/critical (checker.c:8433/8504).

**Subtotal full remaining: +14. Hits 48 + 23 + 14 = 85 ← Phase 1 full complete.**

---

## 4. Phase 4 deferred — 7 list/struct predicates

Cannot extract as `int → int` — require VST list operations OR Phase 4 generic walker with separation logic.

| typing.v line | Predicate | Problem |
|---|---|---|
| 57 | `enum_switch_exhaustive(n, pats)` | Iterates `list switch_pat`; needs `forallb` spec |
| 313 | `has_self_reference_by_value(self_type_id, field_types)` | `existsb` over type id list |
| 348 | `fields_unique(names)` | Recursive duplicate detection on list |
| 502 | `offset_field_exists(field_names, target)` | `existsb` over field name list |
| 993 | `spawn_body_safe(globals_non_shared)` | Pattern match on list cons |
| 1078 | `deadlock_safe(shared_types_accessed)` | Match on list size ≥ 2 |
| 1241 | `shadow_check_valid(param_names, new_name)` | `existsb` over param list |

**Why Phase 4:** each requires VST-proven list operations. The C implementations iterate over arrays with `for` loops — trivial in C, but VST's `semax_body` proof becomes dramatically harder with loops over list-typed arguments. Phase 4 introduces verified list APIs; these then become straightforward.

**Alternative:** extract as `(int ptr, int len) → int` — passing array as primitive ptr+len. Works for some (`fields_unique`, `offset_field_exists`). Cost: VST loop invariant proof (~2 hrs per predicate). Doable but not in same productivity class as the 23+14.

---

## 5. Code-driven predicates (Category C, ~15 — no typing.v oracle)

Rules the compiler enforces that **aren't yet formalized in typing.v**. Extractable as pure functions but Coq spec would be written from C code (weaker — code matches itself). Listed as a SEPARATE CATEGORY; extracting them needs Phase 7 theory extension first.

**If extracted now without oracle:** VST proves "C matches our spec, our spec matches C" — tautology. Zero added safety. **Recommend: defer until typing.v has the rule.**

| Rule (description) | Call site | Why not yet in typing.v |
|---|---|---|
| `@cstr` buffer overflow check (strlen+1 ≤ buf_size) | checker.c:5843 | String semantics not modeled |
| `@cstr` destination const check | checker.c:5822/5831 | Reduces to existing const-preservation (covered) |
| Array size positive + ≤ 4GB | checker.c:1320, 1324 | Size semantics are target-dependent |
| Semaphore count ≥ 0 | checker.c:1177 | Weaker variant of T01 |
| Pool/Ring count must be constant | checker.c:1353, 1366 | "Constness" is comptime-subset (R02 indirectly covers) |
| `@ptrtoint` → non-usize warning | checker.c:6803 | Portability concern, not safety |
| MMIO slice index bound (variable) | checker.c:5075 | Covered by auto-guard, not formalized |
| `arena.reset()` outside defer warning | checker.c:3995 | Position analysis |
| Ring.push pointer warning | checker.c:3821, 3842 | Pointer ownership across threads |
| Union switch lock state | checker.c:2787, 2567, 4920 | Stateful — per-call-site pure, state as input |
| Keep-param escape (6 sub-rules) | checker.c:4306, 4322, 4354, 4360, 4372, 4380 | Composite of escape + keep — split predicates cover it |
| `@ptrcast` type mismatch split | checker.c:5527 | Covered by existing provenance_rules |
| Non-volatile ptr indexing warning | checker.c:5087 | Quality warning, not safety |
| Designated init requires struct | checker.c:794 | Structural, pure typing |
| Heterogeneous `*opaque` array | checker.c:587 | Composite of provenance |

**Most of these (8-10) are REDUCIBLE to existing predicates** after careful analysis. Genuinely novel code-driven rules: ~5-6. Extract those AFTER adding to typing.v.

---

## 6. Runtime traps (Category F, 16 — NOT Phase 1)

Runtime `_zer_trap` / `_zer_bounds_check` emissions in emitter.c. Generated INTO output C, not executed by the compiler. These have no compile-time predicate component — or if they do, it's already in the compile-time predicates above.

| Runtime trap | emitter.c line | Compile-time mirror |
|---|---|---|
| `division by zero` | 1055, 1363 | M1 `zer_div_valid` (compile-time) |
| `signed division overflow` | 1068, 7484 | NEW: could add `zer_signed_div_overflow_possible(divisor)` — low value |
| `@inttoptr outside mmio range` | 2650 | H01 `zer_mmio_addr_in_range` (compile-time) |
| `@inttoptr unaligned` | 2660 | H03 `zer_mmio_inttoptr_allowed` (compile-time) |
| `@ptrcast type mismatch` | 2547, 5784 | J4 `zer_provenance_type_ids_compatible` (compile-time) |
| `slice start > end` | 2258 | L1 `zer_slice_bounds_valid` (compile-time) |
| `type mismatch in cast` | 2410, 7685 | J4 provenance (compile-time) |
| `explicit trap` | 2694, 5740 | User-invoked — not safety |
| `memory access fault` | 4380 | Runtime-only fallback, no predicate |
| `MMIO: no hardware detected` | 4666 | Boot-time probe, no compile predicate |
| `array index out of bounds` | 4424 | L01 `zer_index_in_bounds` (compile-time) |
| `UAF: handle gen mismatch` | 4432 | A `zer_handle_state_is_invalid` (compile-time) |
| `slab UAF / invalid handle` | 4533, 4561 | A + handle tracking (compile-time) |
| `tracked pointer double-free` | 4587 | A + `zer_handle_state_is_freed` (compile-time) |
| `tracked pointer UAF` | 4633 | A + `zer_handle_state_is_invalid` |
| `_zer_bounds_check` emission sites (10+) | 1989, 2028, 2060, 2999, 4422, 4424, 5273, 7619 | All reduce to L01 |

**Bottom line:** runtime traps are the **dynamic safety net** for cases where compile-time can't prove. Each has a compile-time sibling predicate. No new Phase 1 work required.

---

## 7. Summary progression table

| Stage | Count | Delta | Oracle tier |
|---|---|---|---|
| **Starting state** | 48 | — | Mixed tier 1+2 |
| + M section (arith) | 52 | +4 | Tier 2 (typing.v) |
| + L bounds extras | 54 | +2 | Tier 2 |
| + R comptime | 58 | +4 | Tier 2 |
| + S stack | 59 | +1 | Tier 2 |
| + P variant | 61 | +2 | Tier 2 |
| + T container extras | 63 | +2 | Tier 2 |
| + K container source | 64 | +1 | Tier 2 |
| + J-extended casts | **71** | +7 | Tier 2 **← Phase 1 strict complete** |
| + E atomic extras | 74 | +3 | Tier 3 (schematic) |
| + F async | 75 | +1 | Tier 3 |
| + D shared | 79 | +4 | Tier 3 |
| + C thread | **85** | +6 | Tier 3 **← Phase 1 full complete** |
| + Phase 4 list predicates | 92 | +7 | Requires Phase 4 infrastructure |
| + Code-driven (novel only) | ~98 | +5-6 | Requires typing.v extensions |

---

## 8. Extraction roadmap — committed to FULL 85

**Decision (2026-04-22):** extract all 85, not stop at 71. Rationale:

1. All 14 concurrency predicates have REAL Coq theorems in typing.v (not `True. Qed.` placeholders — verified proof bodies like C02_double_join_rejected, D02_shared_plus_yield_rejected, F01_yield_outside_async_rejected).
2. Tier-3 schematic still catches C-vs-Coq divergence via VST. Weaker than operational, not "no guarantee."
3. Phase 6 `check-no-inline-safety` will reject inline concurrency checks — we'd extract them eventually anyway. Doing it now unblocks Phase 6.
4. Phase 7 upgrade cost is low: swap the Coq `Definition X_coq` to reference operational `step_spec_*` instead of typing.v theorem. **C code unchanged.** ~20 min per predicate to upgrade later.

### Batch plan — 11 batches to hit 85

**Batch 1 — M section (4): arith_rules.c — IN PROGRESS 2026-04-22**
- zer_div_valid, zer_divisor_proven_nonzero, zer_narrowing_valid, zer_literal_fits
- Target: 52 extracted

**Batch 2 — L extras (2):** extend range_checks.c
- zer_slice_bounds_valid, zer_bit_index_valid
- Target: 54

**Batch 3 — T + K extras (3):** extend container_rules.c
- zer_container_position_valid, zer_handle_element_valid, zer_container_source_valid
- Target: 57

**Batch 4 — P (2):** variant_rules.c (new)
- zer_union_read_mode_safe, zer_union_arm_op_safe
- Target: 59

**Batch 5 — S (1):** stack_rules.c (new)
- zer_stack_frame_valid
- Target: 60

**Batch 6 — R (4):** comptime_rules.c (new)
- zer_comptime_arg_valid, zer_static_assert_holds, zer_comptime_ops_valid, zer_expr_nesting_valid
- Target: 64

**Batch 7 — J-extended (7):** cast_rules.c (new)
- zer_conversion_safe, zer_bitcast_width_valid, zer_bitcast_operand_valid, zer_cast_distinct_valid, zer_saturate_operand_valid, zer_ptrtoint_source_valid, zer_cast_types_compatible
- Target: **71 ← Phase 1 strict milestone**

**Batch 8 — E atomic extras (3):** extend atomic_rules.c
- zer_atomic_on_packed_valid, zer_condvar_arg_valid, zer_sync_in_packed_valid
- Target: 74

**Batch 9 — F async (1):** extend context_bans.c or async_rules.c (new)
- zer_yield_context_valid
- Target: 75

**Batch 10 — D shared (4):** shared_rules.c (new)
- zer_address_of_shared_valid, zer_shared_in_suspend_valid, zer_volatile_compound_valid, zer_isr_main_access_valid
- Target: 79

**Batch 11 — C thread (6):** thread_rules.c (new)
- zer_thread_op_valid, zer_thread_cleanup_valid, zer_spawn_context_valid, zer_spawn_return_safe, zer_spawn_arg_valid, zer_spawn_arg_is_handle_rejected
- Target: **85 ← Phase 1 full complete**

### Time estimate

Per batch (experienced session): ~30-60 min depending on predicate count.
- Small batch (1-2 predicates): 30 min
- Medium (3-4): 45 min
- Large (6-7): 60-90 min

**Total for 85:** ~6-10 hours of focused work across ~11 sessions.

### Phase 7 upgrade plan (future)

After Phase 7 builds λZER-concurrency/async Iris subsets:
- Batches 8-11 predicates (14 concurrency) get Coq spec upgrade (swap typing.v theorem → operational step_spec)
- C files: unchanged
- Call sites: unchanged
- Makefile: unchanged
- ~20 min per predicate = ~5 hrs total spec upgrade work

**End state:** all 85 predicates at operational-tier guarantee. Phase 1 fully grounded in step-rule semantics.

---

## 9. Extraction recipe (standard procedure)

1. **Add typing.v cross-reference** to the header comment in `.h` file — cite section + theorem names.
2. **Write `src/safety/<file>.c`** — flat cascade of early-return `if` statements. NO `&&`, NO `||`, NO nested `if`, NO compound conditions (VST struggles). If rule needs compound, split into multiple predicates and AND at call site.
3. **Write `src/safety/<file>.h`** — declarations + oracle references.
4. **Wire call sites** in checker.c/zercheck.c/zercheck_ir.c:
   - Replace inline check with `zer_X(...) != 0` (or `== 0` for negated)
   - Add `/* SAFETY: zer_X in src/safety/<file>.c */` comment on same or preceding line
5. **Update Makefile:**
   - `CORE_SRCS += src/safety/<file>.c`
   - `LIB_SRCS += src/safety/<file>.c`
   - `check-vst` target: add clightgen + coqc + `-Q` binding for the new file
6. **Write `proofs/vst/verif_<file>.v`**:
   - `Definition zer_X_coq (args : Z) : Z := ...` — matches typing.v oracle, NOT C code
   - Standard VST funspec with `PRE/POST`
   - Proof: `start_function. repeat forward_if; forward; unfold zer_X_coq; repeat (destruct (Z.eq_dec _ _); try lia); try entailer!. Qed.`
7. **Add `<file>.v` to `src/safety/.gitignore`** — CompCert clightgen generates it.
8. **Test:**
   - `make docker-build` — verify zerc builds with new file
   - `make check-vst` — verify VST proof passes
   - `make docker-check` — full regression
9. **Update this catalog** — mark predicate ⚫ EXTRACTED.
10. **Commit** — one predicate minimum, batch of same-section predicates preferred. Commit message format: `"Phase 1: extract zer_<name> (<section description>)"`.

---

## 10. Sample VST proof (copy-paste template)

For most predicates matching `(int) → int` with nested early-returns:

```coq
(* proofs/vst/verif_arith_rules.v — template *)

Require Import VST.floyd.proofauto.
Require Import VST.floyd.compat.
Require Import zer_safety.arith_rules.

#[export] Instance CompSpecs : compspecs. make_compspecs prog. Defined.
Definition Vprog : varspecs. mk_varspecs prog. Defined.

(* ---- Coq spec (from typing.v oracle) ---- *)

Definition zer_div_valid_coq (divisor : Z) : Z :=
  if Z.eq_dec divisor 0 then 0 else 1.

(* ---- VST funspec ---- *)

Definition zer_div_valid_spec : ident * funspec :=
 DECLARE _zer_div_valid
  WITH divisor : Z
  PRE [ tint ]
    PROP (Int.min_signed <= divisor <= Int.max_signed)
    PARAMS (Vint (Int.repr divisor))
    SEP ()
  POST [ tint ]
    PROP () RETURN (Vint (Int.repr (zer_div_valid_coq divisor))) SEP ().

Definition Gprog : funspecs := [ zer_div_valid_spec ].

(* ---- Proof ---- *)

Lemma body_zer_div_valid:
  semax_body Vprog Gprog f_zer_div_valid zer_div_valid_spec.
Proof.
  start_function.
  repeat forward_if;
    forward;
    unfold zer_div_valid_coq;
    repeat (destruct (Z.eq_dec _ _); try lia);
    try entailer!.
Qed.
```

---

## 11. Complete typing.v bool inventory (66 definitions)

Section-by-section status. Line numbers from typing.v. Status markers: ⚫=extracted, 🟢=strong-ready, 🟡=schematic, 🔴=Phase 4.

### Section Q — switch exhaustiveness
```
line 29  ⚫ bool_switch_exhaustive        → misc_rules.c zer_bool_switch_covers_both
line 57  🔴 enum_switch_exhaustive         (list switch_pat)
line 106 ⚫ int_switch_has_default         → misc_rules.c zer_int_switch_has_default
```

### Section I — qualifier preservation
```
line 125 ⚫ cast_safe                      → coerce_rules.c (split into 5 predicates)
```

### Section N — optional safety
```
line 191 ⚫ permits_null                   → optional_rules.c zer_type_permits_null
line 210 ⚫ can_unwrap                     (alias for permits_null — covered)
line 218 ⚫ has_nested_optional            → optional_rules.c zer_type_is_nested_optional
```

### Section T — container validity
```
line 238 ⚫ pool_count_valid               → range_checks.c zer_count_is_positive
line 787 🟢 container_position_valid       [T1]
line 808 🟢 handle_element_valid           [T2]
```

### Section P — variant safety
```
line 257 ⚫ variant_index_valid            → range_checks.c zer_variant_in_range
line 277 🟢 read_mode_safe                 [P1]
line 298 🟢 arm_safe_op                    [P2]
line 313 🔴 has_self_reference_by_value    (list nat)
line 348 🔴 fields_unique                  (list nat, has_duplicates)
line 366 ⚫ field_type_valid               → container_rules.c zer_field_type_valid
line 381 ⚫ container_depth_valid          → container_rules.c zer_container_depth_valid
```

### Section G — control-flow context
```
line 417 ⚫ return_safe                    → context_bans.c zer_return_allowed_in_context
line 425 ⚫ break_safe                     → context_bans.c zer_break_allowed_in_context
line 446 ⚫ defer_safe                     → context_bans.c zer_defer_allowed_in_context
line 459 ⚫ asm_safe                       → context_bans.c zer_asm_allowed_in_context
```

### Section K — intrinsic shape checks
```
line 490 🟢 container_source_valid         [K1]
line 502 🔴 offset_field_exists            (list nat)
line 522 ⚫ type_has_size                  → container_rules.c zer_type_has_size
```

### Section L — bounds safety
```
line 542 ⚫ array_index_valid              → range_checks.c zer_index_in_bounds
line 554 🟢 slice_bounds_valid             [L1]
line 584 🟢 bit_index_valid                [L2]
```

### Section M — arithmetic safety
```
line 604 🟢 div_valid                      [M1]
line 619 🟢 divisor_proven_nonzero         [M2]
line 631 🟢 narrowing_valid                [M3]
line 650 🟢 literal_fits                   [M4]
```

### Section R — comptime soundness
```
line 674 🟢 comptime_arg_valid             [R1]
line 686 🟢 static_assert_holds            [R2]
line 706 🟢 comptime_ops_valid             [R3]
line 720 🟢 expr_nesting_valid             [R4]
```

### Section S — resource limits
```
line 740 🟢 stack_frame_valid              [S1]
line 757 ⚫ slab_alloc_context_valid       → isr_rules.c (split into 2 predicates)
```

### Section J-extended — cast intrinsics
```
line 833 🟢 conversion_safe                [J1]
line 852 🟢 bitcast_valid                  [J2]
line 864 🟢 bitcast_operand_valid          [J3]
line 878 🟢 cast_distinct_valid            [J4]
line 892 🟢 saturate_operand_valid         [J5]
line 904 🟢 ptrtoint_source_valid          [J6]
line 916 🟢 cast_types_compatible          [J7]
```

### Section C — thread/spawn (concurrency, schematic)
```
line 941  🟡 thread_op_valid                [C1]
line 957  🟡 thread_cleanup_valid           [C2]
line 970  🟡 spawn_context_valid            [C3] (3-arg form — isr_rules has 2-arg split)
line 993  🔴 spawn_body_safe                (list)
line 1005 🟡 spawn_return_safe              [C4]
line 1017 🟡 spawn_arg_valid                [C5]
line 1033 🟡 spawn_arg_is_handle            [C6]
```

### Section D — shared struct/deadlock (concurrency, schematic)
```
line 1049 🟡 address_of_shared_valid        [D1]
line 1061 🟡 shared_in_suspend_valid        [D2]
line 1078 🔴 deadlock_safe                  (list)
line 1094 🟡 volatile_compound_valid        [D3]
line 1106 🟡 isr_main_access_valid          [D4]
```

### Section E — atomic/sync
```
line 1126 ⚫ atomic_width_valid              → atomic_rules.c zer_atomic_width_valid
line 1146 ⚫ atomic_arg_valid                → atomic_rules.c zer_atomic_arg_is_ptr_to_int
line 1158 🟡 atomic_on_packed_valid          [E1]
line 1166 🟡 condvar_arg_valid               [E2]
line 1178 🟡 sync_in_packed_valid            [E3]
```

### Section F — async context (concurrency, schematic)
```
line 1198 🟡 yield_context_valid             [F1]
line 1241 🔴 shadow_check_valid              (list)
```

### Decidability helpers (NOT predicates — excluded from count)
```
line 1273 bool_switch_exhaustive_dec
line 1277 cast_safe_dec
line 1281 permits_null_dec
```

**Totals:**
- ⚫ EXTRACTED from typing.v: 22 (with cast_safe split into 5 + slab_alloc_context split into 2 = 27 extracted predicates rooted in these 22 Definitions)
- 🟢 READY (strong): 23
- 🟡 SCHEMATIC (concurrency): 14
- 🔴 DEFER (list/Phase 4): 7
- **Total bool Definitions:** 66 (minus 3 decidability helpers = 63 operationally relevant)

Plus 21 operational-subset-derived (handle_state 4 + type_kind 7 + escape 3 + provenance 3 + mmio 2 + move 2) that are **NOT** in typing.v — they come from λZER-* subsets or TypeKind enum.

**Grand totals:**
- Extracted (typing.v + derived): **48**
- Phase 1 strict target: **48 + 23 = 71**
- Phase 1 full target: **48 + 23 + 14 = 85**
- Phase 4 deferred: **+7** (list-iterating)
- Code-driven novel: **+5-6** (after typing.v extension)

---

## 12. Dependency graph (which predicates depend on which)

Most Phase 1 predicates are standalone. A few have logical dependencies:

```
zer_handle_state_is_invalid
    ├── used by zercheck.c is_handle_invalid
    ├── used by zercheck.c is_handle_consumed
    └── used by zercheck_ir.c ir_is_invalid

zer_type_kind_is_integer
    ├── used by types.c type_is_integer
    └── prerequisite for zer_type_kind_is_numeric

zer_type_kind_is_float
    └── prerequisite for zer_type_kind_is_numeric

zer_type_kind_is_numeric
    ├── uses zer_type_kind_is_integer + zer_type_kind_is_float
    └── used by types.c type_is_numeric

zer_coerce_int_widening_allowed ─┐
zer_coerce_usize_same_width_... ─┤
zer_coerce_float_widening_... ───┼── all called from types.c can_implicit_coerce
zer_coerce_preserves_volatile ───┤
zer_coerce_preserves_const ──────┘

zer_region_can_escape
    └── called with output from zer_sym_region_tag helper
        (which calls zer_region_is_local/_arena indirectly)

zer_alloc_allowed_in_isr ─┐
zer_alloc_allowed_in_...  ├── called sequentially (both must pass)
                          └── future: combined form zer_spawn_context_valid(3-arg)

zer_spawn_allowed_in_isr ─┐
zer_spawn_allowed_in_... ─┴── future: subsumed by C3 zer_spawn_context_valid
```

**New predicates (23 strict + 14 full) — mostly independent.** A few pairs:
- M1 `div_valid` and M2 `divisor_proven_nonzero` — called together at same site (one for const, one for VRP-proven)
- L1 `slice_bounds_valid` composed of ≤/≤ checks; could use L01 `array_index_valid` internally (not required)
- T1 `container_position_valid` called in same contexts as T2 `handle_element_valid` (both at container validation)

---

## 13. Per-call-site exhaustive map (47 typing.v-sourced predicates)

Where each EXTRACTED typing.v predicate is called in the compiler. Used to audit `check-no-inline-safety` — every call site should have `/* SAFETY: zer_X */` comment linking back.

### Existing delegations (grep for `SAFETY:` in checker.c):

```
checker.c:16    zer_sym_region_tag helper (wraps zer_region_* predicates)
checker.c:779   check_isr_ban → zer_alloc_allowed_in_isr
checker.c:1217  TYNODE_OPTIONAL → zer_type_is_nested_optional
checker.c:1353  TYNODE_POOL count → zer_count_is_positive
checker.c:1366  TYNODE_RING count → zer_count_is_positive
checker.c:5519  @ptrcast provenance → zer_provenance_check_required
checker.c:5661  @inttoptr constant → zer_mmio_inttoptr_allowed (+ addr_in_range inside loop)
checker.c:5772  @atomic_* width → zer_atomic_width_valid
checker.c:7987  NODE_RETURN → zer_return_allowed_in_context
checker.c:8156  return escape → zer_region_can_escape
checker.c:8377  NODE_BREAK → zer_break_allowed_in_context
checker.c:8393  NODE_GOTO → zer_goto_allowed_in_context
checker.c:8409  NODE_CONTINUE → zer_continue_allowed_in_context
checker.c:8429  NODE_DEFER → zer_defer_allowed_in_context
checker.c:8493  NODE_ASM → zer_asm_allowed_in_context

zercheck_ir.c:251  ir_is_invalid → zer_handle_state_is_invalid
zercheck_ir.c:947  IR_SLAB_ALLOC in ISR → zer_alloc_allowed_in_isr
zercheck_ir.c:951  IR_SLAB_ALLOC in @critical → zer_alloc_allowed_in_critical

types.c:168-193   all type_is_* → zer_type_kind_is_*
types.c:326       can_implicit_coerce → zer_coerce_* (5 predicates)
```

### Delegations TO ADD (after extracting the 23 strict):

```
checker.c:2367  div_val == 0 → zer_div_valid
checker.c:2392  "divisor not proven nonzero" → zer_divisor_proven_nonzero
checker.c:2408  "divisor from function call not proven nonzero" → zer_divisor_proven_nonzero
checker.c:3567  compound /= %= → zer_divisor_proven_nonzero
checker.c:3620  compound narrow → zer_narrowing_valid
checker.c:3519  integer literal range → zer_literal_fits
checker.c:6738  same → zer_literal_fits
checker.c:10523 same → zer_literal_fits

checker.c:5069  slice start > end → zer_slice_bounds_valid
checker.c:5080  slice end > size → zer_slice_bounds_valid
checker.c:5088  slice start > size → zer_slice_bounds_valid
checker.c:5158  bit index OOB → zer_bit_index_valid
checker.c:5164  bit hi < lo → zer_bit_index_valid (or separate predicate)

checker.c:4407  comptime all-const → zer_comptime_arg_valid
checker.c:8351  static_assert stmt → zer_static_assert_holds
checker.c:10481 static_assert top-level → zer_static_assert_holds
checker.c:1956  comptime for iter limit → zer_comptime_ops_valid (+related sites)
checker.c:2283  expr nesting → zer_expr_nesting_valid

checker.c:10154 stack frame size → zer_stack_frame_valid
checker.c:10161 call chain depth → zer_stack_frame_valid

checker.c:4911  union direct read → zer_union_read_mode_safe
checker.c:511   union self-mutation → zer_union_arm_op_safe
checker.c:2787  same → zer_union_arm_op_safe
checker.c:2567  same → zer_union_arm_op_safe

checker.c:4774  Handle elem not struct → zer_handle_element_valid
checker.c:8798  Pool/Ring/Slab as struct field → zer_container_position_valid
checker.c:8941  Pool/Ring/Slab as union variant → zer_container_position_valid

checker.c:5860  @container source → zer_container_source_valid

checker.c:5346  (int)ptr → zer_conversion_safe
checker.c:5352  (Type)int → zer_conversion_safe
checker.c:5558  @bitcast width → zer_bitcast_width_valid
checker.c:5541  @bitcast operand → zer_bitcast_operand_valid
checker.c:5954  @cast distinct → zer_cast_distinct_valid
checker.c:5576  @truncate operand → zer_saturate_operand_valid
checker.c:5590  @saturate operand → zer_saturate_operand_valid
checker.c:5684  @ptrtoint source → zer_ptrtoint_source_valid
checker.c:5359  invalid cast catch-all → zer_cast_types_compatible
```

### Delegations TO ADD (full Phase 1 concurrency):

```
zercheck.c:1084   thread double-join → zer_thread_op_valid
zercheck_ir.c:1766 same → zer_thread_op_valid
zercheck.c:2688   thread not joined at exit → zer_thread_cleanup_valid
zercheck_ir.c:2910 same → zer_thread_cleanup_valid

checker.c:8626  spawn in @critical → zer_spawn_context_valid
checker.c:8632  spawn in async → zer_spawn_context_valid
checker.c:8572  spawn returns resource → zer_spawn_return_safe
checker.c:8611  spawn non-shared ptr → zer_spawn_arg_valid
checker.c:8620  spawn with Handle → zer_spawn_arg_is_handle_rejected

checker.c:2598  &shared field → zer_address_of_shared_valid
checker.c:4792  shared access in yield stmt → zer_shared_in_suspend_valid
checker.c:~9840 volatile compound → zer_volatile_compound_valid
checker.c:9833  ISR+main access → zer_isr_main_access_valid

checker.c:5804  @atomic_ on packed → zer_atomic_on_packed_valid
checker.c:6043  @cond_ non-shared → zer_condvar_arg_valid
checker.c:8806  sync in packed → zer_sync_in_packed_valid

checker.c:8531  yield outside async → zer_yield_context_valid
checker.c:8540  await outside async → zer_yield_context_valid
(+ check_body_effects calls at checker.c:8433, 8504 transitively check)
```

**Total call sites after full Phase 1:** ~70 sites with `/* SAFETY: */` links.

---

## 14. Test fixture catalog

For each Phase 1 predicate, a minimal `.zer` test that exercises both the accept and reject paths. Useful for:
- Verifying the call-site delegation doesn't regress
- Phase 6 `check-coverage-tests` script to verify every predicate has a test

### Template pattern:

Positive (must compile): `tests/zer/phase1_<name>_accept.zer`
```zer
/* Exercise zer_X — should compile successfully */
i32 main() {
    /* code that passes zer_X's accept case */
    return 0;
}
```

Negative (must fail): `tests/zer_fail/phase1_<name>_reject.zer`
```zer
/* EXPECTED: compile error — zer_X's reject case */
i32 main() {
    /* code that triggers zer_X return 0 */
    return 0;
}
```

### Concrete examples for remaining 23:

#### M1 div_valid (reject)
```zer
i32 main() {
    u32 x = 10 / 0;  /* const division by zero */
    return (i32)x;
}
```

#### M2 divisor_proven_nonzero (reject)
```zer
i32 main() {
    u32 d = 5;
    u32 x = 10 / d;  /* divisor not proven nonzero */
    return (i32)x;
}
```

#### L1 slice_bounds_valid (reject: end > size)
```zer
i32 main() {
    u8[4] buf;
    []u8 s = buf[0..10];
    return 0;
}
```

#### R2 static_assert_holds (reject: assert false)
```zer
comptime u32 SIZE() { return 5; }
static_assert(SIZE() == 10, "size mismatch");
i32 main() { return 0; }
```

#### S1 stack_frame_valid (reject: --stack-limit 64)
```zer
/* compile with --stack-limit 64 */
i32 deep() {
    u8[100] big;
    return (i32)big[0];
}
i32 main() { return deep(); }
```

#### J1 conversion_safe (reject: C-style cast int→ptr)
```zer
i32 main() {
    u32 addr = 0x4000;
    *u32 p = (*u32)addr;  /* must use @inttoptr */
    return 0;
}
```

**Full test batch (all 37 remaining):** can be generated mechanically from this catalog.

---

## 15. Existing src/safety implementations — full inventory

All 14 files linked into `zerc` via `CORE_SRCS` in Makefile. Total ~400 lines. VST-verified via `make check-vst` (uses CompCert clightgen on same source).

### File sizes (approximate)

| File | Lines | Predicates | VST proof file |
|---|---|---|---|
| handle_state.c + .h | 30 + 59 | 4 | verif_handle_state.v |
| range_checks.c + .h | 25 + 40 | 3 | verif_range_checks.v |
| type_kind.c + .h | 40 + 75 | 7 | verif_type_kind.v |
| coerce_rules.c + .h | 45 + 64 | 5 | verif_coerce_rules.v |
| context_bans.c + .h | 35 + 45 | 6 | verif_context_bans.v |
| escape_rules.c + .h | 20 + 48 | 3 | verif_escape_rules.v |
| provenance_rules.c + .h | 25 + 52 | 3 | verif_provenance_rules.v |
| mmio_rules.c + .h | 20 + 33 | 2 | verif_mmio_rules.v |
| optional_rules.c + .h | 20 + 39 | 2 | verif_optional_rules.v |
| move_rules.c + .h | 20 + 46 | 2 | verif_move_rules.v |
| atomic_rules.c + .h | 20 + 33 | 2 | verif_atomic_rules.v |
| container_rules.c + .h | 25 + 40 | 3 | verif_container_rules.v |
| misc_rules.c + .h | 25 + 38 | 2 | verif_misc_rules.v |
| isr_rules.c + .h | 30 + 44 | 4 | verif_isr_rules.v |
| **Total** | **~400 C + ~700 H** | **48** | 14 proof files |

### Pattern enforcement

All extractions follow the SAME structural pattern:
- `int zer_X(...)` signatures — no pointers, no structs
- Early-return form: `if (cond) return 0; ... return 1;`
- No `&&`, `||`, nested `if`, or compound boolean expressions
- Unsigned/signed int args — NO `uint64_t` or custom enum types in signature (use `int` constants)
- Constants defined in `.h` file (`ZER_HS_*`, `ZER_TK_*`, `ZER_REGION_*`, `ZER_DP_*`, etc.)

This pattern is what makes VST proofs 1-liner. ANY deviation (compound conditions, nested if, pointer args) breaks the standard proof.

---

## 16. CLAUDE.md 4-model mapping

From CLAUDE.md "ZER Safety Architecture — 4 Models", each Phase 1 predicate maps to one model:

### Model 1 (State Machines) — 6 extracted predicates

Track entity lifecycles with explicit state transitions:
- handle_state.c: all 4 (UNKNOWN → ALIVE → FREED/MAYBE_FREED/TRANSFERRED)
- move_rules.c: 2 (move struct tracks via HS_TRANSFERRED)

**None in remaining 23** — all state-machine tracking is centralized in zercheck.c/zercheck_ir.c (Phase 4 will extract these).

### Model 2 (Program Point Properties) — 8 extracted

Values change at assignments/branches:
- escape_rules.c: 3 (region flags)
- provenance_rules.c: 3 (provenance type per Symbol)
- mmio_rules.c: 2 (address/alignment — set at @inttoptr)

**Remaining 23 adds:**
- L1 slice_bounds_valid, L2 bit_index_valid (VRP-driven)
- M3 narrowing_valid (width-derived)
- P1/P2 variant safety (switch lock state)

### Model 3 (Function Summaries) — 0 extracted

Per-function cached properties (FuncProps tracking system). **Not Phase 1** — requires cross-function DFS which Phase 4 handles.

**Remaining 14 (concurrency) touch this model:**
- C1/C2 thread lifecycle
- C3 spawn_context_valid (reads context, not per-function)
- D2 shared_in_suspend_valid

### Model 4 (Static Annotations) — 34 extracted + remaining 23

Declaration-level constraints:
- type_kind.c: 7 (inherent to TypeKind)
- coerce_rules.c: 5 (qualifier preservation)
- context_bans.c: 6 (context flags at declaration/scope)
- optional_rules.c: 2 (optional structure)
- atomic_rules.c: 2 (width from type)
- container_rules.c: 3 (position + size)
- misc_rules.c: 2 (switch exhaustiveness)
- isr_rules.c: 4 (context at call site)
- **All 23 remaining** — M (width/value), L (size), R (comptime annotations), S (limit), P (variant position), T (container position), K (type category), J-ext (cast shapes)

Phase 1 is **dominated by Model 4** — static annotations on declarations. This is why Phase 1 proofs are so tractable — no state mutation, just checking declaration facts.

---

## 17. Why Phase 1 matters (safety claim)

Each extracted + VST-verified predicate closes a specific safety hole. The chain is:

```
typing.v Coq proof        ← HUMAN trust (oracle correctness)
       ↓  extract to .c
src/safety/<file>.c       ← MECHANICAL trust (VST verifies match)
       ↓  link into zerc
checker.c delegation site ← HUMAN trust (correct delegation via /* SAFETY: */ comment)
       ↓  compile user program
Compiled ZER binary       ← USER guarantee
```

The three HUMAN trust points are:
1. **Oracle correctness** — did we write the right rule in typing.v?
2. **Call-site delegation** — is the predicate actually called with correct args at every relevant site?
3. **Completeness** — did we miss a safety rule that needs a predicate?

Phase 1 addresses #1 (via oracle review) and #2 (via `/* SAFETY: */` comments + future `check-no-inline-safety` in Phase 6). #3 is bounded by typing.v coverage — every `Definition X : bool` should have an extraction, enforced by Phase 6 `check-theory-extracted`.

**At Phase 1 completion (85 predicates), the compiler's per-site decisions are mechanically verified against the oracle. What remains to verify:**
- State-machine orchestration (Phase 2: decision extraction)
- AST walker completeness (Phase 3: generic walker)
- Global state invariants (Phase 4: verified state APIs)
- Pass ordering (Phase 5: phase-typed checker)
- Discipline (Phase 6: CI enforcement)

---

## 18. BUGS-FIXED.md cross-references

Each Phase 1 predicate corresponds to one or more previously-fixed bugs. Listed where known (from BUGS-FIXED.md):

| Predicate | Related BUG-XXX |
|---|---|
| zer_count_is_positive | BUG-423 (comptime calls in Pool count) |
| zer_type_permits_null, zer_type_is_nested_optional | BUG-409 (distinct typedef optional), BUG-506 (distinct unwrap) |
| zer_coerce_preserves_volatile | BUG-258 (@ptrcast strip volatile), BUG-282 (assign volatile→non), BUG-341 (@bitcast strip volatile) |
| zer_coerce_preserves_const | BUG-304 (@ptrcast strip const), BUG-448 (cast strip const) |
| zer_mmio_inttoptr_allowed | BUG-371 (constant expression @inttoptr) |
| zer_provenance_type_ids_compatible | BUG-446 (cast provenance), BUG-393 (compound key prov) |
| zer_return_allowed_in_context | BUG-442 (return expr before defer), class bug pattern |
| zer_break_allowed_in_context | (G05 outside loop checks) |
| zer_defer_allowed_in_context | Gap 7 (nested defer ban) |
| zer_region_can_escape | BUG-205/355 (local-derived escape), BUG-240/377 (nested slice) |
| zer_container_depth_valid | Container template depth guard (implicit in monomorphization) |
| zer_atomic_width_valid | BUG-493 (@atomic on packed), width validation |
| zer_alloc_allowed_in_isr | Handle/slab ISR bans |

**For future bugs:** before extracting a new predicate, check if the BUG might be due to:
1. Missing predicate entirely (add to typing.v + extract)
2. Predicate exists but not called at this site (add delegation)
3. Predicate exists and called but wrong args (fix call site)

---

## 19. Next session action items

When resuming extraction work:

1. **Verify catalog is current** — run `grep "checker_error\|zc_error\|ir_zc_error" checker.c zercheck.c zercheck_ir.c | wc -l` and compare to baseline 424. If increased, new error sites may need new predicates.

2. **Pick next batch** — start with M section (4 predicates, ~1 hr). Provides immediate strong Phase 1 expansion.

3. **Follow extraction recipe** — in this doc (§9) + `docs/proof-internals.md` "Phase 1 extraction recipe".

4. **Commit per-batch** — batch boundaries: M, L, T+K, P, S, R, J-ext, E, F, D, C.

5. **Update this catalog** — mark predicate ⚫ EXTRACTED in §11 (typing.v inventory) and §1 (extracted section).

6. **Update CLAUDE.md progress counter** — current says "48/77", update to new total after each batch. Current true target is 71 strict / 85 full (NOT 77 — my earlier count was wrong).

7. **Update docs/formal_verification_plan.md Phase 1 section** — revise "Remaining to reach 77" table with this catalog's §11.

The 48/71/85 counts are the **definitive targets** as of 2026-04-22. Do NOT re-litigate the count — the enumeration in §11 is complete and verified against typing.v (66 bool Definitions counted line by line).

---

## 20. Long-term considerations

After Phase 1 at 85:

- **Phase 7** converts the 14 concurrency 🟡 schematic to tier 1 operational. Adds new `lambda_zer_concurrency/` subset. Same predicates, stronger oracle.
- **Phase 4** unlocks the 7 🔴 list predicates. After verified list APIs, those become extractable.
- **Code-driven** novel rules (5-6) need typing.v extensions first. Add predicate to typing.v, prove theorem, then extract.

**Total theoretical ceiling:** ~98-100 pure predicates after all Phase 1-7 work. This represents the full strong-oracle safety coverage of the compiler.

Beyond 100 predicates is Phase 2/3/4 territory — decision extraction, walker verification, state API verification — which is not "more predicates" but "verified orchestration around predicates."

---

**END OF CATALOG**

Last updated: 2026-04-22
Authored from: full-read audit (~17,500 lines of C + Coq + docs)
Status: DEFINITIVE for the stated date. Update after each extraction batch.
