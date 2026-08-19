#!/bin/bash
# negative_reason_audit.sh — find negative tests that may pass for the WRONG reason.
#
# WHY (CLAUDE.md "VACUOUS TESTS", extended 2026-08-19).
# `tests/zer_fail/` passes a test on ANY non-zero exit. The `// expect-error:`
# directive turns that into an assertion, but it is OPTIONAL and was designed to
# be backfilled — as of 2026-08-19, **124 of 575 negatives have one; 451 do not**.
# Every one of those 451 still passes if some UNRELATED rule rejects the file,
# which is the silent-green generator the directive exists to close. It gets
# worse as the compiler gets better: each new rule adds masking surface.
#
# This script does NOT backfill directives — auto-generating them from current
# output is exactly the forbidden move (it would freeze whatever the compiler
# happens to print today, wrong reasons included). It TRIAGES: for every
# directive-less negative it captures the first diagnostic and flags the file
# when the diagnostic shares no significant word with the file's own name. A
# flag is a "look at this", not a verdict — most flags are vocabulary mismatches
# (`*_uaf` vs "use after free"). What it is good at is surfacing the real ones:
# its first run found `compound_field_maybe_freed.zer` rejected with
# `expected type at '<<'` — an unresolved GIT MERGE CONFLICT committed into the
# file, so the BUG-650 regression guard had been testing nothing but the parser.
#
# Two shapes worth looking for in the output:
#   - a PARSE/TYPE error where a SAFETY rule was intended (the test is vacuous)
#   - a diagnostic naming a different safety rule (the intended rule is MASKED;
#     route around the masking rule, then add the directive)
#
# Per-file `// zerc-flags:` are honoured — several asm negatives only reach their
# rule under a specific `--target-arch`, and without the flags they are rejected
# by the assembler instead.
#
# Usage:  bash tools/negative_reason_audit.sh [dir]     (default tests/zer_fail)
# Always exits 0 — this is a report, not a gate.

set -u
cd "$(dirname "$0")/.."
DIR="${1:-tests/zer_fail}"
ZERC="${ZERC:-./zerc}"
[ -x "$ZERC" ] || { echo "zerc not executable at $ZERC" >&2; exit 2; }

TMP=$(mktemp /tmp/neg_audit.XXXXXX)
trap 'rm -f "$TMP" /tmp/_neg_audit_out.c' EXIT

total=0; with_directive=0; checked=0; flagged=0

for f in "$DIR"/*.zer; do
    [ -f "$f" ] || continue
    total=$((total + 1))
    if head -5 "$f" | grep -q 'expect-error:'; then
        with_directive=$((with_directive + 1))
        continue
    fi
    checked=$((checked + 1))
    name=$(basename "$f" .zer)
    file_flags=$(head -1 "$f" | grep -oE '// zerc-flags: .*$' | sed 's|// zerc-flags: ||')
    $ZERC "$f" $file_flags -o /tmp/_neg_audit_out.c >"$TMP" 2>&1
    diag=$(grep -m1 -E 'error|zercheck:' "$TMP" | sed 's/^[^:]*:[0-9]*: *//' | cut -c1-100)
    [ -n "$diag" ] || diag='(no diagnostic captured — may be a GCC-stage rejection)'

    # Significant words of the file name: >=4 chars, minus generic scaffolding.
    hit=0
    for tok in $(echo "$name" | tr '_0-9-' ' ' | tr 'A-Z' 'a-z'); do
        case "$tok" in
            ''|test|zer|fail|case|bad|bug|gap|the|and|via|with) continue;;
        esac
        [ ${#tok} -ge 4 ] || continue
        if echo "$diag" | tr 'A-Z' 'a-z' | grep -qF "$tok"; then hit=1; break; fi
    done
    if [ $hit -eq 0 ]; then
        flagged=$((flagged + 1))
        printf '  %-46s %s\n' "$name" "$diag"
    fi
done

echo ""
echo "=== negative-reason audit over $DIR ==="
echo "  total negatives            : $total"
echo "  with // expect-error:      : $with_directive   (asserted reason)"
echo "  without                    : $checked   (pass on ANY non-zero exit)"
echo "  of those, name/diagnostic share no word: $flagged   (triage above)"
echo ""
echo "A flag is a prompt to read the file, not a failure. Backfill"
echo "'// expect-error: <substring>' with the reason the rule is SUPPOSED to"
echo "give — never a paste of what it currently prints."
exit 0
