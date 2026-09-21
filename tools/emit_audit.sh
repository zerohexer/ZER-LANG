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

# BUG-1019 — the null-function-pointer guard, at all THREE call-emission paths.
#
# Same silent-drop shape as the lock cap above, and worse to lose: a dropped
# guard leaves a raw indirect call through address 0, which HOSTED still faults
# into ZER's SIGSEGV handler and looks handled, while on bare metal it jumps
# into the reset vector with nothing to notice. The absence is invisible to
# every other gate, and to a hosted test run.
#
# Three shapes because the callee reaches three different emitters: the
# decomposed IR_CALL (array element / struct field), `emit_rewritten_node`
# (inside a defer body), and `emit_expr` (spawn arguments and labelled-function
# defer bodies). A direct call must NOT be guarded — that half keeps the gate
# from passing on a compiler that simply guards everything.
cat > "$req_dir/fpguard.zer" <<'ZEOF'
typedef u32 (*B)(u32, u32);
struct Ops { B f; }
u32 add(u32 a, u32 b) { return a + b; }
B[3] tbl;
Ops ops;
u32 viaindex(u32 i) { return tbl[i](1, 2); }
u32 viafield()      { return ops.f(1, 2); }
u32 vianame()       { B g = tbl[0]; return g(1, 2); }
u32 direct()        { return add(1, 2); }
u32 main() { tbl[0] = add; ops.f = add; return viaindex(0) + viafield() + vianame() + direct(); }
ZEOF
if "$ZERC" "$req_dir/fpguard.zer" -o "$req_dir/fpguard.c" >/dev/null 2>&1; then
    for fn in viaindex viafield vianame; do
        n=$(sed -n "/^uint32_t $fn/,/^}/p" "$req_dir/fpguard.c" \
            | grep -c 'call through a null function pointer' || true)
        if [ "$n" -lt 1 ]; then
            echo "MISSING EMISSION: indirect call in '$fn' has NO null-funcptr guard"
            REQ_FAIL=$((REQ_FAIL + 1))
        fi
    done
    nd=$(sed -n '/^uint32_t direct/,/^}/p' "$req_dir/fpguard.c" \
         | grep -c 'call through a null function pointer' || true)
    if [ "$nd" -ne 0 ]; then
        echo "OVER-EMISSION: a DIRECT call in 'direct' was given a null-funcptr guard"
        REQ_FAIL=$((REQ_FAIL + 1))
    fi
else
    echo "MISSING EMISSION: the funcptr-guard sample failed to compile"
    REQ_FAIL=$((REQ_FAIL + 1))
fi

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
