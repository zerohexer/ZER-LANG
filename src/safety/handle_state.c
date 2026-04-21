/* src/safety/handle_state.c — shared safety predicates on handle state.
 *
 * See handle_state.h for design notes. This .c file is:
 *   - Linked into `zerc` (Makefile CORE_SRCS + LIB_SRCS).
 *   - Clightgen'd by `make check-vst` for VST verification.
 *
 * Functions are intentionally:
 *   - Self-contained (no includes beyond this header).
 *   - Pure (no global state, no side effects, no mutation).
 *   - Primitive types only (no AST nodes, no compiler structs).
 *
 * This keeps the VST proof simple and the correctness guarantee
 * mechanical across the entire input space.
 */
#include "handle_state.h"

int zer_handle_state_is_invalid(int state) {
    if (state == ZER_HS_FREED) {
        return 1;
    }
    if (state == ZER_HS_MAYBE_FREED) {
        return 1;
    }
    if (state == ZER_HS_TRANSFERRED) {
        return 1;
    }
    return 0;
}
