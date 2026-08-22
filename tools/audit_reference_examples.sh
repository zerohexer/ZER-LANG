#!/bin/bash
# audit_reference_examples.sh — keep docs/reference.md's ```zer examples COMPILING.
#
# WHAT PROBLEM THIS SOLVES
# ------------------------
# reference.md is the user-facing language reference and, for anyone without web
# access, the ONLY description of the language. Its examples were never executed
# by anything, so they decayed exactly the way an unexecuted test does: silently,
# and then into negative value once people believe they are checked. Measured
# while writing the systems-intrinsic section: four freshly-written examples were
# wrong in ways the compiler catches instantly (a stripped `volatile` through
# `@ptrcast`, `@ptrtoint` into `u64` instead of `usize`, an integer address where
# `@cache_*` wants a pointer). Nothing would have caught them.
#
# HOW IT WORKS
# ------------
# Every fenced ```zer block is extracted and compiled with `zerc -o <tmp>.c`
# (CHECKER ONLY — GCC's verdict is deliberately not part of this gate, so a
# missing cross-toolchain or a hosted-only construct cannot make it red).
#
# A block is only expected to compile when it is a COMPLETE program. Most blocks
# in reference.md are fragments — three statements, a type declaration, a syntax
# sketch, or a deliberate COMPILE-ERROR demonstration — so the rule has to be
# mechanical and unambiguous:
#
#     a block is CHECKED iff it declares `main`, or carries `// audit: check`
#     (for a complete program that has no main, e.g. a library module).
#     `// audit: skip` excludes a block that would otherwise qualify.
#
# "Declares main" was chosen over "looks like it has a function body" after
# measuring: the looser rule classified 60 blocks as complete and 29 of those
# were fragments that merely CONTAINED a function, so the gate reported failures
# for examples that were never meant to stand alone. A gate that cries wolf gets
# switched off.
#
# Everything else is COUNTED and reported, never failed. The counts are printed
# so a drop in coverage is visible rather than silent.
#
# Usage: bash tools/audit_reference_examples.sh [zerc-path] [doc-path]

set -u
cd "$(dirname "$0")/.."

ZERC="${1:-./zerc}"
DOC="${2:-docs/reference.md}"

if [ ! -x "$ZERC" ]; then
    echo "SKIP — no zerc at '$ZERC' (build it first)"
    exit 0
fi
if [ ! -f "$DOC" ]; then
    echo "FAIL — no such document: $DOC"
    exit 1
fi

TMPD=$(mktemp -d)
trap 'rm -rf "$TMPD"' EXIT

# --- split the document into ```zer blocks -----------------------------------
awk -v out="$TMPD" '
    /^```zer$/       { inb=1; n++; f=sprintf("%s/b%03d.zer", out, n); next }
    inb && /^```$/   { inb=0; next }
    inb              { print > f }
' "$DOC"

total=0; checked=0; frag=0; skipped=0; failed=0
FAILLOG="$TMPD/fail.txt"; : > "$FAILLOG"

for f in "$TMPD"/b*.zer; do
    [ -f "$f" ] || continue
    total=$((total + 1))

    if grep -q '// audit: skip' "$f"; then
        skipped=$((skipped + 1)); continue
    fi
    # A complete program declares `main` (or opts in explicitly).
    if ! grep -qE '(^|[^A-Za-z_0-9])main[[:space:]]*\(' "$f" && \
       ! grep -q '// audit: check' "$f"; then
        frag=$((frag + 1)); continue
    fi

    checked=$((checked + 1))
    if ! "$ZERC" "$f" -o "$TMPD/out.c" > "$TMPD/err.txt" 2>&1; then
        failed=$((failed + 1))
        {
            echo "  --- example #$(basename "$f" .zer) ---"
            grep -E 'error:' "$TMPD/err.txt" | head -3 | sed 's/^/      /'
            echo "      source:"
            sed 's/^/        /' "$f" | head -20
        } >> "$FAILLOG"
        continue
    fi
    # A warning is a defect in a REFERENCE example: the doc is meant to show the
    # form a user should copy, and the compiler is telling us it is not that form.
    if grep -qE 'warning:' "$TMPD/err.txt"; then
        failed=$((failed + 1))
        {
            echo "  --- example #$(basename "$f" .zer) (compiles, but WARNS) ---"
            grep -E 'warning:' "$TMPD/err.txt" | head -3 | sed 's/^/      /'
        } >> "$FAILLOG"
    fi
done

echo "reference examples: $total total | $checked checked | $frag fragments | $skipped skipped"

if [ "$failed" -gt 0 ]; then
    echo "FAIL — $failed reference example(s) do not compile clean:"
    cat "$FAILLOG"
    echo
    echo "Fix the example, or — if it is deliberately partial — make it a fragment"
    echo "(drop the enclosing function) or mark it with '// audit: skip' and say why."
    exit 1
fi

echo "OK — every complete reference example compiles without error or warning."
exit 0
