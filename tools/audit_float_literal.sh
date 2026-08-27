#!/bin/bash
# audit_float_literal.sh — every `double` reaching emitted C goes through emit_double_lit.
#
# BUG-913: `%.17g` is round-trip exact for FINITE doubles and produces `inf` /
# `-inf` / `nan` for the three that are not — none of which is a C token. It was
# spelled out at FIVE emission sites, so the defect was five defects. The durable
# form is ONE helper; this gate is what stops a sixth site from spelling it again.
#
# The rule: emitter.c may contain a floating-point conversion specifier ONLY
# inside emit_double_lit. Anywhere else is a new site that must call the helper.
#
# Verify this gate FIRES before trusting it (CLAUDE.md: a gate that has only ever
# printed OK is a script, not a net):
#     sed -i 's/emit_double_lit(e, node->float_lit.value);/emit(e, "%.17g", node->float_lit.value);/' emitter.c
#     bash tools/audit_float_literal.sh    # must report the new site and exit 1
set -u
F=emitter.c
[ -f "$F" ] || { echo "audit_float_literal: $F not found" >&2; exit 1; }

# Body of emit_double_lit: from its definition to the closing brace at column 1.
HELPER_RANGE=$(awk '/^static void emit_double_lit\(/{s=NR} s&&/^}/{print s"-"NR; exit}' "$F")
if [ -z "$HELPER_RANGE" ]; then
    echo "audit_float_literal: emit_double_lit not found in $F — did it get renamed?" >&2
    exit 1
fi
HS=${HELPER_RANGE%-*}; HE=${HELPER_RANGE#*-}

# A float conversion inside a string literal passed to emit()/fprintf().
# %[flags][width][.prec](e|f|g|a) — the specifiers printf uses for a double.
BAD=$(grep -nE '"[^"]*%[-+ #0]*[0-9]*(\.[0-9]+)?[efgaEFGA][^"]*"' "$F" \
      | grep -vE '^[0-9]+:[[:space:]]*\*' \
      | awk -F: -v s="$HS" -v e="$HE" '($1 < s || $1 > e)')

if [ -n "$BAD" ]; then
    echo "$BAD"
    echo "FAIL — float conversion outside emit_double_lit (lines $HS-$HE)."
    echo "       A double reaching emitted C must go through emit_double_lit(),"
    echo "       which renders inf/-inf/nan as valid C (__builtin_inf/__builtin_nan)."
    exit 1
fi
echo "OK — every emitted float literal goes through emit_double_lit."
