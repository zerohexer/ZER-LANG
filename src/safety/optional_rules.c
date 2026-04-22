/* src/safety/optional_rules.c — optional-type safety predicates.
 *
 * See optional_rules.h. Pure predicates matched by Coq specs in
 * proofs/vst/verif_optional_rules.v. Oracle: typing.v Section N.
 */
#include "optional_rules.h"

int zer_type_permits_null(int type_kind) {
    if (type_kind == ZER_TK_OPTIONAL) {
        return 1;
    }
    return 0;
}

int zer_type_is_nested_optional(int outer_kind, int inner_kind) {
    if (outer_kind != ZER_TK_OPTIONAL) {
        return 0;
    }
    if (inner_kind != ZER_TK_OPTIONAL) {
        return 0;
    }
    return 1;
}
