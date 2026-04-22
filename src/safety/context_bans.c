/* src/safety/context_bans.c — context-ban predicates.
 *
 * See context_bans.h. Pure predicates matched by Coq specs in
 * proofs/vst/verif_context_bans.v. Linked into zerc via Makefile.
 *
 * VST-friendly C style: flat cascade of early-return ifs, no nesting,
 * no compound conditions. When logic wants compound, split into
 * separate single-condition ifs.
 */
#include "context_bans.h"

int zer_return_allowed_in_context(int defer_depth, int critical_depth) {
    if (defer_depth > 0) {
        return 0;   /* cannot return from inside defer body */
    }
    if (critical_depth > 0) {
        return 0;   /* return would skip interrupt re-enable */
    }
    return 1;
}

int zer_break_allowed_in_context(int defer_depth, int critical_depth, int in_loop) {
    if (defer_depth > 0) {
        return 0;
    }
    if (critical_depth > 0) {
        return 0;
    }
    if (in_loop == 0) {
        return 0;   /* break only makes sense inside a loop */
    }
    return 1;
}

int zer_continue_allowed_in_context(int defer_depth, int critical_depth, int in_loop) {
    if (defer_depth > 0) {
        return 0;
    }
    if (critical_depth > 0) {
        return 0;
    }
    if (in_loop == 0) {
        return 0;
    }
    return 1;
}

int zer_goto_allowed_in_context(int defer_depth, int critical_depth) {
    if (defer_depth > 0) {
        return 0;
    }
    if (critical_depth > 0) {
        return 0;
    }
    return 1;
}

int zer_defer_allowed_in_context(int defer_depth) {
    if (defer_depth > 0) {
        return 0;   /* nested defer banned */
    }
    return 1;
}

int zer_asm_allowed_in_context(int in_naked) {
    if (in_naked == 0) {
        return 0;   /* asm outside naked would corrupt frame */
    }
    return 1;
}
