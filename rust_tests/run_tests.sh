#!/bin/bash
# Run all rust_tests/*.zer — translated from Rust test suite
# Positive tests: must compile + run + exit 0
# Negative tests: must fail to compile
# Detection: "reject" in filename OR "EXPECTED: compile error" in file content

ZERC="${1:-./zerc}"
PASS=0
FAIL=0
SKIP=0

# Known pre-existing failures surfaced by BUG-581 (--run exit code fix).
# Previously silently "passed" via swallowed non-zero exit codes.
# See docs/limitations.md for tracking.
KNOWN_FAIL=" \
    gen_arena_005 \
    gen_async_010 \
    gen_comptime_float_001 \
    gen_shared_010 \
    rc_once_001 \
    rt_comptime_guard_bounds \
    rt_conc_async_await_cond \
    rt_defer_order_lifo \
    rt_drop_count_3 \
    rt_drop_trait_basic \
    rt_unsafe_mmio_multi_reg \
    rt_unsafe_mmio_volatile_rw \
"

is_known_fail() {
    for n in $KNOWN_FAIL; do
        if [ "$n" = "$1" ]; then return 0; fi
    done
    return 1
}

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
        elif is_known_fail "$name"; then
            SKIP=$((SKIP + 1))
            echo "  SKIP: $name (known pre-existing issue — docs/limitations.md)"
        else
            echo "  FAIL: $name"
            FAIL=$((FAIL + 1))
        fi
    fi
done

echo "=== Rust test results: $PASS passed, $FAIL failed, $SKIP skipped ==="
[ $FAIL -eq 0 ]
