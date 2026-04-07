#!/bin/bash
# Run all rust_tests/*.zer — translated from Rust test suite
# Positive tests (no "reject" in name): must compile + run + exit 0
# Negative tests ("reject" in name): must fail to compile

ZERC="${1:-./zerc}"
PASS=0
FAIL=0

echo "=== Rust-equivalent safety tests ==="

for f in "$(dirname "$0")"/*.zer; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .zer)

    if echo "$name" | grep -qE "reject|div_zero|mod_zero|bounds_oob|uaf_handle|double_free|dangling_return|narrowing_reject|null_deref|maybe_freed"; then
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
