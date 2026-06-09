# Audit 2026-06-09 — open-gap reproducers (NOT auto-run)

These 7 `.zer` files reproduce silent safety gaps confirmed OPEN in main as of
2026-06-09 (originally found on branch claude/cool-johnson-6u360k). They are
NOT in tests/test_zer.sh — each currently compiles clean (or runs without trap),
which is exactly the bug. They are tripwires for the fix sessions tracked in
docs/limitations.md "OPEN — 6u360k audit (2026-06-09)". When a gap is fixed,
move its reproducer into tests/zer_fail/ (compile-time) or tests/zer_trap/
(runtime) so the suite guards it.

GAP-5 (orelse-reassignment overwrite leak) from the original audit is NOT here —
it was closed by BUG-734 (2026-06-09); see tests/zer_fail/overwrite_alive_handle_*.zer.
