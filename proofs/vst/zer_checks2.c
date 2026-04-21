/* zer_checks2.c — additional safety predicates.
 * Continues the Level-3 coverage. See zer_checks.c for the
 * pattern; this file adds more checks from sections G, I, N, R, S.
 */

/* ---- Section R02: Comptime argument must be compile-time constant ---- */
/* Model: is_constant is 1 if the argument was evaluated to a constant,
 * 0 if it's a runtime value. */
int comptime_arg_ok(int is_constant) {
    if (is_constant != 0) {
        return 1;
    }
    return 0;
}

/* ---- Section R04: static_assert condition must evaluate true ---- */
int static_assert_ok(int condition) {
    if (condition != 0) {
        return 1;
    }
    return 0;
}

/* ---- Section S01: Stack frame within limit ---- */
int stack_frame_ok(int limit, int frame_size) {
    if (frame_size <= limit) {
        return 1;
    }
    return 0;
}

/* ---- Section S04: slab.alloc() forbidden in ISR ---- */
int slab_alloc_allowed(int in_isr) {
    if (in_isr != 0) {
        return 0;  /* in ISR — reject */
    }
    return 1;
}

/* ---- Section S05: slab.alloc() forbidden in @critical ---- */
int slab_in_critical_allowed(int in_critical) {
    if (in_critical != 0) {
        return 0;
    }
    return 1;
}

/* ---- Section G01-G03: context-flag checks ---- */

/* return_safe: return allowed unless inside @critical or defer. */
int return_safe(int in_critical, int in_defer) {
    if (in_critical != 0) {
        return 0;
    }
    if (in_defer != 0) {
        return 0;
    }
    return 1;
}

/* ---- Section G04: nested defer forbidden ---- */
int defer_safe(int in_defer) {
    if (in_defer != 0) {
        return 0;
    }
    return 1;
}

/* ---- Section G10: asm only in naked functions ---- */
int asm_safe(int in_naked) {
    if (in_naked != 0) {
        return 1;
    }
    return 0;
}

/* ---- Section F01/F02: yield/await only in async ---- */
int yield_safe(int in_async) {
    if (in_async != 0) {
        return 1;
    }
    return 0;
}

/* ---- Section C03: spawn forbidden in ISR ---- */
int spawn_in_isr_allowed(int in_isr) {
    if (in_isr != 0) {
        return 0;
    }
    return 1;
}

/* ---- Section C02: double-join detection ---- */
/* thread_state: 0 = ThreadAlive (not joined), 1 = ThreadJoined */
int thread_join_safe(int thread_state) {
    if (thread_state != 0) {
        return 0;  /* already joined — reject */
    }
    return 1;
}

/* ---- Section D01: cannot take address of shared struct field ---- */
int address_of_safe(int is_shared_field) {
    if (is_shared_field != 0) {
        return 0;
    }
    return 1;
}

/* ---- Section E02: atomic arg must be ptr-to-int ---- */
int atomic_arg_ok(int is_ptr_to_int) {
    if (is_ptr_to_int != 0) {
        return 1;
    }
    return 0;
}
