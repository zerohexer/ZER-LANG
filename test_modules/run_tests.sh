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

# Multi-module: shared struct + spawn across modules (BUG-472 + BUG-473 fixed)
run_test shared_user 0
# BUG-865: spawn of an IMPORTED function emitted the unmangled name (link failure)
run_test spawn_user 0
# BUG-866/867: async and container(T) across a module boundary (both emitted names
# the use site could not name)
run_test xfeat_user 0
# Multi-module: move struct across modules
run_test move_user 0
# Multi-module: handle lifecycle across modules
run_test handle_user 0
# Multi-module: comptime functions across modules
run_test comptime_user 0
# Multi-module: enum + switch across modules
run_test enum_user 0

# BUG-921: the deferred whole-program passes used to be handed ONLY the main
# module's AST, so every imported module was silently exempt from them. Each
# case below compiled clean before the fix. Assert the REASON, not the exit
# code — several of these files can be rejected by unrelated rules.
expect_diag() {
    local name=$1; local want=$2; shift 2
    local out
    out=$($ZERC $name.zer "$@" -o /dev/null 2>&1)
    if echo "$out" | grep -qF "$want"; then
        PASS=$((PASS+1))
    else
        echo "  FAIL: $name (expected diagnostic containing: $want)"
        echo "$out" | head -3
        FAIL=$((FAIL+1))
    fi
}
expect_clean() {
    local name=$1; shift
    local out
    out=$($ZERC $name.zer "$@" -o _$name.c 2>&1)
    if [ $? -eq 0 ] && ! echo "$out" | grep -q "error"; then
        PASS=$((PASS+1))
    else
        echo "  FAIL: $name (should compile clean)"
        echo "$out" | head -3
        FAIL=$((FAIL+1))
    fi
}
# arena declared+used in an import, never given a backing store -> alloc() null forever
expect_diag xmod_post_arena_user "never given a backing store"
# two shared structs in one statement, in an import, unreachable from main
expect_diag xmod_post_lock_user  "deadlock: single statement accesses both"
# *opaque provenance mismatch at a call site inside an import
expect_diag xmod_post_prov_user  "wrong *opaque type"
# --stack-limit against a 4096-byte frame that lives in an import
expect_diag xmod_post_hog_user   "function 'xmod_hog' local stack 4096 bytes exceeds" --stack-limit 512
# a call chain that CROSSES the module boundary must be SUMMED (200+200+200 words)
expect_diag xmod_post_stack_user "entry 'main' max call chain stack 2400 bytes exceeds" --stack-limit 1200
# an ISR declared in an import is an entry point AND part of the concurrent peak
expect_diag xmod_post_isr_user   "concurrent stack peak 1200 bytes exceeds" --stack-limit 800
# control: the same program under a budget it fits must NOT be rejected
expect_clean xmod_post_isr_user --stack-limit 2000
# BUG-922: a zercheck diagnostic for an imported module must name THAT file
expect_diag xmod_post_zc_user "xmod_post_zc_lib.zer:12: zercheck: use after free"
# controls for the two FALSE-POSITIVE directions the whole-program design must avoid:
#   resource declared+used in a module but initialised from main
#   two modules that each define their own same-named *opaque handler
expect_clean xmod_post_init_user
expect_clean xmod_post_coll_user

# cleanup
rm -f _*.c _*.exe _*.o _*[!.]*

echo "=== Module tests: $PASS passed, $FAIL failed ==="
exit $FAIL
