#!/bin/bash
# audit_limitations.sh — docs/limitations.md holds OPEN entries only.
#
# The file's own first rule was "entries removed once fixed". Nothing checked it, so
# closed entries were struck through and left in place until the file was 634 KB and
# finding out what was actually open meant reading everything that was not. A rule
# that is not gated drifts; this is the gate.
#
# FAILS when docs/limitations.md contains:
#   1. a section heading that declares itself closed (## CLOSED / DONE / FIXED /
#      RESOLVED / SUPERSEDED, or a struck-through heading);
#   2. a top-level list item that starts struck through (`3. ~~...~~ — CLOSED`);
#   3. more than LIMIT bytes — closed or unverified text is accumulating again.
#
# The remedy is in the file's header: move the closed entry to the END of
# docs/limitations-closed.md, and write what the fix left open as its own entry
# tagged `[residual of: ...]`. Raise LIMIT only as a conscious decision.
#
# Usage: bash tools/audit_limitations.sh [file]      (default docs/limitations.md)
set -u
F="${1:-docs/limitations.md}"
LIMIT=150000
[ -f "$F" ] || { echo "FAIL — $F not found"; exit 1; }

echo "=== Limitations ledger audit (open entries only) ==="
heads=$(grep -nE '^## (CLOSED|DONE|FIXED|RESOLVED|SUPERSEDED|~~)' "$F" || true)
items=$(grep -nE '^([0-9]+\.|-|\*) +~~' "$F" || true)
size=$(wc -c < "$F")
fail=0
if [ -n "$heads" ]; then
    echo "FAIL — closed SECTIONS in $F (move each to the end of docs/limitations-closed.md):"
    echo "$heads" | cut -c1-140 | sed 's/^/  /'
    fail=1
fi
if [ -n "$items" ]; then
    echo "FAIL — closed LIST ITEMS in $F (move each to docs/limitations-closed.md; what the"
    echo "fix left open becomes its own entry tagged [residual of: ...]):"
    echo "$items" | cut -c1-140 | sed 's/^/  /'
    fail=1
fi
if [ "$size" -gt "$LIMIT" ]; then
    echo "FAIL — $F is $size bytes (budget $LIMIT). It is meant to be read whole: move"
    echo "closed entries to docs/limitations-closed.md, or raise LIMIT in this script on purpose."
    fail=1
fi
[ "$fail" -ne 0 ] && exit 1
n=$(grep -c '^## ' "$F")
echo "OK — limitations.md holds open entries only ($n sections, $size bytes)."
