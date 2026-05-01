#!/bin/bash
# Run all .zer integration tests
# tests/zer/       — must compile + run + exit 0  (positive tests)
# tests/zer_fail/  — must FAIL to compile         (negative tests)
# tests/zer_trap/  — must compile + run + EXIT NON-ZERO (runtime safety traps)
# tests/zer_proof/ — theorem-linked regression tests (see README)
#                    — *_bad.zer must FAIL to compile
#                    — other .zer files must compile + run + exit 0
# Usage: test_zer.sh [extra-flags]
#   e.g. test_zer.sh --some-future-flag

ZERC="./zerc"
EXTRA_FLAGS="$1"
PASS=0
FAIL=0
SKIP=0
TOTAL=0

# Tests with known pre-existing failures, documented in docs/limitations.md.
# Empty — BUG-590 closed the shadowing case, everything in tests/zer/
# compiles + runs + exits 0.
KNOWN_FAIL_POSITIVE=""

is_known_fail() {
    local needle="$1"
    for name in $KNOWN_FAIL_POSITIVE; do
        if [ "$name" = "$needle" ]; then return 0; fi
    done
    return 1
}

echo "=== ZER Integration Tests (positive) ${EXTRA_FLAGS:+[$EXTRA_FLAGS]} ==="

for f in tests/zer/*.zer; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .zer)
    TOTAL=$((TOTAL + 1))
    # Per-file flags: first line `// zerc-flags: --foo --bar=baz` is parsed
    # and appended to ZERC invocation. Used for tests that need specific
    # target features (e.g., --target-features=avx512f).
    file_flags=$(head -1 "$f" | grep -oE '// zerc-flags: .*$' | sed 's|// zerc-flags: ||')
    $ZERC "$f" $EXTRA_FLAGS $file_flags --run 2>/dev/null
    ret=$?
    if [ $ret -eq 0 ]; then
        PASS=$((PASS + 1))
        echo "  PASS: $name"
    else
        if is_known_fail "$name"; then
            SKIP=$((SKIP + 1))
            echo "  SKIP: $name (exit $ret — known pre-existing issue, docs/limitations.md)"
        else
            FAIL=$((FAIL + 1))
            echo "  FAIL: $name (exit $ret)"
            $ZERC "$f" $EXTRA_FLAGS $file_flags --run 2>&1 | head -5
        fi
    fi
    rm -f "${f%.zer}.c" "${f%.zer}.exe" "${f%.zer}" 2>/dev/null
done

echo ""
echo "=== ZER Runtime-Trap Tests (must compile + trap at runtime) ==="

for f in tests/zer_trap/*.zer; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .zer)
    TOTAL=$((TOTAL + 1))
    # Runtime-trap tests: compile clean, run, EXPECT non-zero exit (SIGTRAP = 133).
    $ZERC "$f" $EXTRA_FLAGS --run 2>/dev/null
    ret=$?
    if [ $ret -ne 0 ]; then
        PASS=$((PASS + 1))
        echo "  PASS: $name (correctly trapped, exit $ret)"
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL: $name (should have trapped at runtime but exited 0)"
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
echo "=== ZER Proof-linked tests (tests/zer_proof/) ==="
# Each *.zer here exercises a proven Iris theorem.
# *_bad.zer = violation program (must FAIL to compile).
# Others = safe programs (must compile + run + exit 0).

for f in tests/zer_proof/*.zer; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .zer)
    TOTAL=$((TOTAL + 1))
    if [[ "$name" == *_bad ]]; then
        # Negative: must fail to compile.
        $ZERC "$f" -o /dev/null 2>/dev/null
        ret=$?
        if [ $ret -ne 0 ]; then
            PASS=$((PASS + 1))
            echo "  PASS: $name (correctly rejected — theorem holds)"
        else
            FAIL=$((FAIL + 1))
            echo "  FAIL: $name (PROOF VIOLATION — compiler accepted a program the Iris theorem rejects)"
        fi
    else
        # Positive: must compile + run + exit 0.
        $ZERC "$f" --run 2>/dev/null
        ret=$?
        if [ $ret -eq 0 ]; then
            PASS=$((PASS + 1))
            echo "  PASS: $name"
        else
            FAIL=$((FAIL + 1))
            echo "  FAIL: $name (safe program should compile + run)"
        fi
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
nowarn_check tests/zer/no_warn_u64_atomic_64bit.zer "no-warn-u64-atomic-64bit-target"

echo ""
echo "=== Results ==="
echo "  Passed:  $PASS"
echo "  Failed:  $FAIL"
echo "  Skipped: $SKIP (known issues — see docs/limitations.md)"
echo "  Total:   $TOTAL"

if [ $FAIL -gt 0 ]; then
    echo ""
    echo "ZER INTEGRATION TESTS FAILED"
    exit 1
fi

echo ""
echo "ALL ZER TESTS PASSED"
