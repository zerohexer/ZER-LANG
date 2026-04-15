#!/bin/bash
# Run all .zer integration tests
# tests/zer/     — must compile + run + exit 0 (positive tests)
# tests/zer_fail/ — must FAIL to compile (negative tests)
# Usage: test_zer.sh [extra-flags]
#   e.g. test_zer.sh --use-ir

ZERC="./zerc"
EXTRA_FLAGS="$1"
PASS=0
FAIL=0
TOTAL=0

echo "=== ZER Integration Tests (positive) ${EXTRA_FLAGS:+[$EXTRA_FLAGS]} ==="

for f in tests/zer/*.zer; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .zer)
    TOTAL=$((TOTAL + 1))
    $ZERC "$f" $EXTRA_FLAGS --run 2>/dev/null
    ret=$?
    if [ $ret -eq 0 ]; then
        PASS=$((PASS + 1))
        echo "  PASS: $name"
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL: $name (exit $ret)"
        $ZERC "$f" $EXTRA_FLAGS --run 2>&1 | head -5
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
echo "=== ZER Warning Verification (must compile + warn + exit 0) ==="

# Verify auto-guard warnings are emitted for dynamic array UAF
warn_check() {
    local f="$1" pattern="$2" name="$3"
    TOTAL=$((TOTAL + 1))
    output=$($ZERC "$f" --run 2>&1)
    ret=$?
    if [ $ret -eq 0 ] && echo "$output" | grep -q "$pattern"; then
        PASS=$((PASS + 1))
        echo "  PASS: $name (warning verified)"
    elif [ $ret -ne 0 ]; then
        FAIL=$((FAIL + 1))
        echo "  FAIL: $name (exit $ret, expected 0)"
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL: $name (no warning matching '$pattern')"
    fi
    rm -f "${f%.zer}.c" "${f%.zer}.exe" "${f%.zer}" 2>/dev/null
}

warn_check tests/zer/dyn_array_autoguard_crash.zer "auto-guard inserted" "autoguard-warning-emitted"
warn_check tests/zer/dyn_array_guard.zer "auto-guard inserted" "dynguard-warning-emitted"

echo ""
echo "=== ZER No-Warning Verification (must compile + NO warnings + exit 0) ==="

nowarn_check() {
    local f="$1" name="$2"
    TOTAL=$((TOTAL + 1))
    output=$($ZERC "$f" --run 2>&1)
    ret=$?
    if [ $ret -eq 0 ] && ! echo "$output" | grep -qi "warning"; then
        PASS=$((PASS + 1))
        echo "  PASS: $name (no warnings, zero overhead)"
    elif [ $ret -ne 0 ]; then
        FAIL=$((FAIL + 1))
        echo "  FAIL: $name (exit $ret, expected 0)"
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL: $name (unexpected warning — auto-guard fired on proven index)"
        echo "$output" | grep -i "warning" | head -3
    fi
    rm -f "${f%.zer}.c" "${f%.zer}.exe" "${f%.zer}" 2>/dev/null
}

nowarn_check tests/zer/no_autoguard_proven.zer "no-autoguard-all-proven"
nowarn_check tests/zer/no_autoguard_stress.zer "no-autoguard-stress-31-accesses"
nowarn_check tests/zer/inline_call_range.zer "no-autoguard-inline-call"
nowarn_check tests/zer/inline_range_deep.zer "no-autoguard-deep-chain"
nowarn_check tests/zer/guard_clamp_range.zer "no-autoguard-guard-clamp"

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
