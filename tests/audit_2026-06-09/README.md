# Audit 2026-06-09 — open-gap reproducers (NOT auto-run)

These `.zer` files reproduce silent safety gaps confirmed OPEN in main as of
2026-06-09 (originally found on branch claude/cool-johnson-6u360k). They are
NOT in tests/test_zer.sh — each currently compiles clean (or runs without trap),
which is exactly the bug. They are tripwires for the fix sessions tracked in
docs/limitations.md "OPEN — 6u360k audit (2026-06-09)". When a gap is fixed,
move its reproducer into tests/zer_fail/ (compile-time) or tests/zer_trap/
(runtime) so the suite guards it.

CLOSED (reproducers moved/removed): GAP-5 orelse overwrite leak (BUG-734,
see tests/zer_fail/overwrite_alive_handle_*.zer); GAP-1 @ptrcast concrete type
confusion (BUG-735, see tests/zer_fail/ptrcast_concrete_confusion.zer);
GAP-2 --no-strict-mmio dropped runtime alignment trap (BUG-736, see
tests/zer_trap/inttoptr_unaligned_nostrict.zer); GAP-8 by-value struct param
laundering (BUG-737, see tests/zer_fail/arena_escape_struct_param.zer).
