#!/bin/bash
# ============================================================================
# ubsan_sweep.sh — run the positive corpus under UBSan + ASan and report any
# undefined behaviour in the EMITTED C.
#
# WHY
# ---
# ZER's safety table promises "No undefined behavior (overflow wraps, shift by
# >=width = 0)". Neither harness can check it: a positive test asserts exit 0
# and a negative asserts a diagnostic, and UB produces NEITHER — it produces
# whatever the C compiler felt like doing, which is usually the right answer
# until it isn't. BUG-845 (float -> integer out of range) lived in the tree with
# every gate green.
#
# HOW THIS DIFFERS FROM ub_sweep.sh
# ---------------------------------
# `ub_sweep.sh` is DIFFERENTIAL: it compiles at -O0 and -O2 and reports a
# disagreement. That finds UB whose effect the optimiser changes, and misses UB
# that happens to do the same thing at both levels — which is most of it, most
# of the time, until a compiler upgrade.
#
# This one asks the sanitizers directly, so it does not depend on two
# optimisation levels disagreeing. The two are complementary; neither
# supersedes the other.
#
# WHAT IS AND IS NOT UB HERE
# --------------------------
# `-fwrapv` is KEPT, deliberately. ZER DEFINES signed overflow as wrapping, so a
# wrap is not a violation of ZER's contract and must not be reported. The same
# reasoning covers shifts: ZER defines shift-by->=width as 0 and the emitter
# lowers it through `_zer_shl`/`_zer_shr`, so a report there would be a real
# defect in that lowering, not a false positive.
#
# What remains for the sanitizers to find is UB that ZER does NOT define and
# therefore should never emit: out-of-range float->int conversions, misaligned
# accesses, null dereferences, out-of-bounds, invalid enum-ish loads, and (with
# ASan) heap and stack errors in ZER-generated code.
#
# ZER's own runtime traps are EXPECTED for the trap tests and are not run here —
# only `tests/zer/`, whose contract is "compiles, runs, exits 0".
#
# Not wired into `make check`: instrumented builds are slow. Run it after
# touching the emitter, and periodically.
#
# Usage: bash tools/ubsan_sweep.sh [zerc] [dir]
# Exit:  0 iff no sanitizer diagnostic and no exit-status change.
# ============================================================================
set -u
cd "$(dirname "$0")/.."
ZERC="${1:-./zerc}"
DIR="${2:-tests/zer}"
[ -x "$ZERC" ] || { echo "ubsan_sweep: no compiler at $ZERC (run 'make zerc')"; exit 2; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# `float-cast-overflow` is NOT part of `-fsanitize=undefined` in GCC and must be
# asked for by name. Verified the hard way: with the BUG-845 f2i guard disabled,
# this sweep reported CLEAN on a program doing `(u32)(-1.5)` until the option was
# added — the sweep would have missed the exact class it exists to find. Anything
# added to this list needs the same check: break the guard, confirm the sweep
# fires, restore.
SAN="-fsanitize=undefined,address,float-cast-overflow -fno-sanitize-recover=all"
# ZER DEFINES these; a report on them would be about ZER's semantics, not a bug.
SAN="$SAN -fno-sanitize=signed-integer-overflow,shift"

# Programs whose sanitizer finding is the DOCUMENTED MECHANISM, not a defect.
# Reviewed individually; a NEW name failing is a real finding.
#
#   rt_unsafe_probe_mmio — `@probe(addr)` exists to ATTEMPT a read that may
#     fault and catch it (SIGSEGV + setjmp/longjmp), returning null. Probing
#     address 0 is therefore a null load BY CONSTRUCTION, and it is the only way
#     to implement a safe MMIO probe in portable C. The load cannot be optimised
#     away — the pointer is `volatile`, which forbids eliding it — so the fault
#     path really is reached. Under this sweep the sanitizer aborts before the
#     handler runs, which is the sanitizer being right about C and wrong about
#     what the program means.
#
#   probe_fault_returns_null — the SAME mechanism, one sibling later. It probes
#     0x0 deliberately (the fault path had no test until 2026-08-30; only the
#     success path was covered). Added to the baseline the run after the test
#     landed, which is the lesson worth keeping: a new test that exercises an
#     already-baselined mechanism needs its baseline row IN THE SAME COMMIT, or
#     the next sweep reports a "new" finding for a reason that was reviewed and
#     accepted long ago. Verified before baselining that the finding reproduces
#     on a from-HEAD build — i.e. it is the mechanism, not a regression.
BASELINE_NAMES=" rt_unsafe_probe_mmio probe_fault_returns_null "

total=0; ran=0; skipped=0; hits=0; known=0
FAILLOG="$TMP/fail.txt"; : > "$FAILLOG"

for f in "$DIR"/*.zer; do
    [ -f "$f" ] || continue
    total=$((total+1))
    name=$(basename "$f" .zer)

    # per-file flags, same directive the runners honour
    flags=$(head -5 "$f" | grep -oE '// zerc-flags: .*$' | sed 's|// zerc-flags: ||')

    if ! "$ZERC" "$f" $flags -o "$TMP/$name.c" >/dev/null 2>&1; then
        skipped=$((skipped+1)); continue          # does not compile here; not our question
    fi
    [ -s "$TMP/$name.c" ] || { skipped=$((skipped+1)); continue; }

    # Baseline: plain build, plain run. Establishes the expected exit status, so
    # a sanitizer ABORT is distinguishable from a program that always failed.
    if ! gcc -std=c99 -O1 -fwrapv -fno-strict-aliasing \
             -o "$TMP/$name.base" "$TMP/$name.c" -lpthread -lm >/dev/null 2>&1; then
        skipped=$((skipped+1)); continue
    fi
    timeout 20 "$TMP/$name.base" >/dev/null 2>&1; base_rc=$?
    [ $base_rc -eq 0 ] || { skipped=$((skipped+1)); continue; }   # only exit-0 programs

    if ! gcc -std=c99 -O1 -fwrapv -fno-strict-aliasing $SAN \
             -o "$TMP/$name.san" "$TMP/$name.c" -lpthread -lm >/dev/null 2>&1; then
        skipped=$((skipped+1)); continue
    fi
    ran=$((ran+1))
    out=$(timeout 60 "$TMP/$name.san" 2>&1); san_rc=$?

    if echo "$out" | grep -qE 'runtime error:|AddressSanitizer|UndefinedBehaviorSanitizer'; then
        case "$BASELINE_NAMES" in *" $name "*) known=$((known+1)); continue;; esac
        hits=$((hits+1))
        {
            echo "--- $name"
            echo "$out" | grep -E 'runtime error:|AddressSanitizer|SUMMARY' | head -3 | sed 's/^/    /'
        } >> "$FAILLOG"
    elif [ $san_rc -ne 0 ]; then
        case "$BASELINE_NAMES" in *" $name "*) known=$((known+1)); continue;; esac
        hits=$((hits+1))
        { echo "--- $name"; echo "    exit $san_rc under sanitizers, 0 without"; } >> "$FAILLOG"
    fi
done

[ -s "$FAILLOG" ] && cat "$FAILLOG"
echo ""
echo "==================================================================="
echo "ubsan sweep: $total programs — $ran instrumented, $skipped skipped, $known baselined, $hits with findings"
if [ "$hits" -gt 0 ]; then
    echo "UB / memory errors in EMITTED C. Each is the compiler's fault, not the test's."
    exit 1
fi
echo "OK — no sanitizer findings in emitted C."
exit 0
