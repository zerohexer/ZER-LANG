/* src/safety/comptime_rules.h
 *
 * Pure predicates for comptime soundness (typing.v Section R).
 * Linked into zerc, VST-verified in proofs/vst/verif_comptime_rules.v.
 *
 * Oracle theorems (typing.v):
 *   R02_const_arg_ok / R02_runtime_arg_rejected
 *   R04_true_const_ok / R04_false_const_rejected
 *   R06_under_budget_ok / R06_over_budget_rejected
 *   R07_under_nesting_limit_ok / R07_over_nesting_limit_rejected
 */
#ifndef ZER_SAFETY_COMPTIME_RULES_H
#define ZER_SAFETY_COMPTIME_RULES_H

/* Budget and nesting limits — must match typing.v constants. */
#define ZER_COMPTIME_OPS_BUDGET  1000000
#define ZER_EXPR_NESTING_LIMIT   1000

/* Returns 1 iff the comptime function arg is a compile-time constant.
 * Caller passes is_constant=1 if eval_const_expr succeeded, 0 otherwise.
 *
 * Oracle: typing.v:674 comptime_arg_valid (R02).
 * Callers: checker.c comptime function call all-const check. */
int zer_comptime_arg_valid(int is_constant);

/* Returns 1 iff static_assert condition is both constant AND truthy.
 * Both inputs must be nonzero.
 *
 * Oracle: typing.v:686 static_assert_holds (R04).
 * Callers: checker.c NODE_STATIC_ASSERT statement + top-level. */
int zer_static_assert_holds(int is_constant, int value);

/* Returns 1 iff the comptime evaluator has operations budget remaining.
 * Budget is 1,000,000 — prevents DoS from pathological comptime loops.
 *
 * Oracle: typing.v:706 comptime_ops_valid (R06).
 * Callers: checker.c eval_comptime_block global counter increment. */
int zer_comptime_ops_valid(int ops_count);

/* Returns 1 iff expression nesting depth is within limit (1000).
 *
 * Oracle: typing.v:720 expr_nesting_valid (R07).
 * Callers: checker.c check_expr recursion guard. */
int zer_expr_nesting_valid(int depth);

#endif
