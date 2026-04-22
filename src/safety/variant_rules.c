/* src/safety/variant_rules.c — Phase 1 P section
 *
 * Union variant safety predicates. Linked into zerc, VST-verified in
 * proofs/vst/verif_variant_rules.v against typing.v Section P.
 *
 * Oracle rules:
 *   P01 — union variant read mode: Direct (unchecked) is unsafe,
 *         Switch (captured) is safe
 *   P02 — cannot mutate union variable inside its own switch arm
 *         (self_access is invalid)
 *
 * See docs/phase1_catalog.md §2.P + docs/formal_verification_plan.md.
 */
#include "variant_rules.h"

int zer_union_read_mode_safe(int mode) {
    if (mode == ZER_URM_SWITCH) {
        return 1;
    }
    return 0;
}

int zer_union_arm_op_safe(int self_access) {
    if (self_access != 0) {
        return 0;
    }
    return 1;
}
