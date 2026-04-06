#!/bin/bash
ZERC="../zerc"
if [ ! -f "$ZERC" ] && [ -f "../zerc.exe" ]; then ZERC="../zerc.exe"; fi

# Platform-specific executable extension
EXT=""
if [[ "$OSTYPE" == "msys" ]] || [[ "$OSTYPE" == "cygwin" ]] || [[ -n "$WINDIR" ]]; then
    EXT=".exe"
fi

PASS=0
FAIL=0

run_test() {
    local name=$1
    local expected=$2
    $ZERC $name.zer -o _$name.c 2>/dev/null
    if [ $? -ne 0 ]; then echo "  FAIL: $name (zerc failed)"; FAIL=$((FAIL+1)); return; fi
    gcc -std=c99 -w -o _${name}${EXT} _$name.c 2>/dev/null
    if [ $? -ne 0 ]; then echo "  FAIL: $name (gcc failed)"; FAIL=$((FAIL+1)); return; fi
    ./_${name}${EXT}
    local got=$?
    if [ "$got" -eq "$expected" ]; then
        PASS=$((PASS+1))
    else
        echo "  FAIL: $name (expected $expected, got $got)"
        FAIL=$((FAIL+1))
    fi
}

run_test main 37
run_test app 17
run_test diamond 44
run_test use_types 50
run_test use_defs 42
run_test diamond2 30
run_test collision_test 170
run_test static_coll 30
run_test gcoll 30
run_test transitive 3
run_test opaque_wrap 0
run_test opaque_deep_ok 0
run_test stress_diamond 0
run_test stress_game 0
run_test range_user 0
run_test defer_user 0
run_test defer_deep_user 0

# Cross-module range proving: NO auto-guard warnings should fire
output=$($ZERC range_user.zer --run 2>&1)
ret=$?
if [ $ret -eq 0 ] && ! echo "$output" | grep -qi "warning"; then
    PASS=$((PASS+1))
else
    echo "  FAIL: range_user no-warn (auto-guard fired on cross-module proven index)"
    echo "$output" | grep -i "warning" | head -3
    FAIL=$((FAIL+1))
fi
rm -f range_user.c range_user range_user.exe 2>/dev/null

# Cross-module *opaque negative: double-free and UAF must be rejected
$ZERC opaque_wrap_df.zer -o /dev/null 2>/dev/null
if [ $? -ne 0 ]; then PASS=$((PASS+1)); else echo "  FAIL: opaque_wrap_df (should reject double-free)"; FAIL=$((FAIL+1)); fi
$ZERC opaque_wrap_uaf.zer -o /dev/null 2>/dev/null
if [ $? -ne 0 ]; then PASS=$((PASS+1)); else echo "  FAIL: opaque_wrap_uaf (should reject UAF)"; FAIL=$((FAIL+1)); fi
$ZERC opaque_deep_df.zer -o /dev/null 2>/dev/null
if [ $? -ne 0 ]; then PASS=$((PASS+1)); else echo "  FAIL: opaque_deep_df (should reject 3-layer double-free)"; FAIL=$((FAIL+1)); fi
$ZERC opaque_deep_uaf.zer -o /dev/null 2>/dev/null
if [ $? -ne 0 ]; then PASS=$((PASS+1)); else echo "  FAIL: opaque_deep_uaf (should reject 3-layer UAF)"; FAIL=$((FAIL+1)); fi

# BUG-087: imported interrupt — compile-only (interrupt attr is ARM-specific)
$ZERC use_hal.zer -o _use_hal.c 2>/dev/null
if [ $? -eq 0 ] && grep -q "USART1_IRQHandler" _use_hal.c; then
    echo "  use_hal: interrupt handler emitted (compile-only)"
    PASS=$((PASS+1))
else
    echo "  FAIL: use_hal (interrupt handler not emitted)"
    FAIL=$((FAIL+1))
fi

# cleanup
rm -f _*.c _*.exe _*.o _*[!.]*

echo "=== Module tests: $PASS passed, $FAIL failed ==="
exit $FAIL
