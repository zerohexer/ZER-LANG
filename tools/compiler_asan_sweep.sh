#!/bin/bash
# ============================================================================
# compiler_asan_sweep.sh — run ZERC ITSELF under ASan+UBSan over the whole
# corpus and report memory errors or undefined behaviour IN THE COMPILER.
#
# WHY
# ---
# Every existing gate asks about the PROGRAM ZER compiles: does the checker
# reject it, does the emitted C behave. `ubsan_sweep.sh` and `ub_sweep.sh`
# instrument the EMITTED C. Nothing instrumented the compiler, so a
# use-after-free inside the analyzer was invisible — it does not crash (the
# freed block is still mapped and usually still holds the old bytes), it just
# makes a safety verdict out of whatever the allocator left behind.
#
# That is not hypothetical. The first run of this sweep, 2026-08-30, found:
#
#   zercheck_ir.c  heap-use-after-free — three sites read an IRHandleInfo
#                  through a pointer that `ir_add_handle` had just REALLOCATED
#                  away, one of them copying the entire alias record (alloc_id,
#                  pool_name, escaped, the view set) out of freed memory. So a
#                  leak or wrong-pool verdict could be decided by stale bytes.
#                  ASan caught it on tests/zer/tokenizer.zer; nothing else could
#                  have, because the wrong answer is silent and non-deterministic.
#
#   emitter.c      `1LL << 63` and the INT64_MIN - 1 that followed — signed
#                  overflow in the compiler while computing an @saturate bound.
#                  The emitted C happened to be right because GCC wraps.
#
# Both are the realloc-invalidation / signed-shift classes that no amount of
# .zer testing can see, because the compiler produces an ANSWER either way.
#
# HOW IT DIFFERS FROM THE OTHER TWO SWEEPS
# ----------------------------------------
#   ub_sweep.sh      — differential -O0 vs -O2 on the EMITTED C
#   ubsan_sweep.sh   — sanitizers on the EMITTED C
#   this one         — sanitizers on THE COMPILER
# They are complementary; none subsumes another.
#
# NOT IN `make check`: it needs a separate instrumented build and takes several
# minutes over ~2200 inputs. Run it after any change to zercheck_ir.c's handle
# bookkeeping, to the IR path state, or to arena/realloc-backed structures —
# and periodically regardless, which is how these two were found.
#
# Usage: bash tools/compiler_asan_sweep.sh
# Exit:  0 iff no input produced a sanitizer finding.
# ============================================================================
set -u
cd "$(dirname "$0")/.."
ROOT="$PWD"

BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT

echo "building an ASan+UBSan zerc (this takes a minute) ..."
# Copy sources rather than building in-tree: an instrumented build would leave
# incompatible .o files behind, and CLAUDE.md's #1 phantom-bug trap is a stale
# object file. Nothing this script does can touch the normal build.
cp -r "$ROOT"/*.c "$ROOT"/*.h "$BUILD"/ 2>/dev/null
mkdir -p "$BUILD/src" && cp -r "$ROOT/src/safety" "$BUILD/src/" 2>/dev/null

if ! (cd "$BUILD" && gcc -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
        -std=c99 -w -I. -o zerc_asan \
        lexer.c parser.c ast.c types.c checker.c emitter.c zercheck.c \
        zercheck_ir.c ir.c ir_lower.c zerc_main.c src/safety/*.c 2> "$BUILD/build.log"); then
    echo "FAIL — instrumented build did not compile:"
    tail -20 "$BUILD/build.log"
    exit 1
fi

DIRS=(tests/zer tests/zer_fail tests/zer_trap tests/zer_gaps
      rust_tests zig_tests test_modules lib examples/qemu-cortex-m3)

TOTAL=0; BAD=0
FINDINGS="$BUILD/findings.txt"; : > "$FINDINGS"

for d in "${DIRS[@]}"; do
    [ -d "$d" ] || continue
    for f in "$d"/*.zer; do
        [ -f "$f" ] || continue
        TOTAL=$((TOTAL + 1))
        # detect_leaks=0: zerc allocates from an arena it never frees, by design.
        # A leak report would be 2000 lines of noise about a deliberate choice.
        out=$(ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 \
              timeout 60 "$BUILD/zerc_asan" "$f" -o "$BUILD/out.c" 2>&1)
        if printf '%s' "$out" | grep -qE 'AddressSanitizer|runtime error:|SUMMARY: Undefined'; then
            BAD=$((BAD + 1))
            {
                echo "--- $f"
                printf '%s' "$out" | grep -E 'AddressSanitizer:|runtime error:|SUMMARY:' | head -4
                printf '%s' "$out" | grep -E '^    #[0-9]+ .*\.(c|h):[0-9]+' | head -4
            } >> "$FINDINGS"
        fi
    done
done

echo ""
echo "==================================================================="
echo "compiler sanitizer sweep: $TOTAL inputs — $BAD with findings"
if [ "$BAD" -gt 0 ]; then
    cat "$FINDINGS"
    echo
    echo "Each block above is a memory error or UB inside zerc itself. A safety"
    echo "analyzer that reads freed memory can return any verdict, silently — fix"
    echo "before trusting any result from the affected path."
    exit 1
fi
echo "OK — no sanitizer findings in the compiler."
exit 0
