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

# cleanup
rm -f _*.c _*.exe

echo "=== Module tests: $PASS passed, $FAIL failed ==="
exit $FAIL
