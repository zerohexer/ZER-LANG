# Three-Address-Code Transition — IR Phase 8

## Problem

ZER's IR uses tree expressions (`Node *expr`) inside instructions:
```
%x = ASSIGN <expr: a.b[i].c + d.e>
```

This means `emit_expr` (AST tree walker) is still used for expression emission.
Variable references inside expressions use SOURCE NAMES, not IR local IDs.
When two locals have the same name but different types (different scopes),
the emitter can't distinguish them.

Failing test: `rt_conc_ring_producer_consumer` — `Msg m` (loop 1) and `?Msg m` (loop 2).
IR deduplicates to one `m` with type `Msg`. The `?Msg` usage breaks.

## Solution: Replace Tree Expressions with Local-ID References

Every expression decomposed into individual instructions. Variable references
become local IDs (`%N`), not names.

### Before (current hybrid IR):
```
bb0:
  %x = ASSIGN <expr: a + b * c>     ← tree expression, uses names
  BRANCH <expr: x > 0> → bb1, bb2   ← tree expression
```

### After (three-address-code):
```
bb0:
  %3 = MUL %1, %2       ← b * c (all operands are local IDs)
  %4 = ADD %0, %3        ← a + %3
  %x = COPY %4           ← assign to x
  %5 = GT %x, 0          ← x > 0
  BRANCH %5 → bb1, bb2   ← branch on local ID
```

Every instruction has at most: 1 dest local + 2 source locals/literals.
No tree nesting. No `emit_expr`. No source names in expressions.

## What Changes

### ir.h: New instruction format
```c
typedef struct IRInst {
    IROpKind op;
    int dest;           // destination local ID (-1 = none)
    int src1;           // first operand local ID (-1 = literal)
    int src2;           // second operand local ID (-1 = literal)
    int64_t imm;        // immediate value (for literals)
    const char *str;    // string literal pointer
    uint32_t str_len;
    int source_line;
    int true_block;     // BRANCH targets
    int false_block;
    int goto_block;     // GOTO target
} IRInst;
```

### ir_lower.c: Expression decomposition
Every expression tree recursively decomposed:
```c
// lower_expr(ctx, node) → returns local ID holding the result
int lower_expr(LowerCtx *ctx, Node *expr) {
    switch (expr->kind) {
    case NODE_INT_LIT:
        return create_const_local(ctx, expr->int_lit.value);
    case NODE_IDENT:
        return ir_find_local(ctx->func, expr->ident.name, ...);
    case NODE_BINARY:
        int left = lower_expr(ctx, expr->binary.left);
        int right = lower_expr(ctx, expr->binary.right);
        int result = create_temp(ctx);
        emit(ctx, IR_BINOP, result, left, right);
        return result;
    case NODE_CALL:
        // lower args, emit IR_CALL, return dest local
    case NODE_FIELD:
        int obj = lower_expr(ctx, expr->field.object);
        int result = create_temp(ctx);
        emit(ctx, IR_FIELD_READ, result, obj, field_name);
        return result;
    ...
    }
}
```

### emitter.c: ID-based emission (replaces emit_expr)
```c
// emit_local(e, func, local_id) → emits C variable name
void emit_local(Emitter *e, IRFunc *func, int id) {
    IRLocal *l = &func->locals[id];
    if (func->is_async)
        emit(e, "self->%.*s", l->name_len, l->name);
    else
        emit(e, "%.*s", l->name_len, l->name);
}

// emit_ir_inst uses local IDs for everything
case IR_ADD:
    emit_local(e, func, inst->dest);
    emit(e, " = ");
    emit_local(e, func, inst->src1);
    emit(e, " + ");
    emit_local(e, func, inst->src2);
    emit(e, ";\n");
```

## Scope Resolution

The key benefit: each local has a UNIQUE ID regardless of name.
Two `m` variables in different scopes get different IDs:
```
LOCALS:
  %5: m : Msg         (line 9, scope: for-loop 1)
  %9: m_1 : ?Msg      (line 17, scope: for-loop 2)
```

`ir_find_local` returns the MOST RECENT local with that name
(innermost scope). The lowering pushes/pops scope depth to
track which `m` is current.

## Current Status (Phase 8a COMPLETE, Phase 8b PENDING)

**Phase 8a (done):** Scope conflict resolved. On-demand locals, ident rewriting, orig_name tracking.
- `lower_expr()` EXISTS (decomposes ident/literal/binary/unary/field/index)
- New IR ops EXIST (IR_COPY, IR_BINOP, IR_LITERAL, etc.) with emission handlers
- 195/195 ZER + 761/761 rust pass (0 hang)
- BUT: `lower_expr` is NOT WIRED into `lower_stmt`. `emit_expr` still used everywhere.

**Phase 8b (pending):** Wire lower_expr into lower_stmt. Eliminate emit_expr from emit_ir_inst.

## Phase 8b — Concrete Steps

### The wiring (lower_stmt changes):

1. `NODE_VAR_DECL` default init: replace `inst.expr = init` with `src = lower_expr(ctx, init); IR_COPY(dest, src)`
2. `NODE_EXPR_STMT` simple assign: replace `inst.expr = expr` with `src = lower_expr(ctx, value); IR_COPY(dest, src)`
3. `NODE_EXPR_STMT` call: replace `inst.expr = expr` with `lower_expr(ctx, expr)` (void result)
4. `NODE_IF` condition: already has `br.expr = cond` → replace with `cond_local = lower_expr(ctx, cond)`
5. `NODE_FOR` condition/step: replace `br.expr = cond` and `step.expr = step`
6. `NODE_WHILE/DO_WHILE` condition: replace `br.expr = cond`
7. `NODE_RETURN` expression: replace `ret.expr = node` with `src = lower_expr(ctx, ret_expr)`

### Edge cases that BROKE during first attempt:

1. **Void type temps:** `checker_get_type(NODE_NULL_LIT)` returns NULL → void temp → GCC error.
   Fix: DON'T decompose null literals, struct inits, or expressions with NULL/void type.
   Use `can_decompose` check: `init_type && init_type->kind != TYPE_VOID && init->kind != NODE_NULL_LIT`.

2. **Type adaptation in IR_COPY:** dest `?u32` + src `u32` needs `{val, 1}` wrapping.
   Already fixed in Phase 8a IR_COPY handler (wrap/unwrap/slice coercion).

3. **Enum/module qualified access:** `Color.red` — NODE_FIELD with non-local object.
   `lower_expr(NODE_FIELD)` must check if object is a local before decomposing.
   If not local (enum type, module prefix) → passthrough to emit_expr.

4. **Builtins (pool/slab/ring/arena):** Emit inline C code. Can't be decomposed.
   Keep as IR_POOL_ALLOC etc. with expr passthrough.

5. **Orelse:** Already lowered to IR branches in async/loop contexts.
   Non-async orelse uses GCC statement expressions → keep as expr passthrough.

### Emission changes (emit_ir_inst):

For each new op kind, the emitter uses `func->locals[id].name` instead of `emit_expr(expr)`:
- IR_COPY: `dest_name = src_name;` (with type adaptation)
- IR_BINOP: `dest_name = src1_name OP src2_name;`
- IR_LITERAL: `dest_name = literal_value;`
- IR_FIELD_READ: `dest_name = src_name.field;`
- IR_INDEX_READ: `dest_name = src_name[idx_name];` (with bounds check)

### Verification

After Phase 8b: `grep emit_expr` in `emit_ir_inst` function body must return ZERO results.
(Except IR_NOP passthrough for spawn/asm/union-switch which deliberately delegates to AST.)

### Estimated effort

~500 lines of changes across ir_lower.c + emitter.c. Main risk: type adaptation edge cases
in IR_COPY emission. Test after EACH expression type conversion. Do NOT batch changes.
