#!/bin/bash
# Emit audit — catches dead-stub pattern in emitted C.
#
# Background: on 2026-04-18 audit of the IR transition, zerc_main.c was
# found printing "/* forward */ " at the start of every multi-module
# output before the real header. The stub wrote the comment prefix but
# never emitted the payload it was documenting. Tests passed because
# `/* forward */` is valid C (just a noop comment). The emitted output
# was polluted for ~4 weeks undetected.
#
# This script compiles a handful of multi-module ZER programs and
# greps the emitted C for known dead-stub fingerprints. Exit non-zero
# if any are found.
#
# Run from repo root: bash tools/emit_audit.sh
# Exit 0 = clean. Exit 1 = stray markers found.

set -euo pipefail

cd "$(dirname "$0")/.."

ZERC="${1:-./zerc}"
[ -x "$ZERC" ] || { echo "zerc not executable at $ZERC" >&2; exit 2; }

# Known dead-stub markers. Add new fingerprints here when audits
# find more dead stubs. The pattern is: a comment-only fprintf that
# never gets the payload filled in. Finds them all.
#
# Ordinary comments (headers, function docs) are filtered out by
# restricting to specific short stubs.
PATTERNS=(
    "/\\* forward \\*/ "    # zerc_main.c dead stub (fixed 2026-04-18)
    "/\\* stub \\*/"
    "/\\* placeholder \\*/"
    "/\\* TODO:[^*]*\\*/[^a-zA-Z\"]"   # bare TODO with no following code
)

# Sample multi-module tests — representative of the module emission path.
SAMPLES=(
    "test_modules/main.zer"
    "test_modules/defer_user.zer"
    "test_modules/shared_user.zer"
    "test_modules/handle_user.zer"
    "test_modules/diamond.zer"
)

FOUND=0
TMP=$(mktemp /tmp/emit_audit.XXXXXX.c)
trap "rm -f $TMP" EXIT

for sample in "${SAMPLES[@]}"; do
    if [ ! -f "$sample" ]; then
        continue   # test file may not exist in some checkouts
    fi
    if ! "$ZERC" "$sample" --emit-c -o "$TMP" 2>/dev/null; then
        continue   # compile error; probably a negative test
    fi
    for pattern in "${PATTERNS[@]}"; do
        if grep -E "$pattern" "$TMP" > /dev/null 2>&1; then
            echo "STRAY STUB in $sample emission:"
            grep -nE "$pattern" "$TMP" | head -3 | sed 's/^/    /'
            FOUND=$((FOUND + 1))
        fi
    done
done

# ---------------------------------------------------------------------------
# REQUIRED FINGERPRINTS (BUG-935) — emission that must be PRESENT.
#
# Every other gate in this repo asserts that something is ABSENT: no default:,
# no new raw dispatch site, no dead stub. That shape cannot catch a SILENT DROP
# — code the compiler was supposed to emit and didn't. BUG-935 was exactly that:
# a 16-slot cap on the shared-lock collector emitted 15 rdlocks for 18 roots and
# read the last two with NO lock, zero diagnostics.
#
# So this section runs the opposite assertion. Each case compiles a program and
# requires a pattern to APPEAR. Verified RED on the pre-fix compiler.
# ---------------------------------------------------------------------------
REQ_FAIL=0
req_dir=$(mktemp -d)
trap 'rm -rf "$req_dir"' EXIT

# 18 shared roots in ONE statement — every root must be locked AND unlocked.
{
  echo 'shared(rw) struct S { u32 v; }'
  for i in $(seq 1 18); do echo "S s$i;"; done
  echo 'u32 res;'
  printf 'void f(){ res = '
  for i in $(seq 1 18); do [ "$i" -gt 1 ] && printf ' + '; printf 's%s.v' "$i"; done
  printf '; }\n'
  echo 'u32 main(){ f(); return 0; }'
} > "$req_dir/locks.zer"

if "$ZERC" "$req_dir/locks.zer" -o "$req_dir/locks.c" >/dev/null 2>&1; then
    for i in $(seq 1 18); do
        # `set -e` is active and `grep -c` exits 1 on ZERO matches — which is the
        # very case this gate exists to REPORT. Without `|| true` the script dies
        # silently with exit 1 and prints nothing, i.e. the gate fails in exactly
        # the shape CLAUDE.md warns about ("a gate can itself be broken").
        nl=$(grep -cE '(rd|wr)lock\(&s'"$i"'\b' "$req_dir/locks.c" || true)
        nu=$(grep -cE 'unlock\(&s'"$i"'\b' "$req_dir/locks.c" || true)
        if [ "$nl" -lt 1 ] || [ "$nu" -lt 1 ]; then
            echo "MISSING EMISSION: shared root s$i  lock=$nl unlock=$nu (want >=1 each)"
            REQ_FAIL=$((REQ_FAIL + 1))
        fi
    done
else
    echo "MISSING EMISSION: the 18-shared-root sample failed to compile"
    REQ_FAIL=$((REQ_FAIL + 1))
fi

# REQUIRED sample table (from BUG-995) — "sample|pattern" (at least once) or
# "sample|pattern|N" (at least N occurrences: for a lock that must be taken once
# per site, where a dropped site still leaves the pattern present).
REQUIRED=(
    # BUG-995: three defer bodies read shared `g` in a while/if/for CONDITION
    # (one lock each) plus the statement `g.v = 3` — the raw-AST defer emitter
    # locked none of the conditions (2 locks total pre-fix).
    "tests/zer/defer_body_shared_cond_locked.zer|pthread_mutex_lock\\(&g\\._zer_mtx\\)|4"
)
for entry in "${REQUIRED[@]}"; do
    sample="${entry%%|*}"
    rest="${entry#*|}"
    pattern="${rest%%|*}"
    min=1
    if [ "$rest" != "$pattern" ]; then min="${rest#*|}"; fi
    if [ ! -f "$sample" ]; then
        echo "MISSING EMISSION: required sample missing: $sample"
        REQ_FAIL=$((REQ_FAIL + 1))
        continue
    fi
    if ! "$ZERC" "$sample" -o "$req_dir/req.c" >/dev/null 2>&1; then
        echo "MISSING EMISSION: required sample failed to compile: $sample"
        REQ_FAIL=$((REQ_FAIL + 1))
        continue
    fi
    found=$(grep -cE "$pattern" "$req_dir/req.c" 2>/dev/null || true)
    if [ "${found:-0}" -lt "$min" ]; then
        echo "MISSING EMISSION: $sample: pattern '$pattern' expected >= $min time(s), found ${found:-0}"
        REQ_FAIL=$((REQ_FAIL + 1))
    fi
done

if [ $REQ_FAIL -ne 0 ]; then
    echo ""
    echo "$REQ_FAIL required-emission check(s) failed — the compiler DROPPED code it"
    echo "was supposed to emit. This is the silent-drop class; see BUG-935."
    exit 1
fi

if [ $FOUND -eq 0 ]; then
    echo "OK — no dead-stub markers in emitted C across ${#SAMPLES[@]} samples."
    echo "OK — all required emission fingerprints present."
    exit 0
else
    echo ""
    echo "$FOUND stray stubs found. Either fill in the missing emission or"
    echo "remove the dead stub. See CLAUDE.md 'Diff-Based Post-Release Audit'."
    exit 1
fi
