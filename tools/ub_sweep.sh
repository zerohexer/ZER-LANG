#!/bin/bash
# ub_sweep.sh — differential detector for UNDEFINED BEHAVIOUR in the emitted C.
#
# WHAT IT FINDS, and why nothing else finds it. ZER's guarantee is that a
# program has one defined meaning. If the SAME emitted .c produces different
# answers under different optimisation levels or aliasing assumptions, then the
# emitted C has UB in it and the "one meaning" claim is false for that program —
# even though the checker was silent, GCC was silent, and the test suite passed
# (it runs exactly one configuration). That is the definition of a silent gap:
# compile-time misses it and run-time misses it, and on bare metal there is no
# second configuration to disagree with.
#
# This is how the float->integer UB of BUG-845 was pinned: one emitted .c, one
# gcc, `4294967295` at -O0 and `0` at -O2.
#
# METHOD. For each positive .zer test: emit C once, then build that ONE .c four
# ways and compare stdout+exit status:
#     -O0                       (baseline)
#     -O2                       (optimiser folds constants differently)
#     -O2 -fstrict-aliasing     (ZER normally emits with -fno-strict-aliasing;
#                               a divergence here means the emitted C relies on
#                               type-punning the standard does not allow)
#     -Os                       (different inlining/folding decisions again)
# Emitting once and compiling four times is deliberate: it isolates the C
# semantics from any nondeterminism in the compiler itself.
#
# PROBE NOTE, recorded because it cost a wrong conclusion the first time this
# class was investigated: a `volatile` source SUPPRESSES the float-cast
# divergence, because volatile forces the conversion instruction at every -O
# level while the bug lives in the CONSTANT-FOLDING path. Probe with plain
# constants, never volatile ones.
#
# EXPECTED FAILURES. A test that legitimately depends on timing or thread
# interleaving will differ between builds for reasons that are not UB. Mark it:
#     // ub-sweep: skip <reason>
# in the first five lines. The skip count and reasons are printed.
#
# Usage:  bash tools/ub_sweep.sh [dir ...]        (default: tests/zer)
# Exit:   0 = every program agreed across all four builds.

set -u
cd "$(dirname "$0")/.." || exit 1

ZERC=${ZERC:-./zerc}
[ -x "$ZERC" ] || ZERC=./zerc.exe
if [ ! -x "$ZERC" ]; then
    echo "ub_sweep: no zerc binary — run 'make zerc' first" >&2
    exit 1
fi

DIRS=${*:-tests/zer}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

TIMEOUT=${ZER_RUN_TIMEOUT:-10}
CONFIGS=("-O0" "-O2" "-O2 -fstrict-aliasing" "-Os")

checked=0; diverged=0; skipped=0; unbuildable=0

for d in $DIRS; do
    [ -d "$d" ] || { echo "ub_sweep: no such directory: $d" >&2; exit 1; }
    for src in "$d"/*.zer; do
        [ -e "$src" ] || continue
        name=$(basename "$src" .zer)

        if head -5 "$src" | grep -q '// ub-sweep: skip'; then
            skipped=$((skipped + 1))
            echo "  skip: $name — $(head -5 "$src" | grep -m1 -o '// ub-sweep: skip.*')"
            continue
        fi

        # Emit ONCE. A program the checker rejects is not this gate's business.
        if ! "$ZERC" "$src" -o "$TMP/$name.c" >/dev/null 2>&1; then
            unbuildable=$((unbuildable + 1)); continue
        fi
        [ -s "$TMP/$name.c" ] || { unbuildable=$((unbuildable + 1)); continue; }

        base_out=""; base_rc=""; ok=1; report=""; nobuild=0
        ci=0
        for cfg in "${CONFIGS[@]}"; do
            ci=$((ci + 1))
            # ZER's own driver passes -fwrapv and -fno-strict-aliasing; the third
            # config deliberately drops the latter to probe aliasing reliance.
            extra="-fno-strict-aliasing"
            case "$cfg" in *-fstrict-aliasing*) extra="" ;; esac
            # shellcheck disable=SC2086
            # A build failure is NOT a divergence. Some programs need an
            # include path for a cinclude'd header that lives beside the .zer;
            # pass the source's own directory. If it still will not build, count
            # it as not-built rather than reporting it as UB, which is what the
            # first run of this sweep did (2 of its 3 "divergences" were
            # `cinclude` tests missing -I).
            if ! gcc -std=c99 $cfg -fwrapv $extra -w -I "$d" -o "$TMP/$name.bin" \
                     "$TMP/$name.c" >/dev/null 2>&1; then
                nobuild=1; break
            fi
            out=$(timeout "$TIMEOUT" "$TMP/$name.bin" 2>&1); rc=$?
            if [ "$ci" -eq 1 ]; then
                base_out=$out; base_rc=$rc
            elif [ "$rc" != "$base_rc" ] || [ "$out" != "$base_out" ]; then
                ok=0
                report="$report\n    -O0 gave exit=$base_rc out=[$base_out]"
                report="$report\n    '$cfg' gave exit=$rc out=[$out]"
                break
            fi
        done

        if [ "$nobuild" -eq 1 ]; then
            unbuildable=$((unbuildable + 1)); continue
        fi
        checked=$((checked + 1))
        if [ "$ok" -eq 0 ]; then
            diverged=$((diverged + 1))
            echo "DIVERGENT: $name"
            # NOTE: printf, not echo -e joined into one string. A previous
            # implementation of this idea joined every run's stdout into a
            # single delimiter-separated string and re-split it, so any
            # MULTI-LINE program output split at the newline and was reported
            # divergent with identical output. Keep the outputs separate.
            printf '%b\n' "$report"
        fi
    done
done

echo "ub sweep: $checked programs agreed across ${#CONFIGS[@]} builds, \
$diverged DIVERGENT, $skipped skipped, $unbuildable not built"

if [ "$diverged" -gt 0 ]; then
    echo "UB SWEEP FAILED — the same emitted C answered differently under two"
    echo "optimisation settings. That is undefined behaviour in the emitted C,"
    echo "and on bare metal there is no second setting to disagree with it."
    exit 1
fi
echo "OK — no optimisation-dependent behaviour"
exit 0
