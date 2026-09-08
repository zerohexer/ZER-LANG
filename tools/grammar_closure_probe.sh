#!/bin/bash
# ============================================================================
# grammar_closure_probe.sh — does an INTEGER ever become a POINTER without
# going through @inttoptr?
#
# WHY
# ---
# This tests ZER's single most load-bearing claim, the one CLAUDE.md states as
# the reason the language exists at all:
#
#   "No in-language `unsafe` keyword. The grammar enforces that values must
#    enter through typed boundaries — no integer-to-pointer cast except through
#    @inttoptr with mandatory mmio."
#
# Everything downstream rests on that closure: if an integer can reach pointer
# position by some other route, it carries no mmio range, no alignment check and
# no bounds check, and every guarantee built on top is conditional.
#
# The claim was ASSERTED but never MEASURED. It failed on first measurement
# (2026-08-23, BUG-861): `@cstr(some_u32, slice)` emitted
# `memcpy(some_u32, sl.ptr, sl.len)` and BUILT — gcc's -Wint-conversion is a
# WARNING, so `zerc file.zer` exited 0 and produced a binary that writes through
# an address taken from an integer variable. Hosted, that faults; on bare metal
# address 4096 is ordinary RAM or flash and the write silently lands.
#
# HOW — TWO AXES, because one oracle cannot see both routes
# ---
# AXIS 1 (ARGUMENT POSITION). Enumerate every intrinsic the checker recognises,
# pass an INTEGER in each argument position, and compile the emitted C with
# -Werror=int-conversion. GCC's own front end is the oracle: an int-conversion
# diagnostic means an integer reached a pointer parameter. @inttoptr is excluded
# — it is the sanctioned door.
#
# AXIS 2 (MEMORY REINTERPRETATION) — added 2026-08-27 after axis 1 printed OK on
# a compiler that HAD a breach. BUG-916: an integer never appears in pointer
# position at all. Instead a POINTER TO the integer is reinterpreted as a pointer
# to a struct whose field is a pointer, and that field is READ:
#
#     struct P { *u32 p; }
#     u64 addr = 0x1000;  *u64 ap = &addr;
#     *P pp = @pun(*P, ap);        // no int-conversion anywhere in the emitted C
#     *u32 q = pp.p;               // an integer just became a pointer
#
# Nothing GCC's front end can see: every C-level assignment is pointer-to-pointer,
# and the forge happens through memory. So axis 1's oracle is STRUCTURALLY BLIND
# to it, and its headline overstated the claim for four years of that route's
# existence. Axis 2's oracle is ZERC ITSELF: for every pointer-producing
# conversion intrinsic, a target that CARRIES a pointer, reinterpreted from a
# source that does not, must be REJECTED.
#
# The general lesson, recorded because it is the reason this file now has two
# axes: **a probe measures its ORACLE's reach, not its headline.** Ask what the
# oracle cannot see before believing what it prints.
#
# This is a POSITIVE-CONTROL probe, so it must be checked for vacuity like any
# other gate: axis 1 verified 2026-08-23 to report 2 hits against a pre-fix build
# and 0 after; axis 2 verified 2026-08-27 the same way (3 breaches pre-BUG-916).
#
# Not wired into `make check` — it is ~900 compiler+gcc invocations. Run it
# after touching intrinsic argument handling.
#
# Usage: bash tools/grammar_closure_probe.sh [zerc]
# Exit:  0 iff no integer reaches pointer position outside @inttoptr.
# ============================================================================
set +e
ZERC="${1:-./zerc}"
[ -x "$ZERC" ] || { echo "grammar_closure_probe: no compiler at $ZERC"; exit 2; }

D=$(mktemp -d); trap 'rm -rf "$D"' EXIT

# The intrinsic set, read from the checker itself rather than a hand-kept list —
# a new intrinsic is covered the day it is added.
grep -oE 'memcmp\(name, "[a-z0-9_]+"' checker.c \
  | sed 's/.*"\(.*\)"/\1/' | sort -u > "$D/names.txt"
count=$(wc -l < "$D/names.txt")
[ "$count" -gt 50 ] || { echo "grammar_closure_probe: only $count intrinsics found — extraction broke"; exit 2; }

hits=0; probed=0
while read -r n; do
  case "$n" in _zer_*|""|inttoptr) continue;; esac
  for args in "gi" "gi, gi" "gi, gi, gi" "u32, gi" "gi, sl" "sl, gi"; do
    f="$D/i.zer"
    cat > "$f" <<EOF
struct Dev { u32 tag; }
u32 gi;
u32 main() {
    const [*]u8 sl = "hi";
    gi = 4096;
    @$n($args);
    return 0;
}
EOF
    "$ZERC" "$f" --no-strict-mmio -o "$D/i.c" >/dev/null 2>&1 || continue
    [ -s "$D/i.c" ] || continue
    probed=$((probed+1))
    err=$(gcc -std=c99 -fwrapv -fno-strict-aliasing -Werror=int-conversion \
              -c "$D/i.c" -o "$D/i.o" 2>&1)
    if echo "$err" | grep -q 'int-conversion'; then
      echo "  INT->PTR: @$n($args)"
      hits=$((hits+1))
    fi
  done
done < "$D/names.txt"

# ---------------------------------------------------------------------------
# AXIS 2 — memory reinterpretation. The oracle is ZERC: reinterpreting a
# non-pointer-carrying source as a pointer-CARRYING target must be REJECTED,
# because reading the target's pointer field manufactures a pointer out of
# whatever bits were there.
#
# Sources are all NON-pointer-carrying (a plain integer of each width, and a
# float) so a rejection can only be about the forge. Targets carry a pointer at
# increasing nesting depth, which is the wrapper-hides-the-inner-kind axis: a
# fix that checks only the top level passes the first row and fails the rest.
# ---------------------------------------------------------------------------
echo ""
echo "--- axis 2: memory reinterpretation into a pointer-carrying target ---"
reint=0; reint_probed=0
TARGETS='P1 P2 P3 P4 P5'
for src in "u32 sv; *u32 sp = &sv;" "u64 sv; *u64 sp = &sv;" "f64 sv; *f64 sp = &sv;"; do
  for tgt in $TARGETS; do
    for intr in pun ptrcast; do
      f="$D/r.zer"
      cat > "$f" <<EOF
struct P1 { *u32 p; }
struct P2 { u32 pad; *u32 p; }
struct P3 { P1 inner; }
struct P4 { [*]u8 s; }
struct P5 { *(u32) -> u32 f; }
u32 main() {
    $src
    *$tgt tp = @$intr(*$tgt, sp);
    return 0;
}
EOF
      out=$("$ZERC" "$f" --no-strict-mmio -o "$D/r.c" 2>&1)
      # A parse/type error unrelated to the forge (e.g. the form is not
      # expressible) is NOT a probe result — only count forms the compiler
      # actually reached a verdict on.
      case "$out" in
        *"expected"*|*"unknown"*) continue;;
      esac
      reint_probed=$((reint_probed+1))
      if ! echo "$out" | grep -qE '(^|[: ])(error|zercheck):'; then
        echo "  FORGE ACCEPTED: @$intr(*$tgt, <${src%% *} ptr>)"
        reint=$((reint+1))
      fi
    done
  done
done

echo ""
echo "==================================================================="
echo "grammar closure: $count intrinsics, $probed accepted forms compiled (axis 1)"
echo "                 $reint_probed reinterpretation forms judged (axis 2)"
if [ "$hits" -gt 0 ] || [ "$reint" -gt 0 ]; then
  [ "$hits" -gt 0 ] &&     echo "BREACH ($hits) — an integer reached pointer position without @inttoptr."
  [ "$reint" -gt 0 ] &&     echo "BREACH ($reint) — a pointer-carrying target was forged from non-pointer bits."
  echo "GRAMMAR CLOSURE: $((hits + reint)) BREACH(ES)"
  exit 1
fi
echo "OK — no integer reaches pointer position, by argument OR by reinterpretation."
exit 0
