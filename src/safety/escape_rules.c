/* src/safety/escape_rules.c — pointer-escape rule predicates.
 *
 * See escape_rules.h. Pure predicates matched by Coq specs in
 * proofs/vst/verif_escape_rules.v. Linked into zerc via Makefile.
 *
 * Oracle: lambda_zer_escape/iris_escape_specs.v proves (operationally)
 * that only RegStatic pointers allow step_spec_store_global. Local and
 * Arena pointers leave the state stuck. These predicates encode that
 * rule for the checker.
 */
#include "escape_rules.h"

int zer_region_can_escape(int region) {
    /* Per the λZER-escape oracle: only STATIC region allows escape. */
    if (region == ZER_REGION_STATIC) {
        return 1;
    }
    return 0;
}

int zer_region_is_local(int region) {
    if (region == ZER_REGION_LOCAL) {
        return 1;
    }
    return 0;
}

int zer_region_is_arena(int region) {
    if (region == ZER_REGION_ARENA) {
        return 1;
    }
    return 0;
}
