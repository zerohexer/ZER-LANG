#!/bin/bash
# audit_docs_budget.sh — the documents that must stay SMALL have a byte budget.
#
# CLAUDE.md is loaded into every session; it reached 308 KB of dated case studies
# before anyone noticed, because nothing measured it. limitations.md is meant to be
# read whole. compiler-internals.md had grown to 1 MB by appending a dated section
# per session. A rule that is not gated drifts, so each has a budget here.
#
# A budget is a RATCHET, not a target: lower it when the file shrinks, and raise it
# only as a decision you can defend in the commit message. The remedy when a file is
# over budget is never "raise the number" by default —
#   CLAUDE.md                    one or two lines there, the story in docs/session-playbook.md
#   docs/limitations.md          move closed entries to docs/limitations-closed.md
#   docs/multi-site-classes.md   tighten rows; history belongs in BUGS-FIXED.md
#   docs/compiler-internals.md   move a section that became history to the END of
#                                docs/compiler-internals-history.md, or tighten one in place
#
# The archives (limitations-closed, compiler-internals-history,
# session-playbook) are grep-only and have no budget.
#
# Usage: bash tools/audit_docs_budget.sh
set -u
echo "=== Document budget audit (always-loaded and read-whole files stay small) ==="
fail=0
check() {   # file  budget  why
    local f="$1" max="$2" why="$3"
    [ -f "$f" ] || { echo "FAIL — $f not found"; fail=1; return; }
    local sz; sz=$(wc -c < "$f")
    if [ "$sz" -gt "$max" ]; then
        echo "FAIL — $f is $sz bytes, budget $max ($why). See the remedies at the top of"
        echo "       tools/audit_docs_budget.sh."
        fail=1
    else
        printf '  %-32s %7d / %7d bytes\n' "$f" "$sz" "$max"
    fi
}
check CLAUDE.md                    60000  "loaded into every session"
check docs/limitations.md          150000 "meant to be read whole"
check docs/multi-site-classes.md   110000 "the registry; its index is read before any safety edit"
check docs/compiler-internals.md   722000 "ratchet: history moves to compiler-internals-history.md"
[ "$fail" -ne 0 ] && exit 1
echo "OK — every budgeted document is within its size budget."
