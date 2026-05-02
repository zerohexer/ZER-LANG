/* src/safety/asm_instruction_table.h — D-Alpha-7.5 Session F4
 *
 * Per-instruction safety metadata, generated from arch_data/<arch>.zerdata.
 * Maps each mnemonic to its safety category bitmap, required CPU features,
 * and vendor citation for error diagnostics.
 *
 * F4: x86_64 instruction table (gold set of 10 instructions in F4.1, full
 * coverage in F4.2). F5/F6 add aarch64/riscv64 tables.
 *
 * Architecture: vendored .c file linked into zerc binary. Lookup at
 * runtime is linear scan (suffices for current N; can become hash table
 * if performance ever matters). NOT user-extensible — the design decision
 * to keep this compiler-team-owned was made 2026-04-29; see
 * docs/asm_plan.md "Sub-Extension Architecture" section.
 */
#ifndef ZER_ASM_INSTRUCTION_TABLE_H
#define ZER_ASM_INSTRUCTION_TABLE_H

#include <stddef.h>
#include <stdint.h>
#include "asm_categories.h"

/* F7-full Step 1 (2026-05-02): per-operand constraint encoding.
 *
 * Each instruction may impose constraints on individual operands at the
 * time of asm-block dispatch. Constraints are derived from vendor specs
 * (Intel SDM, ARM ARM, RISC-V ISA) and encoded in arch_data/<arch>.zerdata
 * via `operand[N].constraint = ...` syntax.
 *
 * Step 1 (this entry): the data plumbing — generator now emits these,
 * struct now stores them, vendored tables now contain them.
 *
 * Step 2 (future session): the dispatch — at NODE_ASM check, when a
 * C1/C2-classified instruction matches, look up which operand index
 * has the constraint, find the matching asm operand binding, query
 * VRP (for C1) or qualifier system (for C2), error if unprovable. */

typedef enum {
    ZER_OPC_NONE                          = 0,  /* no constraint on this operand */
    ZER_OPC_NONZERO                       = 1,  /* operand must be provably non-zero — VRP check */
    ZER_OPC_ALIGNED                       = 2,  /* memory operand must be N-byte aligned (param1 = N) */
    ZER_OPC_BOUNDED                       = 3,  /* operand value in [param1, param2] */
    ZER_OPC_COMPOUND_NONZERO_NOT_INTMIN   = 4,  /* IDIV-style: non-zero AND not INT_MIN/-1 overflow */
} ZerOperandConstraintKind;

typedef struct {
    uint8_t kind;       /* ZerOperandConstraintKind */
    uint32_t param1;    /* alignment N, or BOUNDED min */
    int32_t param2;     /* BOUNDED max (signed for negative ranges) */
} ZerOperandConstraint;

#define ZER_OPC_MAX_OPERANDS 4

/* Per-instruction entry. Fields:
 *   mnemonic            — null-terminated string, e.g., "bsr"
 *   mnemonic_len        — strlen, cached for fast scan
 *   category_bits       — ZerAsmCategory bitmap (C1-C8)
 *   feature_bits        — ZerCpuFeature bitmap (e.g., AVX512F)
 *   source              — vendor citation, e.g., "Intel SDM Vol 2A BSR"
 *   consequence         — what goes wrong if precondition violated
 *   operand_count       — declared operand count (0 if not classified)
 *   operand_constraints — per-operand constraint (indexed by operand position;
 *                         positions ≥ operand_count are ZER_OPC_NONE) */
typedef struct {
    const char *mnemonic;
    size_t mnemonic_len;
    uint32_t category_bits;
    uint32_t feature_bits;
    const char *source;
    const char *consequence;
    uint8_t operand_count;
    ZerOperandConstraint operand_constraints[ZER_OPC_MAX_OPERANDS];
} ZerInstructionEntry;

/* Per-arch tables. extern declarations; definitions in arch-specific
 * AUTO-GENERATED .c files committed by scripts/gen_instruction_table.sh.
 *
 * Today (F4.1): only x86_64 table populated with gold set of 10 entries.
 * F5 adds aarch64; F6 adds riscv64; F4.2 expands x86_64 to ~1500 entries. */
extern const ZerInstructionEntry zer_x86_64_instructions[];
extern const size_t zer_x86_64_instruction_count;

/* F5 (2026-04-29): aarch64 instruction table — 31 safety-relevant
 * entries spanning C3 (LL/SC pairs), C4 (NEON/AES/SHA/CRC features),
 * C5 (privileged: ERET, HVC, SMC, MSR, AT, TLBI, etc.), and C8 (DMB,
 * DSB, ISB barriers). Generated from arch_data/aarch64.zerdata. */
extern const ZerInstructionEntry zer_aarch64_instructions[];
extern const size_t zer_aarch64_instruction_count;

/* F6 (2026-05-02): riscv64 instruction table — 30 safety-relevant
 * entries spanning C2+C3 (LR/SC + AMO atomic ops), C4 (F/D/V/C/Zbb
 * extensions), C5 (privileged: MRET/SRET/WFI/CSR ops/SFENCE), and
 * C8 (FENCE/FENCE.I/FENCE.TSO barriers). RISC-V semantics differ
 * from x86/aarch64: DIV doesn't trap on zero (no C1), AMO requires
 * natural alignment (C2+C4), CSRs gate privilege.
 * Generated from arch_data/riscv64.zerdata. */
extern const ZerInstructionEntry zer_riscv64_instructions[];
extern const size_t zer_riscv64_instruction_count;

/* Diagnostic info for a matched instruction. Populated by extended lookup
 * variant when caller wants the citation/consequence for error messages.
 *
 * F7-full Step 2 (2026-05-02): includes operand_count + operand_constraints
 * so the dispatcher can enforce per-operand NONZERO/ALIGNED/BOUNDED checks
 * via existing VRP / qualifier safety systems. */
typedef struct {
    uint32_t category_bits;
    uint32_t feature_bits;
    const char *source;       /* vendor citation; NULL if unknown instruction */
    const char *consequence;  /* effect of violation; NULL if unknown */
    uint8_t operand_count;
    ZerOperandConstraint operand_constraints[ZER_OPC_MAX_OPERANDS];
} ZerInstructionInfo;

/* Look up full instruction info (including source citation + consequence
 * for error messages). Returns 1 if found, 0 if unknown.
 *
 * Used by checker NODE_ASM handler to dispatch per-category safety checks
 * with informative error messages. */
int zer_asm_instruction_info(
    ZerArchId arch,
    const char *mnemonic,
    size_t mnemonic_len,
    ZerInstructionInfo *out_info);

#endif /* ZER_ASM_INSTRUCTION_TABLE_H */
