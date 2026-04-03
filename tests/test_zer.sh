#!/bin/bash
# Run all .zer integration tests
# tests/zer/     — must compile + run + exit 0 (positive tests)
# tests/zer_fail/ — must FAIL to compile (negative tests)

ZERC="./zerc"
PASS=0
FAIL=0
TOTAL=0

echo "=== ZER Integration Tests (positive) ==="

for f in tests/zer/*.zer; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .zer)
    TOTAL=$((TOTAL + 1))
    $ZERC "$f" --run 2>/dev/null
    ret=$?
    if [ $ret -eq 0 ]; then
        PASS=$((PASS + 1))
        echo "  PASS: $name"
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL: $name (exit $ret)"
        $ZERC "$f" --run 2>&1 | head -5
    fi
    rm -f "${f%.zer}.c" "${f%.zer}.exe" "${f%.zer}" 2>/dev/null
done

echo ""
echo "=== ZER Integration Tests (negative — must fail) ==="

for f in tests/zer_fail/*.zer; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .zer)
    TOTAL=$((TOTAL + 1))
    # Compile only (not --run), expect failure
    $ZERC "$f" -o /dev/null 2>/dev/null
    ret=$?
    if [ $ret -ne 0 ]; then
        PASS=$((PASS + 1))
        echo "  PASS: $name (correctly rejected)"
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL: $name (should have been rejected but compiled!)"
    fi
    rm -f "${f%.zer}.c" 2>/dev/null
done

echo ""
echo "=== Results ==="
echo "  Passed: $PASS"
echo "  Failed: $FAIL"
echo "  Total:  $TOTAL"

if [ $FAIL -gt 0 ]; then
    echo ""
    echo "ZER INTEGRATION TESTS FAILED"
    exit 1
fi

echo ""
echo "ALL ZER TESTS PASSED"
