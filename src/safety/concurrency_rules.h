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

/* ---- scoped-spawn WINDOW decisions (BUG-1648 / BUG-1651) ----
 * Oracle: proofs/operational/lambda_zer_concurrency/window_cfg.v (operational:
 * `window_sound` over an arbitrary control-flow graph). VST:
 * proofs/vst/verif_window_rules.v. */

/* Does a `h.join()` release what h lent? A CLOSED window: yes (the textual join;
 * `closed_window_ok`). An OPEN one: only its TAIL join — outside every
 * conditional body and @once, after the function's last goto / label
 * (`tail_window_ok.Hentry`). */
int zer_window_join_releases(int closed, int branch_depth, int in_once, int after_last_jump);

/* Does this statement leave a window's interior? `return` / `goto`: always.
 * `break` / `continue`: only when no loop was entered inside the interior
 * (`closed_window_ok.Hlive_exit`). */
int zer_window_stmt_leaves(int is_return_or_goto, int is_loop_jump, int loop_depth);

/* Is a scoped spawn counted live AT its spawn statement? An OPEN window in a
 * latched walk (a function with a goto / label) was counted at the entry. */
int zer_window_counts_at_spawn(int closed, int latched);

#endif
