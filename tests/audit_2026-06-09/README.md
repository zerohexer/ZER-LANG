# Audit 2026-06-09 — Silent Gaps Discovered

This directory contains reproducers for silent gaps discovered during the
2026-06-09 codebase audit. **6 of 8 reproducers compile cleanly** with
the released `zerc`; the other 2 (`gap_container_ptr_optional_arg`,
`gap_nostrict_mmio_drops_runtime`) document gaps observable only via
specific compile flags or by inspecting emitted C — they are not
runnable regression tests today.

Full root-cause + fix-sketch in `docs/audit_2026-06-09.md`.

When a gap is fixed, move the corresponding `.zer` to `tests/zer_fail/`
(compile-time rejection) or `tests/zer_trap/` (runtime trap) and trim
the row from this README and from `docs/limitations.md`.

## Gaps

| ID | File | Compile? | Runtime? | Severity | Class |
|---|---|---|---|---|---|
| GAP-1 | `gap_ptrcast_concrete_unrelated.zer` | clean | exit=42 (wrong) | HIGH | Type confusion via `@ptrcast` between two unrelated concrete pointer types |
| GAP-2 | `gap_nostrict_mmio_drops_runtime.zer` | clean (with `--no-strict-mmio`) | no trap | HIGH | `--no-strict-mmio` drops the *runtime* alignment check too, not just compile-time |
| GAP-3 | `gap_alloc_ptr_global_alias_uaf.zer` | clean | exit=0 (UAF) | HIGH | `alloc_ptr` global alias UAF: no Handle-style gen counter on `*T` |
| GAP-4 | `gap_funcptr_double_free.zer` | clean | exit=0 (double-free) | HIGH | Function-pointer call freeing a Handle is not tracked |
| GAP-5 | `gap_orelse_overwrite_leak.zer` | clean | leak | MEDIUM | `h = pool.alloc() orelse return;` re-assigned to alive Handle leaks |
| GAP-6 | `gap_arr_var_index_dfree.zer` | clean | exit=0 (double-free) | MEDIUM | `arr[k]` with variable index escapes compound-key tracking |
| GAP-7 | `gap_container_ptr_optional_arg.zer` | zerc clean, GCC error | n/a | MEDIUM-UX | `Box(?u32)` / `Box(*u32)` emit invalid C identifiers |
| GAP-8 | `gap_arena_escape_via_struct_copy.zer` | clean | runtime fault (hosted) | MEDIUM | Arena escape via value-typed struct param launder |

## Verification

Run all reproducers and confirm 6 compile clean, 2 reject (one for
GCC syntax, one for missing flag):

```sh
for f in tests/audit_2026-06-09/gap_*.zer; do
  ./zerc "$f" -o /dev/null >/dev/null 2>&1 && \
    echo "OPEN  $(basename "$f" .zer)" || \
    echo "REJECT (loud / wrong reason) $(basename "$f" .zer)"
done
```

Expected:
```
OPEN  gap_alloc_ptr_global_alias_uaf
OPEN  gap_arena_escape_via_struct_copy
OPEN  gap_arr_var_index_dfree
REJECT (loud / wrong reason) gap_container_ptr_optional_arg
OPEN  gap_funcptr_double_free
REJECT (loud / wrong reason) gap_nostrict_mmio_drops_runtime
OPEN  gap_orelse_overwrite_leak
OPEN  gap_ptrcast_concrete_unrelated
```

To verify GAP-2:
```sh
./zerc tests/audit_2026-06-09/gap_nostrict_mmio_drops_runtime.zer \
       --no-strict-mmio --emit-c -o /tmp/gap2.c
grep -E "mmio range|unaligned address" /tmp/gap2.c
# Expected: 1 occurrence (in the _zer_trap function definition)
# Actual: 1 — confirms no inline check at the @inttoptr site
```

To verify GAP-1 runtime behavior:
```sh
./zerc tests/audit_2026-06-09/gap_ptrcast_concrete_unrelated.zer \
       -o /tmp/gap1 && /tmp/gap1; echo "exit=$?"
# Expected (gap closed): exit=133 (ZER trap) or compile rejection
# Actual: exit=42 — value laundered via type confusion
```
