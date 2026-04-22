/* src/safety/escape_rules.h
 *
 * Pure predicates for pointer-escape rules. Linked into zerc,
 * VST-verified in proofs/vst/verif_escape_rules.v against the
 * operational oracle in proofs/operational/lambda_zer_escape/
 * (Iris proof: only RegStatic pointers can store-global / return).
 *
 * Region tags (matching lambda_zer_escape/syntax.v):
 *   STATIC — globals, statics, keep-marked params. CAN escape.
 *   LOCAL  — stack-allocated. CANNOT escape (dangling on return).
 *   ARENA  — arena-allocated. CANNOT escape (freed by arena.reset()).
 *
 * The checker tracks per-Symbol flags `is_local_derived` and
 * `is_arena_derived`. Convert to region tag before calling these
 * predicates (STATIC if neither flag set; LOCAL or ARENA otherwise).
 *
 * See docs/formal_verification_plan.md Level 3.
 */
#ifndef ZER_SAFETY_ESCAPE_RULES_H
#define ZER_SAFETY_ESCAPE_RULES_H

#define ZER_REGION_STATIC  0
#define ZER_REGION_LOCAL   1
#define ZER_REGION_ARENA   2

/* Returns 1 iff a pointer with the given region tag can escape the
 * current scope (be returned, stored in a global, or stored in a
 * param-owned struct field).
 *
 * Rule: only STATIC pointers can escape. This matches the Iris proof
 * step_spec_store_global_static in lambda_zer_escape/iris_escape_specs.v.
 *
 * Callers: checker.c NODE_RETURN escape-check path, global-assign path,
 * keep-param violation path. All delegate after converting the
 * is_local_derived / is_arena_derived flags to a region tag. */
int zer_region_can_escape(int region);

/* Returns 1 iff the pointer is LOCAL-derived (stack memory).
 * Helper for callers that want to distinguish local vs arena
 * in error messages. Not needed for safety itself (zer_region_can_escape
 * rejects both), but nice for readable diagnostics. */
int zer_region_is_local(int region);

/* Returns 1 iff the pointer is ARENA-derived. Symmetric helper. */
int zer_region_is_arena(int region);

#endif
