#!/bin/bash
# QEMU-based tests for MMIO access — addresses that a hosted Linux
# runner can't execute. Each test:
#   1. ZER → C via ./zerc --lib (no hosted preamble; we link our own startup)
#   2. C → Cortex-M3 ELF via arm-none-eabi-gcc with startup.c + link.ld
#   3. Run in qemu-system-arm -machine lm3s6965evb -semihosting
#   4. Check exit code (semihost_exit in startup.c plumbs main's return)
#
# Usage: rust_tests/qemu/run_tests.sh [path/to/zerc]
#
# Requires: qemu-system-arm, arm-none-eabi-gcc (installed in Dockerfile).

set -u

ZERC="${1:-./zerc}"
DIR="$(dirname "$0")"
PASS=0
FAIL=0
SKIP=0

# If the cross-toolchain or QEMU aren't available, skip entire suite
# cleanly — this runner is invoked unconditionally from `make check`
# but we don't want to fail a developer box that lacks ARM tools.
if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    echo "=== QEMU MMIO tests: SKIPPED (arm-none-eabi-gcc not installed) ==="
    exit 0
fi
if ! command -v qemu-system-arm >/dev/null 2>&1; then
    echo "=== QEMU MMIO tests: SKIPPED (qemu-system-arm not installed) ==="
    exit 0
fi

echo "=== QEMU MMIO tests (Cortex-M3, semihosting exit) ==="

for f in "$DIR"/rt_*.zer; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .zer)
    base="$DIR/$name"

    # Step 1: ZER → C (--lib strips the hosted preamble so the emitted
    # C has no pthread / stdlib references that the bare-metal target
    # can't link).
    if ! "$ZERC" "$f" --lib -o "$base.c" >/dev/null 2>&1; then
        echo "  FAIL: $name (zerc compile error)"
        FAIL=$((FAIL + 1))
        continue
    fi

    # Step 2: cross-compile with startup + linker script. -include stdint.h
    # gives the ZER-emitted C the uint32_t / int32_t typedefs that --lib
    # strips (preamble removed).
    if ! arm-none-eabi-gcc \
        -mcpu=cortex-m3 -mthumb -nostdlib -ffreestanding -O2 -fwrapv \
        -include stdint.h \
        -T "$DIR/link.ld" -Wl,--gc-sections \
        -o "$base.elf" "$DIR/startup.c" "$base.c" 2> "$base.build.log"; then
        echo "  FAIL: $name (arm-none-eabi-gcc error — see $base.build.log)"
        FAIL=$((FAIL + 1))
        continue
    fi

    # Step 3: run under QEMU with semihosting; capture exit code
    timeout 10 qemu-system-arm \
        -machine lm3s6965evb -nographic -monitor none -serial none \
        -semihosting-config enable=on,target=native \
        -kernel "$base.elf" >/dev/null 2>&1
    ret=$?

    if [ $ret -eq 0 ]; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name (qemu exit $ret)"
        FAIL=$((FAIL + 1))
    fi

    # Cleanup per-test artifacts
    rm -f "$base.c" "$base.elf" "$base.build.log"
done

echo "=== QEMU MMIO tests: $PASS passed, $FAIL failed, $SKIP skipped ==="
[ $FAIL -eq 0 ]
