#!/bin/bash
# audit_reference_examples.sh — every ```zer block in docs/reference.md must
# actually compile.
#
# WHY THIS EXISTS
# ---------------
# reference.md is the user-facing language reference and, for anyone without web
# access, the ONLY description of the language. A stale example there is not a
# typo — it is the documentation asserting behaviour the compiler does not have.
# Measured 2026-08-24 while adding ~100 previously-undocumented intrinsics: the
# only way to know a signature was right was to feed it to `zerc`.
#
# HOW IT WORKS
# ------------
# Each fenced ```zer block is extracted and compiled with `zerc -o <tmp>.c`
# (CHECKER verdict only — GCC is not involved, so a privileged intrinsic that
# cannot RUN on the host is still checked).
#
# Most blocks are FRAGMENTS, not programs. A block is compiled as-is when it
# already declares something at top level; otherwise it is wrapped in a
# `i32 main() { ... }`. A block that needs neither (a syntax sketch, a table of
# signatures, an error-illustrating snippet) is skipped, and the skip COUNT is
# printed — a silent skip is how this kind of gate rots.
#
# OPT OUT per block by putting one of these on the line before the fence:
#   <!-- audit: skip -->        not compilable on purpose (error illustration)
#   <!-- audit: fragment -->    wrap in main() even if it looks top-level
#
# Exit 0 iff every non-skipped block compiles.

set -u
cd "$(dirname "$0")/.."
ZERC="${1:-./zerc}"
DOC="${2:-docs/reference.md}"

[ -x "$ZERC" ] || { echo "FAIL — no zerc at $ZERC (run 'make zerc' first)"; exit 1; }
[ -f "$DOC" ] || { echo "FAIL — no such doc: $DOC"; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Split the doc into blocks: <startline>\t<skipmode>\t<file>
python3 - "$DOC" "$TMP" <<'PY'
import sys, os, re
doc, tmp = sys.argv[1], sys.argv[2]
lines = open(doc, encoding='utf-8').read().split('\n')
blocks = []
i = 0
mode = ''
while i < len(lines):
    l = lines[i]
    m = re.match(r'\s*<!--\s*audit:\s*(skip|fragment)\s*-->\s*$', l)
    if m:
        mode = m.group(1); i += 1; continue
    if l.strip() == '```zer':
        start = i + 1
        j = i + 1
        while j < len(lines) and lines[j].strip() != '```':
            j += 1
        body = '\n'.join(lines[start:j])
        blocks.append((start + 1, mode, body))
        mode = ''
        i = j + 1
        continue
    if l.strip().startswith('```'):
        # any other fenced block cancels a pending directive
        mode = ''
    i += 1

idx = 0
with open(os.path.join(tmp, 'index'), 'w', encoding='utf-8') as ix:
    for (ln, md, body) in blocks:
        idx += 1
        f = os.path.join(tmp, 'b%04d.zer' % idx)
        open(f, 'w', encoding='utf-8').write(body + '\n')
        import hashlib
        h = hashlib.sha1(body.encode('utf-8')).hexdigest()[:16]
        ix.write('%d\t%s\t%s\t%s\n' % (ln, md or '-', f, h))
PY

TOTAL=0; OK=0; SKIP=0; FAILED=0; KNOWN=0
BASELINE="tools/reference_example_baseline.txt"
: > "$TMP/seen"; : > "$TMP/nowok"
FAILLOG="$TMP/fail.log"; : > "$FAILLOG"

# Shared prelude — the standing cast of reference.md fragments. A doc example is
# allowed to say `*Task t` without first defining Task; that is what makes it an
# EXAMPLE. Everything here is a name the doc already uses in more than one
# fragment, measured from the failures, so the prelude documents the doc rather
# than inventing an API. An mmio range covering everything lets @inttoptr
# examples check; a doc block declaring its OWN mmio range is skipped by the
# overlap rule and listed in the baseline.
PRELUDE_DECLS=(
  "Task|struct Task { u32 id; u32 priority; }"
  "Point|struct Point { u32 x; u32 y; }"
  "Sensor|struct Sensor { u32 reading; }"
  "SensorData|struct SensorData { u32 value; }"
  "ListHead|struct ListHead { u32 link; }"
  "Command|struct Command { u32 op; }"
  "heap|Slab(Task) heap;"
  "pool|Pool(Task, 8) pool;"
)

# Emit the prelude entries this block does not define for itself. A block that
# declares its own `struct Task` must not get a second one, and a block that
# declares its own mmio range must not overlap the catch-all.
#
# `heap` and `pool` DEPEND on Task, and that dependency used to be silent: when a
# block declared its OWN `struct Task`, the prelude correctly suppressed its
# `struct Task` and then emitted `Slab(Task) heap;` / `Pool(Task, 8) pool;`
# anyway — ABOVE the block, where Task does not exist yet. The block failed with
# "undefined type 'Task'" pointing at a PRELUDE line, so five reference.md
# examples sat in the baseline for a defect in this harness rather than anything
# wrong with the doc. A harness that manufactures its own failures is the same
# false-confidence shape this repo gates against everywhere else, one level up.
make_prelude() {
    local blk="$1" out="$2" name decl task_from_prelude=0
    : > "$out"
    grep -qE '^[[:space:]]*mmio[[:space:]]' "$blk" || \
        echo "mmio 0x0..0xFFFFFFFFFFFFFFFF;" >> "$out"
    for entry in "${PRELUDE_DECLS[@]}"; do
        name="${entry%%|*}"; decl="${entry#*|}"
        grep -qE "(struct|union|enum|container)[[:space:]]+$name\b|\b$name[[:space:]]*[;=]|\)[[:space:]]+$name[[:space:]]*;" "$blk" && continue
        # Task-dependent entries: only sound while the prelude owns Task.
        case "$name" in
            heap|pool) [ "$task_from_prelude" = 1 ] || continue ;;
            Task)      task_from_prelude=1 ;;
        esac
        echo "$decl" >> "$out"
    done
}

# A block is "top-level" if any line starts a declaration we can compile directly.
is_toplevel() {
    grep -qE '^[[:space:]]*(struct|union|enum|container|move|shared|packed|typedef|import|cinclude|mmio|const|volatile|threadlocal|comptime|async|naked|interrupt|Pool\(|Slab\(|Ring\(|Arena|Semaphore\(|Barrier)[[:space:](]' "$1" && return 0
    # a function DEFINITION at column 0: `... ident(...) {`
    grep -qE '^[A-Za-z_?*\[].*\)[[:space:]]*\{[[:space:]]*$' "$1" && return 0
    # a bodyless declaration at column 0: `... ident(...);`
    grep -qE '^[A-Za-z_?*\[].*\)[[:space:]]*;[[:space:]]*$' "$1" && return 0
    return 1
}

while IFS=$'\t' read -r LN MODE F HASH; do
    TOTAL=$((TOTAL+1))
    if [ "$MODE" = "skip" ]; then SKIP=$((SKIP+1)); continue; fi
    # blocks that are pure signature/prose tables have no ZER statement at all
    if ! grep -qE '[;{]' "$F"; then SKIP=$((SKIP+1)); continue; fi
    # a block that illustrates a rejection documents the rejection, not a program
    if grep -qiE '(COMPILE ERROR|PARSE ERROR|ERROR —|// ERROR)' "$F"; then SKIP=$((SKIP+1)); continue; fi

    SRC="$TMP/u_$(basename "$F")"
    make_prelude "$F" "$TMP/prelude.zer"
    {
        cat "$TMP/prelude.zer"
        if [ "$MODE" != "fragment" ] && is_toplevel "$F" >/dev/null 2>&1; then
            cat "$F"
            grep -qE '^[[:space:]]*(i32|u32)[[:space:]]+main[[:space:]]*\(' "$F" || \
                echo "i32 zer_doc_main() { return 0; }"
        else
            echo "i32 zer_doc_main() {"
            cat "$F"
            echo "return 0; }"
        fi
    } > "$SRC"

    if OUT=$("$ZERC" "$SRC" -o "$SRC.c" 2>&1) && ! echo "$OUT" | grep -q ': error:'; then
        OK=$((OK+1))
        grep -q "^$HASH " "$BASELINE" 2>/dev/null && echo "$HASH" >> "$TMP/nowok"
    elif grep -q "^$HASH " "$BASELINE" 2>/dev/null; then
        KNOWN=$((KNOWN+1))
        echo "$HASH" >> "$TMP/seen"
    else
        FAILED=$((FAILED+1))
        {
            echo "--- $DOC:$LN (block starts here) ---"
            echo "$OUT" | grep ': error:' | head -3
            echo "    baseline row if this block is a deliberate fragment:"
            echo "    $HASH  ${DOC##*/}:$LN"
        } >> "$FAILLOG"
    fi
done < "$TMP/index"

echo "=== ${DOC##*/} example audit: $TOTAL blocks — $OK compiled, $SKIP skipped, $KNOWN baselined, $FAILED failed ==="

# A baseline row whose block now COMPILES (or no longer exists) is stale. Report
# it: a frozen list that outlives its entries is the false-confidence failure
# this repo warns about for every other gate.
if [ -f "$BASELINE" ]; then
    STALE=$(grep -vE '^\s*(#|$)' "$BASELINE" | awk '{print $1}' | while read -r h; do
        grep -q "^$h$" "$TMP/seen" || echo "$h"
    done)
    if [ -n "$STALE" ]; then
        echo "NOTE — baseline rows that no longer apply (block compiles now, or was edited/removed):"
        printf '  %s\n' $STALE
        echo "Remove them from $BASELINE in the same commit."
    fi
fi

if [ "$FAILED" -gt 0 ]; then
    cat "$FAILLOG"
    echo
    echo "Each block above is a zer example in $DOC that does not compile and is"
    echo "not baselined. Fix the example, mark it '<!-- audit: skip -->' if it"
    echo "deliberately illustrates a rejection, or add the printed row to"
    echo "$BASELINE with a justification."
    exit 1
fi
echo "OK — every non-baselined ${DOC##*/} example builds."
exit 0
