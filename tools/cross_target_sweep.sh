#!/bin/bash
# ============================================================================
# cross_target_sweep.sh — does the EMITTED C actually build for a non-x86 target?
#
# WHY
# ---
# ZER's architecture story is "delegate the ISA to GCC", and the emitter backs
# that with per-arch `#if` cascades for `@critical`, the `@cpu_*` intrinsics, the
# memory barriers and the trap. NOTHING in `make check` ever compiles the emitted
# C for any target but the host, so a wrong arm in one of those cascades is
# invisible: the host takes a different branch and every gate stays green.
#
# That is not hypothetical. Measured 2026-09-15, on a tree where `make check` was
# fully green:
#
#   BUG-1020  `@critical` emitted `mrs primask` / `cpsid i` on hosted AND
#             bare-metal aarch64 — ARMv7-M encodings that do not exist on
#             ARMv8-A. `@critical` could not be compiled for 64-bit Raspberry Pi
#             OS, Apple silicon Linux or Graviton at all. On hosted RISC-V it
#             built clean and would have faulted on a machine-mode CSR.
#   BUG-1021  `@cpu_disable_int` / `@cpu_enable_int` emitted `cpsid i` / `cpsie i`
#             on aarch64 (same non-existent encodings); `@cpu_save_int_state` /
#             `@cpu_restore_int_state` emitted `mrs primask` on ARM A-profile
#             (M-profile only); `@cpu_breakpoint` emitted the aarch64 `brk #0` on
#             ARMv7, where the mnemonic is `bkpt`.
#
# Every one of those is a BUILD failure, so none of them can hide — but only if
# someone builds. This is that someone.
#
# HOW IT DIFFERS from the emit_audit.sh per-target fingerprint case
# -----------------------------------------------------------------
# That case asks the PREPROCESSOR which arm is selected, needs no toolchain, and
# runs in `make check`. It proves the right arm is CHOSEN. This proves the chosen
# arm ASSEMBLES — which is the half that failed above, since every broken arm was
# correctly chosen and simply not a real instruction on that target. Keep both.
#
# USAGE
#   bash tools/cross_target_sweep.sh [zerc] [dir]
#
# Install the toolchains with:
#   apt-get install gcc-aarch64-linux-gnu gcc-arm-linux-gnueabihf gcc-riscv64-linux-gnu
# Absent toolchains are SKIPPED, not failed, so the script is safe to run
# anywhere. It is a measurement sweep, like ubsan_sweep.sh — NOT part of
# `make check`, because it depends on toolchains a checkout has no right to
# assume.
# ============================================================================
set -u

ZERC="${1:-./zerc}"
DIR="${2:-tests/zer}"

if [ ! -x "$ZERC" ]; then echo "no zerc at $ZERC"; exit 2; fi

# target-triple-gcc | extra flags | label
TARGETS="
aarch64-linux-gnu-gcc||aarch64 hosted
aarch64-linux-gnu-gcc|-ffreestanding|aarch64 bare
arm-linux-gnueabihf-gcc||arm A-profile hosted
arm-linux-gnueabihf-gcc|-ffreestanding|arm A-profile bare
riscv64-linux-gnu-gcc||riscv64 hosted
riscv64-linux-gnu-gcc|-ffreestanding|riscv64 bare
"

# EXPECTED failures — a test that is x86-only BY DESIGN, not a portability bug.
# Matched against the .zer basename. Keep this list SHORT and justified: every
# entry is a program that cannot build off-x86 for a reason the author chose.
#   asm_*                  — the test's own source contains x86 inline asm, which
#                            is the point of the test (ZER's `asm` block delegates
#                            the ISA to GCC, so an x86 asm block is x86-only).
#   orelse_in_asm_operand  — same, an x86 asm block.
#   cinclude_*/cinterop_*  — needs a companion .h on the include path.
#   uN_128bit_*            — `__int128` does not exist on a 32-bit target.
#   dalpha4_context_switch — names specific callee-saved registers, which GCC
#                            reserves on ARM/RISC-V ("s0 cannot be used in asm").
#                            A real limitation, recorded in limitations.md.
#   dalpha12_privileged_*  — `@cpu_syscall`/`sysret`/`iret` have no 32-bit ARM
#                            implementation and say so with a loud `#error`.
#   barrier_*/sem_*/       — Barrier and Semaphore are pthread-based, so they
#   resource_pointer_param   cannot exist on a FREESTANDING target. That is a
#                            floor, not a bug; BUG-1022 made the resulting GCC
#                            error name the reason instead of reporting a missing
#                            typedef in generated C. Only the `bare` rows fail.
#   rt_sem_*/rt_conc_*/    — the same floor across the rust_tests corpus, plus
#   rc_cond_*/rt_cond_*      `@cond_timedwait`, which needs `struct timespec` and
#                            `clock_gettime`. Those still report the raw C message
#                            ("storage size of '_zer_ts0' isn't known") rather than
#                            a named reason — a message-quality residual of
#                            BUG-1022, recorded in limitations.md. Only `bare`.
is_expected() {
    case "$1" in
        asm_*|orelse_in_asm_operand_ok) return 0 ;;
        cinclude_*|cinterop_*)          return 0 ;;
        uN_128bit_*)                    return 0 ;;
        dalpha4_context_switch)         return 0 ;;
        dalpha12_privileged_transitions) return 0 ;;
        barrier_*|sem_*|resource_pointer_param_ok) return 0 ;;
        rt_sem_*|rc_sem_*|rt_conc_*|rc_cond_*|rt_cond_*) return 0 ;;
    esac
    return 1
}

: > /tmp/_zer_xt_count
printf '%s' "$TARGETS" | while IFS='|' read -r cc flags label; do
    [ -z "$cc" ] && continue
    if ! command -v "$cc" >/dev/null 2>&1; then
        printf '  %-26s SKIPPED (no %s)\n' "$label" "$cc"
        continue
    fi
    built=0; expected=0; unexpected=0; names=""
    for f in "$DIR"/*.zer; do
        b=$(basename "$f" .zer)
        "$ZERC" "$f" -o /tmp/_zer_xt.c >/dev/null 2>/dev/null || continue
        if err=$($cc $flags -fwrapv -w -c -o /tmp/_zer_xt.o /tmp/_zer_xt.c 2>&1) && [ -z "$err" ]; then
            built=$((built + 1))
        elif is_expected "$b"; then
            expected=$((expected + 1))
        else
            unexpected=$((unexpected + 1))
            names="$names
    $b: $(printf '%s' "$err" | grep -iE 'error' | head -1 | cut -c1-96)"
        fi
    done
    printf '  %-26s built=%-4d expected-fail=%-3d UNEXPECTED=%d%s\n' \
           "$label" "$built" "$expected" "$unexpected" "$names"
    echo "$unexpected" >> /tmp/_zer_xt_count
done

echo ""
TOTAL_UNEXPECTED=$(awk '{s+=$1} END{print s+0}' /tmp/_zer_xt_count 2>/dev/null)
rm -f /tmp/_zer_xt_count
if [ "$TOTAL_UNEXPECTED" -eq 0 ]; then
    echo "OK — the emitted C builds for every available cross-target."
    exit 0
fi
echo "$TOTAL_UNEXPECTED unexpected cross-target build failure(s)."
echo "Each is emitted C that does not assemble for that target — almost always a"
echo "per-arch #if arm selecting an instruction the target does not have. Fix the"
echo "arm, or justify the program in is_expected() above."
exit 1
