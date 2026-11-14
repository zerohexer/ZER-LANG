/* src/safety/concurrency_rules.c — Phase 1 Batches 9-11
 *
 * Concurrency safety predicates (typing.v sections C, D, F).
 * Linked into zerc, VST-verified in proofs/vst/verif_concurrency_rules.v.
 *
 * Oracle tier: schematic — typing.v has real Coq theorems for all 11 but
 * no operational Iris subset yet (Phase 7 upgrades to operational).
 *
 * Oracle rules:
 *   C01/C02 thread_op_valid         — ThreadHandle alive/joined state machine
 *   C01     thread_cleanup_valid    — unjoined handle at scope exit = leak
 *   C03/C04/C05 spawn_context_valid — spawn banned in ISR/@critical/async
 *   C07     spawn_return_safe       — spawn target cannot return resource
 *   C09     spawn_arg_valid         — spawn arg must be shared ptr OR value
 *   C10     spawn_arg_is_handle     — Handle to spawn rejected
 *   D01     address_of_shared_valid — cannot take &shared_field
 *   D02     shared_in_suspend_valid — lock cannot be held across yield/await
 *   D04     volatile_compound_valid — volatile + compound RMW = race
 *   D05     isr_main_access_valid   — ISR+main access requires volatile
 *   F01-F04 yield_context_valid     — yield/await only in async, not in critical/defer
 */
#include "concurrency_rules.h"

/* C01/C02 — ThreadHandle state machine. joining=1 attempts join. */
int zer_thread_op_valid(int state, int joining) {
    if (joining == 0) {
        return 1;
    }
    if (state == ZER_THR_ALIVE) {
        return 1;
    }
    return 0;
}

/* C01 — unjoined thread at scope exit = leak. */
int zer_thread_cleanup_valid(int state) {
    if (state == ZER_THR_JOINED) {
        return 1;
    }
    return 0;
}

/* C03/C04/C05 — full spawn context check. */
int zer_spawn_context_valid(int in_isr, int in_critical, int in_async) {
    if (in_isr != 0) {
        return 0;
    }
    if (in_critical != 0) {
        return 0;
    }
    if (in_async != 0) {
        return 0;
    }
    return 1;
}

/* C07 — spawn target returning Handle or move struct = resource leak. */
int zer_spawn_return_safe(int returns_resource) {
    if (returns_resource != 0) {
        return 0;
    }
    return 1;
}

/* C09 — spawn arg must be shared pointer OR value (not non-shared ptr). */
int zer_spawn_arg_valid(int is_shared_ptr, int is_value) {
    if (is_shared_ptr != 0) {
        return 1;
    }
    if (is_value != 0) {
        return 1;
    }
    return 0;
}

/* C10 — Handle to spawn rejected (Pool not thread-safe). */
int zer_spawn_arg_is_handle_rejected(int is_handle) {
    if (is_handle != 0) {
        return 0;
    }
    return 1;
}

/* D01 — cannot take &(shared field) — bypasses auto-lock. */
int zer_address_of_shared_valid(int is_shared_field) {
    if (is_shared_field != 0) {
        return 0;
    }
    return 1;
}

/* D02 — lock cannot be held across yield/await.
 * Flat cascade: valid iff NOT (accesses_shared AND has_yield). */
int zer_shared_in_suspend_valid(int accesses_shared, int has_yield) {
    if (accesses_shared == 0) {
        return 1;
    }
    if (has_yield == 0) {
        return 1;
    }
    return 0;
}

/* D04 — volatile + compound RMW = non-atomic race.
 * Flat cascade: valid iff NOT (is_volatile AND is_compound_op). */
int zer_volatile_compound_valid(int is_volatile, int is_compound_op) {
    if (is_volatile == 0) {
        return 1;
    }
    if (is_compound_op == 0) {
        return 1;
    }
    return 0;
}

/* D05 — globals accessed from ISR+main MUST be volatile. */
int zer_isr_main_access_valid(int accessed_in_isr, int accessed_in_main, int is_volatile) {
    if (is_volatile != 0) {
        return 1;
    }
    if (accessed_in_isr == 0) {
        return 1;
    }
    if (accessed_in_main == 0) {
        return 1;
    }
    return 0;
}

/* F01-F04 — yield/await context: in_async required, not in critical/defer. */
int zer_yield_context_valid(int in_async, int in_critical, int in_defer) {
    if (in_async == 0) {
        return 0;
    }
    if (in_critical != 0) {
        return 0;
    }
    if (in_defer != 0) {
        return 0;
    }
    return 1;
}

/* ---- scoped-spawn WINDOW decisions (BUG-1648 / BUG-1651) ----
 * Oracle: lambda_zer_concurrency/window_cfg.v. Flat cascades, VST style. */

/* A closed window's textual join releases; an open one needs its tail join. */
int zer_window_join_releases(int closed, int branch_depth, int in_once, int after_last_jump) {
    if (closed != 0) {
        return 1;
    }
    if (branch_depth != 0) {
        return 0;
    }
    if (in_once != 0) {
        return 0;
    }
    if (after_last_jump == 0) {
        return 0;
    }
    return 1;
}

/* return / goto leave the interior; break / continue only at loop depth 0. */
int zer_window_stmt_leaves(int is_return_or_goto, int is_loop_jump, int loop_depth) {
    if (is_return_or_goto != 0) {
        return 1;
    }
    if (is_loop_jump == 0) {
        return 0;
    }
    if (loop_depth != 0) {
        return 0;
    }
    return 1;
}

/* An open window in a latched walk was already counted at the function entry. */
int zer_window_counts_at_spawn(int closed, int latched) {
    if (closed != 0) {
        return 1;
    }
    if (latched != 0) {
        return 0;
    }
    return 1;
}

/* ---- spin-lock REGION decisions (BUG-1663) ----
 * Oracle: lambda_zer_concurrency/lock_region.v (definitions shared through
 * concurrency_decisions.v). Flat cascades, VST style. */

/* A release is in role iff it closes the region its own acquire opened, once. */
int zer_lock_release_in_role(int closes_own_region, int already_released) {
    if (closes_own_region == 0) {
        return 0;
    }
    if (already_released != 0) {
        return 0;
    }
    return 1;
}

/* An atomic write of a lock word is in role iff it is the sanctioned one
 * (the acquire test / the in-role release) and the word is a whole global. */
int zer_lock_write_in_role(int sanctioned, int whole_word) {
    if (sanctioned == 0) {
        return 0;
    }
    if (whole_word == 0) {
        return 0;
    }
    return 1;
}

/* A local copy of the flag is fresh iff its own non-static declaration read it
 * from sources that are themselves in this section. */
int zer_rmw_copy_fresh(int by_own_declaration, int is_static, int sources_in_section) {
    if (by_own_declaration == 0) {
        return 0;
    }
    if (is_static != 0) {
        return 0;
    }
    if (sources_in_section == 0) {
        return 0;
    }
    return 1;
}

/* ... and may be written back only in the region it was taken in. */
int zer_rmw_copy_in_section(int fresh, int same_region) {
    if (fresh == 0) {
        return 0;
    }
    if (same_region == 0) {
        return 0;
    }
    return 1;
}

/* A read-modify-write is inside one critical section iff it is one statement
 * or writes back only copies taken in this section. */
int zer_rmw_one_section(int one_statement, int copies_in_section) {
    if (one_statement != 0) {
        return 1;
    }
    if (copies_in_section == 0) {
        return 0;
    }
    return 1;
}

/* A held lock covers a read-modify-write only if it is inside one section. */
int zer_rmw_lock_covers(int lock_held, int one_section) {
    if (lock_held == 0) {
        return 0;
    }
    if (one_section == 0) {
        return 0;
    }
    return 1;
}

/* The covered read-modify-write is exempt iff it was seen under exactly one
 * lock and no other thread stores the flag outside that lock. */
int zer_rmw_locked_exempt(int seen, int lock_held, int mixed, int store_outside) {
    if (seen == 0) {
        return 0;
    }
    if (lock_held == 0) {
        return 0;
    }
    if (mixed != 0) {
        return 0;
    }
    if (store_outside != 0) {
        return 0;
    }
    return 1;
}

/* A cell is a lock word iff it is a whole global that every thread shares and
 * the function does not bind the name locally (a `static` local of that name is
 * another word; a `threadlocal` one is a word per thread). */
int zer_lock_word_ok(int whole_word, int thread_local, int shadowed) {
    if (whole_word == 0) {
        return 0;
    }
    if (thread_local != 0) {
        return 0;
    }
    if (shadowed != 0) {
        return 0;
    }
    return 1;
}

/* Every acquire of one word must test the same FREE value: the first acquire
 * fixes it, a later one must agree. */
int zer_lock_acquire_agrees(int first, int same_free) {
    if (first != 0) {
        return 1;
    }
    if (same_free == 0) {
        return 0;
    }
    return 1;
}

/* A `defer` body registered in the block that opened a region fires at that
 * block's exit: with the lock only if the release was deferred BEFORE it. One
 * registered in a block nested in the region fires inside the region. */
int zer_lock_covers_defer(int opened_here, int release_deferred) {
    if (opened_here == 0) {
        return 1;
    }
    if (release_deferred == 0) {
        return 0;
    }
    return 1;
}

/* ---- flag HAND-OFF decisions (BUG-1664) ----
 * Oracle: lambda_zer_concurrency/flag_handoff.v (definitions shared through
 * concurrency_decisions.v). Flat cascades, VST style. */

/* What a comparison of an atomic load with a literal tells its thread about
 * the flag on the branch taken: 1 = it holds the literal, 2 = it holds the
 * OTHER value (only when the flag has exactly two values), 0 = nothing.
 * `agrees`: the operator's sense matches the branch (`==` taken, `!=` not). */
int zer_flag_obs(int agrees, int two_valued) {
    if (agrees != 0) {
        return 1;
    }
    if (two_valued == 0) {
        return 0;
    }
    return 2;
}

/* Under which held value a store of the flag is made: 1 = the value its thread
 * observed (and has not stored since), 2 = the INITIAL value — the thread's
 * first store, made once (in its entry function, outside any loop, in a
 * function with no label), 0 = none: the thread holds nothing. */
int zer_flag_store_from(int holds_explicit, int entry_fn, int in_loop, int in_jump_fn,
                        int stored_before) {
    if (holds_explicit != 0) {
        return 1;
    }
    if (entry_fn == 0) {
        return 0;
    }
    if (in_loop != 0) {
        return 0;
    }
    if (in_jump_fn != 0) {
        return 0;
    }
    if (stored_before != 0) {
        return 0;
    }
    return 2;
}

/* A value that is stored FROM must be claimed by one thread, running once. */
int zer_flag_claim_unique(int single_thread, int repeated) {
    if (single_thread == 0) {
        return 0;
    }
    if (repeated != 0) {
        return 0;
    }
    return 1;
}

/* Two accesses are ordered by a flag iff the flag keeps the hand-off
 * discipline and they are made under two DIFFERENT held values. */
int zer_flag_orders(int flag_valid, int values_differ) {
    if (flag_valid == 0) {
        return 0;
    }
    if (values_differ == 0) {
        return 0;
    }
    return 1;
}
