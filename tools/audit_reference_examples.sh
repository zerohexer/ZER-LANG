#!/bin/bash
# audit_reference_examples.sh — docs/reference.md examples must actually compile.
#
# WHY. reference.md is the user-facing language reference and, per CLAUDE.md, the
# ONE place a reader (or an LLM with no web access) can look up ZER syntax. An
# example that does not compile is worse than a missing one: it is confidently
# wrong. This is the "a stale gate is worse than none" rule applied to the docs.
#
# It has already caught real defects in both directions:
#   - an example that was wrong  (a u64 intrinsic result assigned to a u32)
#   - an example that was RIGHT while the COMPILER was wrong (BUG-787: the
#     documented `defer dev_close(d)` RAII idiom produced a false leak)
#
# WHAT IS CHECKED. Every ```zer block that contains `main(` — i.e. looks like a
# whole program — is compiled with `zerc -o <tmp>.c`, which returns non-zero on a
# checker OR zercheck error. Fragments without `main(` are not checked; they are
# illustrative snippets, not programs.
#
# OPTING OUT. A block that CANNOT compile by design must say so on its first
# line, or it counts as a failure:
#
#     // doc-example: not-compilable — <reason>
#
# Two legitimate reasons exist today: a block deliberately demonstrating a
# compile ERROR, and a block showing two source FILES concatenated. Anything
# else should be fixed, not marked.
#
# Run from repo root:  bash tools/audit_reference_examples.sh
# Exit 0 = every checkable example compiles. Exit 1 = at least one does not.
set -u
cd "$(dirname "$0")/.."

DOC="docs/reference.md"
ZERC="./zerc"
[ -x "$ZERC" ] || { echo "ERROR: $ZERC not built"; exit 2; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

python3 - "$DOC" "$WORK" <<'PY'
import sys, re, os
doc, work = sys.argv[1], sys.argv[2]
lines = open(doc).read().split('\n')
cur, start, n = None, 0, 0
for i, l in enumerate(lines):
    if l.startswith('```zer'):
        cur, start = [], i + 2          # 1-based line of the block's first line
    elif l.startswith('```') and cur is not None:
        body = '\n'.join(cur)
        if re.search(r'\bmain\s*\(', body):
            first = cur[0] if cur else ''
            if 'doc-example: not-compilable' not in first:
                open(os.path.join(work, f'ex{n:03d}_{start}.zer'), 'w').write(body + '\n')
                n += 1
        cur = None
    elif cur is not None:
        cur.append(l)
print(n)
PY

FAIL=0
COUNT=0
for f in "$WORK"/*.zer; do
    [ -e "$f" ] || continue
    COUNT=$((COUNT + 1))
    if ! "$ZERC" "$f" -o "$WORK/out.c" > "$WORK/err.log" 2>&1; then
        FAIL=$((FAIL + 1))
        LINE=$(basename "$f" .zer | sed 's/.*_//')
        echo "  FAIL $DOC:$LINE — example does not compile:"
        grep -E 'error|zercheck' "$WORK/err.log" | head -2 | sed 's/^/      /'
    fi
done

if [ "$FAIL" -ne 0 ]; then
    echo "REFERENCE EXAMPLES BROKEN — $FAIL of $COUNT full-program examples in $DOC"
    echo "do not compile. Fix the example, or (if it cannot compile by design) put"
    echo "  // doc-example: not-compilable — <reason>"
    echo "on its first line."
    exit 1
fi
echo "OK — all $COUNT full-program examples in $DOC compile."
exit 0
