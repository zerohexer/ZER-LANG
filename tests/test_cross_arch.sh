#!/bin/bash
# tests/test_cross_arch.sh — F2/F5/F6-minimum end-to-end verification.
#
# Proves the per-arch register-table dispatch works through the full
# pipeline (ZER → C → cross-gcc → ELF) for all 3 architectures:
#   - x86_64  (native)
#   - aarch64 (via aarch64-linux-gnu-gcc)
#   - riscv64 (via riscv64-linux-gnu-gcc)
#
# Cross-arch positives can't go in tests/zer/ (--run needs native binary).
# Negatives DO live in tests/zer_fail/ since they fail at checker stage,
# which doesn't need a cross-toolchain.
#
# Each positive test below:
#   1. Generates a .zer source using arch-specific register names
#   2. Runs zerc with --target-arch=<arch>
#   3. Cross-compiles the emitted .c with the right toolchain
#   4. Verifies the produced object file is the right ELF arch
#
# Run from repo root: bash tests/test_cross_arch.sh

set -u
cd "$(dirname "$0")/.."

PASS=0
FAIL=0

run_cross_arch_test() {
    local arch="$1"
    local cross_gcc="$2"
    local clobber_list="$3"
    local instr="$4"
    local expected_elf="$5"
    local label="${6:-}"   # optional 6th param: extra suffix on test name
    local name="cross_${arch}${label:+_$label}"

    local src=$(mktemp --suffix=.zer)
    local emitted=$(mktemp --suffix=.c)
    local obj=$(mktemp --suffix=.o)

    cat > "$src" <<EOF
naked void cross_clobber() {
    asm {
        instructions: "$instr"
        clobbers: $clobber_list
        safety: "Cross-arch dispatch test for $arch"
    }
}
i32 main() { return 0; }
EOF

    if ! ./zerc "$src" --target-arch="$arch" -o "$emitted" 2>/dev/null; then
        echo "  FAIL: $name (zerc emit failed)"
        FAIL=$((FAIL + 1))
        rm -f "$src" "$emitted" "$obj"
        return
    fi

    if ! command -v "$cross_gcc" >/dev/null 2>&1; then
        echo "  SKIP: $name ($cross_gcc not in PATH)"
        rm -f "$src" "$emitted" "$obj"
        return
    fi

    if ! "$cross_gcc" -std=c99 -O2 -fwrapv -c "$emitted" -o "$obj" 2>/dev/null; then
        echo "  FAIL: $name (cross-gcc compile failed)"
        FAIL=$((FAIL + 1))
        rm -f "$src" "$emitted" "$obj"
        return
    fi

    local elf_info
    elf_info=$(file "$obj" 2>/dev/null)
    if echo "$elf_info" | grep -q "$expected_elf"; then
        echo "  PASS: $name ($expected_elf ELF produced)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name (wrong ELF: $elf_info)"
        FAIL=$((FAIL + 1))
    fi
    rm -f "$src" "$emitted" "$obj"
}

echo "=== Cross-arch end-to-end tests (F2/F5/F6-minimum) ==="

# x86_64 — host arch (no cross-gcc needed)
run_cross_arch_test \
    "x86_64" "gcc" \
    '[ "rax", "rbx" ]' \
    "movq \$42, %%rax" \
    "x86-64"

# aarch64 — cross-gcc
run_cross_arch_test \
    "aarch64" "aarch64-linux-gnu-gcc" \
    '[ "x0", "x1" ]' \
    "mov x0, #42" \
    "ARM aarch64"

# aarch64 — F5 classified instruction (DMB barrier — C8 category)
# Verifies the instruction-level dispatch on aarch64 picks up DMB and
# doesn't false-reject. Cross-compiles to ARM ELF.
run_cross_arch_test \
    "aarch64" "aarch64-linux-gnu-gcc" \
    '[ "memory" ]' \
    "dmb sy" \
    "ARM aarch64" \
    "dmb_f5"

# riscv64 — F6 classified instruction (FENCE.I — C8 category)
# Verifies the instruction-level dispatch on riscv64 picks up FENCE.I
# (dot in mnemonic — exercises the dot-aware mnemonic parser fix in
# checker.c NODE_ASM and the dot-aware section-header regex in
# scripts/gen_instruction_table.sh).
run_cross_arch_test \
    "riscv64" "riscv64-linux-gnu-gcc" \
    '[ "memory" ]' \
    "fence.i" \
    "RISC-V" \
    "fence_i_f6"

# riscv64 — cross-gcc
run_cross_arch_test \
    "riscv64" "riscv64-linux-gnu-gcc" \
    '[ "a0", "a1" ]' \
    "li a0, 42" \
    "RISC-V"

echo ""
echo "=== Cross-arch results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
