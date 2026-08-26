#!/bin/bash
# ============================================================================
# qualifier_closure_probe.sh — can `const` or `volatile` be STRIPPED from a
# pointer by passing it through an intrinsic?
#
# WHY
# ---
# ZER's safety table states the guarantee flatly:
#
#   | Volatile/const strip | `@ptrcast`, `@bitcast`, `@cast` all check
#   |                      | qualifier preservation |
#
# and the reference documents the rejections one by one (`(*u32)volatile_ptr`
# → ERROR, `(*u32)const_ptr` → ERROR). Those are three intrinsics named in a
# table. The compiler recognises ~155.
#
# The failure mode is the reason this deserves its own probe rather than a few
# negative tests. Losing `const` writes through `.rodata`. Losing `volatile` is
# worse and is exactly the class this project keeps finding: GCC is then free to
# coalesce, reorder, hoist out of a loop, or DELETE a peripheral access. There
# is no fault, no diagnostic and no wrong value on the host — the program is
# simply not talking to the hardware any more. Nothing in `make check` can see
# that, because `make check` runs on x86 with no peripherals. It is the
# canonical "silent on bare metal" defect.
#
# HOW
# ---
# Same shape as grammar_closure_probe.sh, and the same trick: let GCC be the
# oracle instead of hand-listing which intrinsics ought to care. Feed a
# `const *u32` and a `volatile *u32` into every argument position of every
# intrinsic the checker recognises, compile the emitted C, and see whether the
# qualifier survived to the C level.
#
# THE ORACLE IS `-Wcast-qual`, AND THAT CHOICE IS THE WHOLE PROBE. The obvious
# pick, -Wdiscarded-qualifiers, is VACUOUS here and the first version of this
# file used it: ZER's emitter writes an EXPLICIT C cast
# (`_zer_t1 = (uint32_t*)(vp);`), and an explicit cast SILENCES that warning by
# design. The probe printed OK against a compiler with the volatile-strip check
# deliberately removed — a green gate over a live defect, which is the exact
# failure this repo keeps warning about. -Wcast-qual is the warning that asks
# the question actually being asked: does a cast REMOVE a qualifier?
#
# The intrinsic list is READ FROM THE CHECKER, not kept here, so an intrinsic
# added tomorrow is probed the day it lands.
#
# Reading the result:
#   - ZER REJECTS the program            -> nothing to compile, not a finding.
#   - ZER accepts and the qualifier holds -> good.
#   - ZER accepts and GCC objects         -> STRIP: report it.
#
# VACUITY: this is a positive-control probe and must be checked like any gate.
# Verified by disabling check_volatile_strip and confirming the probe reports
# `@ptrcast(*u32, vp)`, then restoring it. That check is what caught the vacuous
# first version described above. A probe that has only ever printed OK is a
# script, not a net.
#
# Not wired into `make check` — it is ~1800 compiler+gcc invocations. Run it
# after touching intrinsic argument handling or qualifier propagation.
#
# Usage: bash tools/qualifier_closure_probe.sh [zerc]
# Exit:  0 iff no intrinsic strips const or volatile from a pointer.
# ============================================================================
set +e
ZERC="${1:-./zerc}"
[ -x "$ZERC" ] || { echo "qualifier_closure_probe: no compiler at $ZERC"; exit 2; }

D=$(mktemp -d); trap 'rm -rf "$D"' EXIT

grep -oE 'memcmp\(name, "[a-z0-9_]+"' checker.c \
  | sed 's/.*"\(.*\)"/\1/' | sort -u > "$D/names.txt"
count=$(wc -l < "$D/names.txt")
[ "$count" -gt 50 ] || { echo "qualifier_closure_probe: only $count intrinsics found — extraction broke"; exit 2; }

hits=0; probed=0
while read -r n; do
  case "$n" in _zer_*|"") continue;; esac
  # `cp` is a const *u32, `vp` a volatile *u32. Both point at a real global so
  # the program is otherwise well-formed and any diagnostic is about the
  # qualifier and nothing else.
  for args in "cp" "vp" "cp, cp" "vp, vp" "u32, cp" "u32, vp" "cp, gi" "vp, gi"; do
    f="$D/q.zer"
    cat > "$f" <<EOF
mmio 0x0..0xFFFFFFFFFFFFFFFF;
u32 backing;
u32 gi;
u32 main() {
    const *u32 cp = &backing;
    volatile *u32 vp = @inttoptr(*u32, 0x1000);
    gi = 4;
    @$n($args);
    return 0;
}
EOF
    "$ZERC" "$f" --no-strict-mmio -o "$D/q.c" >/dev/null 2>&1 || continue
    [ -s "$D/q.c" ] || continue
    probed=$((probed+1))
    err=$(gcc -std=c99 -fwrapv -fno-strict-aliasing \
              -Wcast-qual -Werror=discarded-qualifiers \
              -Werror=incompatible-pointer-types \
              -c "$D/q.c" -o "$D/q.o" 2>&1)
    if echo "$err" | grep -qE 'cast-qual|discarded-qualifiers|discards .(const|volatile).'; then
      echo "@$n($args)" >> "$D/found.txt"
    fi
  done
done < "$D/names.txt"

touch "$D/found.txt"
sort -u -o "$D/found.txt" "$D/found.txt"
BASELINE="tools/qualifier_closure_baseline.txt"
grep -v '^#' "$BASELINE" 2>/dev/null | grep -v '^$' | sort -u > "$D/base.txt" || : > "$D/base.txt"
NEW=$(comm -23 "$D/found.txt" "$D/base.txt")
GONE=$(comm -13 "$D/found.txt" "$D/base.txt")
hits=$(printf '%s\n' "$NEW" | grep -c . )

echo ""
echo "==================================================================="
echo "qualifier closure: $count intrinsics, $probed accepted forms compiled"
echo "                   $(grep -c . "$D/found.txt") strip(s) found, $(grep -c . "$D/base.txt") baselined as benign"
if [ -n "$GONE" ]; then
  echo ""
  echo "NOTE — baselined strips that no longer occur (remove them from"
  echo "$BASELINE in the same commit; a stale baseline is worse than none):"
  printf '%s\n' "$GONE" | sed 's/^/  /'
fi
if [ "$hits" -gt 0 ]; then
  echo ""
  echo "NEW STRIP ($hits) — an intrinsic dropped const or volatile from a pointer:"
  printf '%s\n' "$NEW" | sed 's/^/  /'
  echo ""
  echo "Ask, in this order: does the emitted C DEREFERENCE the stripped pointer,"
  echo "or only hand its value to an asm operand? Only the first is a defect."
  echo "QUALIFIER CLOSURE: $hits NEW STRIP(S)"
  exit 1
fi
[ -n "$GONE" ] && exit 1
echo "OK — no new const/volatile strip."
exit 0
