#!/bin/bash
# Run all .zer integration tests
# Each .zer file is compiled with zerc --run and must exit 0

ZERC="./zerc"
DIR="tests/zer"
PASS=0
FAIL=0
TOTAL=0

echo "=== ZER Integration Tests ==="

for f in "$DIR"/*.zer; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .zer)
    TOTAL=$((TOTAL + 1))
    # Compile and run, capture exit code (suppress warnings)
    $ZERC "$f" --run 2>/dev/null
    ret=$?
    if [ $ret -eq 0 ]; then
        PASS=$((PASS + 1))
        echo "  PASS: $name"
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL: $name (exit $ret)"
        # Re-run with stderr visible for debugging
        $ZERC "$f" --run 2>&1 | head -5
    fi
    # Clean up generated files
    rm -f "${f%.zer}.c" "${f%.zer}.exe" "${f%.zer}" 2>/dev/null
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
