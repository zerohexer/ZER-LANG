/* src/safety/cast_rules.c — Phase 1 J-extended section
 *
 * Cast / intrinsic shape predicates. Linked into zerc, VST-verified in
 * proofs/vst/verif_cast_rules.v against typing.v Section J-extended.
 *
 * Oracle rules:
 *   J02/J03 conversion_safe        — int↔ptr conversions must use @inttoptr/@ptrtoint
 *   J05     bitcast_valid          — @bitcast requires same width
 *   J06     bitcast_operand_valid  — @bitcast requires primitive type
 *   J07     cast_distinct_valid    — @cast requires at least one distinct side
 *   J08     saturate_operand_valid — @saturate/@truncate requires numeric
 *   J09     ptrtoint_source_valid  — @ptrtoint requires pointer source
 *   J10     cast_types_compatible  — catch-all for unrelated types
 */
#include "cast_rules.h"

int zer_conversion_safe(int kind) {
    if (kind == ZER_CONV_CSTYLE) {
        return 0;
    }
    return 1;
}

int zer_bitcast_width_valid(int src_width, int dst_width) {
    if (src_width == dst_width) {
        return 1;
    }
    return 0;
}

int zer_bitcast_operand_valid(int is_primitive) {
    if (is_primitive == 0) {
        return 0;
    }
    return 1;
}

int zer_cast_distinct_valid(int src_is_distinct, int dst_is_distinct) {
    if (src_is_distinct != 0) {
        return 1;
    }
    if (dst_is_distinct != 0) {
        return 1;
    }
    return 0;
}

int zer_saturate_operand_valid(int is_numeric) {
    if (is_numeric == 0) {
        return 0;
    }
    return 1;
}

int zer_ptrtoint_source_valid(int is_pointer) {
    if (is_pointer == 0) {
        return 0;
    }
    return 1;
}

int zer_cast_types_compatible(int src_tag, int dst_tag) {
    if (src_tag == dst_tag) {
        return 1;
    }
    return 0;
}
