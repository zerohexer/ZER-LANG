/* src/safety/stack_rules.c — Phase 1 S section
 *
 * Stack-resource predicates. Linked into zerc, VST-verified in
 * proofs/vst/verif_stack_rules.v against typing.v Section S.
 *
 * Oracle rules:
 *   S01 — per-function frame size <= --stack-limit
 *   S02 — entry-point call chain depth <= --stack-limit (same predicate)
 *
 * See docs/phase1_catalog.md §2.S.
 */
#include "stack_rules.h"

int zer_stack_frame_valid(int limit, int frame) {
    if (frame <= limit) {
        return 1;
    }
    return 0;
}
