# zer_trap/ — runtime safety tests (must compile + trap)

Tests that verify runtime safety checks fire correctly. Each test
compiles clean but MUST trap at runtime with the expected message.

The test runner checks for:
1. Compile succeeds (exit 0 from `zerc <file> -o /tmp/x.c`)
2. GCC compiles the emitted C successfully
3. Running the binary exits non-zero (SIGTRAP = exit 133)
4. Optional: trap message substring match

Added 2026-04-19 as regression guards for BUG-595 through BUG-599
(AST→IR emission safety check regressions).

## Files

| File | What it verifies |
|---|---|
| `slice_bounds_oob_trap.zer` | Slice indexing with runtime OOB traps via `_zer_bounds_check` |
| `slice_range_underflow_trap.zer` | `arr[lo..hi]` with `lo > hi` traps via slice range check |
| `signed_div_overflow_trap.zer` | `INT_MIN / -1` traps via signed overflow check |
| `inttoptr_out_of_mmio_trap.zer` | `@inttoptr` with address outside `mmio` range traps |
| `inttoptr_unaligned_trap.zer` | `@inttoptr` with unaligned address traps |

Shift safety (`_zer_shl`/`_zer_shr`) doesn't go here — it's tested
positively in `tests/zer/shift_over_width_is_zero.zer` because
per-spec shift-by-over-width returns 0, it doesn't trap.
