#!/bin/bash
# Flag-Handler Matrix — automated bug-finding via cross-reference
#
# For each control-flow NODE_ handler in checker.c, checks which
# context flags it validates. Missing checks = potential bugs.
#
# Usage: bash tools/audit_matrix.sh [checker.c path]

FILE="${1:-checker.c}"

if [ ! -f "$FILE" ]; then
    echo "ERROR: $FILE not found"
    exit 1
fi

echo "================================================================"
echo "FLAG-HANDLER MATRIX: control flow nodes x context flags"
echo "================================================================"
echo ""
echo "Legend: Y = checked, - = not checked, SET = node SETS the flag"
echo ""

# Flags and nodes
FLAGS="in_loop defer_depth critical_depth in_async in_interrupt in_naked"
NODES="NODE_RETURN NODE_BREAK NODE_CONTINUE NODE_GOTO NODE_YIELD NODE_AWAIT NODE_SPAWN NODE_DEFER NODE_CRITICAL NODE_ASM NODE_ONCE"

# Header
printf "%-14s" ""
for flag in $FLAGS; do
    printf "%-16s" "$flag"
done
echo ""

# Separator
printf "%-14s" ""
for flag in $FLAGS; do
    printf "%-16s" "---------------"
done
echo ""

# For each node, find handler in check_stmt (between lines 6700-8200)
for node in $NODES; do
    case_line=$(grep -n "case $node:" "$FILE" | awk -F: '$1 > 6700 && $1 < 8200 {print $1; exit}')

    if [ -z "$case_line" ]; then
        printf "%-14s (not found)\n" "$(echo $node | sed 's/NODE_//')"
        continue
    fi

    # Extract handler body (up to 200 lines, stop at next case)
    end_line=$((case_line + 200))
    handler=$(sed -n "${case_line},${end_line}p" "$FILE" | head -200)
    handler_end=$(echo "$handler" | grep -n "^    case \|^    default:" | head -2 | tail -1 | cut -d: -f1)
    if [ -n "$handler_end" ] && [ "$handler_end" -gt 1 ]; then
        handler=$(echo "$handler" | head -$((handler_end - 1)))
    fi

    printf "%-14s" "$(echo $node | sed 's/NODE_//')"
    for flag in $FLAGS; do
        if echo "$handler" | grep -q "${flag}++\|${flag}--\|${flag} = "; then
            printf "%-16s" "SET"
        elif echo "$handler" | grep -q "$flag"; then
            printf "%-16s" "Y"
        else
            printf "%-16s" "-"
        fi
    done
    echo "(line $case_line)"
done

echo ""
echo "================================================================"
echo "SAFETY CONTRACTS (expected checks):"
echo "================================================================"
echo ""
echo "EXIT nodes (return/break/continue/goto/yield/await):"
echo "  defer_depth    - MUST (banned in defer body)"
echo "  critical_depth - MUST (banned in @critical body)"
echo "  in_loop        - break/continue only"
echo ""
echo "SUSPEND nodes (yield/await):"
echo "  critical_depth - MUST (lock held across yield = deadlock)"
echo "  defer_depth    - MUST (defer across yield = state corruption)"
echo ""
echo "THREAD nodes (spawn):"
echo "  critical_depth - MUST (thread create with IRQ disabled)"
echo "  in_interrupt   - MUST (pthread_create in ISR = unsafe)"
echo ""

echo "================================================================"
echo "GAPS FOUND (- where Y expected):"
echo "================================================================"
echo ""

# Automated gap detection
for node in $NODES; do
    case_line=$(grep -n "case $node:" "$FILE" | awk -F: '$1 > 6700 && $1 < 8200 {print $1; exit}')
    [ -z "$case_line" ] && continue

    end_line=$((case_line + 200))
    handler=$(sed -n "${case_line},${end_line}p" "$FILE" | head -200)
    handler_end=$(echo "$handler" | grep -n "^    case \|^    default:" | head -2 | tail -1 | cut -d: -f1)
    if [ -n "$handler_end" ] && [ "$handler_end" -gt 1 ]; then
        handler=$(echo "$handler" | head -$((handler_end - 1)))
    fi

    short=$(echo $node | sed 's/NODE_//')

    # yield/await must check defer_depth and critical_depth
    if [ "$short" = "YIELD" ] || [ "$short" = "AWAIT" ]; then
        echo "$handler" | grep -q "defer_depth" || echo "  BUG: $short missing defer_depth check (line $case_line)"
        echo "$handler" | grep -q "critical_depth" || echo "  BUG: $short missing critical_depth check (line $case_line)"
    fi

    # spawn must check in_interrupt
    if [ "$short" = "SPAWN" ]; then
        echo "$handler" | grep -q "in_interrupt" || echo "  BUG: $short missing in_interrupt check (line $case_line)"
    fi
done

echo ""
echo "Done."
