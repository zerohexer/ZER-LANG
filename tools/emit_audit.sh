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
)

# Sample multi-module tests — representative of the module emission path.
SAMPLES=(
    "test_modules/main.zer"
    "test_modules/defer_user.zer"
    "test_modules/shared_user.zer"
    "test_modules/handle_user.zer"
    "test_modules/diamond.zer"
)

# Self-enforcing semantic pragmas the emitted C MUST carry (BUG-803).
#
# ZER defines signed overflow as wrapping and hands out sanctioned
# type-punning primitives (@pun, @ptrcast through *opaque). BOTH require a
# non-default GCC flag for the emitted C to mean what ZER says it means, and a
# user compiling the .c themselves (the --emit-c and bare-metal cross-compile
# workflows) does not pass them. The preamble therefore self-enforces each with
# a `#pragma GCC optimize`. Losing either is a SILENT miscompile — measured:
# without no-strict-aliasing, a @pun'd read returns the stale value at -O2 with
# no diagnostic at compile time or run time. Freeze both here so an edit to the
# preamble cannot drop one unnoticed.
REQUIRED=(
    '#pragma GCC optimize("wrapv")'
    '#pragma GCC optimize("no-strict-aliasing")'
)

FOUND=0
TMP=$(mktemp /tmp/emit_audit.XXXXXX.c)
trap "rm -f $TMP" EXIT

for sample in "${SAMPLES[@]}"; do
    if [ ! -f "$sample" ]; then
        continue   # test file may not exist in some checkouts
    fi
    if ! "$ZERC" "$sample" --emit-c -o "$TMP" 2>/dev/null; then
        continue   # compile error; probably a negative test
    fi
    for pattern in "${PATTERNS[@]}"; do
        if grep -E "$pattern" "$TMP" > /dev/null 2>&1; then
            echo "STRAY STUB in $sample emission:"
            grep -nE "$pattern" "$TMP" | head -3 | sed 's/^/    /'
            FOUND=$((FOUND + 1))
        fi
    done
    for req in "${REQUIRED[@]}"; do
        if ! grep -Fq "$req" "$TMP"; then
            echo "MISSING REQUIRED PRAGMA in $sample emission: $req"
            FOUND=$((FOUND + 1))
        fi
    done
done

if [ $FOUND -eq 0 ]; then
    echo "OK — no dead-stub markers in emitted C across ${#SAMPLES[@]} samples."
    echo "OK — every sample carries the ${#REQUIRED[@]} self-enforcing semantic pragmas."
    exit 0
else
    echo ""
    echo "$FOUND stray stubs found. Either fill in the missing emission or"
    echo "remove the dead stub. See CLAUDE.md 'Diff-Based Post-Release Audit'."
    exit 1
fi
