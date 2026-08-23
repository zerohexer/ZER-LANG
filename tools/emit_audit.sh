#!/bin/bash
# Emit audit — catches dead-stub pattern in emitted C.
#
# Background: on 2026-04-18 audit of the IR transition, zerc_main.c was
# found printing "/* forward */ " at the start of every multi-module
# output before the real header. The stub wrote the comment prefix but
# never emitted the payload it was documenting. Tests passed because
# `/* forward */` is valid C (just a noop comment). The emitted output
# was polluted for ~4 weeks undetected.
#
# This script compiles a handful of multi-module ZER programs and
# greps the emitted C for known dead-stub fingerprints. Exit non-zero
# if any are found.
#
# Run from repo root: bash tools/emit_audit.sh
# Exit 0 = clean. Exit 1 = stray markers found.

set -euo pipefail

cd "$(dirname "$0")/.."

ZERC="${1:-./zerc}"
[ -x "$ZERC" ] || { echo "zerc not executable at $ZERC" >&2; exit 2; }

# Known dead-stub markers. Add new fingerprints here when audits
# find more dead stubs. The pattern is: a comment-only fprintf that
# never gets the payload filled in. Finds them all.
#
# Ordinary comments (headers, function docs) are filtered out by
# restricting to specific short stubs.
PATTERNS=(
    "/\\* forward \\*/ "    # zerc_main.c dead stub (fixed 2026-04-18)
    "/\\* stub \\*/"
    "/\\* placeholder \\*/"
    "/\\* TODO:[^*]*\\*/[^a-zA-Z\"]"   # bare TODO with no following code
    # BUG-856 (2026-08-23): the emitter's GIVE-UP branches. Each writes a
    # comment where the code should be and then continues with the operands, so
    # the result is a valid C parenthesised/comma expression: it compiles, it
    # runs, and it does NOTHING. `arr[0].fn(10)` emitted `t4 = (t3);` — the call
    # never happened and the result was the argument. No diagnostic, no fault,
    # a wrong answer. These fingerprints are what that looks like in the output.
    "/\\* complex callee \\*/"
    "/\\* complex index callee \\*/"
    "/\\* unknown callee \\*/"
    "/\\* unhandled expr "
    "/\\* unknown slice \\*/"
    "/\\* unknown len \\*/"
    "/\\* struct/union compare unsupported \\*/"
    "/\\* handle auto-deref no alloc \\*/"
    "/\\* ERROR: no allocator"
    "— missing args \\*/"
)

# The module-emission path (multi-file, the original 2026-04-18 defect), plus
# EVERY positive ZER program. Sampling five files was enough for a stub that
# fired on every module emission; it is not enough for a give-up branch that
# needs one specific callee shape to reach it, which is how BUG-856 sat
# unreachable-looking for months while being one array-of-pointers away.
# Emission only — no gcc — so the whole corpus costs about twenty seconds.
SAMPLES=(
    "test_modules/main.zer"
    "test_modules/defer_user.zer"
    "test_modules/shared_user.zer"
    "test_modules/handle_user.zer"
    "test_modules/diamond.zer"
)
for f in tests/zer/*.zer rust_tests/rt_*.zer zig_tests/zt_*.zer; do
    [ -e "$f" ] && SAMPLES+=("$f")
done

FOUND=0
TMP=$(mktemp /tmp/emit_audit.XXXXXX.c)
trap "rm -f $TMP" EXIT

for sample in "${SAMPLES[@]}"; do
    if [ ! -f "$sample" ]; then
        continue   # test file may not exist in some checkouts
    fi
    if ! "$ZERC" "$sample" --emit-c -o "$TMP" >/dev/null 2>&1; then
        continue   # compile error; probably a negative test
    fi
    for pattern in "${PATTERNS[@]}"; do
        if grep -E "$pattern" "$TMP" > /dev/null 2>&1; then
            echo "STRAY STUB in $sample emission:"
            grep -nE "$pattern" "$TMP" | head -3 | sed 's/^/    /'
            FOUND=$((FOUND + 1))
        fi
    done
done

if [ $FOUND -eq 0 ]; then
    echo "OK — no dead-stub markers in emitted C across ${#SAMPLES[@]} programs."
    exit 0
else
    echo ""
    echo "$FOUND stray stubs found. Either fill in the missing emission or"
    echo "remove the dead stub. See CLAUDE.md 'Diff-Based Post-Release Audit'."
    exit 1
fi
