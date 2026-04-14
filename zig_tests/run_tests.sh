#!/bin/bash
# Run all Zig-translated ZER tests
# zt_*.zer (not reject) — must compile + run + exit 0
# zt_*reject*.zer — must FAIL to compile

ZERC="${1:-./zerc}"
PASS=0
FAIL=0
TOTAL=0

echo "=== Zig-translated ZER tests ==="

for f in $(dirname "$0")/zt_*.zer; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .zer)
    TOTAL=$((TOTAL + 1))

    # Negative test: filename contains "reject"
    if echo "$name" | grep -q "reject"; then
        $ZERC "$f" -o /dev/null 2>/dev/null
        if [ $? -ne 0 ]; then
            PASS=$((PASS + 1))
            echo "  PASS: $name (correctly rejected)"
        else
            FAIL=$((FAIL + 1))
            echo "  FAIL: $name (should have been rejected but compiled!)"
        fi
    else
        # Positive test
        $ZERC "$f" --run 2>/dev/null
        if [ $? -eq 0 ]; then
            PASS=$((PASS + 1))
            echo "  PASS: $name"
        else
            FAIL=$((FAIL + 1))
            echo "  FAIL: $name (exit $?)"
            $ZERC "$f" --run 2>&1 | head -5
        fi
    fi
    rm -f "${f%.zer}.c" "${f%.zer}.exe" "${f%.zer}" 2>/dev/null
done

echo ""
echo "=== Zig test results: $PASS passed, $FAIL failed ==="

if [ $FAIL -gt 0 ]; then
    exit 1
fi
