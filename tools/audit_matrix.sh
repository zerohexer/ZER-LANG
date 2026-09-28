#!/bin/bash
# Flag-handler matrix — control-flow construct x context, asserted BEHAVIOURALLY.
#
# Each cell compiles one program and asserts the checker's verdict: a banned
# construct in a context (return in a defer body, spawn in @critical, yield in a
# sync function, ...) must be refused FOR ITS OWN REASON (the substring), and the
# boundary cells (a loop NESTED inside the defer / @critical body may break) must
# compile.
#
# This replaces the old grep of checker.c line windows, which had drifted onto
# decoy `case NODE_X:` labels and reported 16 false positives (limitations.md,
# closed 2026-09-25). A verdict cannot drift that way.
#
# Usage: bash tools/audit_matrix.sh [path/to/zerc]
ZERC="${1:-./zerc}"
[ -x "$ZERC" ] || { echo "ERROR: $ZERC not found"; exit 1; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0; fail=0

# cell NAME reject|compile 'SUBSTRING' 'PROGRAM'
cell() {
    local name="$1" want="$2" sub="$3" prog="$4"
    printf '%s\n' "$prog" > "$T/$name.zer"
    local out rc
    out=$("$ZERC" "$T/$name.zer" -o "$T/$name.c" 2>&1); rc=$?
    local ok=0
    if [ "$want" = reject ]; then
        [ $rc -ne 0 ] && printf '%s' "$out" | grep -qF -- "$sub" && ok=1
    else
        [ $rc -eq 0 ] && ok=1
    fi
    if [ $ok -eq 1 ]; then pass=$((pass + 1)); else
        fail=$((fail + 1))
        echo "  MISMATCH $name (want $want${sub:+: '$sub'}): $(printf '%s' "$out" | grep -m1 error | cut -c1-140)"
    fi
}

H='u32 g = 0; void w() { } async void aw() { yield; }'
# defer body: control flow that would leave the cleanup, or suspend inside it
cell defer_return   reject "cannot use 'return' inside defer"   "$H u32 main() { defer { return; } return 0; }"
cell defer_break    reject "cannot use 'break' inside defer"    "$H u32 main() { for (u32 i = 0; i < 2; i += 1) { defer { break; } } return 0; }"
cell defer_continue reject "cannot use 'continue' inside defer" "$H u32 main() { for (u32 i = 0; i < 2; i += 1) { defer { continue; } } return 0; }"
cell defer_goto     reject "cannot use 'goto' inside defer"     "$H u32 main() { defer { goto out; } out: return 0; }"
cell defer_yield    reject "cannot yield inside defer"          "$H async void a() { defer { yield; } } u32 main() { return 0; }"
cell defer_await    reject "inside defer"                       "$H async void a() { defer { await g == 1; } } u32 main() { return 0; }"
# @critical: leaving it skips the interrupt re-enable
cell crit_return    reject "inside @critical"   "$H u32 main() { @critical { return 1; } return 0; }"
cell crit_break     reject "inside @critical"   "$H u32 main() { for (u32 i = 0; i < 2; i += 1) { @critical { break; } } return 0; }"
cell crit_continue  reject "inside @critical"   "$H u32 main() { for (u32 i = 0; i < 2; i += 1) { @critical { continue; } } return 0; }"
cell crit_goto      reject "inside @critical"   "$H u32 main() { @critical { goto out; } out: return 0; }"
cell crit_yield     reject "inside @critical"   "$H async void a() { @critical { yield; } } u32 main() { return 0; }"
cell crit_await     reject "inside @critical"   "$H async void a() { @critical { await g == 1; } } u32 main() { return 0; }"
cell crit_spawn     reject "inside @critical"   "$H u32 main() { @critical { spawn w(); } return 0; }"
# loop / async / interrupt / naked contexts
cell noloop_break    reject "'break' outside of loop"    "$H u32 main() { break; return 0; }"
cell noloop_continue reject "'continue' outside of loop" "$H u32 main() { continue; return 0; }"
cell sync_yield      reject "only allowed inside async"  "$H void a() { yield; } u32 main() { return 0; }"
cell sync_await      reject "async"                      "$H void a() { await g == 1; } u32 main() { return 0; }"
cell async_spawn     reject "inside async function"      "$H async void a() { spawn w(); yield; } u32 main() { return 0; }"
cell isr_spawn       reject "inside interrupt handler"   "$H interrupt USART1 { spawn w(); } u32 main() { return 0; }"
cell isr_alloc       reject "not allowed in interrupt handler" "struct T { u32 v; } interrupt USART1 { ?*T p = alloc(T); } u32 main() { return 0; }"
cell naked_stmt      reject "naked function must only contain" "naked void n() { u32 x = 1; } u32 main() { return 0; }"
# BOUNDARY: a loop nested inside the body owns its own break; a spawn in a defer
# body is a plain call at exit; ordinary loop control flow.
cell ok_defer_innerloop compile "" "$H u32 main() { defer { for (u32 i = 0; i < 2; i += 1) { if (i == 1) { break; } g += 1; } } return 0; }"
cell ok_crit_innerloop  compile "" "$H u32 main() { @critical { for (u32 i = 0; i < 2; i += 1) { if (i == 1) { break; } g += 1; } } return 0; }"
cell ok_defer_spawn     compile "" "$H u32 main() { defer { spawn w(); } return 0; }"
cell ok_loop_break      compile "" "$H u32 main() { for (u32 i = 0; i < 2; i += 1) { if (i == 1) { continue; } break; } return 0; }"

echo "flag-handler matrix: $pass ok, $fail mismatch"
if [ $fail -eq 0 ]; then echo "OK — every construct x context verdict holds."; exit 0; fi
exit 1
