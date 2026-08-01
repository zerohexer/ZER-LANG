#!/bin/bash
# audit_carrier_dispatch.sh — the "wrapper hides the inner kind" class-kill
# (2026-08-01). Sibling of audit_type_dispatch.sh; different question.
#
# THE BUG CLASS. A safety sink asks a SEMANTIC question ("can this value carry
# a reference into my frame?") using a SYNTACTIC test (a hand-rolled disjunction
# `k == TYPE_POINTER || k == TYPE_SLICE`). Every WRAPPER that hides the inner
# kind then slips the sink silently:
#
#     *T           -> TYPE_POINTER   matches   -> check runs
#     ?*T          -> TYPE_OPTIONAL  NO match  -> falls through EVERY arm
#     distinct *T  -> TYPE_DISTINCT  NO match  -> falls through
#     struct{ *T } -> TYPE_STRUCT    NO match  -> falls through
#     (*T)[N]      -> TYPE_ARRAY     NO match  -> falls through
#
# The wrapper set is FINITE and the codebase already has predicates that close
# over ALL of it (they recurse optional/array/struct/union and unwrap distinct):
#
#     type_carries_data_pointer(t, depth)   — deep: any data pointer at any nesting
#     type_can_carry_pointer(t)             — pointer/slice/struct/union/opaque
#     escape_type_carries_ref(t)            — pointer|slice|optional-of-those
#
# So this is ONE gate for the whole class, not one gate per wrapper. That matters:
# the 2026-08-01 sweep hit THREE of the four wrappers — C2/D2 (optional),
# C7/D1 (by-value struct carrying a pointer), C3/C4 (slice + distinct) — and an
# optional-only gate would have left the struct-carrier shape wide open.
#
# WHY NOT A BLANKET ACCESSOR (the distinct playbook)? Because unwrapping the
# optional is NOT always correct. `type_dispatch_kind()` works for distinct
# because distinct never changes representation. `?T` DOES (`.has_value`), and
# ~93 sites in emitter.c legitimately dispatch on TYPE_OPTIONAL to decide
# emission. A blanket `type_optional_kind()` would fix the safety sinks and
# break the emitter. Hence: a linter that forces a CHOICE per site, not a
# rewrite.
#
# WHEN THIS FIRES you have two honest options:
#   1. Call a carrier predicate above. The line then has no hand-rolled
#      disjunction and never trips this linter. Preferred in a safety sink.
#   2. The site is REPRESENTATION, not safety (emitter deciding `.has_value`,
#      a null-sentinel test, an already-unwrapped `_eff` local, a
#      primitive-only context) -> append the file:content line to
#      tools/carrier_dispatch_baseline.txt with a justification in the commit.
#
# Detection is line-number-agnostic (file:content), same as its siblings, so it
# survives edits that do not change the dispatch line itself.
#
# Run from repo root:  bash tools/audit_carrier_dispatch.sh
# Exit 0 = no new sites. Exit 1 = new sites, listed as file:content.

set -u

cd "$(dirname "$0")/.."

BASELINE="tools/carrier_dispatch_baseline.txt"
if [ ! -f "$BASELINE" ]; then
    echo "ERROR: baseline file missing — $BASELINE" >&2
    exit 2
fi

# The two SAFETY-DECISION files. emitter.c is deliberately EXCLUDED: its
# TYPE_OPTIONAL dispatch is representation (emitting `.has_value` / a null
# sentinel), which is exactly the case that must NOT unwrap.
FILES="checker.c zercheck_ir.c"

CURRENT=$(mktemp)
trap "rm -f $CURRENT" EXIT

for f in $FILES; do
    [ -f "$f" ] || continue
    # A hand-rolled REFERENCE-CARRIER disjunction: two or more carrier kinds
    # OR-ed on one line. That shape is the tell — a single `== TYPE_POINTER`
    # is usually a precise question ("is this exactly a pointer?"), whereas
    # `POINTER || SLICE` is almost always the semantic question "does this
    # carry a reference?", which a predicate should answer.
    grep -nE -- 'kind == TYPE_(POINTER|SLICE|OPAQUE|ARRAY|STRUCT|UNION)[[:space:]]*\|\|' "$f" \
        | sed -E 's|^[0-9]+:[[:space:]]*||' \
        | sed "s|^|$f:|"
done | tr -d '\r' | sort -u > "$CURRENT"

NEW=$(comm -23 "$CURRENT" <(tr -d '\r' < "$BASELINE" | sort -u) || true)

if [ -z "$NEW" ]; then
    total=$(wc -l < "$CURRENT")
    echo "OK — no new hand-rolled carrier dispatches."
    echo "(audit covers $total known sites; new safety code should call"
    echo " type_carries_data_pointer / type_can_carry_pointer /"
    echo " escape_type_carries_ref instead — see checker.c)"
    exit 0
fi

count=$(echo "$NEW" | wc -l)
echo "=== Carrier-dispatch audit: $count NEW hand-rolled carrier test(s) ==="
echo ""
echo "Each line asks 'does this value carry a reference?' with a SYNTACTIC"
echo "kind disjunction. Every wrapper that hides the inner kind (?T, distinct,"
echo "array-of, by-value struct carrying a pointer) will slip it silently —"
echo "the 2026-08-01 §C/§D bug class (C2/C3/C4/C7, D1/D2/D3)."
echo ""
echo "$NEW" | sed 's/^/  /'
echo ""
echo "Prefer a carrier predicate:"
echo "  type_carries_data_pointer(t, 0)  — deep, recurses struct/union/array/optional"
echo "  type_can_carry_pointer(t)        — pointer/slice/struct/union/opaque"
echo "  escape_type_carries_ref(t)       — pointer|slice|optional-of-those"
echo ""
echo "If the site is REPRESENTATION (emitting .has_value, a null-sentinel test,"
echo "an already-unwrapped _eff local), append the file:content line to"
echo "tools/carrier_dispatch_baseline.txt with a justification in the commit."
exit 1
