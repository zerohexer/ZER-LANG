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

## Migration Strategy

1. Add `lower_expr()` function that decomposes trees → temp locals + instructions
2. Change `lower_stmt` to call `lower_expr` instead of storing `Node *expr`
3. Change `IRInst` to use `src1/src2` local IDs instead of `Node *expr`
4. Change `emit_ir_inst` to emit from local IDs instead of `emit_expr`
5. Delete `emit_expr` calls from IR emission path
6. Verify all tests pass at each step

## Impact

- Eliminates the last architectural gap (scope vs flat locals)
- `emit_expr` no longer needed for IR path (can be deleted when AST path removed)
- All 761/761 rust tests should pass
- Foundation for SSA form (future: phi nodes at merge points)

## Estimated Size

- `lower_expr()`: ~300 lines (recursive tree → instruction decomposition)
- `IRInst` changes: ~50 lines (new fields, remove `Node *expr`)
- `emit_ir_inst` changes: ~200 lines (emit from local IDs)
- New IR op kinds: ~20 (ADD, SUB, MUL, DIV, GT, LT, EQ, NE, FIELD_READ, INDEX, CALL, etc.)
- Total: ~600 lines new, ~400 lines removed (emit_expr calls)
- Net: ~200 lines added
