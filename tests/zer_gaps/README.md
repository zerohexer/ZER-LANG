# zer_gaps/ — known compile-time blind spots

Reproducers for gaps discovered in the 2026-04-19 audit. Each `.zer`
file here currently COMPILES CLEAN when it SHOULD error. They are
checked into the tree as documentation of the gap, not run by
`make check`.

When a gap is fixed, its reproducer should move to `tests/zer_fail/`
so it becomes a permanent regression guard.

See `docs/limitations.md` for the full gap list with priorities and
fix estimates.

## Files

| File | Gap |
|---|---|
| `gap1_cross_block_goto.zer` | Cross-block backward goto UAF — runtime-caught |
| `gap2_same_line_uaf.zer` | Same-line UAF not detected (free + use on same line) |
| `gap3_yield_outside_async.zer` | `yield` in non-async function silently stripped |
| `gap4_async_shared_across_yield.zer` | shared struct access across yield in async |
| `gap5_container_move.zer` | move struct transferred through container field not tracked |
| `gap6_goto_into_capture.zer` | goto into if-unwrap scope skips capture binding |
| `gap7_defer_in_defer.zer` | defer nested in defer body accepted (spec says reject) |
| ~~`prec1_vrp_literal_i.zer`~~ | **CLOSED 2026-08-17 (BUG-800)** — promoted to `tests/zer_fail/bounds_ident_always_oob.zer`. Its own claim that this was "just a precision/message difference" and "safe at runtime" was WRONG: the auto-guard's runtime form is `if (i >= 4) { return 0; }`, so the function returns EARLY and every statement after the access never runs — silent at both ends, and on bare metal a peripheral write that simply never happens. |
| `prec2_opaque_wrong_type.zer` | *opaque cast to wrong type inside same function |
| `audit_2026-06-02_slice_oob.zer` | `arr[0..end]` with end > arr.len silently constructs OOB slice |
| `audit_2026-06-02_ptrcast_unrelated.zer` | `@ptrcast(*B, &a)` between unrelated concrete struct pointers accepted |
| `audit_2026-06-02_nostrict_mmio_no_runtime.zer` | `--no-strict-mmio` drops runtime range/alignment check |
| `audit_2026-06-02_container_optional_type_arg.zer` | `container Box(T)` with `Box(?u32)` emits invalid C identifier |
