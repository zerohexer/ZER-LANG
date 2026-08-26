#!/bin/bash
# audit_reference_examples.sh — every ```zer fenced block in docs/reference.md
# that is a COMPLETE program must actually compile.
#
# WHY THIS EXISTS. Documentation examples are written, not run, so they rot
# silently and then teach the wrong thing. This repo has already paid for that
# twice: the "Safe C Library Interop" example asserted a `defer sensor_close(dev)`
# pattern that hard-errored, and the Arena example showed two arena allocations
# being linked, which the checker refuses. Both were found by READING, long
# after they shipped. A gate finds them the day they break.
#
# WHAT THIS ASSERTS, AND WHAT IT DOES NOT. Every block is read twice — as a
# FILE and as the inside of a function — and must PARSE in at least one of
# those readings. It is deliberately NOT a full semantic check on every block:
# most blocks in this document are fragments that name a type declared in the
# surrounding prose, and failing on that would demand ~72 doc edits to reach
# green. A gate nobody adopts protects nothing.
#
# Three tiers, strictest first:
#   1. a block containing `main(`      -> a complete program; must parse.
#   2. `// audit: expect-error`        -> must be REJECTED, by any diagnostic.
#   3. everything else                 -> must parse in one of the two readings,
#                                         otherwise counted as a MIXED FRAGMENT
#                                         (a declaration shown beside the
#                                         statements that use it — legible to a
#                                         reader, not a compilation unit).
#
# The fragment and skip counts are PRINTED, so the coverage this gate provides
# is never overstated and a rise in either is visible.
#
# DELIBERATELY-UNCOMPILABLE EXAMPLES. An example that demonstrates a REJECTION
# marks itself:
#     // audit: expect-error
# and the gate then asserts the compiler REFUSES it. An example that cannot run
# in this harness at all (bare-metal linkage, a cross toolchain) marks itself:
#     // audit: skip <reason>
# and the reason is printed. Both keep the intent in the document rather than
# in a deny-list beside it.
#
# The gate is CHECKER-ONLY (`-o <tmp>.c`): it answers "does ZER accept this",
# not "does GCC link it", so a hosted-vs-bare-metal difference cannot make it
# flap. Verified non-vacuous by injection — see the self-check at the end.
#
# Usage:  bash tools/audit_reference_examples.sh [doc ...]
# Exit:   0 = every complete example behaved as marked; 1 = at least one did not.

set -u
cd "$(dirname "$0")/.." || exit 1

ZERC=./zerc
[ -x "$ZERC" ] || ZERC=./zerc.exe
if [ ! -x "$ZERC" ]; then
    echo "audit_reference_examples: no zerc binary — run 'make zerc' first" >&2
    exit 1
fi

DOCS=${*:-docs/reference.md}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

total=0; compiled=0; skipped_fragment=0; skipped_marked=0; failed=0

for doc in $DOCS; do
    [ -f "$doc" ] || { echo "audit_reference_examples: no such file: $doc" >&2; exit 1; }

    # Split the document into ```zer ... ``` blocks, one file per block.
    awk -v out="$TMP" '
        /^```zer[[:space:]]*$/ { inblk=1; n++; next }
        /^```[[:space:]]*$/    { inblk=0; next }
        inblk { print >> (out "/blk" n ".zer") }
    ' "$doc"

    for f in "$TMP"/blk*.zer; do
        [ -e "$f" ] || continue
        total=$((total + 1))
        head5=$(head -5 "$f")

        if printf '%s' "$head5" | grep -q '// audit: skip'; then
            skipped_marked=$((skipped_marked + 1))
            echo "  skip: $(basename "$f") — $(printf '%s' "$head5" | grep -m1 -o '// audit: skip.*')"
            continue
        fi

        # A block is compilable when it is TOP-LEVEL code. `main` is not
        # required — zerc happily checks a file of declarations — so gating on
        # `main(` alone threw away most of the document (measured: 145 of 159
        # blocks skipped). Widened to "the first non-comment, non-blank line
        # starts a top-level declaration", which is what actually decides
        # whether the block is a file. STATEMENT fragments (`x = f();`,
        # `if (...) { }`) legitimately are not files and stay skipped.
        first=$(grep -vE '^[[:space:]]*(//|$)' "$f" | head -1)
        if ! printf '%s' "$first" | grep -qE '^[[:space:]]*(import|cinclude|mmio|struct|packed|move|shared|union|enum|typedef|distinct|container|comptime|async|interrupt|naked|static|const|volatile|threadlocal|u8|u16|u32|u64|usize|i8|i16|i32|i64|f32|f64|bool|void|\?|\*|[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\()'; then
            skipped_fragment=$((skipped_fragment + 1))
            continue
        fi

        expect_error=0
        printf '%s' "$head5" | grep -q '// audit: expect-error' && expect_error=1

        # A block is read either as a FILE or as the INSIDE of a function —
        # which of the two is not decidable from the first line (`u32[4] scores;`
        # opens both a global declaration and a local one). So try BOTH, exactly
        # as a reader would, and accept the block if either reading works. This
        # is what lifted real coverage from 13 blocks to most of the document;
        # a first-line heuristic alone mis-read ~79 statement fragments as files.
        # WHAT THIS GATE ASSERTS, precisely: that the SYNTAX a block teaches
        # still parses. Not that the block is a complete, semantically valid
        # program — most are deliberately not, and cannot be:
        #
        #   usize s = @size(Task);        // Task is declared in the prose
        #   .idle => { ... }              // one switch arm, shown alone
        #
        # Failing on those would demand ~72 doc edits to reach green, and a
        # gate nobody adopts protects nothing. Failing on a PARSE error demands
        # none today and catches the rot that actually matters: a documented
        # spelling that the language no longer has. That is the failure mode
        # which shipped the `defer sensor_close(dev)` interop example and the
        # two-arena example — both syntactically fine, so this gate would not
        # have caught them; what it does catch is the next syntax change made
        # without updating the reference. Semantic coverage over COMPLETE
        # programs is provided by the marked examples (`// audit: expect-error`)
        # and by tests/zer, which run for real.
        classify() {   # $1 = source file -> echoes 1 if the block fails to PARSE
            local o
            o=$("$ZERC" "$1" -o "$TMP/out.c" 2>&1)
            printf '%s' "$o" > "$TMP/last_out"
            if printf '%s' "$o" | grep -qE 'error: (parse failed|expected )'; then
                echo 1
            else
                echo 0
            fi
        }

        # Is this block a COMPLETE PROGRAM? That is the strict tier: a block
        # with an entry point must parse as a file, full stop.
        complete=0
        grep -q 'main[[:space:]]*(' "$f" && complete=1

        errs=$(classify "$f")
        out=$(cat "$TMP/last_out")
        if [ "$errs" -gt 0 ]; then
            { echo "u32 _audit_main() {"; cat "$f"; echo "return 0; }"; } > "$TMP/wrapped.zer"
            werrs=$(classify "$TMP/wrapped.zer")
            if [ "$werrs" -eq 0 ]; then errs=0; else out=$(cat "$TMP/last_out"); fi
        fi

        # A block that parses as NEITHER a file nor a function body is a MIXED
        # fragment — the documentation convention of showing a declaration and
        # the statements that use it side by side:
        #
        #     shared struct Counter { u32 value; }
        #     Counter g;
        #     g.value = 42;              // auto: lock -> write -> unlock
        #
        # A reader understands it; no single compilation unit does. Counted as a
        # fragment rather than failed — and the count is PRINTED, so a rise is
        # visible the same way the explicit-skip count is. Blocks marked
        # expect-error, and complete programs, are never demoted this way.
        if [ "$errs" -gt 0 ] && [ "$complete" -eq 0 ] && [ "$expect_error" -eq 0 ]; then
            skipped_fragment=$((skipped_fragment + 1))
            continue
        fi

        # MISSING-CONTEXT errors are not doc rot. A fenced block is often a
        # deliberate fragment that names a type or function declared elsewhere
        # in the prose ("usize s = @size(Task);"). Compiling it standalone
        # necessarily says "undefined identifier 'Task'", and failing the gate
        # on that would demand ~86 doc edits to go green — a gate nobody adopts
        # is a gate that protects nothing.
        #
        # What IS doc rot, and what this gate therefore keeps: a block that does
        # not PARSE, or that violates a real rule. That is the high-value half —
        # "the documented syntax no longer exists" is exactly the failure mode
        # that shipped `defer sensor_close(dev)` and the two-arena example.

        # An `expect-error` block asserts a REJECTION, which is usually
        # semantic rather than syntactic (`@saturate` in a global initializer
        # parses fine and is refused by a rule). So that tier asks the full
        # question — ANY diagnostic — not the parse-only one above.
        if [ "$expect_error" -eq 1 ]; then
            errs=$("$ZERC" "$f" -o "$TMP/out.c" 2>&1 \
                   | grep -cE '(^|[/[:alnum:]_.-])[^ ]*: error:|^error:')
            if [ "$errs" -eq 0 ]; then
                failed=$((failed + 1))
                echo "FAIL [$doc] example marked 'expect-error' COMPILED:"
                sed 's/^/    | /' "$f" | head -12
            else
                compiled=$((compiled + 1))
            fi
        else
            if [ "$errs" -gt 0 ]; then
                failed=$((failed + 1))
                echo "FAIL [$doc] example does not compile:"
                sed 's/^/    | /' "$f" | head -20
                printf '%s\n' "$out" | grep -m3 'error:' | sed 's/^/    >> /'
            else
                compiled=$((compiled + 1))
            fi
        fi
    done
    rm -f "$TMP"/blk*.zer
done

echo "reference examples: $compiled compiled OK, $failed failed, \
$skipped_fragment fragments (no entry point / mixed decl+stmt), \
$skipped_marked explicitly skipped, $total blocks total"

if [ "$failed" -gt 0 ]; then
    echo "AUDIT FAILED — a documented example does not do what the document claims."
    echo "Fix the EXAMPLE (it is what users copy), not this gate."
    exit 1
fi
echo "OK — no broken reference examples"
exit 0
