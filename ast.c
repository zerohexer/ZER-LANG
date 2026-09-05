#include "ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compilation-trace gates (see ast.h). Default off. --trace sets g_zer_trace;
 * --trace-calls sets both (call-graph tracer lives in zer_trace.c). */
int g_zer_trace = 0;
int g_zer_trace_calls = 0;
int g_zer_in_converge = 0;

/* ================================================================
 * Arena allocator — bump allocator for AST nodes.
 * One big block. Allocate by bumping pointer. Free everything at once.
 * Same pattern ZER teaches with Arena builtin.
 * ================================================================ */

void arena_init(Arena *a, size_t capacity) {
    a->buf = (char *)malloc(capacity);
    if (!a->buf) {
        fprintf(stderr, "error: arena allocation failed (%zu bytes)\n", capacity);
        exit(1);
    }
    /* first 8 bytes = chain pointer (NULL for first block) */
    *(char **)a->buf = NULL;
    a->used = 8;
    a->capacity = capacity;
}

void *arena_alloc(Arena *a, size_t size) {
    /* align to 8 bytes */
    size = (size + 7) & ~(size_t)7;
    if (a->used + size > a->capacity) {
        /* Cannot realloc — would invalidate all pointers into the arena.
         * Allocate a new block. Chain the old block via first 8 bytes of new block. */
        size_t new_cap = a->capacity * 2;
        if (new_cap < size + 16) new_cap = size + 16;
        char *new_buf = (char *)malloc(new_cap);
        if (!new_buf) {
            fprintf(stderr, "error: arena expansion failed (%zu bytes)\n", new_cap);
            exit(1);
        }
        /* store pointer to old block at start of new block (for freeing) */
        *(char **)new_buf = a->buf;
        a->buf = new_buf;
        a->used = 8; /* skip the chain pointer */
        a->capacity = new_cap;
    }
    void *ptr = a->buf + a->used;
    a->used += size;
    memset(ptr, 0, size);
    return ptr;
}

void arena_free(Arena *a) {
    /* walk the chain of blocks and free all */
    char *block = a->buf;
    while (block) {
        char *prev = *(char **)block; /* chain pointer at start */
        free(block);
        block = prev;
    }
    a->buf = NULL;
    a->used = 0;
    a->capacity = 0;
}

/* ================================================================
 * Node kind names — for debugging and error messages
 * ================================================================ */

const char *node_kind_name(NodeKind kind) {
    switch (kind) {
    case NODE_FILE:         return "FILE";
    case NODE_FUNC_DECL:    return "FUNC_DECL";
    case NODE_STRUCT_DECL:  return "STRUCT_DECL";
    case NODE_ENUM_DECL:    return "ENUM_DECL";
    case NODE_UNION_DECL:   return "UNION_DECL";
    case NODE_TYPEDEF:      return "TYPEDEF";
    case NODE_IMPORT:       return "IMPORT";
    case NODE_INTERRUPT:    return "INTERRUPT";
    case NODE_GLOBAL_VAR:   return "GLOBAL_VAR";
    case NODE_VAR_DECL:     return "VAR_DECL";
    case NODE_BLOCK:        return "BLOCK";
    case NODE_IF:           return "IF";
    case NODE_FOR:          return "FOR";
    case NODE_WHILE:        return "WHILE";
    case NODE_SWITCH:       return "SWITCH";
    case NODE_RETURN:       return "RETURN";
    case NODE_BREAK:        return "BREAK";
    case NODE_CONTINUE:     return "CONTINUE";
    case NODE_DEFER:        return "DEFER";
    case NODE_EXPR_STMT:    return "EXPR_STMT";
    case NODE_ASM:          return "ASM";
    case NODE_CRITICAL:     return "CRITICAL";
    case NODE_ONCE:         return "ONCE";
    case NODE_SPAWN:        return "SPAWN";
    case NODE_YIELD:        return "YIELD";
    case NODE_AWAIT:        return "AWAIT";
    case NODE_INT_LIT:      return "INT_LIT";
    case NODE_FLOAT_LIT:    return "FLOAT_LIT";
    case NODE_STRING_LIT:   return "STRING_LIT";
    case NODE_CHAR_LIT:     return "CHAR_LIT";
    case NODE_BOOL_LIT:     return "BOOL_LIT";
    case NODE_NULL_LIT:     return "NULL_LIT";
    case NODE_IDENT:        return "IDENT";
    case NODE_BINARY:       return "BINARY";
    case NODE_UNARY:        return "UNARY";
    case NODE_ASSIGN:       return "ASSIGN";
    case NODE_CALL:         return "CALL";
    case NODE_FIELD:        return "FIELD";
    case NODE_INDEX:        return "INDEX";
    case NODE_SLICE:        return "SLICE";
    case NODE_ORELSE:       return "ORELSE";
    case NODE_INTRINSIC:    return "INTRINSIC";
    case NODE_CAST:         return "CAST";
    case NODE_TYPECAST:     return "TYPECAST";
    case NODE_SIZEOF:       return "SIZEOF";
    case NODE_STRUCT_INIT:     return "STRUCT_INIT";
    case NODE_CONTAINER_DECL:  return "CONTAINER_DECL";
    case NODE_DO_WHILE:        return "DO_WHILE";
    case NODE_CINCLUDE:     return "CINCLUDE";
    /* Stage 2 Part B (2026-04-28): exhaustive — added missing kinds. */
    case NODE_MMIO:         return "MMIO";
    case NODE_GOTO:         return "GOTO";
    case NODE_LABEL:        return "LABEL";
    case NODE_STATIC_ASSERT: return "STATIC_ASSERT";
    }
    return "UNKNOWN";
}

/* ================================================================
 * AST printer — for debugging. Prints tree with indentation.
 * ================================================================ */

static void indent(int depth) {
    for (int i = 0; i < depth; i++) printf("  ");
}

static void print_type(TypeNode *t, int depth);

static void print_type(TypeNode *t, int depth) {
    if (!t) { printf("(null)"); return; }
    switch (t->kind) {
    case TYNODE_U8:    printf("u8"); break;
    case TYNODE_U16:   printf("u16"); break;
    case TYNODE_U32:   printf("u32"); break;
    case TYNODE_U64:   printf("u64"); break;
    case TYNODE_I8:    printf("i8"); break;
    case TYNODE_I16:   printf("i16"); break;
    case TYNODE_I32:   printf("i32"); break;
    case TYNODE_I64:   printf("i64"); break;
    case TYNODE_USIZE: printf("usize"); break;
    case TYNODE_F32:   printf("f32"); break;
    case TYNODE_F64:   printf("f64"); break;
    case TYNODE_BOOL:  printf("bool"); break;
    case TYNODE_VOID:  printf("void"); break;
    case TYNODE_OPAQUE: printf("opaque"); break;
    case TYNODE_POINTER:
        printf("*");
        print_type(t->pointer.inner, depth);
        break;
    case TYNODE_OPTIONAL:
        printf("?");
        print_type(t->optional.inner, depth);
        break;
    case TYNODE_SLICE:
        printf("[]");
        print_type(t->slice.inner, depth);
        break;
    case TYNODE_ARRAY:
        print_type(t->array.elem, depth);
        printf("[...]");
        break;
    case TYNODE_NAMED:
        printf("%.*s", (int)t->named.name_len, t->named.name);
        break;
    case TYNODE_FUNC_PTR:
        print_type(t->func_ptr.return_type, depth);
        printf(" (*)(...)");
        break;
    case TYNODE_POOL:
        printf("Pool(");
        print_type(t->pool.elem, depth);
        printf(", ...)");
        break;
    case TYNODE_RING:
        printf("Ring(");
        print_type(t->ring.elem, depth);
        printf(", ...)");
        break;
    case TYNODE_ARENA:
        printf("Arena");
        break;
    case TYNODE_HANDLE:
        printf("Handle(");
        print_type(t->handle.elem, depth);
        printf(")");
        break;
    case TYNODE_CONST:
        printf("const ");
        print_type(t->qualified.inner, depth);
        break;
    case TYNODE_CONTAINER:
        printf("%.*s(", (int)t->container.name_len, t->container.name);
        print_type(t->container.type_arg, depth);
        printf(")");
        break;
    case TYNODE_VOLATILE:
        printf("volatile ");
        print_type(t->qualified.inner, depth);
        break;
    /* Stage 2 Part B (2026-04-28): exhaustive — added missing TYNODE kinds. */
    case TYNODE_BARRIER:
        printf("Barrier");
        break;
    case TYNODE_SLAB:
        printf("Slab(");
        print_type(t->slab.elem, depth);
        printf(")");
        break;
    case TYNODE_SEMAPHORE:
        printf("Semaphore(...)");
        break;
    }
}

void ast_print(Node *node, int depth) {
    if (!node) return;

    indent(depth);

    switch (node->kind) {
    case NODE_FILE:
        printf("File (%d decls)\n", node->file.decl_count);
        for (int i = 0; i < node->file.decl_count; i++)
            ast_print(node->file.decls[i], depth + 1);
        break;

    case NODE_FUNC_DECL:
        printf("FuncDecl '%.*s' -> ",
               (int)node->func_decl.name_len, node->func_decl.name);
        print_type(node->func_decl.return_type, depth);
        printf(" (%d params)\n", node->func_decl.param_count);
        if (node->func_decl.body)
            ast_print(node->func_decl.body, depth + 1);
        break;

    case NODE_STRUCT_DECL:
        printf("%sStruct '%.*s' (%d fields)\n",
               node->struct_decl.is_packed ? "Packed " : "",
               (int)node->struct_decl.name_len, node->struct_decl.name,
               node->struct_decl.field_count);
        break;

    case NODE_ENUM_DECL:
        printf("Enum '%.*s' (%d variants)\n",
               (int)node->enum_decl.name_len, node->enum_decl.name,
               node->enum_decl.variant_count);
        break;

    case NODE_UNION_DECL:
        printf("Union '%.*s' (%d variants)\n",
               (int)node->union_decl.name_len, node->union_decl.name,
               node->union_decl.variant_count);
        break;

    case NODE_TYPEDEF:
        printf("%sTypedef '%.*s' = ",
               node->typedef_decl.is_distinct ? "Distinct " : "",
               (int)node->typedef_decl.name_len, node->typedef_decl.name);
        print_type(node->typedef_decl.type, depth);
        printf("\n");
        break;

    case NODE_IMPORT:
        printf("Import '%.*s'\n",
               (int)node->import.module_name_len, node->import.module_name);
        break;

    case NODE_CINCLUDE:
        printf("CInclude '%.*s'\n",
               (int)node->cinclude.path_len, node->cinclude.path);
        break;

    case NODE_INTERRUPT:
        printf("Interrupt '%.*s'", (int)node->interrupt.name_len, node->interrupt.name);
        if (node->interrupt.as_name)
            printf(" as \"%.*s\"", (int)node->interrupt.as_name_len, node->interrupt.as_name);
        printf("\n");
        if (node->interrupt.body)
            ast_print(node->interrupt.body, depth + 1);
        break;

    case NODE_GLOBAL_VAR:
    case NODE_VAR_DECL:
        printf("VarDecl '%.*s' : ",
               (int)node->var_decl.name_len, node->var_decl.name);
        print_type(node->var_decl.type, depth);
        if (node->var_decl.is_const) printf(" [const]");
        if (node->var_decl.is_static) printf(" [static]");
        printf("\n");
        if (node->var_decl.init)
            ast_print(node->var_decl.init, depth + 1);
        break;

    case NODE_BLOCK:
        printf("Block (%d stmts)\n", node->block.stmt_count);
        for (int i = 0; i < node->block.stmt_count; i++)
            ast_print(node->block.stmts[i], depth + 1);
        break;

    case NODE_IF:
        printf("If");
        if (node->if_stmt.capture_name)
            printf(" |%s%.*s|",
                   node->if_stmt.capture_is_ptr ? "*" : "",
                   (int)node->if_stmt.capture_name_len,
                   node->if_stmt.capture_name);
        printf("\n");
        indent(depth + 1); printf("cond:\n");
        ast_print(node->if_stmt.cond, depth + 2);
        indent(depth + 1); printf("then:\n");
        ast_print(node->if_stmt.then_body, depth + 2);
        if (node->if_stmt.else_body) {
            indent(depth + 1); printf("else:\n");
            ast_print(node->if_stmt.else_body, depth + 2);
        }
        break;

    case NODE_FOR:
        printf("For\n");
        if (node->for_stmt.init) {
            indent(depth + 1); printf("init:\n");
            ast_print(node->for_stmt.init, depth + 2);
        }
        if (node->for_stmt.cond) {
            indent(depth + 1); printf("cond:\n");
            ast_print(node->for_stmt.cond, depth + 2);
        }
        if (node->for_stmt.step) {
            indent(depth + 1); printf("step:\n");
            ast_print(node->for_stmt.step, depth + 2);
        }
        indent(depth + 1); printf("body:\n");
        ast_print(node->for_stmt.body, depth + 2);
        break;

    case NODE_WHILE:
        printf("While\n");
        indent(depth + 1); printf("cond:\n");
        ast_print(node->while_stmt.cond, depth + 2);
        indent(depth + 1); printf("body:\n");
        ast_print(node->while_stmt.body, depth + 2);
        break;

    case NODE_SWITCH:
        printf("Switch (%d arms)\n", node->switch_stmt.arm_count);
        ast_print(node->switch_stmt.expr, depth + 1);
        break;

    case NODE_RETURN:
        printf("Return\n");
        if (node->ret.expr)
            ast_print(node->ret.expr, depth + 1);
        break;

    case NODE_BREAK:
        printf("Break\n");
        break;

    case NODE_CONTINUE:
        printf("Continue\n");
        break;

    case NODE_DEFER:
        printf("Defer\n");
        ast_print(node->defer.body, depth + 1);
        break;

    case NODE_EXPR_STMT:
        printf("ExprStmt\n");
        ast_print(node->expr_stmt.expr, depth + 1);
        break;

    case NODE_ASM:
        printf("Asm(\"%.*s\")\n", (int)node->asm_stmt.code_len, node->asm_stmt.code);
        break;

    case NODE_SPAWN:
        printf("Spawn(%.*s, %d args)\n", (int)node->spawn_stmt.func_name_len,
               node->spawn_stmt.func_name, node->spawn_stmt.arg_count);
        for (int i = 0; i < node->spawn_stmt.arg_count; i++)
            ast_print(node->spawn_stmt.args[i], depth + 1);
        break;

    case NODE_YIELD:
        printf("Yield\n");
        break;

    case NODE_AWAIT:
        printf("Await\n");
        if (node->await_stmt.cond)
            ast_print(node->await_stmt.cond, depth + 1);
        break;

    case NODE_INT_LIT:
        printf("IntLit(%llu)\n", (unsigned long long)node->int_lit.value);
        break;

    case NODE_FLOAT_LIT:
        printf("FloatLit(%f)\n", node->float_lit.value);
        break;

    case NODE_STRING_LIT:
        printf("StringLit(\"%.*s\")\n", (int)node->string_lit.length, node->string_lit.value);
        break;

    case NODE_CHAR_LIT:
        printf("CharLit('%c')\n", node->char_lit.value);
        break;

    case NODE_BOOL_LIT:
        printf("BoolLit(%s)\n", node->bool_lit.value ? "true" : "false");
        break;

    case NODE_NULL_LIT:
        printf("Null\n");
        break;

    case NODE_IDENT:
        printf("Ident('%.*s')\n", (int)node->ident.name_len, node->ident.name);
        break;

    case NODE_BINARY:
        printf("Binary(%s)\n", token_type_name(node->binary.op));
        ast_print(node->binary.left, depth + 1);
        ast_print(node->binary.right, depth + 1);
        break;

    case NODE_UNARY:
        printf("Unary(%s)\n", token_type_name(node->unary.op));
        ast_print(node->unary.operand, depth + 1);
        break;

    case NODE_ASSIGN:
        printf("Assign(%s)\n", token_type_name(node->assign.op));
        ast_print(node->assign.target, depth + 1);
        ast_print(node->assign.value, depth + 1);
        break;

    case NODE_CALL:
        printf("Call (%d args)\n", node->call.arg_count);
        ast_print(node->call.callee, depth + 1);
        for (int i = 0; i < node->call.arg_count; i++)
            ast_print(node->call.args[i], depth + 1);
        break;

    case NODE_FIELD:
        printf("Field '.%.*s'\n",
               (int)node->field.field_name_len, node->field.field_name);
        ast_print(node->field.object, depth + 1);
        break;

    case NODE_INDEX:
        printf("Index\n");
        ast_print(node->index_expr.object, depth + 1);
        ast_print(node->index_expr.index, depth + 1);
        break;

    case NODE_SLICE:
        printf("Slice\n");
        ast_print(node->slice.object, depth + 1);
        if (node->slice.start) ast_print(node->slice.start, depth + 1);
        if (node->slice.end) ast_print(node->slice.end, depth + 1);
        break;

    case NODE_ORELSE:
        printf("Orelse\n");
        ast_print(node->orelse.expr, depth + 1);
        if (node->orelse.fallback)
            ast_print(node->orelse.fallback, depth + 1);
        break;

    case NODE_INTRINSIC:
        printf("Intrinsic '@%.*s' (%d args)\n",
               (int)node->intrinsic.name_len, node->intrinsic.name,
               node->intrinsic.arg_count);
        break;

    case NODE_CAST:
    case NODE_TYPECAST:
    case NODE_SIZEOF:
        printf("%s\n", node_kind_name(node->kind));
        break;

    case NODE_STRUCT_INIT:
        printf("STRUCT_INIT (%d fields)\n", node->struct_init.field_count);
        for (int i = 0; i < node->struct_init.field_count; i++) {
            indent(depth + 1);
            printf(".%.*s = \n", (int)node->struct_init.fields[i].name_len,
                   node->struct_init.fields[i].name);
            ast_print(node->struct_init.fields[i].value, depth + 2);
        }
        break;

    case NODE_CONTAINER_DECL:
        printf("CONTAINER '%.*s'(%.*s) (%d fields)\n",
               (int)node->container_decl.name_len, node->container_decl.name,
               (int)node->container_decl.type_param_len, node->container_decl.type_param,
               node->container_decl.field_count);
        break;

    case NODE_DO_WHILE:
        printf("DO_WHILE\n");
        ast_print(node->while_stmt.body, depth + 1);
        indent(depth + 1);
        printf("COND: ");
        ast_print(node->while_stmt.cond, depth + 2);
        break;
    /* Stage 2 Part B (2026-04-28): exhaustive — debug-printer no-ops for
     * leaf node kinds + recently-added kinds. Adding a new NODE_ to ast.h
     * now forces a deliberate decision here. */
    case NODE_MMIO:
        indent(depth);
        printf("MMIO\n");
        break;
    case NODE_GOTO:
        indent(depth);
        printf("GOTO %.*s\n", (int)node->goto_stmt.label_len, node->goto_stmt.label);
        break;
    case NODE_LABEL:
        indent(depth);
        printf("LABEL %.*s:\n", (int)node->label_stmt.name_len, node->label_stmt.name);
        break;
    case NODE_STATIC_ASSERT:
        indent(depth);
        printf("STATIC_ASSERT\n");
        break;
    case NODE_CRITICAL:
        indent(depth);
        printf("CRITICAL\n");
        ast_print(node->critical.body, depth + 1);
        break;
    case NODE_ONCE:
        indent(depth);
        printf("ONCE\n");
        ast_print(node->once.body, depth + 1);
        break;
    }
}

/* ================================================================
 * Deep clone (BUG-920, 2026-09-05)
 *
 * A defer body is lowered to IR at EVERY fire site (scope exit, every
 * return, every break/continue that leaves its scope, every goto that
 * skips past it). Lowering MUTATES the AST it is handed (pre_lower_orelse
 * replaces an orelse with a temp ident in place; rewrite_capture_name
 * renames captures; for-init rewrites), so lowering one body twice would
 * lower a half-rewritten tree the second time. Each fire therefore lowers a
 * fresh deep copy.
 *
 * `hook(orig, clone, ud)` runs for every cloned node so the caller can
 * mirror node-KEYED side tables — the checker's typemap, proven-safe marks
 * and auto-guard marks — onto the copy; a clone without them would emit
 * with no type and no bounds guard.
 *
 * NODE_SPAWN is SHARED, not cloned: the emitter's thread-wrapper registry is
 * keyed by the spawn node (one wrapper struct + function per spawn site), and
 * the spawn lowering rewrites its argument idents idempotently, so one node
 * serves every fire. Strings and TypeNodes are immutable after parsing and
 * are shared too.
 *
 * Exhaustive over NodeKind (no default:) — a new kind must be placed
 * deliberately, under -Werror=switch.
 * ================================================================ */
static Node **ast_clone_list(Arena *a, Node **items, int count,
                             AstCloneHook hook, void *ud) {
    if (!items || count <= 0) return items;
    Node **out = (Node **)arena_alloc(a, (size_t)count * sizeof(Node *));
    for (int i = 0; i < count; i++) out[i] = ast_clone(a, items[i], hook, ud);
    return out;
}

static AsmOperand *ast_clone_asm_ops(Arena *a, AsmOperand *ops, int count,
                                     AstCloneHook hook, void *ud) {
    if (!ops || count <= 0) return ops;
    AsmOperand *out = (AsmOperand *)arena_alloc(a, (size_t)count * sizeof(AsmOperand));
    for (int i = 0; i < count; i++) {
        out[i] = ops[i];
        out[i].expr = ast_clone(a, ops[i].expr, hook, ud);
    }
    return out;
}

Node *ast_clone(Arena *a, Node *n, AstCloneHook hook, void *ud) {
    if (!n) return NULL;
    if (n->kind == NODE_SPAWN) return n;          /* shared by design — see above */
    Node *c = (Node *)arena_alloc(a, sizeof(Node));
    *c = *n;                                      /* scalars, flags, shared strings */
    switch (n->kind) {
    /* ---- top-level declarations (cannot sit inside a defer body; cloned
     * structurally so the function is total) ---- */
    case NODE_FILE:
        c->file.decls = ast_clone_list(a, n->file.decls, n->file.decl_count, hook, ud);
        break;
    case NODE_FUNC_DECL:
        c->func_decl.body = ast_clone(a, n->func_decl.body, hook, ud);
        break;
    case NODE_INTERRUPT:
        c->interrupt.body = ast_clone(a, n->interrupt.body, hook, ud);
        break;
    case NODE_STRUCT_DECL: case NODE_ENUM_DECL: case NODE_UNION_DECL:
    case NODE_TYPEDEF: case NODE_IMPORT: case NODE_CINCLUDE: case NODE_MMIO:
    case NODE_CONTAINER_DECL:
        break;                                    /* declaration payload is immutable */
    case NODE_GLOBAL_VAR:
    case NODE_VAR_DECL:
        c->var_decl.init = ast_clone(a, n->var_decl.init, hook, ud);
        break;

    /* ---- statements ---- */
    case NODE_BLOCK:
        c->block.stmts = ast_clone_list(a, n->block.stmts, n->block.stmt_count, hook, ud);
        break;
    case NODE_IF:
        c->if_stmt.cond      = ast_clone(a, n->if_stmt.cond, hook, ud);
        c->if_stmt.then_body = ast_clone(a, n->if_stmt.then_body, hook, ud);
        c->if_stmt.else_body = ast_clone(a, n->if_stmt.else_body, hook, ud);
        break;
    case NODE_FOR:
        c->for_stmt.init = ast_clone(a, n->for_stmt.init, hook, ud);
        c->for_stmt.cond = ast_clone(a, n->for_stmt.cond, hook, ud);
        c->for_stmt.step = ast_clone(a, n->for_stmt.step, hook, ud);
        c->for_stmt.body = ast_clone(a, n->for_stmt.body, hook, ud);
        break;
    case NODE_WHILE:
    case NODE_DO_WHILE:
        c->while_stmt.cond = ast_clone(a, n->while_stmt.cond, hook, ud);
        c->while_stmt.body = ast_clone(a, n->while_stmt.body, hook, ud);
        break;
    case NODE_SWITCH: {
        c->switch_stmt.expr = ast_clone(a, n->switch_stmt.expr, hook, ud);
        if (n->switch_stmt.arms && n->switch_stmt.arm_count > 0) {
            SwitchArm *arms = (SwitchArm *)arena_alloc(a,
                (size_t)n->switch_stmt.arm_count * sizeof(SwitchArm));
            for (int i = 0; i < n->switch_stmt.arm_count; i++) {
                arms[i] = n->switch_stmt.arms[i];
                arms[i].values = ast_clone_list(a, n->switch_stmt.arms[i].values,
                                                n->switch_stmt.arms[i].value_count, hook, ud);
                arms[i].body = ast_clone(a, n->switch_stmt.arms[i].body, hook, ud);
            }
            c->switch_stmt.arms = arms;
        }
        break;
    }
    case NODE_RETURN:
        c->ret.expr = ast_clone(a, n->ret.expr, hook, ud);
        break;
    case NODE_DEFER:
        c->defer.body = ast_clone(a, n->defer.body, hook, ud);
        break;
    case NODE_EXPR_STMT:
        c->expr_stmt.expr = ast_clone(a, n->expr_stmt.expr, hook, ud);
        break;
    case NODE_ASM:
        c->asm_stmt.inputs   = ast_clone_asm_ops(a, n->asm_stmt.inputs, n->asm_stmt.input_count, hook, ud);
        c->asm_stmt.outputs  = ast_clone_asm_ops(a, n->asm_stmt.outputs, n->asm_stmt.output_count, hook, ud);
        c->asm_stmt.clobbers = ast_clone_asm_ops(a, n->asm_stmt.clobbers, n->asm_stmt.clobber_count, hook, ud);
        break;
    case NODE_CRITICAL:
        c->critical.body = ast_clone(a, n->critical.body, hook, ud);
        break;
    case NODE_ONCE:
        c->once.body = ast_clone(a, n->once.body, hook, ud);
        break;
    case NODE_AWAIT:
        c->await_stmt.cond = ast_clone(a, n->await_stmt.cond, hook, ud);
        break;
    case NODE_STATIC_ASSERT:
        c->static_assert_stmt.cond = ast_clone(a, n->static_assert_stmt.cond, hook, ud);
        break;
    case NODE_SPAWN:                              /* handled above (shared) */
    case NODE_BREAK: case NODE_CONTINUE: case NODE_GOTO: case NODE_LABEL:
    case NODE_YIELD:
        break;

    /* ---- expressions ---- */
    case NODE_INT_LIT: case NODE_FLOAT_LIT: case NODE_STRING_LIT:
    case NODE_CHAR_LIT: case NODE_BOOL_LIT: case NODE_NULL_LIT:
    case NODE_IDENT: case NODE_CAST: case NODE_SIZEOF:
        break;
    case NODE_BINARY:
        c->binary.left  = ast_clone(a, n->binary.left, hook, ud);
        c->binary.right = ast_clone(a, n->binary.right, hook, ud);
        break;
    case NODE_UNARY:
        c->unary.operand = ast_clone(a, n->unary.operand, hook, ud);
        break;
    case NODE_ASSIGN:
        c->assign.target = ast_clone(a, n->assign.target, hook, ud);
        c->assign.value  = ast_clone(a, n->assign.value, hook, ud);
        break;
    case NODE_CALL:
        c->call.callee = ast_clone(a, n->call.callee, hook, ud);
        c->call.args   = ast_clone_list(a, n->call.args, n->call.arg_count, hook, ud);
        c->call.comptime_struct_init = ast_clone(a, n->call.comptime_struct_init, hook, ud);
        break;
    case NODE_FIELD:
        c->field.object = ast_clone(a, n->field.object, hook, ud);
        break;
    case NODE_INDEX:
        c->index_expr.object = ast_clone(a, n->index_expr.object, hook, ud);
        c->index_expr.index  = ast_clone(a, n->index_expr.index, hook, ud);
        break;
    case NODE_SLICE:
        c->slice.object = ast_clone(a, n->slice.object, hook, ud);
        c->slice.start  = ast_clone(a, n->slice.start, hook, ud);
        c->slice.end    = ast_clone(a, n->slice.end, hook, ud);
        break;
    case NODE_ORELSE:
        c->orelse.expr     = ast_clone(a, n->orelse.expr, hook, ud);
        c->orelse.fallback = ast_clone(a, n->orelse.fallback, hook, ud);
        break;
    case NODE_INTRINSIC:
        c->intrinsic.args = ast_clone_list(a, n->intrinsic.args, n->intrinsic.arg_count, hook, ud);
        break;
    case NODE_TYPECAST:
        c->typecast.expr = ast_clone(a, n->typecast.expr, hook, ud);
        break;
    case NODE_STRUCT_INIT:
        if (n->struct_init.fields && n->struct_init.field_count > 0) {
            DesigField *f = (DesigField *)arena_alloc(a,
                (size_t)n->struct_init.field_count * sizeof(DesigField));
            for (int i = 0; i < n->struct_init.field_count; i++) {
                f[i] = n->struct_init.fields[i];
                f[i].value = ast_clone(a, n->struct_init.fields[i].value, hook, ud);
            }
            c->struct_init.fields = f;
        }
        break;
    }
    if (hook) hook(n, c, ud);
    return c;
}
