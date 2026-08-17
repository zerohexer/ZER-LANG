#!/bin/bash
# Verify that MARKED examples in docs/reference.md still behave as annotated.
#
# WHY THIS EXISTS. reference.md is the user-facing language reference and users have
# nothing to check it against — it IS their source of truth. An example that stopped
# compiling, or that documents a rule the compiler no longer has, is worse than a
# missing one: it is a confidently wrong instruction. Same principle CLAUDE.md states
# for gates and tests ("a stale matrix is worse than none — false confidence"),
# applied to documentation.
#
# OPT-IN, and it asserts the REASON. Put an HTML comment immediately before a fenced
# ```zer block (invisible in rendered markdown, keeps syntax highlighting):
#
#     <!-- verify: ok -->                          block must COMPILE
#     <!-- verify: error "substring" -->           block must be REJECTED, and the
#                                                  diagnostic must contain <substring>
#
# The substring is mandatory on error cases for the same reason `// expect-error` is
# mandatory on negative tests: ZER has many independent safety rules over the same
# programs, so "it failed to compile" is a WEAK ORACLE — whichever rule fires first is
# usually not the one the prose is teaching. Assert the reason the example is SUPPOSED
# to give, never paste whatever it currently prints.
#
# Marking is deliberate and backfillable highest-value-first: most blocks in the file
# are intentional FRAGMENTS that illustrate syntax and cannot compile alone, so a
# heuristic "is this a whole program?" gate produces false failures (measured: 9 of 48,
# every one an artifact — including a block whose COMPILE-ERROR note was on a
# commented-out line).
set -u
ZERC=${ZERC:-./zerc}
REF=${REF:-docs/reference.md}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

python3 - "$REF" "$TMP" <<'PY'
import re, sys, os
ref, tmp = sys.argv[1], sys.argv[2]
s = open(ref).read()
# marker immediately before a ```zer fence (blank lines allowed between)
pat = re.compile(r'<!--\s*verify:\s*(ok|error)\s*(?:"([^"]*)")?\s*-->\s*\n+```zer\n(.*?)```', re.S)
n = 0
for m in pat.finditer(s):
    kind, sub, body = m.group(1), m.group(2) or '', m.group(3)
    open(os.path.join(tmp, 'ref%03d.zer' % n), 'w').write(body)
    open(os.path.join(tmp, 'ref%03d.want' % n), 'w').write(kind + '\n' + sub + '\n')
    n += 1
PY

total=0; fail=0
for f in "$TMP"/ref*.zer; do
    [ -f "$f" ] || continue
    total=$((total+1))
    want=$(head -1 "${f%.zer}.want")
    sub=$(sed -n 2p "${f%.zer}.want")
    out=$("$ZERC" "$f" -o "$TMP/out.c" 2>&1)
    if echo "$out" | grep -qE "error"; then got=ERROR; else got=OK; fi
    if [ "$want" = "ok" ] && [ "$got" != "OK" ]; then
        fail=$((fail+1))
        echo "  MISMATCH: example $(basename "$f" .zer) marked 'ok' but was REJECTED"
        echo "$out" | grep -oE "error: .*" | head -2 | sed 's/^/      /'
    elif [ "$want" = "error" ]; then
        if [ "$got" != "ERROR" ]; then
            fail=$((fail+1))
            echo "  MISMATCH: example $(basename "$f" .zer) marked 'error' but COMPILED"
        elif [ -n "$sub" ] && ! echo "$out" | grep -qF "$sub"; then
            fail=$((fail+1))
            echo "  WRONG REASON: example $(basename "$f" .zer) expected \"$sub\""
            echo "$out" | grep -oE "error: .*" | head -1 | sed 's/^/      got: /'
        fi
    fi
done

echo "=== reference.md example audit: $total marked example(s), $fail mismatch(es) ==="
if [ "$fail" -ne 0 ]; then
    echo ""
    echo "A MARKED example in docs/reference.md no longer behaves as annotated."
    echo "Either the example is stale (fix the example) or a rule changed (fix the"
    echo "prose AND the marker). Do not just weaken the marker."
    exit 1
fi
if [ "$total" -eq 0 ]; then
    echo "WARNING: no marked examples found — the gate is vacuous. Check the marker syntax."
    exit 1
fi
echo "OK — every marked example in reference.md behaves as annotated."
exit 0
