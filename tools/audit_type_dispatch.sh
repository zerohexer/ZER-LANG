#!/bin/bash
# audit_type_dispatch.sh — distinct-unwrap structural kill (2026-06-07).
#
# The #1 historical bug class in this compiler (BUG-409 audit "35+ sites",
# GAP-F 2026-06-06): dispatching a SAFETY decision on `result->kind == TYPE_X`
# where `result` came from checker_get_type()/check_expr()/resolve_type()
# WITHOUT unwrapping TYPE_DISTINCT first. A `distinct typedef *u32 P;` target
# then has kind TYPE_DISTINCT, the `== TYPE_POINTER` check silently fails,
# and the safety check (const-strip, provenance, target-validation) is skipped.
#
# The fix is the `type_dispatch_kind(t)` accessor in types.h (unwraps all
# distinct levels, NULL-safe) — `type_dispatch_kind(result) == TYPE_POINTER`
# cannot forget the unwrap. This linter is the ENFORCEMENT half (the RF14
# `-Wswitch` analog for type dispatch): it freezes the current `->kind ==
# TYPE_` / `->kind != TYPE_` surface and FAILS on any NEW site.
#
# When you add a new type-dispatch site, you have two honest choices:
#   1. Use `type_dispatch_kind(t) == TYPE_X` — preferred when `t` is a fresh
#      result that could be a distinct typedef. The line then contains no
#      `->kind == TYPE_` and never trips this linter.
#   2. If the read is genuinely safe (already-unwrapped `_eff` local, an
#      inner type like `.pointer.inner->kind`, or a primitive-only context),
#      append the `file:content` line to tools/type_dispatch_baseline.txt
#      with a justification in the commit message.
#
# Detection is line-number-agnostic (file:content) so it survives edits that
# don't change the dispatch line itself — same design as audit_fixed_buffers.sh.
#
# Run from repo root:
#   bash tools/audit_type_dispatch.sh
# Exit 0 = no new sites. Exit 1 = new sites found, listed with file:content.

set -u

cd "$(dirname "$0")/.."

BASELINE="tools/type_dispatch_baseline.txt"
if [ ! -f "$BASELINE" ]; then
    echo "ERROR: baseline file missing — $BASELINE" >&2
    exit 2
fi

# Safety-relevant compiler internals. `TYPE_` uniquely identifies semantic
# type-kind dispatch (NODE_ kind dispatch uses NODE_, caught by the walker
# audits instead), so Node/Type ->kind ambiguity is not a problem here.
FILES="checker.c emitter.c zercheck_ir.c ir_lower.c types.c ir.c"

CURRENT=$(mktemp)
trap "rm -f $CURRENT" EXIT

for f in $FILES; do
    [ -f "$f" ] || continue
    # Match the GAP-F comparison shape: `<expr>->kind == TYPE_` and `!= TYPE_`.
    # FLAG #2 (2026-07-01): ALSO the syntactic-TypeNode axis `->kind == TYNODE_`
    # — the T1.4 bug class (gating a safety decision on a syntactic TypeNode kind
    # instead of resolving the type first; a typedef'd pointer is TYNODE_NAMED so
    # a `== TYNODE_POINTER` gate silently drops it). Legitimate TYNODE dispatch
    # (a NAMED pre-filter that then resolves, a declared-qualifier check) is
    # baselined; a NEW site forces resolve-then-check or a justification.
    # Output file:content (leading whitespace trimmed, CR stripped) so the
    # baseline is line-number-agnostic.
    grep -nE -- '->kind == TYPE_|->kind != TYPE_|->kind == TYNODE_|->kind != TYNODE_' "$f" \
        | sed -E 's|^[0-9]+:[[:space:]]*||' \
        | sed "s|^|$f:|"
done | tr -d '\r' | sort -u > "$CURRENT"

# Lines in CURRENT but not BASELINE = new dispatch sites.
NEW=$(comm -23 "$CURRENT" <(tr -d '\r' < "$BASELINE" | sort -u) || true)

if [ -z "$NEW" ]; then
    total=$(wc -l < "$CURRENT")
    echo "OK — no new raw type-dispatch sites."
    echo "(audit covers $total known sites from baseline; new code should"
    echo " prefer type_dispatch_kind() — see types.h)"
    exit 0
fi

count=$(echo "$NEW" | wc -l)
echo "=== Type-dispatch audit: $count NEW raw '->kind == TYPE_' site(s) ==="
echo ""
echo "Each line below dispatches on a type kind WITHOUT going through"
echo "type_dispatch_kind(). If the type came from checker_get_type()/"
echo "check_expr()/resolve_type() and could be a distinct typedef, this is"
echo "the GAP-F / BUG-409 bug class — use type_dispatch_kind(t) == TYPE_X."
echo ""
echo "$NEW" | sed 's/^/  /'
echo ""
echo "If the read is genuinely safe (already-unwrapped _eff local, .inner"
echo "read, or primitive-only context), append the file:content line to"
echo "tools/type_dispatch_baseline.txt with a justification in the commit."
exit 1
