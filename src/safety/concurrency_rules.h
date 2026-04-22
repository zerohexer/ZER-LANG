/* src/safety/concurrency_rules.h
 *
 * Pure predicates for concurrency safety (typing.v C + D + F sections).
 * Linked into zerc, VST-verified in proofs/vst/verif_concurrency_rules.v.
 *
 * Oracle tier: SCHEMATIC — typing.v has real theorems but no operational
 * Iris subset yet. Phase 7 will add λZER-concurrency for operational backing.
 * Current VST guarantee: C code matches typing.v predicate definition.
 */
#ifndef ZER_SAFETY_CONCURRENCY_RULES_H
#define ZER_SAFETY_CONCURRENCY_RULES_H

/* Thread state encoding (C01/C02). */
#define ZER_THR_ALIVE   0
#define ZER_THR_JOINED  1

/* C01/C02 — ThreadHandle state transition. */
int zer_thread_op_valid(int state, int joining);

/* C01 — ThreadHandle must be Joined at scope exit. */
int zer_thread_cleanup_valid(int state);

/* C03/C04/C05 — spawn context check (combined). */
int zer_spawn_context_valid(int in_isr, int in_critical, int in_async);

/* C07 — spawn target return type safety. */
int zer_spawn_return_safe(int returns_resource);

/* C09 — spawn arg must be shared ptr OR value. */
int zer_spawn_arg_valid(int is_shared_ptr, int is_value);

/* C10 — Handle as spawn arg rejected. */
int zer_spawn_arg_is_handle_rejected(int is_handle);

/* D01 — cannot take address of shared struct field. */
int zer_address_of_shared_valid(int is_shared_field);

/* D02 — shared access in yield/await statement rejected. */
int zer_shared_in_suspend_valid(int accesses_shared, int has_yield);

/* D04 — volatile global with compound RMW rejected. */
int zer_volatile_compound_valid(int is_volatile, int is_compound_op);

/* D05 — ISR+main access requires volatile. */
int zer_isr_main_access_valid(int accessed_in_isr, int accessed_in_main, int is_volatile);

/* F01-F04 — yield/await only valid in async, not in critical or defer. */
int zer_yield_context_valid(int in_async, int in_critical, int in_defer);

#endif
