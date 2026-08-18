#!/bin/bash
# ============================================================================
# Freestanding-preamble audit (BUG-812).
#
# ZER's bare-metal branches key on __STDC_HOSTED__, which is a COMPILER-FLAG
# property (it is what -ffreestanding sets), not a target property — and zerc
# never learns which C flags the emitted file will be compiled with. There was
# also no way for the user to SAY "this is a freestanding target". Most hosted
# branches taken by mistake fail LOUDLY at link time; one did not, and that is
# why this gate exists:
#
#   `@critical` on x86 fell through to __atomic_thread_fence(SEQ_CST) — a memory
#   fence that does NOT disable interrupts. An interrupt landing inside a
#   @critical block runs anyway and every invariant the block protects is
#   unprotected, with no diagnostic and no fault.
#
# `-DZER_FREESTANDING` now declares it explicitly. This audit checks the
# emitted C actually honours that, WITHOUT needing a cross-toolchain: it
# preprocesses with the host gcc and inspects which branches survive.
#
# Checks, on a program using @critical, a trap-capable access, and @once:
#   1. FREESTANDING: no hosted libc header is pulled in (stdio/stdlib/string),
#      and no fprintf/abort/pthread reference survives.
#   2. FREESTANDING on x86: the interrupt-disable branch (cli) is selected,
#      not the fence fallback.
#   3. HOSTED (no flag): unchanged — libc present, fence fallback selected.
#      This is the regression half; the flag must be a pure widening.
#
# Usage: bash tools/audit_freestanding.sh [zerc_path]
# Exit: 0 iff every check holds.
# ============================================================================
set +e
ZERC="${1:-./zerc}"
DIR="$(mktemp -d)"
trap 'rm -rf "$DIR"' EXIT

if [ ! -x "$ZERC" ]; then echo "freestanding audit: no zerc at $ZERC" >&2; exit 1; fi

cat > "$DIR/fs.zer" <<'ZEOF'
volatile u32 counter;
u32[4] table;

void bump() {
    @critical {
        u32 t = counter;
        t += 1;
        counter = t;
    }
}

u32 read_at(u32 i) {
    return table[i];          // unprovable index -> guard/trap machinery
}

u32 main() {
    bump();
    @once { counter = 0; }
    return read_at(0);
}
ZEOF

if ! "$ZERC" "$DIR/fs.zer" -o "$DIR/fs.c" >/dev/null 2>&1; then
    echo "freestanding audit: probe failed to compile" >&2
    "$ZERC" "$DIR/fs.zer" -o "$DIR/fs.c"
    exit 1
fi

fail=0
note() { echo "  $1"; }

# --- 0: the CASE THAT MOTIVATED THE FIX -------------------------------------
# -DZER_FREESTANDING ALONE, with NO -ffreestanding. This is the configuration
# the bug was about: a bare-metal build whose C flags leave __STDC_HOSTED__ at 1
# (gcc -m64 -nostdlib does exactly that), where the old predicate could only
# select the hosted branches. Under -ffreestanding both the old and the new
# predicate agree, so testing only that combination would score a pre-fix
# compiler as PASSING on the @critical half — measured, it does.
if ! gcc -DZER_FREESTANDING -E "$DIR/fs.c" > "$DIR/fs_decl.i" 2>/dev/null; then
    note "FAIL: emitted C does not preprocess with -DZER_FREESTANDING alone"
    fail=1
else
    if grep -q 'cli' "$DIR/fs_decl.i"; then
        note "ok: -DZER_FREESTANDING alone selects the interrupt-disable branch"
    else
        note "FAIL: -DZER_FREESTANDING alone still yields the fence fallback —"
        note "      a bare-metal x86 build without -ffreestanding has NO way to"
        note "      get a @critical that actually disables interrupts"
        fail=1
    fi
    if grep -qE '"[^"]*/stdio\.h"' "$DIR/fs_decl.i"; then
        note "FAIL: -DZER_FREESTANDING alone still pulls in hosted libc"
        fail=1
    fi
fi

# --- 1 + 2: freestanding ----------------------------------------------------
if ! gcc -ffreestanding -DZER_FREESTANDING -E "$DIR/fs.c" > "$DIR/fs.i" 2>"$DIR/fs.err"; then
    echo "  FAIL: emitted C does not PREPROCESS with -ffreestanding -DZER_FREESTANDING"
    head -5 "$DIR/fs.err"
    fail=1
else
    hosted_hdr=$(grep -cE '"[^"]*/(stdio|stdlib|string)\.h"' "$DIR/fs.i")
    if [ "$hosted_hdr" -ne 0 ]; then
        note "FAIL: $hosted_hdr hosted libc header(s) still included under ZER_FREESTANDING"
        fail=1
    else
        note "ok: no hosted libc header under ZER_FREESTANDING"
    fi
    for sym in fprintf abort pthread_mutex_lock sched_yield; do
        if grep -q "\b$sym\b" "$DIR/fs.i"; then
            note "FAIL: '$sym' still referenced under ZER_FREESTANDING"
            fail=1
        fi
    done
    # x86 host: the interrupt-disable branch must win over the fence fallback.
    if grep -q 'cli' "$DIR/fs.i"; then
        note "ok: @critical selects the interrupt-disable branch under ZER_FREESTANDING"
    else
        note "FAIL: @critical still falls back to a fence under ZER_FREESTANDING —"
        note "      a fence does not disable interrupts; the block is unprotected"
        fail=1
    fi
fi

# --- 3: hosted regression ---------------------------------------------------
if ! gcc -E "$DIR/fs.c" > "$DIR/fs_hosted.i" 2>/dev/null; then
    note "FAIL: emitted C no longer preprocesses in the DEFAULT hosted mode"
    fail=1
else
    if grep -qE '"[^"]*/stdio\.h"' "$DIR/fs_hosted.i"; then
        note "ok: hosted mode unchanged (libc still included)"
    else
        note "FAIL: hosted mode lost its libc includes — the flag must be a pure widening"
        fail=1
    fi
    if grep -q '__atomic_thread_fence' "$DIR/fs_hosted.i"; then
        note "ok: hosted @critical still uses the fence (cli/sti is illegal in user mode)"
    else
        note "FAIL: hosted @critical no longer uses the fence fallback"
        fail=1
    fi
fi

echo ""
if [ "$fail" -ne 0 ]; then
    echo "FREESTANDING AUDIT FAILED"
    exit 1
fi
echo "OK — freestanding preamble honours -DZER_FREESTANDING; hosted unchanged."
exit 0
