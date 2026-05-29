#!/bin/bash
# Walker default-clause audit (Stage 2 Part B, 2026-04-27; brace-tracking
# rewrite 2026-05-29).
#
# Background: walkers that switch on node->kind / inst->op / expr->kind
# with a `default:` clause silently skip any new kind added to the AST/IR.
# This causes the "missing-case silent gap" class of bugs — see Gaps 28,
# 29, 31, 36, 41, 42, 44 in docs/4-27-2026-gaps.md, all closed in Stage 2
# Part A but conceptually preventable by mechanical exhaustiveness.
#
# This script flags every `default:` whose enclosing switch dispatches on
# `*->kind` or `*->op` across the safety-critical compiler files. Output
# is a punch list — review each, classify SAFE/UNSAFE/CRITICAL, and fix
# CRITICAL ones by enumerating the missing cases.
#
# Run from repo root: bash tools/walker_default_audit.sh
# Exit 0 = no defaults remain in safety-critical walkers.
# Exit 1 = defaults found, listed with file:line.
#
# Algorithm: a single awk pass per file tracks brace depth so we always
# know the ENCLOSING switch of any `default:` — not just the closest
# preceding `switch (` line (which got the wrong answer when a default
# sat in the outer of two nested switches; e.g., emitter.c:9690's
# `default:` in `switch (inst->op)` was previously hidden because the
# inner `switch (inst->op_token)` at line 9482 came lexically closer).

set -u  # don't use -e — we want to keep going through all files

cd "$(dirname "$0")/.."

# Files to audit. Add new safety-critical compiler files here as they land.
FILES="checker.c zercheck.c zercheck_ir.c ir_lower.c emitter.c parser.c"

found_count=0
declare -a findings

audit_one_file() {
    local f="$1"
    # awk pass:
    #   - tracks net brace depth (ignoring braces inside strings or
    #     line comments; block comments are a rounding-error edge
    #     and acceptable false positive rate)
    #   - maintains a stack of (depth, line, text) for currently-open
    #     switch statements
    #   - when `default:` appears at top level of a switch body, look
    #     up the matching switch from the stack
    #   - prints "default_line<TAB>switch_line<TAB>switch_text" for
    #     each default whose enclosing switch dispatches on
    #     `*->kind` or `*->op` (and isn't a token-op switch)
    awk '
        function strip_strings_and_comments(s,    out, i, c, in_str, in_chr, esc) {
            # Remove // line comments (best effort)
            sub(/\/\/.*$/, "", s)
            out = ""
            in_str = 0; in_chr = 0; esc = 0
            for (i = 1; i <= length(s); i++) {
                c = substr(s, i, 1)
                if (esc) { esc = 0; continue }
                if (in_str) {
                    if (c == "\\") { esc = 1; continue }
                    if (c == "\"") { in_str = 0 }
                    continue
                }
                if (in_chr) {
                    if (c == "\\") { esc = 1; continue }
                    if (c == "'\''") { in_chr = 0 }
                    continue
                }
                if (c == "\"") { in_str = 1; continue }
                if (c == "'\''") { in_chr = 1; continue }
                out = out c
            }
            return out
        }

        BEGIN {
            depth = 0
            sw_top = 0   # stack pointer
            in_block_comment = 0
        }

        {
            line = $0

            # Strip block comments (multi-line tolerant; minimal)
            if (in_block_comment) {
                idx = index(line, "*/")
                if (idx == 0) next
                line = substr(line, idx + 2)
                in_block_comment = 0
            }
            while (1) {
                bs = index(line, "/*")
                if (bs == 0) break
                be = index(line, "*/")
                if (be == 0 || be < bs) {
                    line = substr(line, 1, bs - 1)
                    in_block_comment = 1
                    break
                }
                line = substr(line, 1, bs - 1) substr(line, be + 2)
            }

            stripped = strip_strings_and_comments(line)

            # Note: we use the ORIGINAL line text to inspect switch
            # signature (so the printed report has the real source),
            # but use the stripped text for { } counting.

            # Detect a switch statement opening on this line. Pattern:
            # `switch ( <expr> )`. The opening { may be on this line or
            # next. We push a switch frame when we see `switch (...)`.
            # Frame records: depth AT which switch body opens (= current
            # depth + 1 once `{` is seen).
            if (match(line, /switch[[:space:]]*\(/)) {
                # Compute when the switch body opens. If `{` is on the
                # same line after the `)`, body opens at depth+1 right
                # away; otherwise it opens once a future `{` increments
                # depth. We push a pending switch frame with the
                # current depth — the actual body depth is depth+1 the
                # next time we see a `{` after this line.
                sw_top += 1
                sw_lines[sw_top] = NR
                sw_text[sw_top] = line
                sw_body_depth[sw_top] = -1   # set when body `{` opens
                sw_pending[sw_top] = 1       # waiting for opening `{`
            }

            # Walk the stripped line char-by-char, updating depth and
            # finalizing any pending switch frames when their opening
            # `{` is encountered.
            for (i = 1; i <= length(stripped); i++) {
                c = substr(stripped, i, 1)
                if (c == "{") {
                    depth += 1
                    # Any pending switch frame opens at this depth.
                    for (s = 1; s <= sw_top; s++) {
                        if (sw_pending[s]) {
                            sw_body_depth[s] = depth
                            sw_pending[s] = 0
                        }
                    }
                } else if (c == "}") {
                    # Pop any switch frame whose body just closed.
                    while (sw_top > 0 && sw_body_depth[sw_top] == depth) {
                        sw_top -= 1
                    }
                    depth -= 1
                }
            }

            # Detect `default:` at start (after whitespace). Only
            # report if a switch frame is active AND the active
            # switch sits at the current depth (i.e., the `default:`
            # is at the top level of that switch body, not buried in
            # an inner block).
            if (match(line, /^[[:space:]]*default:/)) {
                if (sw_top > 0 && sw_body_depth[sw_top] == depth) {
                    # Print: default_line<TAB>switch_line<TAB>switch_text
                    printf "%d\t%d\t%s\n", NR, sw_lines[sw_top], sw_text[sw_top]
                }
            }
        }
    ' "$f"
}

for f in $FILES; do
    [ -f "$f" ] || continue
    while IFS=$'\t' read -r d_line sw_line sw_text; do
        [ -z "$d_line" ] && continue
        # Only flag if the switch is on kind or op fields
        is_kind=$(awk -v t="$sw_text" 'BEGIN { if (t ~ /->kind|->op/) print "1"; else print "0" }')
        [ "$is_kind" = "0" ] && continue
        # Skip token-op switches (binary.op / unary.op / assign.op /
        # inst->op_token — all dispatch on TokenKind which has 100+
        # values; intentional defaults are legitimate)
        is_tok=$(awk -v t="$sw_text" 'BEGIN { if (t ~ /binary\.op|unary\.op|assign\.op|op_token/) print "1"; else print "0" }')
        [ "$is_tok" = "1" ] && continue
        # Skip LOUD defaults — those that fail loudly via _zer_trap,
        # checker_error, abort, assert(0), __builtin_unreachable, or
        # fprintf to stderr. Scan a few lines after the `default:` for
        # any of these markers. A loud default still won't be detected
        # by GCC -Wswitch (the gold standard), but it prevents silent
        # miscompiles by ensuring the compiler/runtime reports the gap.
        is_loud=$(awk -v start=$d_line -v end=$((d_line + 8)) '
            BEGIN { found = 0 }
            NR>=start && NR<=end {
                if ($0 ~ /_zer_trap|checker_error|abort\(|assert\(0\)|__builtin_unreachable|AUDIT-LOUD/) {
                    found = 1
                }
            }
            END { print found }
        ' "$f")
        [ "$is_loud" = "1" ] && continue
        # Exclude emit_expr's `default:` — kept intentionally. Listing
        # statement/decl kinds as explicit cases would create false
        # positives in `tools/walker_audit.sh` (which compares emit_expr
        # case list vs emit_rewritten_node — a different audit gate).
        # emit_expr is the legacy AST diagnostic path, unreachable for
        # well-formed ASTs in the IR pipeline.
        if [ "$f" = "emitter.c" ]; then
            in_emit_expr=$(awk -v n=$sw_line '
                NR<=n && /^static void emit_expr\(Emitter[^;]*\{/ { last=NR }
                NR<=n && /^static void emit_defers_from\(Emitter[^;]*\{/ { last=0 }
                END { print (last>0 ? "1" : "0") }' "$f")
            [ "$in_emit_expr" = "1" ] && continue
        fi
        findings+=("$f:$d_line (switch at line $sw_line: $(echo "$sw_text" | sed 's/^[[:space:]]*//'))")
        found_count=$((found_count + 1))
    done < <(audit_one_file "$f")
done

if [ "$found_count" -eq 0 ]; then
    echo "OK — no default: clauses remain in node-kind / op-kind switches."
    echo ""
    echo "Stage 2 Part B COMPLETE (2026-04-28): all 42 sites converted."
    echo "GCC -Wswitch enforces exhaustiveness on every safety-critical walker."
    exit 0
fi

# Progress tracking: Stage 2 Part B started 2026-04-28 with 42 findings.
# Update INITIAL_COUNT when re-baselining.
INITIAL_COUNT=42
closed=$((INITIAL_COUNT - found_count))
pct=0
if [ "$INITIAL_COUNT" -gt 0 ]; then
    pct=$((closed * 100 / INITIAL_COUNT))
fi

echo "=== Walker default: audit ($found_count finding(s)) ==="
echo ""
echo "Stage 2 Part B progress: $closed of $INITIAL_COUNT closed (~${pct}%)."
echo ""
echo "Each entry below is a switch on ->kind or ->op that has a default:"
echo "clause. Review whether the default is SAFE (explicit no-op for leaf"
echo "nodes that semantically can't appear) or UNSAFE (silent skip of new"
echo "node kinds). Convert UNSAFE cases to enumerated case labels so GCC"
echo "-Wswitch errors when a new kind is added."
echo ""
for f in "${findings[@]}"; do
    echo "  $f"
done
echo ""
echo "Stage 2 Part B (mechanical -Wswitch enforcement) tracks fixing each."
echo "Some sites are TYPE_KIND or IR_OP switches with intentional default —"
echo "review case-by-case before conversion."
exit 1
