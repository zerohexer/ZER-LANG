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
#
# LIMIT, stated so nobody over-trusts a green run: a sample that FAILS TO COMPILE
# is skipped (it is probably a negative test). So a marker reachable only from a
# program the CHECKER rejects is invisible here — which is exactly what happened
# while verifying this gate: an injected `struct == struct` no longer compiles
# (BUG-840), so the injection proved nothing until it was moved to a construct
# every program emits. Verify a new pattern with a marker on a path that
# COMPILING programs reach.

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
    # BUG-854: the emitter's GIVE-UP paths. Both used to emit valid C that did
    # the wrong thing silently — `(a, b)` is a comma expression that calls
    # nothing, and `0` is a value substituted for an expression the emitter did
    # not understand. They now emit an undeclared identifier so GCC stops the
    # build; these patterns catch a REGRESSION back to the quiet form.
    "/\\* complex callee \\*/"
    "/\\* unhandled expr "
    "/\\* struct/union compare unsupported \\*/"
)

# Multi-module samples — representative of the module emission path — PLUS the
# whole positive corpus. Five samples was the original scope, and it could not
# have caught the give-up markers added above: those live in expression emission,
# not module emission. Measured 2026-08-23: scanning every positive program
# (~1100) finds ZERO of them, which is what makes turning them into hard errors
# safe. Scanning the corpus is what turns that measurement into a standing gate
# rather than a one-off.
SAMPLES=(
    "test_modules/main.zer"
    "test_modules/defer_user.zer"
    "test_modules/shared_user.zer"
    "test_modules/handle_user.zer"
    "test_modules/diamond.zer"
)
# ZER_EMIT_AUDIT_FAST=1 keeps the original 5-sample scope for a quick local run.
if [ -z "${ZER_EMIT_AUDIT_FAST:-}" ]; then
    while IFS= read -r f; do SAMPLES+=("$f"); done < <(
        ls tests/zer/*.zer rust_tests/*.zer zig_tests/*.zer 2>/dev/null)
fi

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
    echo "OK — no dead-stub markers in emitted C across ${#SAMPLES[@]} samples."
    exit 0
else
    echo ""
    echo "$FOUND stray stubs found. Either fill in the missing emission or"
    echo "remove the dead stub. See CLAUDE.md 'Diff-Based Post-Release Audit'."
    exit 1
fi
