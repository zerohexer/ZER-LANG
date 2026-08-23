#!/bin/bash
# ============================================================================
# ub_sweep.sh — differential detector for undefined behaviour that SURVIVES
# both harnesses.
#
# WHY
# ---
# ZER's contract is "no undefined behavior". The two test harnesses cannot see
# a violation of it: a positive test asserts exit 0 and a negative test asserts
# a diagnostic, and UB produces neither — it produces a DIFFERENT ANSWER
# depending on what the C compiler felt like doing. Measured 2026-08-23, from
# ONE emitted .c on ONE gcc:
#
#     (u32)(-1.5)   -> 4294967295 at -O0, 0          at -O2
#     (u32)1.0e20   -> 0          at -O0, 4294967295 at -O2
#
# Both runs "passed" every gate in the tree.
#
# WHAT IT DOES
# ------------
# Compiles each program's emitted C twice (or four times with --alias) and
# compares stdout AND exit status. A disagreement is UB (or a
# strict-aliasing violation) in the EMITTED code — the compiler's fault, never
# the test's.
#
#   -O0 vs -O2                  : constant folding vs the runtime instruction
#   -fno-strict-aliasing (opt)  : type-punning assumptions
#
# It is NOT wired into `make check`: it costs two to four gcc invocations per
# program. Run it after touching the emitter, and periodically.
#
# Usage: bash tools/ub_sweep.sh [zerc] [dir ...]
#        ZER_UB_ALIAS=1 bash tools/ub_sweep.sh     # add the aliasing axis
# Exit:  0 iff every program agreed with itself across optimisation levels.
# ============================================================================
set +e
ZERC="${1:-./zerc}"
shift 2>/dev/null
DIRS=("$@")
if [ ${#DIRS[@]} -eq 0 ]; then DIRS=(tests/zer); fi

DIR="$(mktemp -d)"
trap 'rm -rf "$DIR"' EXIT

CFLAGS_BASE="-std=c99 -fwrapv -fno-strict-aliasing"
LEVELS="-O0 -O2"
if [ -n "$ZER_UB_ALIAS" ]; then
  # The emitted C is compiled with -fno-strict-aliasing by zerc itself, so the
  # aliasing axis asks the counterfactual: would it still be correct without?
  EXTRA_SETS="strict"
fi

total=0; ran=0; diverged=0; skipped=0
report=""

for d in "${DIRS[@]}"; do
  for f in "$d"/*.zer; do
    [ -e "$f" ] || continue
    name=$(basename "$f" .zer)
    total=$((total+1))

    # A per-file `// zerc-flags:` directive (first five lines) mirrors the ZER
    # test runner, so cross-arch / no-strict-mmio programs are compiled the
    # same way here.
    fflags=$(head -5 "$f" | grep -oE '// zerc-flags: .*$' | sed 's|// zerc-flags: ||')
    if ! "$ZERC" "$f" $fflags -o "$DIR/$name.c" >/dev/null 2>&1; then
      skipped=$((skipped+1)); continue
    fi
    [ -s "$DIR/$name.c" ] || { skipped=$((skipped+1)); continue; }

    outs=""; codes=""; built=0
    for lvl in $LEVELS; do
      if gcc $CFLAGS_BASE $lvl "$DIR/$name.c" -o "$DIR/$name$lvl.exe" \
             -lpthread >/dev/null 2>&1; then
        o=$(cd "$DIR" && timeout 20 "./$name$lvl.exe" 2>/dev/null); c=$?
        outs="$outs<<$lvl:$o>>"; codes="$codes $c"; built=$((built+1))
      fi
    done
    if [ -n "$EXTRA_SETS" ]; then
      if gcc -std=c99 -fwrapv -O2 "$DIR/$name.c" -o "$DIR/${name}_sa.exe" \
             -lpthread >/dev/null 2>&1; then
        o=$(cd "$DIR" && timeout 20 "./${name}_sa.exe" 2>/dev/null); c=$?
        outs="$outs<<strict:$o>>"; codes="$codes $c"; built=$((built+1))
      fi
    fi
    [ $built -lt 2 ] && { skipped=$((skipped+1)); continue; }
    ran=$((ran+1))

    # Every run must agree on BOTH the exit status and stdout.
    uniq_codes=$(echo $codes | tr ' ' '\n' | sort -u | wc -l)
    first=$(echo "$outs" | sed 's/<</\n<</g' | sed -n '2p' | sed 's/^<<[^:]*://; s/>>$//')
    same_out=1
    while IFS= read -r piece; do
      [ -z "$piece" ] && continue
      v=$(echo "$piece" | sed 's/^<<[^:]*://; s/>>$//')
      [ "$v" != "$first" ] && same_out=0
    done < <(echo "$outs" | sed 's/<</\n<</g' | tail -n +2)

    if [ "$uniq_codes" -gt 1 ] || [ "$same_out" -eq 0 ]; then
      diverged=$((diverged+1))
      report="$report\n  $f\n     exit codes:$codes\n     outputs: $outs"
    fi
    rm -f "$DIR/$name"*.exe "$DIR/$name.c"
  done
done

echo ""
echo "==================================================================="
echo "UB sweep: $total programs — $ran compared, $skipped skipped (did not build)"
if [ $diverged -gt 0 ]; then
  echo "DIVERGENT ($diverged) — the emitted C behaves differently across -O levels:"
  printf "$report\n"
  echo "UB SWEEP: $diverged DIVERGENCE(S)"
  exit 1
fi
echo "OK — every program agreed with itself across optimisation levels."
exit 0
