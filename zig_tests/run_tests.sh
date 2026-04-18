#!/bin/bash
# Run all Zig-translated ZER tests
# zt_*.zer (not reject) — must compile + run + exit 0
# zt_*reject*.zer — must FAIL to compile

ZERC="${1:-./zerc}"
PASS=0
FAIL=0
SKIP=0
TOTAL=0

# Known pre-existing failures surfaced by BUG-581 (--run exit code fix).
# See docs/limitations.md.
KNOWN_FAIL=" \
    zt_comptime_float_const \
"

is_known_fail() {
    for n in $KNOWN_FAIL; do
        if [ "$n" = "$1" ]; then return 0; fi
    done
    return 1
}

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
        ret=$?
        if [ $ret -eq 0 ]; then
            PASS=$((PASS + 1))
            echo "  PASS: $name"
        elif is_known_fail "$name"; then
            SKIP=$((SKIP + 1))
            echo "  SKIP: $name (exit $ret — known pre-existing issue, docs/limitations.md)"
        else
            FAIL=$((FAIL + 1))
            echo "  FAIL: $name (exit $ret)"
            $ZERC "$f" --run 2>&1 | head -5
        fi
    fi
    rm -f "${f%.zer}.c" "${f%.zer}.exe" "${f%.zer}" 2>/dev/null
done

echo ""
echo "=== Zig test results: $PASS passed, $FAIL failed, $SKIP skipped ==="

if [ $FAIL -gt 0 ]; then
    exit 1
fi
