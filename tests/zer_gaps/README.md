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
| `gap4_async_shared_across_yield.zer` | shared struct access across yield in async |
| `gap5_container_move.zer` | move struct transferred through container field not tracked |
| `prec1_vrp_literal_i.zer` | VRP precision: `u32 i = 10; arr[i]` not proven OOB |
| `prec2_opaque_wrong_type.zer` | *opaque cast to wrong type inside same function |

## Fixed since audit (moved to `tests/zer_fail/` as regression guards)

- gap3 (`yield` outside async) — now a compile error
- gap6 (`goto` into if-unwrap/switch capture) — BUG-609, fixed this session
- gap7 (`defer` nested in `defer` body) — now a compile error
