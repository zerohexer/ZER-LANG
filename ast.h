#ifndef ZER_AST_H
#define ZER_AST_H

#include "lexer.h"

/* ================================================================
 * ZER-LANG Abstract Syntax Tree
 *
 * Design: tagged union (chibicc pattern).
 * Each node has a kind enum + union of data for that kind.
 * Nodes point to children via pointers — forms the tree.
 * All nodes allocated from an arena — never freed individually.
 * ================================================================ */

/* ---- Forward declarations ---- */
typedef struct Node Node;
typedef struct TypeNode TypeNode;

/* ---- Arena allocator for AST nodes ---- */
typedef struct {
    char *buf;
    size_t used;
    size_t capacity;
} Arena;

void arena_init(Arena *a, size_t capacity);
void *arena_alloc(Arena *a, size_t size);
void arena_free(Arena *a);

/* ---- Source location ---- */
typedef struct {
    int line;
    const char *file;       /* NULL = current file */
} SrcLoc;

/* ================================================================
 * TYPE NODES — represent type expressions in source code
 *
 * These are SYNTACTIC types (what the programmer wrote).
 * The type checker later resolves these to semantic Type structs.
 * ================================================================ */

typedef enum {
    /* primitives */
    TYNODE_U8, TYNODE_U16, TYNODE_U32, TYNODE_U64,
    TYNODE_I8, TYNODE_I16, TYNODE_I32, TYNODE_I64,
    TYNODE_USIZE,
    TYNODE_F32, TYNODE_F64,
    TYNODE_BOOL,
    TYNODE_VOID,
    TYNODE_OPAQUE,

    /* compound */
    TYNODE_POINTER,         /* *T */
    TYNODE_OPTIONAL,        /* ?T */
    TYNODE_SLICE,           /* []T */
    TYNODE_ARRAY,           /* T[N] */
    TYNODE_NAMED,           /* user-defined type name (struct/enum/union/typedef) */
    TYNODE_FUNC_PTR,        /* void (*cb)(i32, *opaque) */

    /* builtins */
    TYNODE_POOL,            /* Pool(T, N) */
    TYNODE_RING,            /* Ring(T, N) */
    TYNODE_ARENA,           /* Arena */
    TYNODE_HANDLE,          /* Handle(T) */
    TYNODE_SLAB,            /* Slab(T) — dynamic growable pool */

    /* qualifiers */
    TYNODE_CONST,           /* const T */
    TYNODE_VOLATILE,        /* volatile T */
} TypeNodeKind;

struct TypeNode {
    TypeNodeKind kind;
    SrcLoc loc;

    union {
        /* TYNODE_POINTER: *T */
        struct { TypeNode *inner; } pointer;

        /* TYNODE_OPTIONAL: ?T */
        struct { TypeNode *inner; } optional;

        /* TYNODE_SLICE: []T */
        struct { TypeNode *inner; } slice;

        /* TYNODE_ARRAY: T[N] — e.g., u8[256] */
        struct {
            TypeNode *elem;
            Node *size_expr;        /* compile-time constant expression */
        } array;

        /* TYNODE_NAMED: user type name — "Task", "UartError", etc. */
        struct {
            const char *name;
            size_t name_len;
        } named;

        /* TYNODE_FUNC_PTR: void (*name)(params...) */
        struct {
            TypeNode *return_type;
            TypeNode **param_types;
            const char **param_names;   /* NULL if unnamed */
            bool *param_keeps;          /* per-param keep flags (NULL if none) */
            int param_count;
        } func_ptr;

        /* TYNODE_POOL: Pool(T, N) */
        struct {
            TypeNode *elem;
            Node *count_expr;       /* compile-time constant */
        } pool;

        /* TYNODE_RING: Ring(T, N) */
        struct {
            TypeNode *elem;
            Node *count_expr;
        } ring;

        /* TYNODE_HANDLE: Handle(T) */
        struct { TypeNode *elem; } handle;

        /* TYNODE_SLAB: Slab(T) — no count, grows dynamically */
        struct { TypeNode *elem; } slab;

        /* TYNODE_CONST / TYNODE_VOLATILE: qualifier wrapping inner type */
        struct { TypeNode *inner; } qualified;
    };
};

/* ================================================================
 * AST NODES — represent statements and expressions
 * ================================================================ */

typedef enum {
    /* === Top-level declarations === */
    NODE_FILE,              /* root: list of top-level declarations */
    NODE_FUNC_DECL,         /* u32 add(u32 a, u32 b) { ... } */
    NODE_STRUCT_DECL,       /* struct Task { ... } */
    NODE_ENUM_DECL,         /* enum State { idle, running, ... } */
    NODE_UNION_DECL,        /* union Message { SensorData sensor; ... } */
    NODE_TYPEDEF,           /* typedef u32 Milliseconds; */
    NODE_IMPORT,            /* import uart; */
    NODE_CINCLUDE,          /* cinclude "header.h"; */
    NODE_INTERRUPT,         /* interrupt USART1 { ... } */
    NODE_GLOBAL_VAR,        /* top-level variable declaration */

    /* === Statements === */
    NODE_VAR_DECL,          /* u32 x = 5; */
    NODE_BLOCK,             /* { stmt; stmt; ... } */
    NODE_IF,                /* if (cond) |capture| { ... } else { ... } */
    NODE_FOR,               /* for (init; cond; step) { ... } */
    NODE_WHILE,             /* while (cond) { ... } */
    NODE_SWITCH,            /* switch (expr) { arms... } */
    NODE_RETURN,            /* return expr; */
    NODE_BREAK,             /* break; */
    NODE_CONTINUE,          /* continue; */
    NODE_DEFER,             /* defer stmt; */
    NODE_EXPR_STMT,         /* expression as statement: foo(); */
    NODE_ASM,               /* asm("nop"); */

    /* === Expressions === */
    NODE_INT_LIT,           /* 42, 0xFF, 0b1010 */
    NODE_FLOAT_LIT,         /* 3.14 */
    NODE_STRING_LIT,        /* "hello" */
    NODE_CHAR_LIT,          /* 'a' */
    NODE_BOOL_LIT,          /* true, false */
    NODE_NULL_LIT,          /* null */
    NODE_IDENT,             /* x, task, buf */
    NODE_BINARY,            /* a + b, x >= 10, a && b */
    NODE_UNARY,             /* -x, !flag, ~bits, *ptr, &val */
    NODE_ASSIGN,            /* x = 5, x += 1 */
    NODE_CALL,              /* foo(a, b) */
    NODE_FIELD,             /* expr.field */
    NODE_INDEX,             /* expr[index] */
    NODE_SLICE,             /* expr[start..end] */
    NODE_ORELSE,            /* expr orelse value/return/break/block */
    NODE_INTRINSIC,         /* @ptrcast, @truncate, @size, etc. */
    NODE_CAST,              /* implicit cast inserted by type checker */
    NODE_SIZEOF,            /* @size(T) — resolved from intrinsic */
} NodeKind;

/* ---- Switch arm ---- */
typedef struct {
    Node **values;          /* match values: 0, 1, 2 or .variant_name */
    int value_count;        /* number of values (multi-value arms) */
    bool is_default;        /* true if default arm */
    bool is_enum_dot;       /* true if .variant syntax */

    /* capture: |val| or |*val| — NULL if no capture */
    const char *capture_name;
    size_t capture_name_len;
    bool capture_is_ptr;    /* true for |*val| */

    Node *body;             /* arm body (block or expression) */
    SrcLoc loc;
} SwitchArm;

/* ---- Struct field declaration ---- */
typedef struct {
    TypeNode *type;
    const char *name;
    size_t name_len;
    bool is_keep;           /* keep pointer field */
    SrcLoc loc;
} FieldDecl;

/* ---- Function parameter ---- */
typedef struct {
    TypeNode *type;
    const char *name;
    size_t name_len;
    bool is_keep;           /* keep *T param */
    SrcLoc loc;
} ParamDecl;

/* ---- Enum variant ---- */
typedef struct {
    const char *name;
    size_t name_len;
    Node *value;            /* explicit value expression, or NULL */
    SrcLoc loc;
} EnumVariant;

/* ---- Union variant ---- */
typedef struct {
    const char *name;
    size_t name_len;
    TypeNode *type;         /* payload type */
    SrcLoc loc;
} UnionVariant;

/* ================================================================
 * THE NODE — tagged union
 * ================================================================ */

struct Node {
    NodeKind kind;
    SrcLoc loc;

    union {
        /* NODE_FILE: root of compilation unit */
        struct {
            Node **decls;
            int decl_count;
        } file;

        /* NODE_FUNC_DECL: u32 add(u32 a, u32 b) { ... } */
        struct {
            TypeNode *return_type;
            const char *name;
            size_t name_len;
            ParamDecl *params;
            int param_count;
            Node *body;             /* block, or NULL for forward decl */
            bool is_static;         /* static = module-private */
        } func_decl;

        /* NODE_STRUCT_DECL: struct Task { ... } or packed struct { ... } */
        struct {
            const char *name;
            size_t name_len;
            FieldDecl *fields;
            int field_count;
            bool is_packed;
        } struct_decl;

        /* NODE_ENUM_DECL: enum State { idle, running, ... } */
        struct {
            const char *name;
            size_t name_len;
            EnumVariant *variants;
            int variant_count;
        } enum_decl;

        /* NODE_UNION_DECL: union Message { SensorData sensor; ... } */
        struct {
            const char *name;
            size_t name_len;
            UnionVariant *variants;
            int variant_count;
        } union_decl;

        /* NODE_TYPEDEF: typedef u32 Milliseconds; / distinct typedef u32 Celsius; */
        struct {
            TypeNode *type;
            const char *name;
            size_t name_len;
            bool is_distinct;
        } typedef_decl;

        /* NODE_IMPORT: import uart; */
        struct {
            const char *module_name;
            size_t module_name_len;
        } import;

        /* NODE_CINCLUDE: cinclude "header.h"; */
        struct {
            const char *path;
            size_t path_len;
        } cinclude;

        /* NODE_INTERRUPT: interrupt USART1 as "handler_name" { ... } */
        struct {
            const char *name;
            size_t name_len;
            const char *as_name;        /* NULL if no 'as' clause */
            size_t as_name_len;
            Node *body;
        } interrupt;

        /* NODE_GLOBAL_VAR / NODE_VAR_DECL: type name = value; */
        struct {
            TypeNode *type;
            const char *name;
            size_t name_len;
            Node *init;             /* initializer, or NULL */
            bool is_const;
            bool is_static;         /* static storage duration */
            bool is_volatile;
        } var_decl;

        /* NODE_BLOCK: { stmts... } */
        struct {
            Node **stmts;
            int stmt_count;
        } block;

        /* NODE_IF: if (cond) |capture| { then } else { els } */
        struct {
            Node *cond;
            Node *then_body;
            Node *else_body;            /* NULL if no else */
            /* optional capture for if-unwrap: if (maybe) |val| { } */
            const char *capture_name;   /* NULL if no capture */
            size_t capture_name_len;
            bool capture_is_ptr;        /* true for |*val| */
        } if_stmt;

        /* NODE_FOR: for (init; cond; step) { body } */
        struct {
            Node *init;             /* var decl or expr, or NULL */
            Node *cond;             /* condition, or NULL (infinite) */
            Node *step;             /* increment, or NULL */
            Node *body;
        } for_stmt;

        /* NODE_WHILE: while (cond) { body } */
        struct {
            Node *cond;
            Node *body;
        } while_stmt;

        /* NODE_SWITCH: switch (expr) { arms... } */
        struct {
            Node *expr;
            SwitchArm *arms;
            int arm_count;
        } switch_stmt;

        /* NODE_RETURN: return expr; */
        struct { Node *expr; /* NULL for bare return */ } ret;

        /* NODE_DEFER: defer stmt; or defer { block } */
        struct { Node *body; } defer;

        /* NODE_EXPR_STMT: expression used as statement */
        struct { Node *expr; } expr_stmt;

        /* NODE_ASM: asm("nop"); */
        struct {
            const char *code;
            size_t code_len;
        } asm_stmt;

        /* NODE_INT_LIT: 42, 0xFF, 0b1010, 1_000_000 */
        struct { uint64_t value; } int_lit;

        /* NODE_FLOAT_LIT: 3.14 */
        struct { double value; } float_lit;

        /* NODE_STRING_LIT: "hello" */
        struct {
            const char *value;
            size_t length;
        } string_lit;

        /* NODE_CHAR_LIT: 'a', '\n' */
        struct { char value; } char_lit;

        /* NODE_BOOL_LIT: true, false */
        struct { bool value; } bool_lit;

        /* NODE_IDENT: variable/type name reference */
        struct {
            const char *name;
            size_t name_len;
        } ident;

        /* NODE_BINARY: left op right */
        struct {
            TokenType op;       /* TOK_PLUS, TOK_STAR, TOK_AMPAMP, etc. */
            Node *left;
            Node *right;
        } binary;

        /* NODE_UNARY: op expr */
        struct {
            TokenType op;       /* TOK_MINUS, TOK_BANG, TOK_TILDE, TOK_STAR, TOK_AMP */
            Node *operand;
        } unary;

        /* NODE_ASSIGN: target op= value */
        struct {
            TokenType op;       /* TOK_EQ, TOK_PLUSEQ, TOK_MINUSEQ, etc. */
            Node *target;
            Node *value;
        } assign;

        /* NODE_CALL: callee(args...) */
        struct {
            Node *callee;       /* expression — ident, field access, etc. */
            Node **args;
            int arg_count;
        } call;

        /* NODE_FIELD: expr.field */
        struct {
            Node *object;
            const char *field_name;
            size_t field_name_len;
        } field;

        /* NODE_INDEX: expr[index] */
        struct {
            Node *object;
            Node *index;
        } index_expr;

        /* NODE_SLICE: expr[start..end] */
        struct {
            Node *object;
            Node *start;        /* NULL for [..end] */
            Node *end;          /* NULL for [start..] */
        } slice;

        /* NODE_ORELSE: expr orelse fallback */
        struct {
            Node *expr;
            Node *fallback;     /* value, return/break/continue, or block */
            bool fallback_is_return;
            bool fallback_is_break;
            bool fallback_is_continue;
        } orelse;

        /* NODE_INTRINSIC: @name(args...) */
        struct {
            const char *name;
            size_t name_len;
            Node **args;
            int arg_count;
            TypeNode *type_arg;     /* for @ptrcast(*T, expr), @size(T), etc. */
        } intrinsic;
    };
};

/* ---- AST utility functions ---- */
const char *node_kind_name(NodeKind kind);
void ast_print(Node *node, int indent);

/* Evaluate compile-time constant integer expression.
 * Supports: int literals, +, -, *, /, %, <<, >>, &, |, unary -.
 * Returns -1 if not a compile-time constant.
 * Used by both checker (type resolution) and emitter (resolve_type_for_emit). */
/* sentinel for "not a compile-time constant" — INT64_MIN is never a valid
 * array/pool/ring size and won't appear in real constant expressions. */
#define CONST_EVAL_FAIL INT64_MIN

static inline int64_t eval_const_expr(Node *n) {
    if (!n) return CONST_EVAL_FAIL;
    if (n->kind == NODE_INT_LIT) return (int64_t)n->int_lit.value;
    if (n->kind == NODE_UNARY && n->unary.op == TOK_MINUS) {
        int64_t v = eval_const_expr(n->unary.operand);
        return v != CONST_EVAL_FAIL ? -v : CONST_EVAL_FAIL;
    }
    if (n->kind == NODE_BINARY) {
        int64_t l = eval_const_expr(n->binary.left);
        int64_t r = eval_const_expr(n->binary.right);
        if (l == CONST_EVAL_FAIL || r == CONST_EVAL_FAIL) return CONST_EVAL_FAIL;
        switch (n->binary.op) {
        case TOK_PLUS:
            /* overflow check: if signs match and result sign differs */
            if ((r > 0 && l > INT64_MAX - r) || (r < 0 && l < INT64_MIN - r))
                return CONST_EVAL_FAIL;
            return l + r;
        case TOK_MINUS:
            if ((r < 0 && l > INT64_MAX + r) || (r > 0 && l < INT64_MIN + r))
                return CONST_EVAL_FAIL;
            return l - r;
        case TOK_STAR:
            /* overflow check for multiplication */
            if (l != 0 && r != 0) {
                if ((l > 0 && r > 0 && l > INT64_MAX / r) ||
                    (l < 0 && r < 0 && l < INT64_MAX / r) ||
                    (l > 0 && r < 0 && r < INT64_MIN / l) ||
                    (l < 0 && r > 0 && l < INT64_MIN / r))
                    return CONST_EVAL_FAIL;
            }
            return l * r;
        case TOK_SLASH:
            if (r == 0) return CONST_EVAL_FAIL;
            /* BUG-296: INT_MIN / -1 is signed overflow */
            if (l == INT64_MIN && r == -1) return CONST_EVAL_FAIL;
            return l / r;
        case TOK_PERCENT:
            if (r == 0) return CONST_EVAL_FAIL;
            if (l == INT64_MIN && r == -1) return CONST_EVAL_FAIL;
            return l % r;
        case TOK_LSHIFT:
            if (r < 0 || r >= 63) return CONST_EVAL_FAIL;
            return l << r;
        case TOK_RSHIFT:
            if (r < 0 || r >= 63) return CONST_EVAL_FAIL;
            return l >> r;
        case TOK_AMP:     return l & r;
        case TOK_PIPE:    return l | r;
        default: return CONST_EVAL_FAIL;
        }
    }
    return CONST_EVAL_FAIL;
}

#endif /* ZER_AST_H */
