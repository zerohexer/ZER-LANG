# Audit 2026-06-06 — Silent Gaps Documented as Tests

This directory holds reproducers for safety gaps discovered during the
2026-06-06 codebase audit. The gaps that **were fixed in the same audit
session** were moved to `tests/zer_fail/` to join the standard negative
test sweep. The gaps that remain open below stay here as documentation
until they are fixed.

## Open gaps (still silent today)

| File | Severity | Gap |
|---|---|---|
| `audit_extern_alloc_orelse_uaf.zer` | HIGH | `*Res p = extern_alloc() orelse return;` (extern alloc returning `?*T` fused with var-decl + orelse) drops handle tracking → silent UAF. Two-step form catches it; IR_ASSIGN code path for non-Handle pointer returns needs extension. |
| `audit_ptrcast_distinct_const_strip.zer` | MEDIUM | `@ptrcast` const/volatile-strip check uses raw `result->kind == TYPE_POINTER` so `distinct typedef` targets bypass the strip diagnostic; const launders through silently. |

## Closed in this audit session (moved to `tests/zer_fail/`)

| File | What was wrong | Fix |
|---|---|---|
| `audit_summary_compound_double_free.zer` (GAP-A) | `FuncSummary.frees_param[pi]` only matched NODE_IDENT args via the passthrough path → silent cross-function double-free of `c.h` style compound handles. | `zercheck_ir.c` summary path now calls `ir_extract_compound_key` to resolve both bare and compound shapes; if the decomposed temp had no tracked handle, the original AST node is re-checked for a compound key (`probe`). |
| `audit_spawn_compound_move_struct.zer` (GAP-C) | IR_NOP+NODE_SPAWN handler only matched NODE_IDENT args, so `spawn worker(b.t)` (compound move-struct arg) silently skipped IR_HS_TRANSFERRED → silent UAM. | `zercheck_ir.c` IR_NOP+NODE_SPAWN now uses `ir_extract_compound_key` and walks struct field types to decide whether to auto-register the compound for move tracking. |
| `audit_destructor_nonvoid_uaf.zer` (GAP-D) | `ir_is_extern_free_call` never received the `name_looks_like_destructor` widening promised by the AST→IR migration (`docs/refactor_ir.md:2293`). Bodyless non-void destructors like `i32 destroy_resource(*R)` were not recognized as freeing their first arg, producing a misleading "never freed" leak warning instead of a UAF diagnosis. | Restored `ir_name_looks_like_destructor` in `zercheck_ir.c` with the 12-keyword substring list, and widened the bodyless heuristic to non-void returns when the name matches. |
| `audit_ptrcast_typedef_target.zer` (GAP-E, deleted) | Parser `force_type_arg` list at `parser.c:911-917` only included `@cast/@bitcast/@truncate/@saturate`. `@ptrcast(TypedefName, …)` / `@inttoptr/@pun/@container` parsed the typedef as expression, leaving `type_arg` NULL, so the checker silently returned `ty_void` — producing a confusing "cannot initialize 'p' of type '*T' with 'void'" diagnostic at the use site. | Extended `force_type_arg` to include `@ptrcast`, `@pun`, `@inttoptr`, `@container`. The original reproducer is no longer interesting (it now compiles cleanly per the design) so the file was deleted; the distinct-typedef qualifier-strip case above remains as the residual safety gap. |

## How to reproduce an open gap

```
./zerc tests/audit_2026_06_06/audit_extern_alloc_orelse_uaf.zer -o /dev/null
echo "exit: $?"   # 0 today (silent miscompile) — should be non-zero after fix
```

When a gap is fixed, move the corresponding `.zer` file to
`tests/zer_fail/` so it joins the standard negative test sweep and CI
will catch any regression that reintroduces the silent path.
