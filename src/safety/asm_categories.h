/* src/safety/asm_categories.h — universal precondition category dispatch.
 *
 * D-Alpha-7.5 Session F1a: framework only. The 8 universal categories
 * classify per-instruction preconditions across x86-64, ARM64, RISC-V
 * (and extensible to PowerPC, Cortex-M, etc.). Each category maps to
 * an existing ZER safety system (mostly) or System #30 (Atomic Ordering,
 * Session G).
 *
 * Research artifact: docs/asm_preconditions_research.md C1-C8 sections.
 *
 * Per-arch instruction tables generated at zerc build time from
 * Capstone/XED/ARM-XML/RISC-V-opcodes (see docs/asm_plan.md "SESSIONS C
 * + F ARCHITECTURE" notice). Vendored as src/safety/asm_categories_*.c
 * once F2 (build-time-gen pipeline) and F3-F6 (per-arch tables) ship.
 *
 * Today (F1a): empty tables. zer_asm_category returns 0 for everything.
 * Wiring is in place but not load-bearing until F4-F6 land real data.
 */
#ifndef ZER_ASM_CATEGORIES_H
#define ZER_ASM_CATEGORIES_H

#include <stddef.h>
#include <stdint.h>

/* Architecture identifiers. Matches GCC's __ARCH__ macros conceptually. */
typedef enum {
    ZER_ARCH_UNKNOWN = 0,
    ZER_ARCH_X86_64,
    ZER_ARCH_AARCH64,
    ZER_ARCH_RISCV64,
    /* Future: ZER_ARCH_POWERPC, ZER_ARCH_CORTEX_M */
} ZerArchId;

/* The 8 universal precondition categories. Each is a bit flag so an
 * instruction can require multiple categories (e.g., LDXR requires both
 * C2 alignment and C3 state machine). */
typedef enum {
    ZER_CAT_NONE              = 0,
    ZER_CAT_C1_VALUE_RANGE    = 1u << 0, /* operand range (bsr on zero, shift count) — uses #12 VRP */
    ZER_CAT_C2_ALIGNMENT      = 1u << 1, /* memory operand alignment — extends #20 Qualifier */
    ZER_CAT_C3_STATE_MACHINE  = 1u << 2, /* LL/SC pairing, monitor/mwait — Model 1 extension */
    ZER_CAT_C4_CPU_FEATURE    = 1u << 3, /* AVX-512 etc. — extends #24 Context Flags */
    ZER_CAT_C5_PRIVILEGE      = 1u << 4, /* kernel-only insn — extends #24 */
    ZER_CAT_C6_MEMORY_ADDR    = 1u << 5, /* canonical address, segment — uses #19 MMIO */
    ZER_CAT_C7_PROVENANCE     = 1u << 6, /* type-erased aliasing — uses #3 Provenance + #11 Escape */
    ZER_CAT_C8_MEMORY_ORDER   = 1u << 7, /* barrier/atomic ordering — System #30 (Session G) */
} ZerAsmCategory;

/* Look up the category bitmap for a given instruction mnemonic on a
 * given architecture. Returns 0 if the instruction is unknown OR has
 * no category-level preconditions (most instructions are uncategorized
 * — they fall back to per-instruction safety via Z-rules + structural
 * rules).
 *
 * F1a stub: always returns 0. F4-F6 ship per-arch data.
 *
 * The function is pure — same inputs always produce the same output.
 * Suitable for Phase 1 extraction with a deterministic VST proof
 * against the linked vendored tables. */
uint32_t zer_asm_category(ZerArchId arch, const char *insn, size_t insn_len);

/* Translate a category bitmap into a human-readable string for error
 * messages. Caller-owned static buffer; not thread-safe (single-threaded
 * checker). Returns "none" for ZER_CAT_NONE.
 *
 * Used by future per-category enforcement (F7) to build clear error
 * messages: "instruction 'bsr' requires C1 (value-range) — operand
 * not proven non-zero". */
const char *zer_asm_category_name(uint32_t cat_bitmap);

#endif /* ZER_ASM_CATEGORIES_H */
