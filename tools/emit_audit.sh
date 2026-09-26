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

# BUG-1020 — the @critical interrupt-disable cascade, per TARGET.
#
# `@critical` must disable interrupts on bare metal and must NOT try to on a
# hosted target, where the instruction is privileged. Getting that wrong is
# invisible to a host test run in both directions: the hosted-ARM arms did not
# BUILD at all (so no ZER test could reach them), and hosted RISC-V built clean
# and would have faulted at run time in user mode.
#
# No cross-toolchain needed: the choice is made entirely by the preprocessor, so
# defining the target's own macros and asking `gcc -E` which arm survives tests
# the real cascade. Measured against actual aarch64 / armhf / riscv64 GCC when
# this was written; the simulation agreed with all of them.
cat > "$req_dir/crit.zer" <<'ZEOF'
shared struct Counter { u32 val; }
Counter c;
i32 main() { @critical { c.val += 1; } return 0; }
ZEOF
if "$ZERC" "$req_dir/crit.zer" -o "$req_dir/crit.c" >/dev/null 2>&1; then
    # target-name | preprocessor macros | instruction that MUST appear
    while IFS='|' read -r tname tmacros twant; do
        [ -z "$tname" ] && continue
        got=$(gcc -E $tmacros -x c "$req_dir/crit.c" 2>/dev/null \
              | sed -n '/int32_t main/,/^}/p' \
              | grep -oE 'primask|daifset|cpsid|csrrci|cli|__atomic_thread_fence' \
              | sort -u | tr '\n' ' ')
        case " $got " in
            *" $twant "*) ;;
            *) echo "MISSING EMISSION: @critical on $tname picked '${got:-nothing}', want '$twant'"
               REQ_FAIL=$((REQ_FAIL + 1)) ;;
        esac
    done <<'TARGETS'
cortex-m (bare)|-D__ARM_ARCH=7 -D__ARM_ARCH_PROFILE=77 -ffreestanding|primask
aarch64 (bare)|-D__aarch64__=1 -D__ARM_ARCH=8 -D__ARM_ARCH_PROFILE=65 -ffreestanding|daifset
aarch64 (hosted)|-D__aarch64__=1 -D__ARM_ARCH=8 -D__ARM_ARCH_PROFILE=65|__atomic_thread_fence
arm A-profile (bare)|-D__ARM_ARCH=7 -D__ARM_ARCH_PROFILE=65 -ffreestanding|cpsid
riscv (bare)|-D__riscv=1 -ffreestanding|csrrci
riscv (hosted)|-D__riscv=1|__atomic_thread_fence
x86 (bare)|-ffreestanding|cli
riscv (bare, newlib: hosted C, no OS)|-U__linux__ -U__linux -Ulinux -U__unix__ -U__unix -Uunix -D__riscv=1|csrrci
aarch64 (bare, newlib: hosted C, no OS)|-U__linux__ -U__linux -Ulinux -U__unix__ -U__unix -Uunix -D__aarch64__=1 -D__ARM_ARCH=8 -D__ARM_ARCH_PROFILE=65|daifset
arm R-profile (bare, newlib)|-U__linux__ -U__linux -Ulinux -U__unix__ -U__unix -Uunix -D__ARM_ARCH=7 -D__ARM_ARCH_PROFILE=82|cpsid
TARGETS
else
    echo "MISSING EMISSION: the @critical cascade sample failed to compile"
    REQ_FAIL=$((REQ_FAIL + 1))
fi

# BUG-1275 — the per-statement shared lock inside a BARE switch arm.
#
# `0 => g.x = 5,` (an arm whose body is a statement, not a block) lowered the arm
# without the statement wrapper that carries the lock, so the store to a shared
# struct ran UNLOCKED — the same silent drop as the lock cap above, invisible to
# a hosted test unless two threads happen to collide. The braced arm is the
# control: both must lock.
cat > "$req_dir/barearm.zer" <<'ZEOF'
shared struct G { u32 x; }
G g;
void bare(u32 k)   { switch (k) { 0 => g.x = 5, default => { } } }
void braced(u32 k) { switch (k) { 0 => { g.x = 5; } default => { } } }
u32 main() { bare(0); braced(0); return 0; }
ZEOF
if "$ZERC" "$req_dir/barearm.zer" -o "$req_dir/barearm.c" >/dev/null 2>&1; then
    for fn in bare braced; do
        n=$(sed -n "/^void $fn(/,/^}/p" "$req_dir/barearm.c" | grep -c 'lock(&g\b' || true)
        if [ "$n" -lt 1 ]; then
            echo "MISSING EMISSION: shared store in a $fn switch arm is not locked"
            REQ_FAIL=$((REQ_FAIL + 1))
        fi
    done
else
    echo "MISSING EMISSION: the bare-switch-arm lock sample failed to compile"
    REQ_FAIL=$((REQ_FAIL + 1))
fi

# BUG-1298 — a shared read in a DEFER-BODY condition, in a function WITH a label.
#
# Such a function kept its defer bodies on the raw-AST emitter (emit_defer_stmt),
# which locked a shared access only in an expression STATEMENT, so the condition
# `if (s.v > 3)` read the shared struct with no mutex held. The label-free function
# is the control: both must lock.
cat > "$req_dir/deferlabel.zer" <<'ZEOF'
shared struct S { u32 v; }
S s;
u32 g = 0;
void labelled(u32 k) {
    defer { if (s.v > 3) { g += 1; } }
    if (k > 5) { goto out; }
    g += 100;
out:
    g += 1000;
}
void plain(u32 k) {
    defer { if (s.v > 3) { g += 1; } }
    if (k > 5) { return; }
    g += 100;
}
u32 main() { labelled(1); plain(1); return 0; }
ZEOF
if "$ZERC" "$req_dir/deferlabel.zer" -o "$req_dir/deferlabel.c" >/dev/null 2>&1; then
    for fn in labelled plain; do
        n=$(sed -n "/^void $fn(/,/^}/p" "$req_dir/deferlabel.c" | grep -c 'lock(&s\b' || true)
        if [ "$n" -lt 1 ]; then
            echo "MISSING EMISSION: shared read in a defer-body condition of $fn is not locked"
            REQ_FAIL=$((REQ_FAIL + 1))
        fi
    done
else
    echo "MISSING EMISSION: the labelled-defer lock sample failed to compile"
    REQ_FAIL=$((REQ_FAIL + 1))
fi

# BUG-1344 / BUG-1345 / BUG-1346 / BUG-1347 — bare-metal qualifiers and symbols
# that were DROPPED from the emitted C with no diagnostic:
#   - a whole-array copy to/from a volatile array was a plain memmove (the
#     qualifier cast away; -O2 merged, deleted and hoisted the device accesses);
#   - a `volatile u32[4]` PARAMETER lost its qualifier in the signature;
#   - section(...) was dropped on a volatile global and on an interrupt;
#   - `interrupt X as "SYM"` emitted X_IRQHandler, never SYM.
cat > "$req_dir/bmq.zer" <<'ZEOF'
mmio 0x40000000..0x4000FFFF;
struct Regs { u32 ctrl; u32[4] fifo; }
volatile u32[4] gv;
section(".dma") volatile u8[64] dmabuf;
u32 rd(volatile u32[4] a) { return a[0]; }
volatile u32 tick;
section(".ramfunc") interrupt TIM2 as "TIM2_Handler" { tick = 1; }
u32 main() {
    volatile *Regs r = @inttoptr(*Regs, 0x40000000);
    u32[4] c;
    c = r.fifo;
    gv = c;
    return rd(gv) + dmabuf[1];
}
ZEOF
if "$ZERC" "$req_dir/bmq.zer" -o "$req_dir/bmq.c" >/dev/null 2>&1; then
    body=$(sed -n '/^uint32_t main/,/^}/p' "$req_dir/bmq.c")
    if printf '%s' "$body" | grep -q 'memmove'; then
        echo "MISSING EMISSION: a volatile array copy became a plain memmove (BUG-1344)"
        REQ_FAIL=$((REQ_FAIL + 1))
    fi
    grep -q 'uint32_t rd(volatile uint32_t a\[4\])' "$req_dir/bmq.c" || {
        echo "MISSING EMISSION: a volatile array parameter lost its qualifier (BUG-1345)"
        REQ_FAIL=$((REQ_FAIL + 1)); }
    grep -q 'section(".dma"))) volatile uint8_t dmabuf' "$req_dir/bmq.c" || {
        echo "MISSING EMISSION: section() dropped on a volatile global (BUG-1346)"
        REQ_FAIL=$((REQ_FAIL + 1)); }
    grep -q 'section(".ramfunc"))) TIM2_Handler(void)' "$req_dir/bmq.c" || {
        echo "MISSING EMISSION: interrupt section()/as-name not emitted (BUG-1346/1347)"
        REQ_FAIL=$((REQ_FAIL + 1)); }
else
    echo "MISSING EMISSION: the bare-metal qualifier sample failed to compile"
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
