#!/bin/bash
# ============================================================================
# compiler_asan_sweep.sh — build zerc ITSELF under ASan + UBSan and compile the
# whole corpus with it, reporting any memory error or UB inside the COMPILER.
#
# WHY: ubsan_sweep.sh / ub_sweep.sh check the EMITTED C. Neither can see the
# compiler reading freed memory — which it did (BUG-1116, 2026-09-23): three
# zercheck_ir sites held an IRHandleInfo* across ir_add_handle(), whose realloc
# freed it. The release build read the stale bytes silently (arena/stale data
# reads "defined" — CLAUDE.md's invisible-to-valgrind class does NOT apply to a
# plain heap realloc, which ASan sees at once). Found by this sweep on the FIRST
# run, on two positives that had passed every gate.
#
# A MEASUREMENT SWEEP, not a gate (it takes minutes). Run by hand after touching
# zercheck_ir.c / checker.c state management:
#     bash tools/compiler_asan_sweep.sh            # prints offending files, exit 1 if any
# ============================================================================
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
cd "$ROOT"
SRCS="lexer.c parser.c ast.c types.c checker.c emitter.c zercheck.c zercheck_ir.c ir.c ir_lower.c zerc_main.c src/safety/*.c"
if ! gcc -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -std=c99 -I. \
        -o "$W/zerc_asan" $SRCS 2>"$W/build.log"; then
    echo "build failed:"; cat "$W/build.log"; exit 2
fi
bad=0
for f in tests/zer/*.zer tests/zer_fail/*.zer tests/zer_trap/*.zer tests/zer_gaps/*.zer \
         rust_tests/*.zer zig_tests/*.zer lib/*.zer examples/*.zer; do
    [ -f "$f" ] || continue
    fl=$(head -5 "$f" | grep -o 'zerc-flags: .*' | sed 's/zerc-flags: //')
    o=$(ASAN_OPTIONS=detect_leaks=0 timeout 60 "$W/zerc_asan" "$f" $fl -o "$W/o.c" 2>&1)
    if echo "$o" | grep -qE 'ERROR: AddressSanitizer|runtime error:'; then
        echo "== $f"
        echo "$o" | grep -E 'ERROR: AddressSanitizer|runtime error:|^    #[0-3] ' | head -6
        bad=$((bad + 1))
    fi
done
(cd test_modules && for f in *.zer; do
    o=$(ASAN_OPTIONS=detect_leaks=0 timeout 60 "$W/zerc_asan" "$f" -o "$W/o.c" 2>&1)
    if echo "$o" | grep -qE 'ERROR: AddressSanitizer|runtime error:'; then
        echo "== test_modules/$f"; echo "$o" | grep -E 'ERROR|runtime error:' | head -3
    fi
done)
if [ "$bad" -eq 0 ]; then echo "OK — the compiler ran the corpus clean under ASan+UBSan."; exit 0; fi
echo "$bad file(s) tripped the sanitizers inside zerc"; exit 1
