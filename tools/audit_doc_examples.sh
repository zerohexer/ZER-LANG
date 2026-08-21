#!/bin/bash
# audit_doc_examples.sh — every self-contained example in docs/reference.md must
# still compile.
#
# WHY
# ---
# docs/reference.md is the user-facing language reference and, for anyone
# without web access, the only description of the language. An example that no
# longer compiles is worse than a missing one: it teaches a program that does
# not build, and nothing was checking.
#
# It happened. `### Safe C Library Interop` taught
#
#     *opaque dev = sensor_open("/dev/spi0");
#     defer sensor_close(dev);        // "zercheck: leak prevented"
#
# and the compiler rejected it — "allocated at line N but never freed" (BUG-812,
# 2026-08-21). The doc had been right when written; the defer sink drifted
# behind the direct-call sink and nobody re-ran the example.
#
# WHAT IT CHECKS
# --------------
# Every ```zer fenced block in docs/reference.md that contains `main(` — i.e.
# claims to be a whole program — is compiled with `zerc -o <tmp>.c`. A block
# WITHOUT a `main` is a fragment by construction and is skipped; that is a real
# limit of this gate, stated rather than hidden (roughly 150 of ~175 blocks).
#
# Some whole-program blocks are SUPPOSED to fail — a block demonstrating a
# compile error, or one that needs a second file. Those are listed in
# EXPECTED_FAIL below, matched by a distinctive substring, and each carries the
# reason. A block in that list that starts COMPILING is also a failure: it means
# the documented rule changed and the prose is now wrong.
#
# Usage: bash tools/audit_doc_examples.sh [path-to-zerc]

set -u
cd "$(dirname "$0")/.."
ZERC="${1:-./zerc}"
DOC="docs/reference.md"

if [ ! -x "$ZERC" ]; then
    echo "SKIP — no zerc at '$ZERC' (build it first: make zerc)"
    exit 0
fi

# Blocks that must NOT compile, keyed by a distinctive substring, with the
# reason. Format: <marker>|<why>
EXPECTED_FAIL=(
  '@saturate(u8, 300);       // COMPILE ERROR — global|deliberately demonstrates the rejected form (global initialiser cannot use @saturate)'
  '// uart.zer:|multi-file module example — needs a second file on disk'
)

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Split the doc into ```zer blocks.
awk -v dir="$TMP" '
  /^```zer$/ { inb=1; n++; next }
  /^```$/    { if (inb) { inb=0 }; next }
  inb        { print > (dir "/b" n ".zer") }
' "$DOC"

total=0; compiled=0; skipped=0; fail=0
for f in "$TMP"/b*.zer; do
    [ -f "$f" ] || continue
    if ! grep -q 'main(' "$f"; then skipped=$((skipped+1)); continue; fi
    total=$((total+1))

    why=""
    for e in "${EXPECTED_FAIL[@]}"; do
        marker="${e%%|*}"
        if grep -qF -- "$marker" "$f"; then why="${e#*|}"; break; fi
    done

    out=$("$ZERC" "$f" -o "$f.c" 2>&1)
    if echo "$out" | grep -q 'error'; then ok=0; else ok=1; fi

    if [ -n "$why" ]; then
        if [ "$ok" = "1" ]; then
            fail=$((fail+1))
            echo "FAIL — an example listed as EXPECTED-TO-FAIL now COMPILES:"
            echo "       reason on file: $why"
            echo "       The documented rule changed; update the prose AND this list."
            sed -n '1,6p' "$f" | sed 's/^/         /'
        fi
        continue
    fi

    if [ "$ok" = "1" ]; then
        compiled=$((compiled+1))
    else
        fail=$((fail+1))
        echo "FAIL — a reference.md example no longer compiles:"
        sed -n '1,8p' "$f" | sed 's/^/         /'
        echo "$out" | grep 'error' | head -2 | sed 's/^/       /'
        echo "       Either fix the example, or — if the compiler is what changed —"
        echo "       fix the compiler; a doc that teaches a non-building program is"
        echo "       worse than no doc. If the failure is intentional, add a marker"
        echo "       to EXPECTED_FAIL in tools/audit_doc_examples.sh with a reason."
    fi
done

if [ "$fail" -gt 0 ]; then
    echo "reference.md examples: $compiled compiled, $fail FAILED ($skipped fragments skipped)"
    exit 1
fi
echo "OK — reference.md: $compiled self-contained examples compile ($skipped fragments not checked)"
exit 0
