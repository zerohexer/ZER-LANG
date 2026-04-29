#!/bin/bash
# audit_fixed_buffers.sh — Stage 3 (2026-04-28).
#
# CLAUDE.md Rule #7: "Never use fixed-size buffers for dynamic data."
# This linter flags suspicious fixed-size buffer declarations in compiler
# internals so future violations don't slip in unnoticed.
#
# Detection strategy:
# - Greps `Type *NAME[N]` and `Type NAME[N]` declarations in *.c files
# - Compares against `tools/fixed_buffer_baseline.txt` (known-accepted set)
# - New patterns NOT in baseline = lint failure
# - To add a legitimate pattern, append to baseline with a justification
#   line above (commit message captures the why)
#
# Run from repo root:
#   bash tools/audit_fixed_buffers.sh
# Exit 0 = no new patterns. Exit 1 = new patterns found, listed with file:line.

set -u

cd "$(dirname "$0")/.."

BASELINE="tools/fixed_buffer_baseline.txt"
if [ ! -f "$BASELINE" ]; then
    echo "ERROR: baseline file missing — $BASELINE" >&2
    exit 2
fi

# Files to audit. Compiler-internal source only (test_*.c omitted because
# fixed-size buffers in unit tests are bounded by test inputs).
FILES="lexer.c parser.c ast.c types.c checker.c emitter.c zercheck.c zercheck_ir.c ir.c ir_lower.c zerc_main.c zer_lsp.c"

CURRENT=$(mktemp)
trap "rm -f $CURRENT" EXIT

# Pattern detection: AST/Symbol/Type pointer arrays + named char/int buffers
# whose name suggests collection (stack, list, items, nodes, args, etc.).
# We deliberately don't flag every char[N] — short name buffers
# (mangled[256], aname[64]) are conventional and have controlled lifetime.
for f in $FILES; do
    [ -f "$f" ] || continue
    # AST/Symbol/Type pointer arrays of any size.
    # Output format: file:CONTENT (no line number — line numbers drift
    # whenever the file is edited, which would break the baseline diff).
    grep -E '^\s*(Node|Symbol|Type|IRBlock|IRInst|IRFunc)\s*\*\s*\w+\[[0-9]+\]' "$f" | \
        sed "s|^|$f:|"
    # uint8_t/uint32_t arrays whose name suggests collection
    grep -E '^\s*(uint8_t|uint16_t|uint32_t|uint64_t|int)\s+\w*(stack|nodes|items|args|ids|covered|reported|checks|list|set|stmts|decls|arr)\w*\[[0-9]+\]' "$f" | \
        sed "s|^|$f:|"
done | tr -d '\r' | sort -u > "$CURRENT"

# Compare against baseline. Lines in CURRENT but not BASELINE = new.
# Strip line numbers from baseline entries (legacy "file:NNN:content" form)
# so a baseline written before this change still matches CURRENT (which
# is now "file:content"). Also CR-strip both inputs for Windows checkouts.
NEW=$(comm -23 "$CURRENT" <(tr -d '\r' < "$BASELINE" | sed -E 's|^([^:]+):[0-9]+:|\1:|' | sort -u) || true)

if [ -z "$NEW" ]; then
    total=$(wc -l < "$CURRENT")
    echo "OK — no new fixed-size buffer declarations."
    echo "(audit covers $total known patterns from baseline)"
    exit 0
fi

count=$(echo "$NEW" | wc -l)
echo "=== Fixed-buffer audit: $count NEW pattern(s) detected ==="
echo ""
echo "Each pattern below is a fixed-size buffer NOT in the baseline whitelist."
echo "Per CLAUDE.md Rule #7: never use fixed-size buffers for dynamic data."
echo ""
echo "$NEW" | sed 's/^/  /'
echo ""
echo "If the pattern is genuinely safe (bounded set, short-lived scratch),"
echo "add it to tools/fixed_buffer_baseline.txt with a one-line justification"
echo "comment above. Otherwise convert to stack-first dynamic (see"
echo "parser.c:2954 stack_decls or zercheck_ir.c:3170 fast_stack for examples)."
exit 1
