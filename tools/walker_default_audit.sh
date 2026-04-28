#!/bin/bash
# Walker default-clause audit (Stage 2 Part B, 2026-04-27).
#
# Background: walkers that switch on node->kind / inst->op / expr->kind
# with a `default:` clause silently skip any new kind added to the AST/IR.
# This causes the "missing-case silent gap" class of bugs — see Gaps 28,
# 29, 31, 36, 41, 42, 44 in docs/4-27-2026-gaps.md, all closed in Stage 2
# Part A but conceptually preventable by mechanical exhaustiveness.
#
# This script flags every `default:` inside a switch on `*->kind` or
# `inst->op` across the safety-critical compiler files. Output is a
# punch list — review each, classify SAFE/UNSAFE/CRITICAL, and fix
# CRITICAL ones by enumerating the missing cases.
#
# Run from repo root: bash tools/walker_default_audit.sh
# Exit 0 = no defaults remain in safety-critical walkers.
# Exit 1 = defaults found, listed with file:line.

set -u  # don't use -e — we want to keep going through all files

cd "$(dirname "$0")/.."

# Files to audit. Add new safety-critical compiler files here as they land.
FILES="checker.c zercheck.c zercheck_ir.c ir_lower.c emitter.c parser.c"

# Detection strategy: find lines containing `switch (...->kind)` or
# `switch (...->op)`, then scan forward up to N lines for `default:`.
# A real exhaustive walker should never have `default:`.
#
# Excluded: token-op switches (binary.op, unary.op, assign.op) — these
# dispatch on TokenKind which has 100+ values; intentional defaults
# for "any unhandled operator" are a legitimate pattern.

WINDOW=400  # max lines a switch can span — tune if any walker is huger

found_count=0
declare -a findings

for f in $FILES; do
    [ -f "$f" ] || continue
    # Find all switch lines
    # Strategy: for each `default:` line, scan UPWARD to find the
    # immediately-enclosing switch (any switch, not just kind/op). Then
    # check whether that switch is on `->kind` or `->op` (and not on
    # token-op fields). This avoids the false-positive of reporting an
    # outer switch when the default is in an inner switch.
    while IFS=: read -r d_line _; do
        # Find the switch line above this default (closest preceding `switch (`)
        sw_line=$(awk -v target=$d_line 'NR<target && /switch *\(/ { last=NR } END { print last }' "$f")
        [ -z "$sw_line" ] && continue
        sw_text=$(awk -v n=$sw_line 'NR==n { print }' "$f")
        # Only flag if the switch is on kind or op fields (use awk to
        # avoid bash redirect interpretation of `->` token in grep args)
        is_kind=$(awk -v t="$sw_text" 'BEGIN { if (t ~ /->kind|->op/) print "1"; else print "0" }')
        [ "$is_kind" = "0" ] && continue
        # Skip token-op switches (binary.op / unary.op / assign.op /
        # inst->op_token — all dispatch on TokenKind which has 100+
        # values; intentional defaults are legitimate)
        is_tok=$(awk -v t="$sw_text" 'BEGIN { if (t ~ /binary\.op|unary\.op|assign\.op|op_token/) print "1"; else print "0" }')
        [ "$is_tok" = "1" ] && continue
        findings+=("$f:$d_line (switch at line $sw_line)")
        found_count=$((found_count + 1))
    done < <(grep -nE '^[[:space:]]*default:' "$f")
done

if [ "$found_count" -eq 0 ]; then
    echo "OK — no default: clauses remain in node-kind / op-kind switches."
    echo ""
    echo "Stage 2 Part B COMPLETE (2026-04-28): all 42 sites converted."
    echo "GCC -Wswitch enforces exhaustiveness on every safety-critical walker."
    exit 0
fi

# Progress tracking: Stage 2 Part B started 2026-04-28 with 42 findings.
# Update INITIAL_COUNT when re-baselining.
INITIAL_COUNT=42
closed=$((INITIAL_COUNT - found_count))
pct=0
if [ "$INITIAL_COUNT" -gt 0 ]; then
    pct=$((closed * 100 / INITIAL_COUNT))
fi

echo "=== Walker default: audit ($found_count finding(s)) ==="
echo ""
echo "Stage 2 Part B progress: $closed of $INITIAL_COUNT closed (~${pct}%)."
echo ""
echo "Each entry below is a switch on ->kind or ->op that has a default:"
echo "clause. Review whether the default is SAFE (explicit no-op for leaf"
echo "nodes that semantically can't appear) or UNSAFE (silent skip of new"
echo "node kinds). Convert UNSAFE cases to enumerated case labels so GCC"
echo "-Wswitch errors when a new kind is added."
echo ""
for f in "${findings[@]}"; do
    echo "  $f"
done
echo ""
echo "Stage 2 Part B (mechanical -Wswitch enforcement) tracks fixing each."
echo "Some sites are TYPE_KIND or IR_OP switches with intentional default —"
echo "review case-by-case before conversion."
exit 1
