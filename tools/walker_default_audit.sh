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

WINDOW=400  # max lines a switch can span — tune if any walker is huger

found_count=0
declare -a findings

for f in $FILES; do
    [ -f "$f" ] || continue
    # Find all switch lines
    while IFS=: read -r line text; do
        # Look for `default:` within the next $WINDOW lines, AND ensure we
        # don't cross into the NEXT switch (would give a false hit).
        end=$((line + WINDOW))
        # Inner switch detection: find next switch starting after current
        next_sw=$(awk -v start=$((line+1)) -v end=$end 'NR>=start && NR<=end && /switch *\(.*->kind\)|switch *\(.*->op\)/ { print NR; exit }' "$f")
        if [ -n "$next_sw" ]; then end=$((next_sw - 1)); fi
        # Find default: within the bounded range
        defaults=$(awk -v start=$((line+1)) -v end=$end 'NR>=start && NR<=end && /^[[:space:]]*default:/ { print NR }' "$f")
        for d in $defaults; do
            # Get one line of context for classification
            ctx=$(awk -v n=$d 'NR==n { print }' "$f")
            findings+=("$f:$d (switch starts at line $line)")
            found_count=$((found_count + 1))
        done
    done < <(grep -nE 'switch *\((.*->kind|.*->op)\)' "$f")
done

if [ "$found_count" -eq 0 ]; then
    echo "OK — no default: clauses remain in node-kind / op-kind switches."
    exit 0
fi

echo "=== Walker default: audit ($found_count finding(s)) ==="
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
exit 1
