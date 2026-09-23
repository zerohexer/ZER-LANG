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
# A block that defines its own `main` is then BUILT and RUN, and must exit 0
# (set ZER_REF_RUN=0 to check only). A doc program that compiles and returns 7
# is the same false claim as one that does not compile.
#
# Per-block directives, on the line before the fence:
#   <!-- audit: skip -->        not compilable on purpose (a syntax sketch)
#   <!-- audit: fragment -->    wrap in main() even if it looks top-level
#   <!-- audit: expect-error: TEXT -->
#                               the block must be REJECTED, and the compiler's
#                               diagnostics must contain TEXT (an error example
#                               that compiles, or fails for another reason, FAILS)
#   <!-- audit: expect-trap: TEXT -->
#                               the block's main must compile, then TRAP at run
#                               time with TEXT on stderr
#   <!-- audit: compile-only: REASON -->
#                               check it, do not run it (an interrupt handler,
#                               privileged code); REASON is mandatory
#
# A block whose FIRST line is `// zerc-flags: <flags>` (the tests/ convention) is
# compiled and built with those flags — how a `--stack-limit` or `--target-bits`
# example is checked.
#
# Exit 0 iff every non-skipped block does what its directive says.

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
arg = ''
while i < len(lines):
    l = lines[i]
    m = re.match(r'\s*<!--\s*audit:\s*(skip|fragment|expect-error|expect-trap|compile-only)\s*(?::\s*(.*?))?\s*-->\s*$', l)
    if m:
        mode = m.group(1); arg = (m.group(2) or '').strip(); i += 1; continue
    if l.strip() == '```zer':
        start = i + 1
        j = i + 1
        while j < len(lines) and lines[j].strip() != '```':
            j += 1
        body = '\n'.join(lines[start:j])
        blocks.append((start + 1, mode, arg, body))
        mode = ''; arg = ''
        i = j + 1
        continue
    if l.strip().startswith('```'):
        # any other fenced block cancels a pending directive
        mode = ''; arg = ''
    i += 1

idx = 0
with open(os.path.join(tmp, 'index'), 'w', encoding='utf-8') as ix:
    for (ln, md, ag, body) in blocks:
        idx += 1
        f = os.path.join(tmp, 'b%04d.zer' % idx)
        open(f, 'w', encoding='utf-8').write(body + '\n')
        import hashlib
        h = hashlib.sha1(body.encode('utf-8')).hexdigest()[:16]
        ix.write('%d\t%s\t%s\t%s\t%s\n' % (ln, md or '-', f, h, ag.replace('\t', ' ') or '-'))
PY

TOTAL=0; OK=0; SKIP=0; FAILED=0; KNOWN=0; RAN=0; REJECTED=0; TRAPPED=0; NORUN=0
BASELINE="tools/reference_example_baseline.txt"
: > "$TMP/seen"; : > "$TMP/nowok"
FAILLOG="$TMP/fail.log"; : > "$FAILLOG"
RUN="${ZER_REF_RUN:-1}"

# Shared prelude — the standing cast of reference.md fragments. A doc example is
# allowed to say `*Task t` without first defining Task; that is what makes it an
# EXAMPLE. Everything here is a name the doc already uses in more than one
# fragment, measured from the failures, so the prelude documents the doc rather
# than inventing an API. An mmio range covering everything lets @inttoptr
# examples check; a doc block declaring its OWN mmio range is skipped by the
# overlap rule and listed in the baseline.
#
# Format: name|dependencies|declaration. An entry is dropped when the block
# declares `name` itself, AND when the block declares any of its dependencies:
# `Pool(Task, 8) pool;` must not be injected ahead of a block's OWN
# `struct Task { ... }` — it would name the type before it exists.
PRELUDE_DECLS=(
  "Task||struct Task { u32 id; u32 priority; }"
  "Point||struct Point { u32 x; u32 y; }"
  "Sensor||struct Sensor { u32 reading; }"
  "SensorData||struct SensorData { u32 value; }"
  "ListHead||struct ListHead { u32 link; }"
  "Command||struct Command { u32 op; }"
  "heap|Task|Slab(Task) heap;"
  "pool|Task|Pool(Task, 8) pool;"
)

block_declares() {   # block_declares <file> <name>
    grep -qE "(struct|union|enum|container)[[:space:]]+$2\b|\b$2[[:space:]]*[;=]|\)[[:space:]]+$2[[:space:]]*;" "$1"
}

# Emit the prelude entries this block does not define for itself. A block that
# declares its own `struct Task` must not get a second one (nor anything built on
# the prelude's), and a block that declares its own mmio range must not overlap
# the catch-all.
make_prelude() {
    local blk="$1" out="$2" entry name rest deps decl d skip
    : > "$out"
    grep -qE '^[[:space:]]*mmio[[:space:]]' "$blk" || \
        echo "mmio 0x0..0xFFFFFFFFFFFFFFFF;" >> "$out"
    for entry in "${PRELUDE_DECLS[@]}"; do
        name="${entry%%|*}"; rest="${entry#*|}"
        deps="${rest%%|*}"; decl="${rest#*|}"
        block_declares "$blk" "$name" && continue
        skip=0
        for d in $deps; do block_declares "$blk" "$d" && skip=1; done
        [ "$skip" = 1 ] && continue
        echo "$decl" >> "$out"
    done
}

# A block is "top-level" if any line starts a declaration we can compile directly.
is_toplevel() {
    grep -qE '^[[:space:]]*(struct|union|enum|container|move|shared|packed|typedef|import|cinclude|mmio|const|volatile|threadlocal|comptime|async|naked|interrupt|Pool\(|Slab\(|Ring\(|Arena|Semaphore\(|Barrier)[[:space:](]' "$1" && return 0
    # a function DEFINITION at column 0: `... ident(...) {`
    grep -qE '^[A-Za-z_?*\[].*\)[[:space:]]*\{[[:space:]]*$' "$1" && return 0
    # ... or a one-line one: `u32 f(u32 n) { return n; }` — a column-0 statement
    # (`if (c) { ... }`) has the same shape, so the control keywords are excluded
    grep -E '^[A-Za-z_?*\[][^=]*\)[[:space:]]*\{.*\}[[:space:]]*(//.*)?$' "$1" | \
        grep -qvE '^(if|while|for|switch|else|do|return|defer|comptime)\b' && return 0
    # a bodyless declaration at column 0: `... ident(...);`
    grep -qE '^[A-Za-z_?*\[].*\)[[:space:]]*;[[:space:]]*$' "$1" && return 0
    return 1
}

has_main() {
    grep -qE '^[[:space:]]*(i32|u32)[[:space:]]+main[[:space:]]*\(' "$1"
}

fail() {   # fail <line> <message...>
    local ln="$1"; shift
    FAILED=$((FAILED+1))
    { echo "--- $DOC:$ln (block starts here) ---"; printf '%s\n' "$@"; } >> "$FAILLOG"
}

while IFS=$'\t' read -r LN MODE F HASH ARG; do
    TOTAL=$((TOTAL+1))
    [ "$ARG" = "-" ] && ARG=""
    if [ "$MODE" = "skip" ]; then SKIP=$((SKIP+1)); continue; fi
    case "$MODE" in
        expect-error|expect-trap|compile-only)
            if [ -z "$ARG" ]; then
                fail "$LN" "    '<!-- audit: $MODE: ... -->' needs its text (the expected diagnostic, or the reason)"
                continue
            fi ;;
        *)
            # blocks that are pure signature/prose tables have no ZER statement at all
            if ! grep -qE '[;{]' "$F"; then SKIP=$((SKIP+1)); continue; fi
            # a block that illustrates a rejection documents the rejection, not a
            # program — unless a directive says what to check about it
            if grep -qiE '(COMPILE ERROR|PARSE ERROR|ERROR —|// ERROR)' "$F"; then SKIP=$((SKIP+1)); continue; fi
            ;;
    esac

    SRC="$TMP/u_$(basename "$F")"
    TOP=0
    make_prelude "$F" "$TMP/prelude.zer"
    {
        cat "$TMP/prelude.zer"
        if [ "$MODE" != "fragment" ] && is_toplevel "$F" >/dev/null 2>&1; then
            TOP=1
            cat "$F"
            has_main "$F" || echo "i32 zer_doc_main() { return 0; }"
        else
            echo "i32 zer_doc_main() {"
            cat "$F"
            echo "return 0; }"
        fi
    } > "$SRC"

    FLAGS=$(head -1 "$F" | sed -n 's|^[[:space:]]*//[[:space:]]*zerc-flags:[[:space:]]*||p')
    # shellcheck disable=SC2086  # FLAGS is a word list on purpose
    OUT=$("$ZERC" "$SRC" $FLAGS -o "$SRC.c" 2>&1); RC=$?
    if [ $RC -eq 0 ] && ! echo "$OUT" | grep -q ': error:'; then CLEAN=1; else CLEAN=0; fi

    if [ "$MODE" = "expect-error" ]; then
        if [ $CLEAN = 1 ]; then
            fail "$LN" "    documented as a compile error ('$ARG') but it COMPILES"
        elif ! echo "$OUT" | grep -qF -- "$ARG"; then
            fail "$LN" "    rejected, but not for the documented reason '$ARG'; actual:" \
                 "$(echo "$OUT" | grep -E ': (error|zercheck)' | head -3)"
        else
            REJECTED=$((REJECTED+1))
        fi
        continue
    fi

    if [ $CLEAN = 1 ]; then
        OK=$((OK+1))
        grep -q "^$HASH " "$BASELINE" 2>/dev/null && echo "$HASH" >> "$TMP/nowok"
    elif grep -q "^$HASH " "$BASELINE" 2>/dev/null; then
        KNOWN=$((KNOWN+1))
        echo "$HASH" >> "$TMP/seen"
        continue
    else
        fail "$LN" "$(echo "$OUT" | grep ': error:' | head -3)" \
             "    baseline row if this block is a deliberate fragment:" \
             "    $HASH  ${DOC##*/}:$LN"
        continue
    fi

    # ---- the block compiled; does it RUN as documented? ----
    if [ "$MODE" = "expect-trap" ] && { [ $TOP = 0 ] || ! has_main "$F"; }; then
        fail "$LN" "    'expect-trap' needs a block with its own main()"
        continue
    fi
    [ $TOP = 1 ] && has_main "$F" || continue
    if [ "$MODE" = "compile-only" ]; then NORUN=$((NORUN+1)); continue; fi
    [ "$RUN" = "1" ] || continue

    EXE="${SRC%.zer}"
    rm -f "$EXE"
    # shellcheck disable=SC2086
    BOUT=$("$ZERC" "$SRC" $FLAGS 2>&1)
    if [ ! -x "$EXE" ]; then
        fail "$LN" "    passes the checker but does not BUILD (GCC rejected the emitted C):" \
             "$(echo "$BOUT" | grep -E 'error' | head -3)"
        continue
    fi
    # the outer 2>/dev/null swallows bash's own "Trace/breakpoint trap" notice
    # for an expected trap; the program's stderr is kept in $EXE.err
    XRC=$( { timeout 10 "$EXE" > /dev/null 2> "$EXE.err" < /dev/null; echo $?; } 2>/dev/null )
    if [ "$MODE" = "expect-trap" ]; then
        if [ $XRC -eq 0 ]; then
            fail "$LN" "    documented to trap ('$ARG') but exits 0"
        elif ! grep -qF -- "$ARG" "$EXE.err"; then
            fail "$LN" "    exits $XRC, but stderr lacks the documented '$ARG':" "$(head -3 "$EXE.err")"
        else
            TRAPPED=$((TRAPPED+1))
        fi
    elif [ $XRC -ne 0 ]; then
        fail "$LN" "    compiles, but main() exits $XRC (a doc program must exit 0;" \
             "    mark it '<!-- audit: compile-only: why -->' if it cannot run on a host)" \
             "$(head -3 "$EXE.err")"
    else
        RAN=$((RAN+1))
    fi
done < "$TMP/index"

RUNNOTE="$RAN ran to exit 0, $TRAPPED trapped as documented, $NORUN compile-only"
[ "$RUN" = "1" ] || RUNNOTE="running disabled (ZER_REF_RUN=0)"
echo "=== ${DOC##*/} example audit: $TOTAL blocks — $OK compiled ($RUNNOTE), $REJECTED rejected as documented, $SKIP skipped, $KNOWN baselined, $FAILED failed ==="

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
    echo "Each block above is a zer example in $DOC that does not do what the doc"
    echo "says: it does not compile (and is not baselined), or its main() does not"
    echo "exit 0, or it carries an expect-error / expect-trap directive it does not"
    echo "meet. Fix the example; mark a deliberate rejection with"
    echo "'<!-- audit: expect-error: <diagnostic text> -->' (or '<!-- audit: skip -->'"
    echo "for a sketch); or add the printed row to $BASELINE with a justification."
    exit 1
fi
echo "OK — every non-baselined ${DOC##*/} example builds."
exit 0
