/* src/safety/asm_categories.c — universal precondition category dispatch.
 *
 * F4 (2026-04-29): real lookup against vendored per-arch instruction
 * tables (was F1a stub returning 0). Linear scan suffices for current N
 * (10 entries today, ~1500 in F4.2). Can become hash table if perf ever
 * matters; LSP-responsive at current sizes.
 *
 * VST-friendly C style: flat cascade, no compound conditions.
 *
 * Oracle: docs/asm_preconditions_research.md (8 categories formally
 * defined). VST proof in proofs/vst/verif_asm_categories.v gains content
 * as F4 data lands; today the lookup matches the vendored table by
 * construction.
 */
#include <string.h>
#include "asm_categories.h"
#include "asm_instruction_table.h"

/* Internal: scan a single per-arch table for a mnemonic. Returns the
 * matching entry or NULL. Comparison is case-insensitive at the byte
 * level — instruction mnemonics in user code may be either case.
 *
 * VST-friendly: flat early-return cascade. */
static const ZerInstructionEntry *scan_table(
    const ZerInstructionEntry *table,
    const char *insn,
    size_t insn_len)
{
    if (table == 0) {
        return 0;
    }
    if (insn == 0) {
        return 0;
    }
    if (insn_len == 0) {
        return 0;
    }
    for (size_t i = 0; table[i].mnemonic != 0; i++) {
        if (table[i].mnemonic_len != insn_len) {
            continue;
        }
        /* Case-insensitive compare: convert each byte to lowercase. */
        size_t j;
        int match = 1;
        for (j = 0; j < insn_len; j++) {
            char a = insn[j];
            char b = table[i].mnemonic[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) { match = 0; break; }
        }
        if (match) {
            return &table[i];
        }
    }
    return 0;
}

uint32_t zer_asm_category(ZerArchId arch, const char *insn, size_t insn_len) {
    if (arch == ZER_ARCH_UNKNOWN) {
        return 0;
    }
    if (insn == 0) {
        return 0;
    }
    if (insn_len == 0) {
        return 0;
    }
    const ZerInstructionEntry *entry = 0;
    if (arch == ZER_ARCH_X86_64) {
        entry = scan_table(zer_x86_64_instructions, insn, insn_len);
    } else if (arch == ZER_ARCH_AARCH64) {
        entry = scan_table(zer_aarch64_instructions, insn, insn_len);
    } else if (arch == ZER_ARCH_RISCV64) {
        /* F6 (2026-05-02): riscv64 instruction-level safety classification.
         * 30 entries covering LR/SC pairs, AMO atomics, F/D/V/C/Zbb
         * features, M/S privileged ops, and FENCE family. */
        entry = scan_table(zer_riscv64_instructions, insn, insn_len);
    }
    if (entry == 0) {
        return 0;
    }
    return entry->category_bits;
}

int zer_asm_instruction_info(
    ZerArchId arch,
    const char *mnemonic,
    size_t mnemonic_len,
    ZerInstructionInfo *out_info)
{
    if (out_info == 0) {
        return 0;
    }
    out_info->category_bits = 0;
    out_info->feature_bits = 0;
    out_info->source = 0;
    out_info->consequence = 0;
    out_info->operand_count = 0;
    for (int i = 0; i < ZER_OPC_MAX_OPERANDS; i++) {
        out_info->operand_constraints[i].kind = 0;
        out_info->operand_constraints[i].param1 = 0;
        out_info->operand_constraints[i].param2 = 0;
    }
    /* Session G Phase 1: zero-initialize the ordering effect. Default
     * (NONE/NONE) is correct for all non-C8 instructions. */
    out_info->ordering.kind = 0;
    out_info->ordering.role = 0;

    if (arch == ZER_ARCH_UNKNOWN) {
        return 0;
    }
    if (mnemonic == 0) {
        return 0;
    }
    if (mnemonic_len == 0) {
        return 0;
    }
    const ZerInstructionEntry *entry = 0;
    if (arch == ZER_ARCH_X86_64) {
        entry = scan_table(zer_x86_64_instructions, mnemonic, mnemonic_len);
    } else if (arch == ZER_ARCH_AARCH64) {
        entry = scan_table(zer_aarch64_instructions, mnemonic, mnemonic_len);
    } else if (arch == ZER_ARCH_RISCV64) {
        entry = scan_table(zer_riscv64_instructions, mnemonic, mnemonic_len);
    }
    if (entry == 0) {
        return 0;
    }
    out_info->category_bits = entry->category_bits;
    out_info->feature_bits = entry->feature_bits;
    out_info->source = entry->source;
    out_info->consequence = entry->consequence;
    /* F7-full Step 2: copy per-operand constraints into the lookup result
     * so the checker's NODE_ASM dispatch can enforce them via existing
     * safety systems (VRP for NONZERO/BOUNDED/COMPOUND, qualifier for
     * ALIGNED). */
    out_info->operand_count = entry->operand_count;
    for (int i = 0; i < ZER_OPC_MAX_OPERANDS; i++) {
        out_info->operand_constraints[i] = entry->operand_constraints[i];
    }
    /* Session G Phase 1: copy ordering effect for Stage 5 System #30
     * dispatch. ZER_BARRIER_NONE / ZER_ORDERING_ROLE_NONE for non-C8
     * entries (no behavior change today). */
    out_info->ordering = entry->ordering;
    return 1;
}

const char *zer_asm_category_name(uint32_t cat_bitmap) {
    if (cat_bitmap == 0) {
        return "none";
    }
    /* Single-category quick names. For combined bitmaps, return the
     * highest-priority single name; full bitmap rendering is left to
     * the diagnostics caller. */
    if (cat_bitmap & 1u) {
        return "C1 (value-range)";
    }
    if (cat_bitmap & 2u) {
        return "C2 (alignment)";
    }
    if (cat_bitmap & 4u) {
        return "C3 (state-machine)";
    }
    if (cat_bitmap & 8u) {
        return "C4 (cpu-feature)";
    }
    if (cat_bitmap & 16u) {
        return "C5 (privilege)";
    }
    if (cat_bitmap & 32u) {
        return "C6 (memory-addressability)";
    }
    if (cat_bitmap & 64u) {
        return "C7 (provenance)";
    }
    if (cat_bitmap & 128u) {
        return "C8 (memory-ordering)";
    }
    return "unknown";
}
