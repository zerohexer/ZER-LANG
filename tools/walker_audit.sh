#!/bin/bash
# Walker audit — detects "tree walker missing a node kind" bugs.
#
# Background: several AST walkers must handle every relevant NODE_ kind.
# Missed kinds cause silent bugs (wrong code generation, infinite loops,
# null derefs). Historically this class has bitten us: BUG-573 (rewrite_idents
# missed NODE_TYPECAST), BUG-577 (find_orelse missed NODE_ASSIGN wrap).
#
# This script cross-references the node kinds handled by AST path's emit_expr
# (battle-tested, 10k+ tests) against the IR path's emit_rewritten_node.
# Any node kind in AST but missing from IR is a silent-bug risk.
#
# Run from repo root: bash tools/walker_audit.sh
# Exit 0 = no gaps. Exit 1 = gaps found, they are printed.

set -euo pipefail

cd "$(dirname "$0")/.."

EMITTER="emitter.c"
[ -f "$EMITTER" ] || { echo "emitter.c not found — run from repo root" >&2; exit 2; }

# Line ranges for the two emission switch DEFINITIONS (not forward decls).
# grep with trailing `{ *$` to match the definition line specifically.
AST_START=$(grep -n '^static void emit_expr(Emitter[^;]*{' "$EMITTER" | head -1 | cut -d: -f1)
IR_START=$(grep -n '^static void emit_rewritten_node(Emitter[^;]*{' "$EMITTER" | head -1 | cut -d: -f1)
# Function-body switch ends where the next top-level function begins.
# emit_stmt was removed 2026-04-19 (IR-only for function bodies) — use the
# next definition in the file instead.
AST_END=$(grep -n '^static void emit_defers_from(Emitter[^;]*{' "$EMITTER" | head -1 | cut -d: -f1)
IR_END=$(grep -n '^static void emit_ir_inst(Emitter' "$EMITTER" | head -1 | cut -d: -f1)

AST_CASES=$(mktemp)
IR_CASES=$(mktemp)
trap "rm -f $AST_CASES $IR_CASES" EXIT

sed -n "${AST_START},${AST_END}p" "$EMITTER" | grep -oE "case NODE_[A-Z_]+" | sort -u > "$AST_CASES"
sed -n "${IR_START},${IR_END}p" "$EMITTER" | grep -oE "case NODE_[A-Z_]+" | sort -u > "$IR_CASES"

# Known exceptions — node kinds pre-lowered by IR before they reach
# emit_rewritten_node, so their absence from the switch is intentional.
# NODE_ORELSE: lower_expr(NODE_ORELSE) routes through lower_orelse_to_dest,
#              decomposes into branches + IR_COPY before emission.
PRELOWERED_AWK=$(mktemp)
trap "rm -f $AST_CASES $IR_CASES $PRELOWERED_AWK" EXIT
cat > "$PRELOWERED_AWK" <<'EOF'
case NODE_ORELSE
EOF
AST_REAL_GAPS=$(mktemp)
comm -23 "$AST_CASES" "$IR_CASES" | grep -vxF -f "$PRELOWERED_AWK" > "$AST_REAL_GAPS" || true

AST_N=$(wc -l < "$AST_CASES")
IR_N=$(wc -l < "$IR_CASES")

echo "emit_expr (AST path)        handles $AST_N node kinds"
echo "emit_rewritten_node (IR)    handles $IR_N node kinds (excluding pre-lowered)"

GAPS=$(cat "$AST_REAL_GAPS")

if [ -z "$GAPS" ]; then
    echo ""
    echo "OK — no gaps. IR emitter covers every node kind the AST emitter does."
    exit 0
else
    echo ""
    echo "GAPS DETECTED — node kinds handled in AST but missing from IR emitter:"
    echo "$GAPS" | sed 's/^/    /'
    echo ""
    echo "Each missing case triggers emit_rewritten_node's default (\"/* unhandled node N */0\")"
    echo "when that node kind appears as an expression in IR-emitted function bodies."
    echo "Silent bugs follow (wrong values, null derefs, hangs)."
    echo ""
    echo "Fix: add a case for each listed kind in emit_rewritten_node."
    exit 1
fi
