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

# Automated gap detection — safety contracts encoded as rules
# Each rule: "node MUST check flag — reason"
# Format: NODE_NAME:flag_name:reason
RULES="
NODE_RETURN:defer_depth:return in defer corrupts control flow
NODE_RETURN:critical_depth:return in @critical skips interrupt re-enable
NODE_BREAK:defer_depth:break in defer corrupts control flow
NODE_BREAK:critical_depth:break in @critical skips interrupt re-enable
NODE_BREAK:in_loop:break outside loop is meaningless
NODE_CONTINUE:defer_depth:continue in defer corrupts control flow
NODE_CONTINUE:critical_depth:continue in @critical skips interrupt re-enable
NODE_CONTINUE:in_loop:continue outside loop is meaningless
NODE_GOTO:defer_depth:goto in defer corrupts control flow
NODE_GOTO:critical_depth:goto in @critical skips interrupt re-enable
NODE_YIELD:defer_depth:yield in defer corrupts Duff device state machine
NODE_YIELD:critical_depth:yield in @critical holds lock across suspend
NODE_AWAIT:defer_depth:await in defer corrupts Duff device state machine
NODE_AWAIT:critical_depth:await in @critical holds lock across suspend
NODE_SPAWN:critical_depth:thread creation with interrupts disabled
NODE_SPAWN:in_interrupt:pthread_create in ISR is unsafe
"

bug_count=0
for rule in $RULES; do
    [ -z "$rule" ] && continue
    node=$(echo "$rule" | cut -d: -f1)
    flag=$(echo "$rule" | cut -d: -f2)
    reason=$(echo "$rule" | sed 's/^[^:]*:[^:]*://')

    case_line=$(grep -n "case $node:" "$FILE" | awk -F: '$1 > 6700 && $1 < 8200 {print $1; exit}')
    [ -z "$case_line" ] && continue

    end_line=$((case_line + 200))
    handler=$(sed -n "${case_line},${end_line}p" "$FILE" | head -200)
    handler_end=$(echo "$handler" | grep -n "^    case \|^    default:" | head -2 | tail -1 | cut -d: -f1)
    if [ -n "$handler_end" ] && [ "$handler_end" -gt 1 ]; then
        handler=$(echo "$handler" | head -$((handler_end - 1)))
    fi

    short=$(echo $node | sed 's/NODE_//')
    if ! echo "$handler" | grep -q "$flag"; then
        echo "  BUG: $short missing $flag check (line $case_line)"
        echo "       reason: $reason"
        bug_count=$((bug_count + 1))
    fi
done

if [ "$bug_count" -eq 0 ]; then
    echo "  CLEAN — all safety contracts satisfied"
fi
echo ""
echo "Total gaps: $bug_count"

echo ""
echo "Done."
