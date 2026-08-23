#!/bin/bash
# ============================================================================
# vrp_join_matrix.sh — the missing auto-gate for the VRP range-JOIN class.
#
# THE CLASS
# ---------
# Value-range propagation narrows an index so a bounds check can be ELIDED at
# zero cost. At a control-flow JOIN the ranges of the incoming paths must be
# UNIONed. Every control-flow kind performs that union in its own code, so a
# kind whose handler forgets it leaves a STALE narrow range and the check is
# elided on a value the program can actually reach — a silent out-of-bounds
# access with no diagnostic and no trap.
#
# It has happened repeatedly and always the same way: BH-18 #2, then nine
# separate leaks in the 2026-08-01 sweep (if-capture, orelse block, @once,
# defer, loop bodies, else-body, ...), each a different node kind, each found by
# a red team rather than by a gate. CLAUDE.md's multi-site table has carried
# "NO auto-gate — checklist every control-flow kind" for this row ever since.
# This is that gate.
#
# HOW IT MEASURES — deliberately in BOTH directions
# -------------------------------------------------
#   guarded  cells: an index reaches the access with a value the range CANNOT
#                   exclude. Compiled with AddressSanitizer and RUN: any
#                   heap/stack overflow report is a HOLE. Behavioural, so it
#                   cannot be fooled by a guard that is emitted but wrong.
#   elided   cells: an index the range CAN prove safe. The emitted C must
#                   contain NO bounds check for it. Without these a compiler
#                   that simply guards everything would score a perfect run,
#                   and the gate would be measuring nothing.
#
# Usage: bash tools/vrp_join_matrix.sh [zerc_path]
# Exit:  0 iff every cell matches its expectation.
# ============================================================================
set +e
ZERC="${1:-./zerc}"
DIR="$(mktemp -d)"
trap 'rm -rf "$DIR"' EXIT

# ASan is how the `guarded` half is measured. If this toolchain cannot build it,
# say so loudly rather than reporting a green run that tested nothing.
printf 'int main(void){return 0;}\n' > "$DIR/_asan.c"
if ! gcc -fsanitize=address "$DIR/_asan.c" -o "$DIR/_asan.exe" >/dev/null 2>&1; then
  echo "vrp_join_matrix: AddressSanitizer unavailable — cannot measure the"
  echo "                 guarded half. NOT reporting a pass."
  exit 2
fi

pass=0; fail=0; holes=""; overguard=""

# cell NAME EXPECT(guarded|elided) CODE
cell() {
  local name="$1" expect="$2" code="$3"
  printf '%s\n' "$code" > "$DIR/$name.zer"
  local out
  out=$("$ZERC" "$DIR/$name.zer" -o "$DIR/$name.c" 2>&1)
  if echo "$out" | grep -vE '^zerc: ' | grep -qE '(^|[: ])(error|zercheck):'; then
    fail=$((fail+1)); holes="$holes $name(rejected)"
    printf "  %-26s want=%-8s got=REJECTED  %s\n" "$name" "$expect" \
      "$(echo "$out" | grep -E 'error|zercheck' | head -1 | cut -c1-60)"
    return
  fi

  if [ "$expect" = elided ]; then
    # The access must carry no bounds check at all.
    local body
    body=$(sed -n "/[a-z_]* main(void)/,\$p" "$DIR/$name.c")
    if echo "$body" | grep -qE "_zer_bounds_check|>= 4u\)"; then
      fail=$((fail+1)); overguard="$overguard $name"
      printf "  %-26s want=elided   got=guarded  (range lost a fact it had)\n" "$name"
    else
      pass=$((pass+1)); printf "  %-26s want=elided   got=elided   ok\n" "$name"
    fi
    return
  fi

  if ! gcc -std=c99 -fwrapv -fno-strict-aliasing -fsanitize=address -g -O0 \
       "$DIR/$name.c" -o "$DIR/$name.exe" >/dev/null 2>&1; then
    fail=$((fail+1)); holes="$holes $name(gcc)"
    printf "  %-26s want=guarded  got=GCC-FAIL\n" "$name"
    return
  fi
  local run
  run=$(cd "$DIR" && ASAN_OPTIONS=detect_leaks=0 "./$name.exe" 2>&1)
  if echo "$run" | grep -q "AddressSanitizer"; then
    fail=$((fail+1)); holes="$holes $name"
    printf "  %-26s want=guarded  got=OOB       *** HOLE (check elided)\n" "$name"
  else
    pass=$((pass+1)); printf "  %-26s want=guarded  got=guarded  ok\n" "$name"
  fi
}

# Every `guarded` body has the same shape: `i` is 0 on one path and 100 on
# another, and `a` is four bytes. If the join keeps only the narrow range the
# check is elided and the store lands 96 bytes past the array.
echo "===== VRP range-JOIN — one cell per control-flow kind ====="

cell j_if        guarded 'volatile u32 sel;
u32 main() { u8[4] a; u32 i = 0; sel = 1;
    if (sel == 1) { i = 100; }
    a[i] = 1; return 0; }'

cell j_if_else   guarded 'volatile u32 sel;
u32 main() { u8[4] a; u32 i; sel = 1;
    if (sel == 0) { i = 1; } else { i = 100; }
    a[i] = 1; return 0; }'

cell j_switch    guarded 'volatile u32 sel;
u32 main() { u8[4] a; u32 i = 0; sel = 2;
    switch (sel) { 1 => { i = 1; } 2 => { i = 100; } default => { i = 0; } }
    a[i] = 1; return 0; }'

cell j_for_body  guarded 'volatile u32 sel;
u32 main() { u8[4] a; u32 i = 0; sel = 1;
    for (u32 k = 0; k < sel; k += 1) { i = 100; }
    a[i] = 1; return 0; }'

cell j_while     guarded 'volatile u32 sel;
u32 main() { u8[4] a; u32 i = 0; sel = 1;
    while (sel == 1) { i = 100; sel = 0; }
    a[i] = 1; return 0; }'

cell j_do_while  guarded 'volatile u32 sel;
u32 main() { u8[4] a; u32 i = 0; sel = 0;
    do { i = 100; } while (sel == 1);
    a[i] = 1; return 0; }'

cell j_orelse    guarded 'volatile u32 sel;
?u32 maybe() { if (sel == 1) { return 100; } return null; }
u32 main() { u8[4] a; u32 i = 0; sel = 1;
    i = maybe() orelse 0;
    a[i] = 1; return 0; }'

cell j_orelse_blk guarded 'volatile u32 sel;
?u32 maybe() { if (sel == 1) { return null; } return 1; }
u32 main() { u8[4] a; u32 i = 0; sel = 1;
    u32 t = maybe() orelse { i = 100; return 0; };
    if (t == 77) { return 1; }
    a[i] = 1; return 0; }'

cell j_if_capture guarded 'volatile u32 sel;
?u32 maybe() { if (sel == 1) { return 100; } return null; }
u32 main() { u8[4] a; u32 i = 0; sel = 1;
    if (maybe()) |v| { i = v; }
    a[i] = 1; return 0; }'

cell j_once      guarded 'volatile u32 sel;
u32 main() { u8[4] a; u32 i = 0; sel = 1;
    @once { i = 100; }
    a[i] = 1; return 0; }'

cell j_goto_fwd  guarded 'volatile u32 sel;
u32 main() { u8[4] a; u32 i = 0; sel = 1;
    if (sel == 0) { goto skip; }
    i = 100;
skip:
    a[i] = 1; return 0; }'

cell j_goto_back guarded 'volatile u32 sel;
u32 main() { u8[4] a; u32 i = 0; u32 n = 0;
again:
    n += 1;
    if (n < 2) { i = 100; goto again; }
    a[i] = 1; return 0; }'

cell j_defer     guarded 'volatile u32 sel;
u32 g_out;
u32 main() { u8[4] a; u32 i = 0;
    defer { i = 100; }
    a[i] = 1; g_out = i; return 0; }'

cell j_nested    guarded 'volatile u32 sel;
u32 main() { u8[4] a; u32 i = 0; sel = 1;
    for (u32 k = 0; k < 2; k += 1) {
        if (sel == 1) { i = 100; break; }
    }
    a[i] = 1; return 0; }'

cell j_call_ret  guarded 'volatile u32 sel;
u32 pick() { if (sel == 1) { return 100; } return 0; }
u32 main() { u8[4] a; sel = 1;
    u32 i = pick();
    a[i] = 1; return 0; }'

echo ""
echo "===== BOUNDARY — the range must still ELIDE what it can prove ====="

cell e_literal   elided 'u32 main() { u8[4] a; u32 i = 2; a[i] = 1; return 0; }'
cell e_mod       elided 'volatile u32 raw;
u32 main() { u8[4] a; raw = 9; u32 i = raw % 4; a[i] = 1; return 0; }'
cell e_mask      elided 'volatile u32 raw;
u32 main() { u8[4] a; raw = 9; u32 i = raw & 3; a[i] = 1; return 0; }'
cell e_both_arms elided 'volatile u32 sel;
u32 main() { u8[4] a; u32 i; sel = 1;
    if (sel == 0) { i = 1; } else { i = 2; }
    a[i] = 1; return 0; }'
cell e_ret_range elided 'volatile u32 raw;
u32 slot() { return raw % 4; }
u32 main() { u8[4] a; raw = 9; u32 i = slot(); a[i] = 1; return 0; }'

echo ""
echo "===== AUTO-ZERO range on an UNINITIALIZED local — every WRITE must widen it ====="
# BUG-854 gave `u32 i;` the range [0,0] (auto-zero is an unconditional language
# guarantee), which is a RELAXATION: the checker will now elide checks it used to
# keep. Soundness therefore rests entirely on every write form widening that
# range again. One cell per form; each writes 100 into a four-byte array's index.

cell z_assign    guarded 'volatile u32 sel;
u32 main() { u8[4] a; u32 i; sel = 100; i = sel; a[i] = 1; return 0; }'
cell z_compound  guarded 'volatile u32 sel;
u32 main() { u8[4] a; u32 i; sel = 100; i += sel; a[i] = 1; return 0; }'
cell z_addr_of   guarded 'void fill(*u32 p) { *p = 100; }
u32 main() { u8[4] a; u32 i; fill(&i); a[i] = 1; return 0; }'
cell z_loop_body guarded 'volatile u32 n;
u32 main() { u8[4] a; u32 i; n = 100;
    for (u32 k = 0; k < 1; k += 1) { i = n; }
    a[i] = 1; return 0; }'
cell z_call_ret  guarded 'volatile u32 n;
u32 big() { return n; }
u32 main() { u8[4] a; u32 i; n = 100; i = big(); a[i] = 1; return 0; }'
cell z_via_local guarded 'volatile u32 n;
u32 main() { u8[4] a; u32 i; u32 j; n = 100; j = n; i = j; a[i] = 1; return 0; }'
cell z_capture   guarded 'volatile u32 n;
?u32 m() { if (n == 100) { return 100; } return null; }
u32 main() { u8[4] a; u32 i; n = 100; if (m()) |v| { i = v; } a[i] = 1; return 0; }'
cell z_thru_ptr  guarded 'volatile u32 n;
u32 main() { u8[4] a; u32 i; n = 100; *u32 p = &i; *p = n; a[i] = 1; return 0; }'
cell z_inner_blk guarded 'volatile u32 n;
u32 main() { u8[4] a; n = 100;
    { u32 i; i = n; a[i] = 1; }
    return 0; }'
# and the precision the relaxation exists to buy
cell z_join_both elided 'volatile u32 sel;
u32 main() { u8[4] a; u32 i; sel = 1;
    if (sel == 0) { i = 1; } else { i = 2; }
    a[i] = 1; return 0; }'
# a STATIC local keeps its value across calls, so [0,0] would be a lie
cell z_static    guarded 'volatile u32 n;
u32 bump() { static u32 i; u8[4] a; u32 r = 0; if (i < 200) { i += 100; } a[i] = 1; r = i; return r; }
u32 main() { bump(); bump(); return 0; }'

echo ""
echo "==================================================================="
echo "vrp-join matrix: $pass ok, $fail mismatch"
[ -n "$holes" ]     && echo "HOLES (bounds check elided on a reachable value):$holes"
[ -n "$overguard" ] && echo "LOST PRECISION (guarded a provably-safe index):$overguard"
[ $fail -eq 0 ] && echo "VRP-JOIN MATRIX CLEAN" || echo "VRP-JOIN MATRIX HAS $fail MISMATCH(es)"
exit $([ $fail -eq 0 ] && echo 0 || echo 1)
