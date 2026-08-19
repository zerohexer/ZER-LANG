#!/bin/bash
# ub_sweep.sh — differential sweep for UNDEFINED BEHAVIOUR in the emitted C.
#
# WHY THIS EXISTS (2026-08-19, the session that produced BUG-802 and BUG-803).
# Every test runner in this repo builds ONE configuration and asks "did it exit
# 0". That is structurally blind to undefined behaviour: when the emitted C is
# UB, each configuration produces *an* answer, no assertion fails, and the defect
# ships. Two live ones were sitting under ~500 green tests —
#   BUG-802  `(u32)(-1.5)` -> 0 at -O2, 4294967295 at -O0
#   BUG-803  a `@pun`'d read -> 1 with plain -O2, 1073741824 with
#            -fno-strict-aliasing
# — and neither is visible to a single-configuration run, by construction.
#
# The detector is a DIFFERENCE, not an assertion: compile the SAME emitted .c
# twice along one axis and compare stdout + exit code. Any difference is UB (or
# a flag the emitted C depends on and does not self-enforce). No expected values
# to maintain, no per-test tabulation — which is what makes it cheap enough to
# re-run after any change to a conversion, an arithmetic wrapper, or the preamble.
#
# TWO AXES, and they catch different things — run both:
#   opt   -O0 vs -O2. Splits when the operand is a COMPILE-TIME CONSTANT (GCC
#         folds it with different semantics than the hardware instruction). An
#         opaque runtime operand will NOT split here; it splits on the target
#         axis instead (e.g. ARM saturates float->int in hardware where x86 does
#         not), which this script cannot reach without a cross toolchain.
#   alias with vs without -fno-strict-aliasing. Splits when the emitted C relies
#         on type-punning that the flag protects. After BUG-803 the preamble
#         self-enforces the flag, so a split here means that self-enforcement
#         was lost.
#
# NOT wired into `make check`: it builds and RUNS two binaries per test (~10 min
# over tests/zer). It is an on-demand audit, like tools/agreement_audit.sh.
#
# Usage:  bash tools/ub_sweep.sh [opt|alias|both] [test-dir]
# Exit 0 = no divergence. Exit 1 = at least one test answered differently.

set -u

MODE="${1:-both}"
DIR="${2:-tests/zer}"
cd "$(dirname "$0")/.."
ZERC="${ZERC:-./zerc}"
[ -x "$ZERC" ] || { echo "zerc not executable at $ZERC" >&2; exit 2; }

WORK=$(mktemp -d /tmp/ub_sweep.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

BASE="-std=c99 -fwrapv"

run_axis() {
    local axis="$1" flags_a="$2" flags_b="$3" label_a="$4" label_b="$5"
    local ran=0 skipped=0 diverged=0
    echo "=== axis: $axis  ($label_a vs $label_b) over $DIR ==="
    for f in "$DIR"/*.zer; do
        [ -f "$f" ] || continue
        local n; n=$(basename "$f" .zer)
        # Concurrency and timing tests are nondeterministic BY DESIGN — a
        # difference there is scheduling, not UB, so they would be pure noise.
        case "$n" in
            *spawn*|*thread*|*conc*|*async*|*barrier*|*sem_*|*cond*|*once*|\
            *shared*|*atomic*|*ring*) skipped=$((skipped + 1)); continue;;
        esac
        "$ZERC" "$f" -o "$WORK/t.c" >/dev/null 2>&1 || { skipped=$((skipped+1)); continue; }
        gcc $BASE $flags_a -o "$WORK/a" "$WORK/t.c" -lpthread >/dev/null 2>&1 \
            || { skipped=$((skipped+1)); continue; }
        gcc $BASE $flags_b -o "$WORK/b" "$WORK/t.c" -lpthread >/dev/null 2>&1 \
            || { skipped=$((skipped+1)); continue; }
        ran=$((ran + 1))
        local oa ra ob rb
        oa=$(timeout 10 "$WORK/a" 2>&1); ra=$?
        ob=$(timeout 10 "$WORK/b" 2>&1); rb=$?
        if [ "$oa" != "$ob" ] || [ "$ra" != "$rb" ]; then
            diverged=$((diverged + 1))
            echo "  DIVERGE: $n"
            echo "     $label_a (exit $ra): $(echo "$oa" | head -2 | tr '\n' '|')"
            echo "     $label_b (exit $rb): $(echo "$ob" | head -2 | tr '\n' '|')"
        fi
    done
    echo "  ran=$ran skipped=$skipped diverged=$diverged"
    return $((diverged > 0))
}

TOTAL_BAD=0
if [ "$MODE" = "opt" ] || [ "$MODE" = "both" ]; then
    run_axis opt "-O0 -fno-strict-aliasing" "-O2 -fno-strict-aliasing" "-O0" "-O2" \
        || TOTAL_BAD=$((TOTAL_BAD + 1))
fi
if [ "$MODE" = "alias" ] || [ "$MODE" = "both" ]; then
    run_axis alias "-O2" "-O2 -fno-strict-aliasing" "plain -O2" "-fno-strict-aliasing" \
        || TOTAL_BAD=$((TOTAL_BAD + 1))
fi

echo ""
if [ $TOTAL_BAD -eq 0 ]; then
    echo "OK — no behavioural divergence. The emitted C means the same thing"
    echo "     under every configuration tried."
    exit 0
fi
echo "$TOTAL_BAD axis/axes diverged. Each divergence is undefined behaviour in the"
echo "emitted C, or a compiler flag it depends on without self-enforcing."
echo "See CLAUDE.md \"THE -O0 vs -O2 DIFF\" and BUGS-FIXED.md BUG-802/803."
exit 1
