#!/bin/bash
# Run all rust_tests/*.zer — translated from Rust test suite
# Positive tests: must compile + run + exit 0
# Negative tests: must fail to compile
# Detection: "reject" in filename OR "EXPECTED: compile error" in file content

ZERC="${1:-./zerc}"
PASS=0
FAIL=0

echo "=== Rust-equivalent safety tests ==="

for f in "$(dirname "$0")"/*.zer; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .zer)

    # Check if this is a negative test (should be rejected by compiler)
    is_negative=false
    if echo "$name" | grep -q "reject"; then
        is_negative=true
    elif grep -q "EXPECTED: compile error" "$f" 2>/dev/null; then
        is_negative=true
    fi

    if $is_negative; then
        # Negative test — must be rejected
        if $ZERC "$f" -o /dev/null 2>/dev/null; then
            echo "  FAIL (should reject): $name"
            FAIL=$((FAIL + 1))
        else
            echo "  PASS: $name (correctly rejected)"
            PASS=$((PASS + 1))
        fi
    else
        # Positive test — must compile + run + exit 0
        if timeout 10 $ZERC "$f" --run 2>/dev/null; then
            echo "  PASS: $name"
            PASS=$((PASS + 1))
        else
            echo "  FAIL: $name"
            FAIL=$((FAIL + 1))
        fi
    fi
done

echo "=== Rust test results: $PASS passed, $FAIL failed ==="
[ $FAIL -eq 0 ]
