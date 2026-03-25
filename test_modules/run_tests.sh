#!/bin/bash
ZERC="../zerc"
if [ ! -f "$ZERC" ] && [ -f "../zerc.exe" ]; then ZERC="../zerc.exe"; fi

PASS=0
FAIL=0

run_test() {
    local name=$1
    local expected=$2
    $ZERC $name.zer -o _$name.c 2>/dev/null
    if [ $? -ne 0 ]; then echo "  FAIL: $name (zerc failed)"; FAIL=$((FAIL+1)); return; fi
    gcc -std=c99 -w -o _$name.exe _$name.c 2>/dev/null
    if [ $? -ne 0 ]; then echo "  FAIL: $name (gcc failed)"; FAIL=$((FAIL+1)); return; fi
    ./_$name.exe
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
rm -f _*.c _*.exe

echo "=== Module tests: $PASS passed, $FAIL failed ==="
exit $FAIL
