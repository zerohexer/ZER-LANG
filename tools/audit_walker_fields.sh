#!/bin/bash
# audit_walker_fields.sh — the FIELD-COVERAGE half of the walker discipline.
#
# WHAT PROBLEM THIS SOLVES
# ------------------------
# `-Werror=switch` + tools/walker_default_audit.sh already guarantee that every
# safety walker NAMES every NodeKind. They say nothing about whether an arm that
# names a kind actually DESCENDS that kind's children.
#
# That second half is where the holes live. Measured examples, all shipped:
#
#   BUG-795 (2026-08-17)  five "which shared roots does this expression touch?"
#                         walkers each descended `call.args` and NONE descended
#                         `call.callee`. `go.cb()` emitted with no mutex at all.
#   2026-08-06            vrp_invalidate_loop_body_writes named NODE_ORELSE but
#                         did not walk `orelse.fallback`; a loop-body index
#                         written in the fallback kept its stale range and the
#                         bounds guard was ELIDED (ASan global-buffer-overflow).
#   BUG-772               the spawn race scan walked `orelse.expr` and dropped
#                         `orelse.fallback`.
#
# Every one of them is the SAME shape: the arm exists, so `-Wswitch` is happy;
# one child field of that arm is never mentioned, so a value reachable only
# through that field is invisible to the analysis. The kind-level gate cannot
# see it. This script can.
#
# HOW IT WORKS
# ------------
# 1. A table below maps each NodeKind to the `Node *` / `Node **` child fields
#    reachable from it (derived from `struct Node` in ast.h).
# 2. The table is SELF-CHECKED against ast.h's NodeKind enum: a new NodeKind
#    that is not in the table FAILS the audit, so the table cannot silently rot.
# 3. For each RECURSIVE walker (a function whose body calls itself) in the
#    audited files, every `case NODE_X:` arm is checked for a mention of each of
#    NODE_X's child accessors.
# 4. Anything missing must appear in tools/walker_field_baseline.txt with a
#    justification, or the audit fails.
#
# The baseline is `file:function:KIND:accessor` — line-number-agnostic, so it
# survives unrelated edits but MUST be updated when a walker's descent set
# genuinely changes. That is the point: adding a walker arm that skips a child
# becomes a conscious, reviewed decision instead of an accident.
#
# LIMITS, stated so nobody over-trusts a green run:
#   - Only `switch (x->kind)` walkers are analysed. An if/else chain over kinds
#     is invisible here (walker_default_audit.sh pushes those toward switches).
#   - Arms are chunked between successive `case NODE_` labels, so a NESTED
#     kind-switch inside an arm is attributed to the enclosing arm. That errs
#     toward silence, never toward a false alarm.
#   - Descent is a textual test: the accessor must appear as a whole argument
#     (followed by `,` `)` `;`) or indexed (`[`). That distinguishes a real
#     descent from an identity test such as
#     `if (n->call.callee && n->call.callee->kind == NODE_IDENT)` — verified by
#     deleting a known descent and watching the gate go red. It still proves
#     REACH, not correctness: a walker that passes the child somewhere useless
#     passes here.
#   - Only text inside a `case NODE_X:` ARM counts. A walker that handles some
#     children BEFORE the switch (ir_defer_scan_uses checks conditions at its
#     head) is credited for them via the baseline, and removing that head code
#     would NOT turn this gate red. Measured, and stated so nobody assumes
#     otherwise.
#
# Usage: bash tools/audit_walker_fields.sh [files...]   (default: the safety set)

set -u
cd "$(dirname "$0")/.."

FILES=("$@")
if [ ${#FILES[@]} -eq 0 ]; then
    FILES=(checker.c zercheck_ir.c ir_lower.c emitter.c)
fi

BASELINE="tools/walker_field_baseline.txt"

# ---------------------------------------------------------------------------
# KIND -> child accessors.  "-" means the kind has no Node child at all.
# Derived from `struct Node` (ast.h). Aux structs contribute their Node member:
#   SwitchArm  -> switch_stmt.arms[..].values / .body   (matched as "switch_stmt.arms")
#   DesigField -> struct_init.fields[..].value          (matched as "struct_init.fields")
#   AsmOperand -> asm_stmt.inputs/.outputs[..].expr
# ---------------------------------------------------------------------------
read -r -d '' KIND_TABLE <<'TABLE'
NODE_FILE|file.decls
NODE_FUNC_DECL|func_decl.body
NODE_STRUCT_DECL|-
NODE_ENUM_DECL|-
NODE_UNION_DECL|-
NODE_TYPEDEF|-
NODE_IMPORT|-
NODE_CINCLUDE|-
NODE_INTERRUPT|interrupt.body
NODE_MMIO|-
NODE_GLOBAL_VAR|var_decl.init
NODE_CONTAINER_DECL|-
NODE_VAR_DECL|var_decl.init
NODE_BLOCK|block.stmts
NODE_IF|if_stmt.cond,if_stmt.then_body,if_stmt.else_body
NODE_FOR|for_stmt.init,for_stmt.cond,for_stmt.step,for_stmt.body
NODE_WHILE|while_stmt.cond,while_stmt.body
NODE_DO_WHILE|while_stmt.cond,while_stmt.body
NODE_SWITCH|switch_stmt.expr,switch_stmt.arms
NODE_RETURN|ret.expr
NODE_BREAK|-
NODE_CONTINUE|-
NODE_DEFER|defer.body
NODE_GOTO|-
NODE_LABEL|-
NODE_EXPR_STMT|expr_stmt.expr
NODE_ASM|asm_stmt.inputs,asm_stmt.outputs
NODE_CRITICAL|critical.body
NODE_ONCE|once.body
NODE_SPAWN|spawn_stmt.args
NODE_YIELD|-
NODE_AWAIT|await_stmt.cond
NODE_STATIC_ASSERT|static_assert_stmt.cond
NODE_INT_LIT|-
NODE_FLOAT_LIT|-
NODE_STRING_LIT|-
NODE_CHAR_LIT|-
NODE_BOOL_LIT|-
NODE_NULL_LIT|-
NODE_IDENT|-
NODE_BINARY|binary.left,binary.right
NODE_UNARY|unary.operand
NODE_ASSIGN|assign.target,assign.value
NODE_CALL|call.callee,call.args
NODE_FIELD|field.object
NODE_INDEX|index_expr.object,index_expr.index
NODE_SLICE|slice.object,slice.start,slice.end
NODE_ORELSE|orelse.expr,orelse.fallback
NODE_INTRINSIC|intrinsic.args
NODE_CAST|-
NODE_TYPECAST|typecast.expr
NODE_SIZEOF|-
NODE_STRUCT_INIT|struct_init.fields
TABLE

# --- self-check: the table must cover exactly ast.h's NodeKind enum ----------
ENUM_KINDS=$(sed -n '/^typedef enum {/,/} NodeKind;/p' ast.h |
             grep -oE '\bNODE_[A-Z_0-9]+\b' | sort -u)
TABLE_KINDS=$(printf '%s\n' "$KIND_TABLE" | cut -d'|' -f1 | sort -u)

MISSING_KINDS=$(comm -23 <(printf '%s\n' "$ENUM_KINDS") <(printf '%s\n' "$TABLE_KINDS"))
EXTRA_KINDS=$(comm -13 <(printf '%s\n' "$ENUM_KINDS") <(printf '%s\n' "$TABLE_KINDS"))

if [ -n "$MISSING_KINDS" ] || [ -n "$EXTRA_KINDS" ]; then
    echo "FAIL — tools/audit_walker_fields.sh KIND_TABLE is out of sync with ast.h."
    [ -n "$MISSING_KINDS" ] && { echo "  NodeKinds in ast.h but not in the table:"; \
                                printf '    %s\n' $MISSING_KINDS; }
    [ -n "$EXTRA_KINDS" ]   && { echo "  In the table but not in ast.h:"; \
                                printf '    %s\n' $EXTRA_KINDS; }
    echo "  Add the new kind's Node* child fields (or '-') to KIND_TABLE."
    exit 1
fi

# --- scan ------------------------------------------------------------------
REPORT=$(mktemp)
trap 'rm -f "$REPORT"' EXIT

for f in "${FILES[@]}"; do
    [ -f "$f" ] || continue
    awk -v FILE="$f" -v TABLE="$KIND_TABLE" '
    # A walker "descends" a child when the accessor is handed to something as a
    # WHOLE argument (or indexed, for the array-valued children) — not merely
    # mentioned. `f(n->call.callee, ...)` and `n->call.args[i]` count;
    # `if (n->call.callee && n->call.callee->kind == NODE_IDENT)` does NOT.
    # Without this distinction the gate reported OK on a walker whose only
    # mention of the field was an identity test — verified by deleting a known
    # descent and watching the audit stay green.
    function descends(txt, acc,   pos, ch) {
        pos = index(txt, acc)
        while (pos > 0) {
            ch = substr(txt, pos + length(acc), 1)
            if (ch == "," || ch == ")" || ch == ";" || ch == "[") return 1
            txt = substr(txt, pos + length(acc))
            pos = index(txt, acc)
        }
        return 0
    }
    BEGIN {
        n = split(TABLE, rows, "\n")
        for (i = 1; i <= n; i++) {
            split(rows[i], kv, "|")
            fields[kv[1]] = kv[2]
        }
    }
    # ---- track brace depth so we know where a function body starts/ends ----
    {
        line = $0
        # strip line comments and string literals crudely (enough for depth)
        gsub(/\/\/.*$/, "", line)
        open  = gsub(/{/, "{", line)
        cbr = gsub(/}/, "}", line)
    }
    # candidate signature: accumulate non-comment, non-blank lines at depth 0.
    # Comment text is dropped so a doc-comment mentioning `@ptrcast(` cannot be
    # mistaken for the function name (it was, before this filter).
    depth == 0 {
        sl = $0
        sub(/\/\*.*\*\//, "", sl)
        if (sl ~ /^[ \t]*[\/\*]/ || sl ~ /^[ \t]*$/ || incomment) {
            if ($0 ~ /\/\*/ && $0 !~ /\*\//) incomment = 1
            if ($0 ~ /\*\//) incomment = 0
            sigbuf = ""
            next
        }
        if ($0 ~ /\/\*/ && $0 !~ /\*\//) incomment = 1
        if (sl ~ /[A-Za-z_][A-Za-z0-9_]*[ \t]*\(/ && sl !~ /^[ \t]*(if|for|while|switch|return)\b/)
            sigbuf = sigbuf " " sl
        if (open == 0 && cbr == 0 && sl ~ /;[ \t]*$/) sigbuf = ""
    }

    {
        prev = depth
        depth += open - cbr
        if (prev == 0 && depth > 0) {
            # entering a function body — the name is the last ident before "("
            s = sigbuf
            fname = "__nofn__"
            # the function name is the LAST identifier immediately before a "("
            # that starts the parameter list (scan left-to-right, keep the last).
            rest = s
            while (match(rest, /[A-Za-z_][A-Za-z0-9_]*[ \t]*\(/)) {
                cand = substr(rest, RSTART, RLENGTH)
                sub(/[ \t]*\($/, "", cand)
                if (cand != "if" && cand != "for" && cand != "while" &&
                    cand != "switch" && cand != "return" && cand != "sizeof")
                    fname = cand
                rest = substr(rest, RSTART + RLENGTH)
                break   # first ident( after the return type IS the name
            }
            sigbuf = ""
            delete armtext
            delete armkinds
            narm = 0
            bodybuf = ""
            infunc = 1
        }
        if (infunc) {
            bodybuf = bodybuf "\n" $0
            # collect case labels; a run of consecutive labels shares one arm
            l = $0
            if (l ~ /case[ \t]+NODE_[A-Z_0-9]+[ \t]*:/) {
                if (!inlabels) { narm++; armkinds[narm] = ""; armtext[narm] = "" }
                inlabels = 1
                while (match(l, /case[ \t]+NODE_[A-Z_0-9]+[ \t]*:/)) {
                    k = substr(l, RSTART, RLENGTH)
                    sub(/^case[ \t]+/, "", k); sub(/[ \t]*:$/, "", k)
                    armkinds[narm] = armkinds[narm] " " k
                    l = substr(l, RSTART + RLENGTH)
                }
                # trailing code on the same line still belongs to the arm
                armtext[narm] = armtext[narm] " " l
            } else if (narm > 0) {
                inlabels = 0
                armtext[narm] = armtext[narm] " " $0
            }
        }
        if (infunc && depth == 0) {
            # leaving the function — only report RECURSIVE walkers
            calls = split(bodybuf, _cparts, fname) - 1
            if (calls >= 2 && narm > 0) {
                for (a = 1; a <= narm; a++) {
                    nk = split(armkinds[a], ks, " ")
                    for (ki = 1; ki <= nk; ki++) {
                        kind = ks[ki]
                        if (!(kind in fields)) continue
                        fl = fields[kind]
                        if (fl == "-") continue
                        nf = split(fl, fs, ",")
                        for (fi = 1; fi <= nf; fi++) {
                            acc = fs[fi]
                            if (!descends(armtext[a], acc))
                                printf "%s:%s:%s:%s\n", FILE, fname, kind, acc
                        }
                    }
                }
            }
            infunc = 0; narm = 0; inlabels = 0; bodybuf = ""
        }
    }
    ' "$f" >> "$REPORT"
done

sort -u -o "$REPORT" "$REPORT"

if [ ! -f "$BASELINE" ]; then
    echo "No baseline at $BASELINE — creating it from the current tree."
    { echo "# walker_field_baseline.txt — see tools/audit_walker_fields.sh"
      echo "# Each line: file:function:NODE_KIND:accessor  — a child field a"
      echo "# recursive kind-walker deliberately does NOT descend."
      cat "$REPORT"
    } > "$BASELINE"
    echo "Baseline written ($(wc -l < "$REPORT") entries). Review it."
    exit 0
fi

BASE_ENTRIES=$(grep -v '^#' "$BASELINE" | grep -v '^[[:space:]]*$' | sort -u)
NEW=$(comm -23 "$REPORT" <(printf '%s\n' "$BASE_ENTRIES"))
GONE=$(comm -13 "$REPORT" <(printf '%s\n' "$BASE_ENTRIES"))

if [ -n "$NEW" ]; then
    echo "FAIL — walker arm(s) that name a NodeKind but never descend one of its children:"
    printf '  %s\n' $NEW
    echo
    echo "Each line is 'file:function:KIND:accessor'. Either descend the field, or add"
    echo "the line to $BASELINE with a one-line justification above it."
    exit 1
fi

if [ -n "$GONE" ]; then
    echo "NOTE — baseline entries that no longer apply (the walker now descends them):"
    printf '  %s\n' $GONE
    echo "Remove them from $BASELINE in the same commit (a stale baseline is worse than none)."
    exit 1
fi

echo "OK — no new walker field-coverage gaps ($(printf '%s\n' "$BASE_ENTRIES" | grep -c . ) baselined)"
exit 0
