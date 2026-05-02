#!/bin/bash
# F0.1 (2026-05-03): per-test IR vs AST agreement reporter.
#
# Runs all test suites with both analyzers active. Captures stderr,
# parses AGREEMENT_FAIL lines emitted by zerc_main.c's dual-run reporter,
# classifies disagreements per file, and prints a summary.
#
# Output:
#   * Total tests run
#   * Disagreements by classification:
#       ir_false_positive  — IR rejects valid program (AST=0/IR>0)
#       ir_false_negative  — IR misses real bug (AST>0/IR=0)
#       ir_count_diff      — both nonzero but different counts
#   * Per-disagreement file list with details
#
# Usage:  bash tools/agreement_audit.sh
# Exit:   0 if zero disagreements, 1 otherwise.

set -u

ZERC="./zerc"
if [ ! -f "$ZERC" ] && [ -f "./zerc.exe" ]; then ZERC="./zerc.exe"; fi

if [ ! -f "$ZERC" ]; then
    echo "ERROR: zerc binary not found. Run from repo root after make zerc."
    exit 2
fi

# F0.1 audit mode: tells zerc to NOT bail on AST failure, so we can
# compare per-test even when AST finds errors (e.g., negative tests).
# Without this, zerc returns 1 before ever reaching the IR analyzer
# and we silently miss IR false negatives.
export ZER_AGREEMENT_AUDIT=1

TMP_DIR=$(mktemp -d)
trap "rm -rf $TMP_DIR" EXIT

ALL_LOG="$TMP_DIR/all.log"
> "$ALL_LOG"

TOTAL=0
DISAGREE=0

# Per-class counters
FP=0  # ir_false_positive
FN=0  # ir_false_negative
CD=0  # ir_count_diff

# Per-class file lists
FP_FILES="$TMP_DIR/fp.txt"
FN_FILES="$TMP_DIR/fn.txt"
CD_FILES="$TMP_DIR/cd.txt"
> "$FP_FILES"
> "$FN_FILES"
> "$CD_FILES"

run_one() {
    local f="$1"
    local mode="$2"  # --run or -o /dev/null
    TOTAL=$((TOTAL + 1))
    # Capture stderr (where AGREEMENT_FAIL goes). The previous form
    # `2>&1 >/dev/null` had wrong redirect order — discarded stderr.
    if [ "$mode" = "--run" ]; then
        OUT=$($ZERC "$f" --run 2>&1)
    else
        OUT=$($ZERC "$f" -o /dev/null 2>&1)
    fi
    # Look for AGREEMENT_FAIL in stderr
    while IFS= read -r line; do
        case "$line" in
            AGREEMENT_FAIL*)
                DISAGREE=$((DISAGREE + 1))
                echo "$line" >> "$ALL_LOG"
                # Parse kind=<value>
                kind=$(echo "$line" | grep -oE 'kind=[a-z_]+' | head -1 | cut -d= -f2)
                ast=$(echo "$line" | grep -oE 'ast=[0-9]+' | head -1 | cut -d= -f2)
                ir=$(echo "$line" | grep -oE 'ir=[0-9]+' | head -1 | cut -d= -f2)
                case "$kind" in
                    ir_false_positive)
                        FP=$((FP + 1))
                        echo "$f (ast=$ast ir=$ir)" >> "$FP_FILES"
                        ;;
                    ir_false_negative)
                        FN=$((FN + 1))
                        echo "$f (ast=$ast ir=$ir)" >> "$FN_FILES"
                        ;;
                    ir_count_diff)
                        CD=$((CD + 1))
                        echo "$f (ast=$ast ir=$ir)" >> "$CD_FILES"
                        ;;
                esac
                ;;
        esac
    done <<< "$OUT"
    # Clean up any artifacts from --run
    rm -f "${f%.zer}.c" "${f%.zer}.exe" "${f%.zer}" 2>/dev/null
}

echo "=== F0.1 IR vs AST Agreement Audit ==="
echo ""

# tests/zer/ — positive integration tests (--run)
echo "--- tests/zer/ (positive) ---"
for f in tests/zer/*.zer; do
    [ -f "$f" ] || continue
    run_one "$f" --run
done

# tests/zer_fail/ — negative tests (-o /dev/null)
echo "--- tests/zer_fail/ (negative) ---"
for f in tests/zer_fail/*.zer; do
    [ -f "$f" ] || continue
    run_one "$f" "-o /dev/null"
done

# tests/zer_proof/ — proof-linked tests
echo "--- tests/zer_proof/ ---"
for f in tests/zer_proof/*.zer; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .zer)
    if [[ "$name" == *_bad ]]; then
        run_one "$f" "-o /dev/null"
    else
        run_one "$f" --run
    fi
done

# test_modules/ — multi-file (run from that directory)
echo "--- test_modules/ ---"
if [ -d test_modules ]; then
    pushd test_modules >/dev/null
    ZERC_MOD="../zerc"
    if [ ! -f "$ZERC_MOD" ] && [ -f "../zerc.exe" ]; then ZERC_MOD="../zerc.exe"; fi
    for f in *.zer; do
        [ -f "$f" ] || continue
        TOTAL=$((TOTAL + 1))
        OUT=$($ZERC_MOD "$f" -o "_$f.c" 2>&1)
        while IFS= read -r line; do
            case "$line" in
                AGREEMENT_FAIL*)
                    DISAGREE=$((DISAGREE + 1))
                    echo "$line" >> "$ALL_LOG"
                    kind=$(echo "$line" | grep -oE 'kind=[a-z_]+' | head -1 | cut -d= -f2)
                    ast=$(echo "$line" | grep -oE 'ast=[0-9]+' | head -1 | cut -d= -f2)
                    ir=$(echo "$line" | grep -oE 'ir=[0-9]+' | head -1 | cut -d= -f2)
                    case "$kind" in
                        ir_false_positive)  FP=$((FP + 1)); echo "test_modules/$f (ast=$ast ir=$ir)" >> "$FP_FILES" ;;
                        ir_false_negative)  FN=$((FN + 1)); echo "test_modules/$f (ast=$ast ir=$ir)" >> "$FN_FILES" ;;
                        ir_count_diff)      CD=$((CD + 1)); echo "test_modules/$f (ast=$ast ir=$ir)" >> "$CD_FILES" ;;
                    esac
                    ;;
            esac
        done <<< "$OUT"
        rm -f "_$f.c" "_$f" "_$f.exe" 2>/dev/null
    done
    popd >/dev/null
fi

# rust_tests/ — Rust translations
echo "--- rust_tests/ ---"
if [ -d rust_tests ]; then
    for f in rust_tests/*.zer; do
        [ -f "$f" ] || continue
        # rust_tests use various conventions; just compile (no --run) to capture errors
        run_one "$f" "-o /dev/null"
    done
fi

# zig_tests/ — Zig translations
echo "--- zig_tests/ ---"
if [ -d zig_tests ]; then
    for f in zig_tests/*.zer; do
        [ -f "$f" ] || continue
        run_one "$f" "-o /dev/null"
    done
fi

echo ""
echo "=== Summary ==="
echo "  Total tests run:          $TOTAL"
echo "  AGREEMENT_FAIL count:     $DISAGREE"
echo ""
echo "  ir_false_positive:        $FP  (IR rejects valid programs)"
echo "  ir_false_negative:        $FN  (IR misses real bugs)"
echo "  ir_count_diff:            $CD  (both nonzero, different counts)"
echo ""

if [ "$FP" -gt 0 ]; then
    echo "=== FALSE POSITIVES (IR over-rejects) ==="
    cat "$FP_FILES"
    echo ""
fi
if [ "$FN" -gt 0 ]; then
    echo "=== FALSE NEGATIVES (IR under-rejects) ==="
    cat "$FN_FILES"
    echo ""
fi
if [ "$CD" -gt 0 ]; then
    echo "=== COUNT DIFFERENCES (both nonzero, different) ==="
    cat "$CD_FILES"
    echo ""
fi

echo "Full log: $DISAGREE lines"

# Exit non-zero if disagreements exist (so CI gates on parity)
[ "$DISAGREE" -eq 0 ] && exit 0 || exit 1
