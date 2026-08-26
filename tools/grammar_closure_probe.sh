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
# HOW
# ---
# Enumerate every intrinsic the checker recognises, pass an INTEGER in each
# argument position, and compile the emitted C with -Werror=int-conversion.
# GCC's own front end is the oracle: an int-conversion diagnostic means an
# integer reached a pointer parameter. @inttoptr is excluded — it is the
# sanctioned door.
#
# This is a POSITIVE-CONTROL probe, so it must be checked for vacuity like any
# other gate: verified 2026-08-23 to report 2 hits against a pre-fix build and 0
# after.
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

echo ""
echo "==================================================================="
echo "grammar closure: $count intrinsics, $probed accepted forms compiled"
if [ "$hits" -gt 0 ]; then
  echo "BREACH ($hits) — an integer reached pointer position without @inttoptr."
  echo "GRAMMAR CLOSURE: $hits BREACH(ES)"
  exit 1
fi
echo "OK — no integer reaches pointer position outside @inttoptr."
exit 0
