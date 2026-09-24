# zer_gaps/ — known compile-time blind spots

Reproducers for gaps where the CHECKER does not reject a program it arguably
should. The expectation here is **INVERTED**: compile-clean IS the gap, so
`tests/test_zer.sh` fails a file that starts being rejected, telling you to
promote it to `tests/zer_fail/` as a permanent regression guard.

## READ THIS BEFORE TRUSTING A ROW

**This directory measures the COMPILE-TIME verdict only.** A gap closed by a
RUNTIME guard therefore stays recorded as "open" forever, because the program
still compiles clean — and seven of the rows below were in exactly that state
when this table was re-measured on 2026-09-15. Two of them
(`audit2_slice_oob`, `audit2_slice_star_oob`) claim ZER does not bounds-check
`[*]T`; it does, at run time, and has for a long time.

So a row here means "the checker says nothing", NOT "the program is unsafe".
The **runtime** column is the one that tells you whether anything can actually
go wrong, and it is measured, not remembered. Re-measure before acting on any
row — that is the same rule `docs/limitations.md` states for its own entries,
and this directory needs it more, because nothing here is executed by the
suite in a way that would notice drift.

**The table was also 9 rows stale** (rows pointing at files deleted long ago)
and missing 17 files that were present, which is the "a stale gate is worse
than none" failure this project documents for its gates, applied to a list. It
is regenerated from the tree below; regenerate it again rather than editing a
row by hand.

## Status, measured 2026-09-15 against the current compiler

`rc` is the exit status of the compiled program. 133 = SIGTRAP = a ZER safety
trap fired.

| File | Checker | Runtime | What this means |
|---|---|---|---|
| `ast_div_by_zero_runtime.zer` | clean | rc=0 | guard present; the divisor is nonzero on the taken path |
| `ast_explicit_trap.zer` | clean | rc=133 `@trap` | not a gap — `@trap()` doing its job |
| `ast_inttoptr_align.zer` | REJECTED | — | masked by the I1 unbounded-index rule, see the file's own note |
| `ast_inttoptr_mmio.zer` | REJECTED | — | masked the same way |
| `ast_probe.zer` | clean | rc=42 | not a gap — `@probe` returning a value |
| `ast_shift_over_width.zer` | clean | rc=0 | **closed**: shift-by->=width really is 0, verified across u8/u32/u64/i32 |
| `ast_signed_div_overflow.zer` | clean | rc=133 | **closed at run time** — traps "signed division overflow" |
| `ast_slice_empty_range.zer` | clean | rc=133 | **closed at run time** — traps "slice start > end" |
| `audit2026-05-24_bool_int_cast.zer` | clean | rc=1 | **NOT a safety gap** — see below |
| `audit2026-05-24_spawn_alias_args.zer` | REJECTED | — | BIT-ROTTED — uses a `*shared T` spelling the grammar never accepted |
| `audit2_nested_if_chain.zer` | REJECTED | — | masked by the wrong-pool rule; the transfer question is untested |
| `audit2_slice_alloc_safety.zer` | REJECTED | — | masked by a UAF elsewhere in the file |
| `audit2_slice_oob.zer` | clean | rc=133 | **closed at run time** — `[*]T` IS bounds-checked; the claim in the file is stale |
| `audit2_slice_star_oob.zer` | clean | rc=133 | **closed at run time** — same |
| `audit_2026-06-02_nostrict_mmio_no_runtime.zer` | REJECTED | — | masked by the strict-mmio requirement; needs `--no-strict-mmio` to probe |
| `audit_2026-06-02_slice_oob.zer` | clean | rc=133 | **closed at run time** — traps "slice end > len" |
| `audit_2026-06-17_defer_goto_fallthrough_drops.zer` | clean | rc=1 | needs re-derivation after refactor L moved defer bodies onto the IR path |
| `gap4_async_shared_across_yield.zer` | clean | rc=0 | LIVE at the checker; see CLAUDE.md — locking is PER-STATEMENT, so the blanket claim it tests is itself wrong |
| `prec2_opaque_wrong_type.zer` | clean | rc=133 | **closed at run time** — the `@ptrcast` type_id check traps |

### `audit2026-05-24_bool_int_cast` — the file's bare-metal claim is FALSE

The file says `(bool)42` "produces a boolean containing the non-{0,1} value 42"
and that an exhaustive `switch` on it is "silently defeated". Measured: the
emitter renders `(bool)n` as `((uint8_t)!!(n))`, so the value is normalised to
0 or 1 and no forged bool exists. The bool switch is also not last-arm-elided —
it falls through when neither arm matches.

What remains is a documentation question, not a safety one: the C-style cast
converts bool<->int in both directions while the *implicit* coercion is banned.
That is deliberate and is now written down in `docs/reference.md` under `bool`.

### Retired 2026-09-15

- `audit2026-05-24_critical_x86_no_cli.zer` — **was already CLOSED** and the row
  was stale. Gap 10 (2026-05-16) gave bare-metal x86 real `pushf`/`cli` and
  `push`/`popf`, gated on `!_ZER_HOSTED`; hosted x86 keeps the fence, which is
  correct there because `cli` is privileged. Verified by preprocessing the
  emitted C with `-ffreestanding`.

- `audit_2026-06-12_critical_hosted_arm.zer` — **CLOSED** by BUG-1020. Its
  diagnosis (the ARM arm lacked the hosted guard that x86 had) was right; its
  predicted symptom was not. Measured with real cross-toolchains: on hosted ARM
  it is a BUILD FAILURE, not SIGILL, because PRIMASK is M-profile only — and the
  SIGILL case it did not mention is hosted RISC-V, which built clean and would
  have faulted on a machine-mode CSR.

- `funcptr_array_null_element.zer` — **CLOSED** by BUG-1019. Calling an
  unassigned element of a non-null funcptr array now traps with "call through a
  null function pointer", from a COMPILED-IN guard, so it fires on bare metal
  too (previously only the hosted SIGSEGV handler noticed, and only hosted).
  Regression guards live in `tests/zer_trap/funcptr_null_*_bug1019.zer`.
