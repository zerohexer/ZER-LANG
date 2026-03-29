#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * ZER-LANG Parser — Recursive Descent
 *
 * Reads tokens from lexer, builds AST.
 * Based on chibicc's pattern: parse functions return Node*.
 * Operator precedence via Pratt parsing for expressions.
 * ================================================================ */

/* ---- Error reporting ---- */

static void error_at(Parser *p, Token *tok, const char *msg) {
    if (p->panic_mode) return;
    p->panic_mode = true;
    p->had_error = true;
    fprintf(stderr, "%s:%d: error: %s", p->file_name, tok->line, msg);
    if (tok->type == TOK_EOF) {
        fprintf(stderr, " at end of file");
    } else if (tok->type != TOK_ERROR) {
        fprintf(stderr, " at '%.*s'", (int)tok->length, tok->start);
    }
    fprintf(stderr, "\n");
}

static void error(Parser *p, const char *msg) {
    error_at(p, &p->previous, msg);
}

static void error_current(Parser *p, const char *msg) {
    error_at(p, &p->current, msg);
}

/* ---- Token consumption ---- */

static void advance(Parser *p) {
    p->previous = p->current;
    for (;;) {
        p->current = next_token(p->scanner);
        if (p->current.type != TOK_ERROR) break;
        error_current(p, p->current.start);
    }
}

static bool check(Parser *p, TokenType type) {
    return p->current.type == type;
}

static bool match(Parser *p, TokenType type) {
    if (!check(p, type)) return false;
    advance(p);
    return true;
}

static void consume(Parser *p, TokenType type, const char *msg) {
    if (p->current.type == type) {
        advance(p);
        return;
    }
    error_current(p, msg);
}

static Token peek(Parser *p) {
    return p->current;
}

/* ---- Arena helpers ---- */

static Node *new_node(Parser *p, NodeKind kind) {
    Node *n = (Node *)arena_alloc(p->arena, sizeof(Node));
    if (!n) { p->oom = true; p->had_error = true; static Node oom_node = {0}; return &oom_node; }
    n->kind = kind;
    n->loc.line = p->previous.line;
    n->loc.file = p->file_name;
    return n;
}

static TypeNode *new_type_node(Parser *p, TypeNodeKind kind) {
    TypeNode *t = (TypeNode *)arena_alloc(p->arena, sizeof(TypeNode));
    if (!t) { p->oom = true; p->had_error = true; static TypeNode oom_tn = {0}; return &oom_tn; }
    t->kind = kind;
    t->loc.line = p->previous.line;
    t->loc.file = p->file_name;
    return t;
}

/* safe arena alloc — sets oom flag on failure */
static void *parser_alloc(Parser *p, size_t size) {
    void *ptr = arena_alloc(p->arena, size);
    if (!ptr) { p->oom = true; p->had_error = true; }
    return ptr;
}

/* allocate array in arena */
static void **arena_array(Parser *p, int count, size_t elem_size) {
    return (void **)arena_alloc(p->arena, count * elem_size);
}

/* ---- Token text helpers ---- */

static const char *tok_text(Token *t) { return t->start; }
static size_t tok_len(Token *t) { return t->length; }

/* copy token text into arena (for names that must persist) */
static const char *tok_str(Parser *p, Token *t) {
    char *s = (char *)arena_alloc(p->arena, t->length + 1);
    memcpy(s, t->start, t->length);
    s[t->length] = '\0';
    return s;
}

/* ---- Forward declarations ---- */
static Node *parse_expression(Parser *p);
static Node *parse_statement(Parser *p);
static Node *parse_block(Parser *p);
static Node *parse_declaration(Parser *p);
static TypeNode *parse_type(Parser *p);

/* ---- Function pointer type helper ----
 * Called when we see '(' '*' after a return type.
 * Parses: (*name)(param_types...) or (*)(param_types...)
 * Returns TYNODE_FUNC_PTR with name stored in func_ptr.param_names[0]
 * if a name was present (for struct fields / var decls).
 */

/* Helper: check if current position is '(' '*' — the start of a function pointer.
 * Does NOT consume tokens — caller must save/restore if needed. */
static bool is_func_ptr_start(Parser *p) {
    if (!check(p, TOK_LPAREN)) return false;
    Scanner saved = *p->scanner;
    Token saved_cur = p->current;
    Token saved_prev = p->previous;
    advance(p); /* consume '(' */
    bool result = check(p, TOK_STAR);
    *p->scanner = saved;
    p->current = saved_cur;
    p->previous = saved_prev;
    return result;
}

static TypeNode *parse_func_ptr_after_ret(Parser *p, TypeNode *ret_type,
                                           const char **out_name,
                                           size_t *out_name_len) {
    /* already consumed '(' and '*' */
    const char *name = NULL;
    size_t name_len = 0;

    /* optional name: (*callback) vs (*) */
    if (check(p, TOK_IDENT)) {
        advance(p);
        name = tok_text(&p->previous);
        name_len = tok_len(&p->previous);
    }
    consume(p, TOK_RPAREN, "expected ')' after function pointer name");

    /* parameter list */
    consume(p, TOK_LPAREN, "expected '(' for function pointer parameters");

    TypeNode *stack_pt[8];
    const char *stack_pn[8];
    bool stack_pk[8];
    TypeNode **param_types = stack_pt;
    const char **param_names = stack_pn;
    bool *param_keeps = stack_pk;
    int fpp_cap = 8;
    int param_count = 0;
    bool any_keep = false;

    if (!check(p, TOK_RPAREN)) {
        do {
            if (param_count >= fpp_cap) {
                int new_cap = fpp_cap * 2;
                TypeNode **new_types = (TypeNode **)parser_alloc(p, new_cap * sizeof(TypeNode *));
                const char **new_names = (const char **)parser_alloc(p, new_cap * sizeof(const char *));
                bool *new_keeps = (bool *)parser_alloc(p, new_cap * sizeof(bool));
                if (!new_types || !new_names || !new_keeps) break;
                memcpy(new_types, param_types, param_count * sizeof(TypeNode *));
                memcpy(new_names, param_names, param_count * sizeof(const char *));
                memcpy(new_keeps, param_keeps, param_count * sizeof(bool));
                param_types = new_types;
                param_names = new_names;
                param_keeps = new_keeps;
                fpp_cap = new_cap;
            }
            param_keeps[param_count] = match(p, TOK_KEEP);
            if (param_keeps[param_count]) any_keep = true;
            param_types[param_count] = parse_type(p);
            /* optional param name */
            if (check(p, TOK_IDENT)) {
                advance(p);
                param_names[param_count] = tok_text(&p->previous);
            } else {
                param_names[param_count] = NULL;
            }
            param_count++;
        } while (match(p, TOK_COMMA));
    }
    consume(p, TOK_RPAREN, "expected ')' after function pointer parameters");

    TypeNode *t = new_type_node(p, TYNODE_FUNC_PTR);
    t->func_ptr.return_type = ret_type;
    t->func_ptr.param_count = param_count;
    if (param_count > 0) {
        t->func_ptr.param_types = (TypeNode **)arena_alloc(p->arena, param_count * sizeof(TypeNode *));
        memcpy(t->func_ptr.param_types, param_types, param_count * sizeof(TypeNode *));
        t->func_ptr.param_names = (const char **)arena_alloc(p->arena, param_count * sizeof(const char *));
        memcpy(t->func_ptr.param_names, param_names, param_count * sizeof(const char *));
        if (any_keep) {
            t->func_ptr.param_keeps = (bool *)arena_alloc(p->arena, param_count * sizeof(bool));
            memcpy(t->func_ptr.param_keeps, param_keeps, param_count * sizeof(bool));
        }
    }

    if (out_name) *out_name = name;
    if (out_name_len) *out_name_len = name_len;

    return t;
}

/* ================================================================
 * TYPE PARSING
 *
 * ZER types: u32, *Task, ?*Task, []u8, u8[256], Pool(T, N), etc.
 * ================================================================ */

static bool is_type_token(TokenType type) {
    switch (type) {
    case TOK_U8: case TOK_U16: case TOK_U32: case TOK_U64:
    case TOK_I8: case TOK_I16: case TOK_I32: case TOK_I64:
    case TOK_USIZE: case TOK_F32: case TOK_F64:
    case TOK_BOOL: case TOK_VOID: case TOK_OPAQUE:
    case TOK_POOL: case TOK_RING: case TOK_ARENA: case TOK_HANDLE: case TOK_SLAB:
    case TOK_CONST: case TOK_VOLATILE:
    case TOK_STAR:      /* *T pointer type */
    case TOK_QUESTION:  /* ?T optional type */
    case TOK_LBRACKET:  /* []T slice type */
    case TOK_IDENT:     /* named type (struct/enum/union/typedef) */
        return true;
    default:
        return false;
    }
}

/* parse base type: u32, bool, void, Task, opaque */
static TypeNode *parse_base_type(Parser *p) {
    Token tok = p->current;
    switch (tok.type) {
    case TOK_U8:    advance(p); return new_type_node(p, TYNODE_U8);
    case TOK_U16:   advance(p); return new_type_node(p, TYNODE_U16);
    case TOK_U32:   advance(p); return new_type_node(p, TYNODE_U32);
    case TOK_U64:   advance(p); return new_type_node(p, TYNODE_U64);
    case TOK_I8:    advance(p); return new_type_node(p, TYNODE_I8);
    case TOK_I16:   advance(p); return new_type_node(p, TYNODE_I16);
    case TOK_I32:   advance(p); return new_type_node(p, TYNODE_I32);
    case TOK_I64:   advance(p); return new_type_node(p, TYNODE_I64);
    case TOK_USIZE: advance(p); return new_type_node(p, TYNODE_USIZE);
    case TOK_F32:   advance(p); return new_type_node(p, TYNODE_F32);
    case TOK_F64:   advance(p); return new_type_node(p, TYNODE_F64);
    case TOK_BOOL:  advance(p); return new_type_node(p, TYNODE_BOOL);
    case TOK_VOID:  advance(p); return new_type_node(p, TYNODE_VOID);
    case TOK_OPAQUE: advance(p); return new_type_node(p, TYNODE_OPAQUE);
    case TOK_ARENA: advance(p); return new_type_node(p, TYNODE_ARENA);

    case TOK_POOL: {
        advance(p);
        consume(p, TOK_LPAREN, "expected '(' after 'Pool'");
        TypeNode *t = new_type_node(p, TYNODE_POOL);
        t->pool.elem = parse_type(p);
        consume(p, TOK_COMMA, "expected ',' in Pool(T, N)");
        t->pool.count_expr = parse_expression(p);
        consume(p, TOK_RPAREN, "expected ')' after Pool(T, N)");
        return t;
    }

    case TOK_RING: {
        advance(p);
        consume(p, TOK_LPAREN, "expected '(' after 'Ring'");
        TypeNode *t = new_type_node(p, TYNODE_RING);
        t->ring.elem = parse_type(p);
        consume(p, TOK_COMMA, "expected ',' in Ring(T, N)");
        t->ring.count_expr = parse_expression(p);
        consume(p, TOK_RPAREN, "expected ')' after Ring(T, N)");
        return t;
    }

    case TOK_HANDLE: {
        advance(p);
        consume(p, TOK_LPAREN, "expected '(' after 'Handle'");
        TypeNode *t = new_type_node(p, TYNODE_HANDLE);
        t->handle.elem = parse_type(p);
        consume(p, TOK_RPAREN, "expected ')' after Handle(T)");
        return t;
    }

    case TOK_SLAB: {
        advance(p);
        consume(p, TOK_LPAREN, "expected '(' after 'Slab'");
        TypeNode *t = new_type_node(p, TYNODE_SLAB);
        t->slab.elem = parse_type(p);
        consume(p, TOK_RPAREN, "expected ')' after Slab(T)");
        return t;
    }

    case TOK_IDENT: {
        advance(p);
        TypeNode *t = new_type_node(p, TYNODE_NAMED);
        t->named.name = tok_text(&p->previous);
        t->named.name_len = tok_len(&p->previous);
        return t;
    }

    default:
        error_current(p, "expected type");
        advance(p);
        return new_type_node(p, TYNODE_VOID);
    }
}

/* parse full type with prefix modifiers: const, volatile, *, ?, [] */
static TypeNode *parse_type(Parser *p) {
    /* const T */
    if (match(p, TOK_CONST)) {
        TypeNode *t = new_type_node(p, TYNODE_CONST);
        t->qualified.inner = parse_type(p);
        return t;
    }

    /* volatile T */
    if (match(p, TOK_VOLATILE)) {
        TypeNode *t = new_type_node(p, TYNODE_VOLATILE);
        t->qualified.inner = parse_type(p);
        return t;
    }

    /* *T — pointer type */
    if (match(p, TOK_STAR)) {
        TypeNode *t = new_type_node(p, TYNODE_POINTER);
        t->pointer.inner = parse_type(p);
        return t;
    }

    /* ?T — optional type */
    if (match(p, TOK_QUESTION)) {
        TypeNode *t = new_type_node(p, TYNODE_OPTIONAL);
        t->optional.inner = parse_type(p);
        return t;
    }

    /* []T — slice type */
    if (check(p, TOK_LBRACKET)) {
        advance(p); /* consume [ */
        if (match(p, TOK_RBRACKET)) {
            /* []T — slice */
            TypeNode *t = new_type_node(p, TYNODE_SLICE);
            t->slice.inner = parse_type(p);
            return t;
        }
        /* not a slice — could be a mistake, but let it fall through */
        error(p, "expected ']' for slice type");
        return new_type_node(p, TYNODE_VOID);
    }

    /* base type, possibly followed by [N] for array */
    TypeNode *base = parse_base_type(p);

    /* check for array suffix: T[N] or T[N][M] (multi-dimensional) */
    if (match(p, TOK_LBRACKET)) {
        TypeNode *arr = new_type_node(p, TYNODE_ARRAY);
        arr->array.elem = base;
        arr->array.size_expr = parse_expression(p);
        consume(p, TOK_RBRACKET, "expected ']' after array size");
        /* multi-dim: T[N][M] → array of M elements of T[N] */
        while (match(p, TOK_LBRACKET)) {
            TypeNode *outer = new_type_node(p, TYNODE_ARRAY);
            outer->array.elem = arr;
            outer->array.size_expr = parse_expression(p);
            consume(p, TOK_RBRACKET, "expected ']' after array size");
            arr = outer;
        }
        return arr;
    }

    return base;
}

/* ================================================================
 * EXPRESSION PARSING — Pratt parser (precedence climbing)
 * ================================================================ */

typedef enum {
    PREC_NONE,
    PREC_ASSIGN,        /* = += -= etc. */
    PREC_ORELSE,        /* orelse */
    PREC_OR,            /* || */
    PREC_AND,           /* && */
    PREC_BIT_OR,        /* | */
    PREC_BIT_XOR,       /* ^ */
    PREC_BIT_AND,       /* & */
    PREC_EQUALITY,      /* == != */
    PREC_COMPARISON,    /* < > <= >= */
    PREC_SHIFT,         /* << >> */
    PREC_TERM,          /* + - */
    PREC_FACTOR,        /* * / % */
    PREC_UNARY,         /* - ! ~ * & */
    PREC_CALL,          /* . () [] */
    PREC_PRIMARY,
} Precedence;

/* get precedence of a binary operator token */
static Precedence get_precedence(TokenType type) {
    switch (type) {
    case TOK_ORELSE:     return PREC_ORELSE;
    case TOK_PIPEPIPE:   return PREC_OR;
    case TOK_AMPAMP:     return PREC_AND;
    case TOK_PIPE:       return PREC_BIT_OR;
    case TOK_CARET:      return PREC_BIT_XOR;
    case TOK_AMP:        return PREC_BIT_AND;
    case TOK_EQEQ:
    case TOK_BANGEQ:     return PREC_EQUALITY;
    case TOK_LT:
    case TOK_GT:
    case TOK_LTEQ:
    case TOK_GTEQ:       return PREC_COMPARISON;
    case TOK_LSHIFT:
    case TOK_RSHIFT:     return PREC_SHIFT;
    case TOK_PLUS:
    case TOK_MINUS:      return PREC_TERM;
    case TOK_STAR:
    case TOK_SLASH:
    case TOK_PERCENT:    return PREC_FACTOR;
    default:             return PREC_NONE;
    }
}

static bool is_assign_op(TokenType type) {
    switch (type) {
    case TOK_EQ: case TOK_PLUSEQ: case TOK_MINUSEQ:
    case TOK_STAREQ: case TOK_SLASHEQ: case TOK_PERCENTEQ:
    case TOK_AMPEQ: case TOK_PIPEEQ: case TOK_CARETEQ:
    case TOK_LSHIFTEQ: case TOK_RSHIFTEQ:
        return true;
    default:
        return false;
    }
}

/* forward */
static Node *parse_precedence(Parser *p, Precedence min_prec);

/* ---- Primary expressions ---- */

static Node *parse_primary(Parser *p) {
    /* integer literal */
    if (match(p, TOK_NUMBER_INT)) {
        Node *n = new_node(p, NODE_INT_LIT);
        /* parse the number value from token text */
        const char *text = p->previous.start;
        size_t len = p->previous.length;
        uint64_t val = 0;
        if (len > 2 && text[0] == '0' && text[1] == 'x') {
            for (size_t i = 2; i < len; i++) {
                if (text[i] == '_') continue;
                val *= 16;
                if (text[i] >= '0' && text[i] <= '9') val += text[i] - '0';
                else if (text[i] >= 'a' && text[i] <= 'f') val += text[i] - 'a' + 10;
                else if (text[i] >= 'A' && text[i] <= 'F') val += text[i] - 'A' + 10;
            }
        } else if (len > 2 && text[0] == '0' && text[1] == 'b') {
            for (size_t i = 2; i < len; i++) {
                if (text[i] == '_') continue;
                val = val * 2 + (text[i] - '0');
            }
        } else {
            for (size_t i = 0; i < len; i++) {
                if (text[i] == '_') continue;
                val = val * 10 + (text[i] - '0');
            }
        }
        n->int_lit.value = val;
        return n;
    }

    /* float literal */
    if (match(p, TOK_NUMBER_FLOAT)) {
        Node *n = new_node(p, NODE_FLOAT_LIT);
        n->float_lit.value = strtod(p->previous.start, NULL);
        return n;
    }

    /* string literal */
    if (match(p, TOK_STRING)) {
        Node *n = new_node(p, NODE_STRING_LIT);
        /* skip opening and closing quotes */
        n->string_lit.value = p->previous.start + 1;
        n->string_lit.length = p->previous.length - 2;
        return n;
    }

    /* char literal */
    if (match(p, TOK_CHAR)) {
        Node *n = new_node(p, NODE_CHAR_LIT);
        /* simple: take char after opening quote */
        const char *text = p->previous.start;
        if (text[1] == '\\') {
            switch (text[2]) {
            case 'n':  n->char_lit.value = '\n'; break;
            case 't':  n->char_lit.value = '\t'; break;
            case 'r':  n->char_lit.value = '\r'; break;
            case '0':  n->char_lit.value = '\0'; break;
            case '\\': n->char_lit.value = '\\'; break;
            case '\'': n->char_lit.value = '\''; break;
            case 'x': {
                /* \xNN hex escape — parse two hex digits */
                uint8_t val = 0;
                for (int i = 3; i <= 4; i++) {
                    char ch = text[i];
                    uint8_t digit;
                    if (ch >= '0' && ch <= '9') digit = ch - '0';
                    else if (ch >= 'a' && ch <= 'f') digit = 10 + ch - 'a';
                    else if (ch >= 'A' && ch <= 'F') digit = 10 + ch - 'A';
                    else { error(p, "invalid hex digit in \\x escape"); digit = 0; }
                    val = (val << 4) | digit;
                }
                n->char_lit.value = val;
                break;
            }
            default:   n->char_lit.value = text[2]; break;
            }
        } else {
            n->char_lit.value = text[1];
        }
        return n;
    }

    /* boolean literals */
    if (match(p, TOK_TRUE)) {
        Node *n = new_node(p, NODE_BOOL_LIT);
        n->bool_lit.value = true;
        return n;
    }
    if (match(p, TOK_FALSE)) {
        Node *n = new_node(p, NODE_BOOL_LIT);
        n->bool_lit.value = false;
        return n;
    }

    /* null */
    if (match(p, TOK_NULL)) {
        return new_node(p, NODE_NULL_LIT);
    }

    /* identifier */
    if (match(p, TOK_IDENT)) {
        Node *n = new_node(p, NODE_IDENT);
        n->ident.name = tok_text(&p->previous);
        n->ident.name_len = tok_len(&p->previous);
        return n;
    }

    /* Arena in expression context — for Arena.over(...) static method */
    if (match(p, TOK_ARENA)) {
        Node *n = new_node(p, NODE_IDENT);
        n->ident.name = "Arena";
        n->ident.name_len = 5;
        return n;
    }

    /* parenthesized expression */
    if (match(p, TOK_LPAREN)) {
        Node *n = parse_expression(p);
        consume(p, TOK_RPAREN, "expected ')' after expression");
        return n;
    }

    /* intrinsic: @name(args...) */
    if (match(p, TOK_AT)) {
        consume(p, TOK_IDENT, "expected intrinsic name after '@'");
        Node *n = new_node(p, NODE_INTRINSIC);
        n->intrinsic.name = tok_text(&p->previous);
        n->intrinsic.name_len = tok_len(&p->previous);
        consume(p, TOK_LPAREN, "expected '(' after intrinsic name");

        /* collect args — first check for type argument */
        /* intrinsics like @ptrcast(*T, expr) and @size(T) take a type as first arg */
        /* @cast always takes a named type (distinct typedef) — allow TOK_IDENT for it */
        /* BUG-316: allow named types (TOK_IDENT) as first arg for intrinsics
         * that take type parameters: @cast, @bitcast, @truncate, @saturate */
        bool force_type_arg = (n->intrinsic.name_len == 4 &&
                               memcmp(n->intrinsic.name, "cast", 4) == 0) ||
                              (n->intrinsic.name_len == 7 &&
                               memcmp(n->intrinsic.name, "bitcast", 7) == 0) ||
                              (n->intrinsic.name_len == 8 &&
                               (memcmp(n->intrinsic.name, "truncate", 8) == 0 ||
                                memcmp(n->intrinsic.name, "saturate", 8) == 0));
        if (is_type_token(p->current.type) &&
            (p->current.type != TOK_IDENT || force_type_arg)) {
            n->intrinsic.type_arg = parse_type(p);
            if (match(p, TOK_COMMA)) {
                /* more args after type */
            }
        }

        /* parse expression arguments */
        Node *stack_iargs[8];
        Node **args = stack_iargs;
        int arg_cap = 8;
        int arg_count = 0;
        if (!check(p, TOK_RPAREN)) {
            do {
                if (arg_count >= arg_cap) {
                    int new_cap = arg_cap * 2;
                    Node **new_args = (Node **)parser_alloc(p, new_cap * sizeof(Node *));
                    if (!new_args) break;
                    memcpy(new_args, args, arg_count * sizeof(Node *));
                    args = new_args;
                    arg_cap = new_cap;
                }
                args[arg_count++] = parse_expression(p);
            } while (match(p, TOK_COMMA));
        }
        consume(p, TOK_RPAREN, "expected ')' after intrinsic arguments");

        n->intrinsic.arg_count = arg_count;
        if (arg_count > 0) {
            n->intrinsic.args = (Node **)arena_alloc(p->arena, arg_count * sizeof(Node *));
            memcpy(n->intrinsic.args, args, arg_count * sizeof(Node *));
        }
        return n;
    }

    error_current(p, "expected expression");
    advance(p);
    return new_node(p, NODE_NULL_LIT);
}

/* ---- Unary expressions ---- */

static Node *parse_postfix(Parser *p, Node *left); /* forward decl */

static Node *parse_unary(Parser *p) {
    /* prefix: - ! ~ * & */
    if (match(p, TOK_MINUS) || match(p, TOK_BANG) || match(p, TOK_TILDE) ||
        match(p, TOK_STAR) || match(p, TOK_AMP)) {
        TokenType op = p->previous.type;
        Node *n = new_node(p, NODE_UNARY);
        n->unary.op = op;
        n->unary.operand = parse_unary(p);
        return n;
    }
    /* parse_primary then postfix (. [] () ) so that &x.field = &(x.field) */
    return parse_postfix(p, parse_primary(p));
}

/* ---- Postfix: calls, field access, indexing, slicing ---- */

static Node *parse_postfix(Parser *p, Node *left) {
    for (;;) {
        /* function call: expr(...) */
        if (match(p, TOK_LPAREN)) {
            Node *n = new_node(p, NODE_CALL);
            n->call.callee = left;
            Node *stack_args[16];
            Node **args = stack_args;
            int arg_cap = 16;
            int arg_count = 0;
            if (!check(p, TOK_RPAREN)) {
                do {
                    if (arg_count >= arg_cap) {
                        int new_cap = arg_cap * 2;
                        Node **new_args = (Node **)parser_alloc(p, new_cap * sizeof(Node *));
                        if (!new_args) break;
                        memcpy(new_args, args, arg_count * sizeof(Node *));
                        args = new_args;
                        arg_cap = new_cap;
                    }
                    args[arg_count++] = parse_expression(p);
                } while (match(p, TOK_COMMA));
            }
            consume(p, TOK_RPAREN, "expected ')' after arguments");
            n->call.arg_count = arg_count;
            if (arg_count > 0) {
                n->call.args = (Node **)arena_alloc(p->arena, arg_count * sizeof(Node *));
                memcpy(n->call.args, args, arg_count * sizeof(Node *));
            }
            left = n;
            continue;
        }

        /* field access: expr.field */
        if (match(p, TOK_DOT)) {
            consume(p, TOK_IDENT, "expected field name after '.'");
            Node *n = new_node(p, NODE_FIELD);
            n->field.object = left;
            n->field.field_name = tok_text(&p->previous);
            n->field.field_name_len = tok_len(&p->previous);
            left = n;
            continue;
        }

        /* index or slice: expr[...] */
        if (match(p, TOK_LBRACKET)) {
            Node *first = NULL;

            /* check for [..end] */
            if (match(p, TOK_DOTDOT)) {
                Node *n = new_node(p, NODE_SLICE);
                n->slice.object = left;
                n->slice.start = NULL;
                n->slice.end = parse_expression(p);
                consume(p, TOK_RBRACKET, "expected ']' after slice");
                left = n;
                continue;
            }

            first = parse_expression(p);

            /* is this a slice? expr[start..end] or expr[start..] */
            if (match(p, TOK_DOTDOT)) {
                Node *n = new_node(p, NODE_SLICE);
                n->slice.object = left;
                n->slice.start = first;
                if (!check(p, TOK_RBRACKET)) {
                    n->slice.end = parse_expression(p);
                }
                consume(p, TOK_RBRACKET, "expected ']' after slice");
                left = n;
                continue;
            }

            /* plain index: expr[index] */
            Node *n = new_node(p, NODE_INDEX);
            n->index_expr.object = left;
            n->index_expr.index = first;
            consume(p, TOK_RBRACKET, "expected ']' after index");
            left = n;
            continue;
        }

        break;
    }
    return left;
}

/* ---- Precedence climbing for binary operators ---- */

static Node *parse_precedence(Parser *p, Precedence min_prec) {
    Node *left = parse_unary(p);
    left = parse_postfix(p, left);

    for (;;) {
        TokenType op = p->current.type;

        /* orelse is special — handle here */
        if (op == TOK_ORELSE && min_prec <= PREC_ORELSE) {
            advance(p);
            Node *n = new_node(p, NODE_ORELSE);
            n->orelse.expr = left;

            /* orelse return / break / continue */
            if (match(p, TOK_RETURN)) {
                n->orelse.fallback_is_return = true;
                n->orelse.fallback = NULL;
            } else if (match(p, TOK_BREAK)) {
                n->orelse.fallback_is_break = true;
                n->orelse.fallback = NULL;
            } else if (match(p, TOK_CONTINUE)) {
                n->orelse.fallback_is_continue = true;
                n->orelse.fallback = NULL;
            } else if (check(p, TOK_LBRACE)) {
                /* orelse { block } */
                n->orelse.fallback = parse_block(p);
            } else {
                /* orelse value */
                n->orelse.fallback = parse_precedence(p, PREC_ORELSE);
            }
            left = n;
            left = parse_postfix(p, left);
            continue;
        }

        /* assignment operators — right-associative */
        if (is_assign_op(op) && min_prec <= PREC_ASSIGN) {
            advance(p);
            Node *n = new_node(p, NODE_ASSIGN);
            n->assign.op = op;
            n->assign.target = left;
            n->assign.value = parse_precedence(p, PREC_ASSIGN);
            left = n;
            continue;
        }

        /* binary operators */
        Precedence prec = get_precedence(op);
        if (prec == PREC_NONE || prec < min_prec) break;

        advance(p);
        Node *right = parse_precedence(p, prec + 1);
        right = parse_postfix(p, right);

        Node *n = new_node(p, NODE_BINARY);
        n->binary.op = op;
        n->binary.left = left;
        n->binary.right = right;
        left = n;
    }

    return left;
}

static Node *parse_expression(Parser *p) {
    return parse_precedence(p, PREC_ASSIGN);
}

/* ================================================================
 * STATEMENT PARSING
 * ================================================================ */

static Node *parse_block(Parser *p) {
    consume(p, TOK_LBRACE, "expected '{'");
    Node *n = new_node(p, NODE_BLOCK);

    if (++p->depth > 64) {
        error(p, "nesting too deep (limit 64)");
        /* skip to matching '}' */
        int brace_depth = 1;
        while (brace_depth > 0 && !check(p, TOK_EOF)) {
            if (check(p, TOK_LBRACE)) brace_depth++;
            else if (check(p, TOK_RBRACE)) brace_depth--;
            if (brace_depth > 0) advance(p);
        }
        if (check(p, TOK_RBRACE)) advance(p);
        p->depth--;
        return n;
    }

    Node *stack_stmts[32];
    Node **stmts = stack_stmts;
    int cap = 32;
    int count = 0;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF) && !p->oom) {
        if (count >= cap) {
            int new_cap = cap * 2;
            Node **new_stmts = (Node **)parser_alloc(p, new_cap * sizeof(Node *));
            if (!new_stmts) break;
            memcpy(new_stmts, stmts, count * sizeof(Node *));
            stmts = new_stmts;
            cap = new_cap;
        }
        Token before = p->current;
        stmts[count++] = parse_statement(p);
        /* safety: if parse_statement didn't advance, skip token to avoid infinite loop */
        if (p->current.start == before.start && p->current.type == before.type) {
            advance(p);
        }
    }

    consume(p, TOK_RBRACE, "expected '}'");

    p->depth--;
    n->block.stmt_count = count;
    if (count > 0) {
        n->block.stmts = (Node **)arena_alloc(p->arena, count * sizeof(Node *));
        memcpy(n->block.stmts, stmts, count * sizeof(Node *));
    }
    return n;
}

/* parse optional capture: |val| or |*val| */
static void parse_capture(Parser *p, const char **name, size_t *name_len, bool *is_ptr) {
    *name = NULL;
    *name_len = 0;
    *is_ptr = false;

    if (match(p, TOK_PIPE)) {
        *is_ptr = match(p, TOK_STAR);
        consume(p, TOK_IDENT, "expected capture variable name");
        *name = tok_text(&p->previous);
        *name_len = tok_len(&p->previous);
        consume(p, TOK_PIPE, "expected '|' after capture name");
    }
}

/* variable declaration: type name = expr; or type name; */
static Node *parse_var_decl(Parser *p, bool is_const, bool is_static, bool is_volatile) {
    TypeNode *type = parse_type(p);

    /* function pointer local: rettype (*name)(params...) or ?rettype (*name)(params...) */
    if (is_func_ptr_start(p)) {
        advance(p); /* consume '(' */
        advance(p); /* consume '*' */
        {
            /* if type is ?T, unwrap for func ptr return type, wrap result in optional */
            bool is_optional_fptr = (type->kind == TYNODE_OPTIONAL);
            TypeNode *ret_type = is_optional_fptr ? type->optional.inner : type;
            const char *fpname = NULL;
            size_t fpname_len = 0;
            type = parse_func_ptr_after_ret(p, ret_type, &fpname, &fpname_len);
            if (is_optional_fptr) {
                TypeNode *opt = new_type_node(p, TYNODE_OPTIONAL);
                opt->optional.inner = type;
                type = opt;
            }
            Node *n = new_node(p, NODE_VAR_DECL);
            n->var_decl.type = type;
            n->var_decl.name = fpname;
            n->var_decl.name_len = fpname_len;
            n->var_decl.is_const = is_const;
            n->var_decl.is_static = is_static;
            n->var_decl.is_volatile = is_volatile;
            if (match(p, TOK_EQ)) {
                n->var_decl.init = parse_expression(p);
            }
            consume(p, TOK_SEMICOLON, "expected ';' after variable declaration");
            return n;
        }
    }

    consume(p, TOK_IDENT, "expected variable name");

    Node *n = new_node(p, NODE_VAR_DECL);
    n->var_decl.type = type;
    n->var_decl.name = tok_text(&p->previous);
    n->var_decl.name_len = tok_len(&p->previous);
    n->var_decl.is_const = is_const;
    n->var_decl.is_static = is_static;
    n->var_decl.is_volatile = is_volatile;

    if (match(p, TOK_EQ)) {
        n->var_decl.init = parse_expression(p);
    }

    consume(p, TOK_SEMICOLON, "expected ';' after variable declaration");
    return n;
}

static Node *parse_if_stmt(Parser *p) {
    Node *n = new_node(p, NODE_IF);
    consume(p, TOK_LPAREN, "expected '(' after 'if'");
    n->if_stmt.cond = parse_expression(p);
    consume(p, TOK_RPAREN, "expected ')' after if condition");

    /* optional capture: |val| or |*val| */
    parse_capture(p, &n->if_stmt.capture_name,
                  &n->if_stmt.capture_name_len,
                  &n->if_stmt.capture_is_ptr);

    n->if_stmt.then_body = parse_block(p);

    if (match(p, TOK_ELSE)) {
        if (check(p, TOK_IF)) {
            /* else if — parse as nested if */
            advance(p);
            n->if_stmt.else_body = parse_if_stmt(p);
        } else {
            n->if_stmt.else_body = parse_block(p);
        }
    }
    return n;
}

static Node *parse_for_stmt(Parser *p) {
    Node *n = new_node(p, NODE_FOR);
    consume(p, TOK_LPAREN, "expected '(' after 'for'");

    /* init */
    if (!check(p, TOK_SEMICOLON)) {
        /* use same lookahead: try parse type, check if followed by ident */
        bool init_is_var_decl = false;
        if (is_type_token(p->current.type)) {
            Scanner saved_scanner = *p->scanner;
            Token saved_cur = p->current;
            Token saved_prev = p->previous;
            bool saved_error = p->had_error;
            bool saved_panic = p->panic_mode;
            p->had_error = false;
            p->panic_mode = true;
            parse_type(p);
            init_is_var_decl = !p->had_error && check(p, TOK_IDENT);
            *p->scanner = saved_scanner;
            p->current = saved_cur;
            p->previous = saved_prev;
            p->had_error = saved_error;
            p->panic_mode = saved_panic;
        }
        if (init_is_var_decl) {
            n->for_stmt.init = parse_var_decl(p, false, false, false);
        } else {
            n->for_stmt.init = parse_expression(p);
            consume(p, TOK_SEMICOLON, "expected ';' after for init");
        }
    } else {
        consume(p, TOK_SEMICOLON, "expected ';'");
    }

    /* condition */
    if (!check(p, TOK_SEMICOLON)) {
        n->for_stmt.cond = parse_expression(p);
    }
    consume(p, TOK_SEMICOLON, "expected ';' after for condition");

    /* step */
    if (!check(p, TOK_RPAREN)) {
        n->for_stmt.step = parse_expression(p);
    }
    consume(p, TOK_RPAREN, "expected ')' after for clauses");

    n->for_stmt.body = parse_block(p);
    return n;
}

static Node *parse_while_stmt(Parser *p) {
    Node *n = new_node(p, NODE_WHILE);
    consume(p, TOK_LPAREN, "expected '(' after 'while'");
    n->while_stmt.cond = parse_expression(p);
    consume(p, TOK_RPAREN, "expected ')' after while condition");
    n->while_stmt.body = parse_block(p);
    return n;
}

static Node *parse_switch_stmt(Parser *p) {
    Node *n = new_node(p, NODE_SWITCH);
    consume(p, TOK_LPAREN, "expected '(' after 'switch'");
    n->switch_stmt.expr = parse_expression(p);
    consume(p, TOK_RPAREN, "expected ')' after switch expression");
    consume(p, TOK_LBRACE, "expected '{'");

    SwitchArm stack_arms[32];
    SwitchArm *arms = stack_arms;
    int arm_cap = 32;
    int arm_count = 0;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF) && !p->oom) {
        Token before = p->current;
        if (arm_count >= arm_cap) {
            int new_cap = arm_cap * 2;
            SwitchArm *new_arms = (SwitchArm *)parser_alloc(p, new_cap * sizeof(SwitchArm));
            if (!new_arms) break;
            memcpy(new_arms, arms, arm_count * sizeof(SwitchArm));
            arms = new_arms;
            arm_cap = new_cap;
        }
        SwitchArm *arm = &arms[arm_count++];
        memset(arm, 0, sizeof(SwitchArm));
        arm->loc.line = p->current.line;

        if (match(p, TOK_DEFAULT)) {
            arm->is_default = true;
        } else {
            /* parse match values: .variant or value, possibly comma-separated */
            Node *stack_vals[16];
            Node **values = stack_vals;
            int val_cap = 16;
            int val_count = 0;
            do {
                if (val_count >= val_cap) {
                    int new_cap = val_cap * 2;
                    Node **new_vals = (Node **)parser_alloc(p, new_cap * sizeof(Node *));
                    if (!new_vals) break;
                    memcpy(new_vals, values, val_count * sizeof(Node *));
                    values = new_vals;
                    val_cap = new_cap;
                }
                if (match(p, TOK_DOT)) {
                    arm->is_enum_dot = true;
                    consume(p, TOK_IDENT, "expected variant name after '.'");
                    Node *v = new_node(p, NODE_IDENT);
                    v->ident.name = tok_text(&p->previous);
                    v->ident.name_len = tok_len(&p->previous);
                    values[val_count++] = v;
                } else {
                    values[val_count++] = parse_expression(p);
                }
            } while (match(p, TOK_COMMA) && !check(p, TOK_ARROW));

            arm->value_count = val_count;
            arm->values = (Node **)arena_alloc(p->arena, val_count * sizeof(Node *));
            memcpy(arm->values, values, val_count * sizeof(Node *));
        }

        consume(p, TOK_ARROW, "expected '=>' in switch arm");

        /* optional capture */
        parse_capture(p, &arm->capture_name, &arm->capture_name_len, &arm->capture_is_ptr);

        /* arm body — block or single expression + comma */
        if (check(p, TOK_LBRACE)) {
            arm->body = parse_block(p);
            match(p, TOK_COMMA); /* optional trailing comma after block */
        } else {
            Node *expr = parse_expression(p);
            /* wrap in expr_stmt */
            Node *stmt = new_node(p, NODE_EXPR_STMT);
            stmt->expr_stmt.expr = expr;
            arm->body = stmt;
            consume(p, TOK_COMMA, "expected ',' after switch arm expression");
        }
        if (p->current.start == before.start && p->current.type == before.type) {
            advance(p);
        }
    }

    consume(p, TOK_RBRACE, "expected '}'");

    n->switch_stmt.arm_count = arm_count;
    if (arm_count > 0) {
        n->switch_stmt.arms = (SwitchArm *)arena_alloc(p->arena, arm_count * sizeof(SwitchArm));
        memcpy(n->switch_stmt.arms, arms, arm_count * sizeof(SwitchArm));
    }
    return n;
}

static Node *parse_statement(Parser *p) {
    /* block */
    if (check(p, TOK_LBRACE))
        return parse_block(p);

    /* if */
    if (match(p, TOK_IF))
        return parse_if_stmt(p);

    /* for */
    if (match(p, TOK_FOR))
        return parse_for_stmt(p);

    /* while */
    if (match(p, TOK_WHILE))
        return parse_while_stmt(p);

    /* switch */
    if (match(p, TOK_SWITCH))
        return parse_switch_stmt(p);

    /* return */
    if (match(p, TOK_RETURN)) {
        Node *n = new_node(p, NODE_RETURN);
        if (!check(p, TOK_SEMICOLON)) {
            n->ret.expr = parse_expression(p);
        }
        consume(p, TOK_SEMICOLON, "expected ';' after return");
        return n;
    }

    /* break */
    if (match(p, TOK_BREAK)) {
        Node *n = new_node(p, NODE_BREAK);
        consume(p, TOK_SEMICOLON, "expected ';' after break");
        return n;
    }

    /* continue */
    if (match(p, TOK_CONTINUE)) {
        Node *n = new_node(p, NODE_CONTINUE);
        consume(p, TOK_SEMICOLON, "expected ';' after continue");
        return n;
    }

    /* defer */
    if (match(p, TOK_DEFER)) {
        Node *n = new_node(p, NODE_DEFER);
        if (check(p, TOK_LBRACE)) {
            n->defer.body = parse_block(p);
        } else {
            /* defer single_statement; */
            Node *expr = parse_expression(p);
            consume(p, TOK_SEMICOLON, "expected ';' after defer statement");
            Node *stmt = new_node(p, NODE_EXPR_STMT);
            stmt->expr_stmt.expr = expr;
            n->defer.body = stmt;
        }
        return n;
    }

    /* asm */
    if (match(p, TOK_ASM)) {
        consume(p, TOK_LPAREN, "expected '(' after 'asm'");
        consume(p, TOK_STRING, "expected string in asm()");
        Node *n = new_node(p, NODE_ASM);
        n->asm_stmt.code = p->previous.start + 1;
        n->asm_stmt.code_len = p->previous.length - 2;
        consume(p, TOK_RPAREN, "expected ')' after asm string");
        consume(p, TOK_SEMICOLON, "expected ';' after asm");
        return n;
    }

    /* const / static / volatile — modifiers before var decl */
    if (check(p, TOK_CONST) || check(p, TOK_STATIC) || check(p, TOK_VOLATILE)) {
        bool is_const = false, is_static = false, is_volatile = false;
        while (check(p, TOK_CONST) || check(p, TOK_STATIC) || check(p, TOK_VOLATILE)) {
            if (match(p, TOK_CONST)) is_const = true;
            else if (match(p, TOK_STATIC)) is_static = true;
            else if (match(p, TOK_VOLATILE)) is_volatile = true;
        }
        return parse_var_decl(p, is_const, is_static, is_volatile);
    }

    /* Detect variable declaration vs expression statement.
     * Instead of speculatively calling parse_type() (which allocates AST nodes
     * and requires error suppression), use lightweight token scanning.
     *
     * A var decl starts with a type followed by an identifier name:
     *   Type Name ...     where Type can be:
     *     keyword         u32, bool, void, etc. (already handled above for const/static/volatile)
     *     Ident           user-defined type (Task, Config)
     *     Ident[N]        array type (Task[10])
     *     *Type           pointer (*Task, **u32)
     *     ?Type           optional (?u32, ?*Task)
     *     []Type          slice ([]u8)
     *     Type(*name)(...)  function pointer
     *
     * For TOK_IDENT specifically (the ambiguous case), we scan forward:
     *   IDENT IDENT          → var decl (Task t)
     *   IDENT [expr] IDENT   → array var decl (Task[10] t)
     *   IDENT (              → could be func ptr decl or call expr
     *   IDENT anything_else  → expression statement
     *
     * For non-IDENT type tokens (*, ?, []), these unambiguously start types,
     * so we still use the speculative parse for those.
     */
    if (is_type_token(p->current.type)) {
        bool is_var = false;

        if (p->current.type == TOK_IDENT) {
            /* Lightweight lookahead for IDENT-starting statements.
             * No AST allocation, no error suppression needed. */
            Scanner saved_scanner = *p->scanner;
            Token saved_cur = p->current;
            Token saved_prev = p->previous;

            advance(p); /* consume the type name */

            if (check(p, TOK_IDENT)) {
                /* IDENT IDENT → definitely a var decl (Task t) */
                is_var = true;
            } else if (check(p, TOK_LBRACKET)) {
                /* IDENT [ — could be array type (Task[10] t) or index (arr[0]).
                 * Scan past balanced brackets (and multi-dim [N][M]), check if followed by IDENT. */
                do {
                    advance(p); /* consume [ */
                    int depth = 1;
                    while (depth > 0 && !check(p, TOK_EOF)) {
                        if (check(p, TOK_LBRACKET)) depth++;
                        else if (check(p, TOK_RBRACKET)) depth--;
                        advance(p);
                    }
                } while (check(p, TOK_LBRACKET)); /* multi-dim: scan next [M] */
                /* after all ] — if IDENT follows, it's an array var decl */
                is_var = check(p, TOK_IDENT);
            } else if (check(p, TOK_LPAREN)) {
                /* IDENT ( — could be func ptr type or function call.
                 * Peek: ( * means function pointer declaration. */
                Scanner saved2 = *p->scanner;
                Token saved2_cur = p->current;
                advance(p); /* consume ( */
                is_var = check(p, TOK_STAR);
                *p->scanner = saved2;
                p->current = saved2_cur;
            }

            /* restore scanner state */
            *p->scanner = saved_scanner;
            p->current = saved_cur;
            p->previous = saved_prev;
        } else {
            /* Non-IDENT type token (*, ?, [], keyword) — unambiguously a type.
             * Use speculative parse for complex types like ?*Task or []u8. */
            Scanner saved_scanner = *p->scanner;
            Token saved_cur = p->current;
            Token saved_prev = p->previous;
            bool saved_error = p->had_error;
            bool saved_panic = p->panic_mode;

            p->had_error = false;
            p->panic_mode = true;

            TypeNode *try_type = parse_type(p);
            (void)try_type;

            bool is_func_ptr = false;
            if (!p->had_error && check(p, TOK_LPAREN)) {
                Scanner saved2 = *p->scanner;
                Token saved2_cur = p->current;
                advance(p);
                is_func_ptr = check(p, TOK_STAR);
                *p->scanner = saved2;
                p->current = saved2_cur;
            }
            is_var = !p->had_error && (check(p, TOK_IDENT) || is_func_ptr);

            *p->scanner = saved_scanner;
            p->current = saved_cur;
            p->previous = saved_prev;
            p->had_error = saved_error;
            p->panic_mode = saved_panic;
        }

        if (is_var) {
            return parse_var_decl(p, false, false, false);
        }
    }

    /* expression statement */
    Node *expr = parse_expression(p);

    /* check if this was actually a var decl: Type name ... */
    /* this handles: Task t; or ?*Task maybe; */
    /* TODO: improve heuristic for user-defined type names */

    consume(p, TOK_SEMICOLON, "expected ';' after expression");
    Node *n = new_node(p, NODE_EXPR_STMT);
    n->expr_stmt.expr = expr;
    return n;
}

/* ================================================================
 * TOP-LEVEL DECLARATION PARSING
 * ================================================================ */

static Node *parse_struct_decl(Parser *p, bool is_packed) {
    consume(p, TOK_IDENT, "expected struct name");
    Node *n = new_node(p, NODE_STRUCT_DECL);
    n->struct_decl.name = tok_text(&p->previous);
    n->struct_decl.name_len = tok_len(&p->previous);
    n->struct_decl.is_packed = is_packed;

    consume(p, TOK_LBRACE, "expected '{' after struct name");

    FieldDecl stack_fields[32];
    FieldDecl *fields = stack_fields;
    int field_cap = 32;
    int count = 0;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF) && !p->oom) {
        Token before = p->current;
        if (count >= field_cap) {
            int new_cap = field_cap * 2;
            FieldDecl *new_fields = (FieldDecl *)parser_alloc(p, new_cap * sizeof(FieldDecl));
            if (!new_fields) break;
            memcpy(new_fields, fields, count * sizeof(FieldDecl));
            fields = new_fields;
            field_cap = new_cap;
        }
        FieldDecl *f = &fields[count++];
        memset(f, 0, sizeof(FieldDecl));
        f->loc.line = p->current.line;

        f->is_keep = match(p, TOK_KEEP);
        TypeNode *type = parse_type(p);

        /* function pointer field: rettype (*name)(params...) or ?rettype (*name)(params...) */
        if (is_func_ptr_start(p)) {
            advance(p); /* consume '(' */
            advance(p); /* consume '*' */
            bool is_opt_fp = (type->kind == TYNODE_OPTIONAL);
            TypeNode *fp_ret = is_opt_fp ? type->optional.inner : type;
            const char *fname = NULL;
            size_t fname_len = 0;
            type = parse_func_ptr_after_ret(p, fp_ret, &fname, &fname_len);
            if (is_opt_fp) {
                TypeNode *opt = new_type_node(p, TYNODE_OPTIONAL);
                opt->optional.inner = type;
                type = opt;
            }
            f->type = type;
            f->name = fname;
            f->name_len = fname_len;
            if (!fname) error(p, "expected name in function pointer field");
            consume(p, TOK_SEMICOLON, "expected ';' after field");
            continue;
        }

        f->type = type;
        consume(p, TOK_IDENT, "expected field name");
        f->name = tok_text(&p->previous);
        f->name_len = tok_len(&p->previous);
        consume(p, TOK_SEMICOLON, "expected ';' after field");
        if (p->current.start == before.start && p->current.type == before.type) {
            advance(p);
        }
    }

    consume(p, TOK_RBRACE, "expected '}' after struct body");

    n->struct_decl.field_count = count;
    if (count > 0) {
        n->struct_decl.fields = (FieldDecl *)arena_alloc(p->arena, count * sizeof(FieldDecl));
        memcpy(n->struct_decl.fields, fields, count * sizeof(FieldDecl));
    }
    return n;
}

static Node *parse_enum_decl(Parser *p) {
    consume(p, TOK_IDENT, "expected enum name");
    Node *n = new_node(p, NODE_ENUM_DECL);
    n->enum_decl.name = tok_text(&p->previous);
    n->enum_decl.name_len = tok_len(&p->previous);

    consume(p, TOK_LBRACE, "expected '{' after enum name");

    EnumVariant stack_variants[64];
    EnumVariant *variants = stack_variants;
    int var_cap = 64;
    int count = 0;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF) && !p->oom) {
        if (count >= var_cap) {
            int new_cap = var_cap * 2;
            EnumVariant *new_v = (EnumVariant *)parser_alloc(p, new_cap * sizeof(EnumVariant));
            if (!new_v) break;
            memcpy(new_v, variants, count * sizeof(EnumVariant));
            variants = new_v;
            var_cap = new_cap;
        }
        Token before = p->current;
        EnumVariant *v = &variants[count++];
        memset(v, 0, sizeof(EnumVariant));
        v->loc.line = p->current.line;

        consume(p, TOK_IDENT, "expected variant name");
        v->name = tok_text(&p->previous);
        v->name_len = tok_len(&p->previous);

        if (match(p, TOK_EQ)) {
            v->value = parse_expression(p);
        }

        match(p, TOK_COMMA); /* optional comma */
        if (p->current.start == before.start && p->current.type == before.type) {
            advance(p); /* skip stuck token */
        }
    }

    consume(p, TOK_RBRACE, "expected '}' after enum body");

    n->enum_decl.variant_count = count;
    if (count > 0) {
        n->enum_decl.variants = (EnumVariant *)arena_alloc(p->arena, count * sizeof(EnumVariant));
        memcpy(n->enum_decl.variants, variants, count * sizeof(EnumVariant));
    }
    return n;
}

static Node *parse_union_decl(Parser *p) {
    consume(p, TOK_IDENT, "expected union name");
    Node *n = new_node(p, NODE_UNION_DECL);
    n->union_decl.name = tok_text(&p->previous);
    n->union_decl.name_len = tok_len(&p->previous);

    consume(p, TOK_LBRACE, "expected '{' after union name");

    UnionVariant stack_uvariants[32];
    UnionVariant *variants = stack_uvariants;
    int uvar_cap = 32;
    int count = 0;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF) && !p->oom) {
        Token before = p->current;
        if (count >= uvar_cap) {
            int new_cap = uvar_cap * 2;
            UnionVariant *new_v = (UnionVariant *)parser_alloc(p, new_cap * sizeof(UnionVariant));
            if (!new_v) break;
            memcpy(new_v, variants, count * sizeof(UnionVariant));
            variants = new_v;
            uvar_cap = new_cap;
        }
        UnionVariant *v = &variants[count++];
        memset(v, 0, sizeof(UnionVariant));
        v->loc.line = p->current.line;

        v->type = parse_type(p);
        consume(p, TOK_IDENT, "expected variant name");
        v->name = tok_text(&p->previous);
        v->name_len = tok_len(&p->previous);
        consume(p, TOK_SEMICOLON, "expected ';' after union variant");
        if (p->current.start == before.start && p->current.type == before.type) {
            advance(p);
        }
    }

    consume(p, TOK_RBRACE, "expected '}' after union body");

    n->union_decl.variant_count = count;
    if (count > 0) {
        n->union_decl.variants = (UnionVariant *)arena_alloc(p->arena, count * sizeof(UnionVariant));
        memcpy(n->union_decl.variants, variants, count * sizeof(UnionVariant));
    }
    return n;
}

/* parse function or global variable declaration */
static Node *parse_func_or_var(Parser *p, bool is_static) {
    TypeNode *type = parse_type(p);

    /* function pointer declaration: type (*name)(params...) or ?type (*name)(params...) */
    if (is_func_ptr_start(p)) {
        advance(p); /* consume '(' */
        advance(p); /* consume '*' */
        {
            bool is_optional_fptr = (type->kind == TYNODE_OPTIONAL);
            TypeNode *ret_type = is_optional_fptr ? type->optional.inner : type;
            const char *fpname = NULL;
            size_t fpname_len = 0;
            TypeNode *fp_type = parse_func_ptr_after_ret(p, ret_type, &fpname, &fpname_len);
            if (is_optional_fptr) {
                TypeNode *opt = new_type_node(p, TYNODE_OPTIONAL);
                opt->optional.inner = fp_type;
                fp_type = opt;
            }
            if (!fpname) {
                error(p, "expected name in function pointer declaration");
                return new_node(p, NODE_VAR_DECL);
            }
            Node *n = new_node(p, NODE_GLOBAL_VAR);
            n->var_decl.type = fp_type;
            n->var_decl.name = fpname;
            n->var_decl.name_len = fpname_len;
            n->var_decl.is_static = is_static;
            if (match(p, TOK_EQ)) {
                n->var_decl.init = parse_expression(p);
            }
            consume(p, TOK_SEMICOLON, "expected ';' after function pointer declaration");
            return n;
        }
    }

    consume(p, TOK_IDENT, "expected name");
    const char *name = tok_text(&p->previous);
    size_t name_len = tok_len(&p->previous);

    /* function declaration: type name(...) { ... } */
    if (match(p, TOK_LPAREN)) {
        Node *n = new_node(p, NODE_FUNC_DECL);
        n->func_decl.return_type = type;
        n->func_decl.name = name;
        n->func_decl.name_len = name_len;
        n->func_decl.is_static = is_static;

        ParamDecl stack_params[16];
        ParamDecl *params = stack_params;
        int param_cap = 16;
        int param_count = 0;

        if (!check(p, TOK_RPAREN)) {
            do {
                if (param_count >= param_cap) {
                    int new_cap = param_cap * 2;
                    ParamDecl *new_p = (ParamDecl *)parser_alloc(p, new_cap * sizeof(ParamDecl));
                    if (!new_p) break;
                    memcpy(new_p, params, param_count * sizeof(ParamDecl));
                    params = new_p;
                    param_cap = new_cap;
                }
                ParamDecl *param = &params[param_count++];
                memset(param, 0, sizeof(ParamDecl));
                param->loc.line = p->current.line;
                param->is_keep = match(p, TOK_KEEP);
                param->type = parse_type(p);

                /* function pointer parameter: rettype (*name)(params...) or ?rettype (*name)(params...) */
                if (is_func_ptr_start(p)) {
                    advance(p); /* consume '(' */
                    advance(p); /* consume '*' */
                    bool is_opt_fp = (param->type->kind == TYNODE_OPTIONAL);
                    TypeNode *fp_ret = is_opt_fp ? param->type->optional.inner : param->type;
                    const char *fpname = NULL;
                    size_t fpname_len = 0;
                    param->type = parse_func_ptr_after_ret(p, fp_ret, &fpname, &fpname_len);
                    if (is_opt_fp) {
                        TypeNode *opt = new_type_node(p, TYNODE_OPTIONAL);
                        opt->optional.inner = param->type;
                        param->type = opt;
                    }
                    param->name = fpname;
                    param->name_len = fpname_len;
                    if (!fpname) error(p, "expected name in function pointer parameter");
                    goto param_done;
                }

                consume(p, TOK_IDENT, "expected parameter name");
                param->name = tok_text(&p->previous);
                param->name_len = tok_len(&p->previous);
                param_done:;
            } while (match(p, TOK_COMMA));
        }

        consume(p, TOK_RPAREN, "expected ')' after parameters");

        n->func_decl.param_count = param_count;
        if (param_count > 0) {
            n->func_decl.params = (ParamDecl *)arena_alloc(p->arena, param_count * sizeof(ParamDecl));
            memcpy(n->func_decl.params, params, param_count * sizeof(ParamDecl));
        }

        /* forward declaration: u32 func(u32 n); — no body */
        if (match(p, TOK_SEMICOLON)) {
            n->func_decl.body = NULL;
            return n;
        }
        n->func_decl.body = parse_block(p);
        return n;
    }

    /* global variable: type name = expr; or type name; */
    Node *n = new_node(p, NODE_GLOBAL_VAR);
    n->var_decl.type = type;
    n->var_decl.name = name;
    n->var_decl.name_len = name_len;
    n->var_decl.is_static = is_static;

    if (match(p, TOK_EQ)) {
        n->var_decl.init = parse_expression(p);
    }

    consume(p, TOK_SEMICOLON, "expected ';' after declaration");
    return n;
}

static Node *parse_declaration(Parser *p) {
    /* import */
    if (match(p, TOK_IMPORT)) {
        consume(p, TOK_IDENT, "expected module name after 'import'");
        Node *n = new_node(p, NODE_IMPORT);
        n->import.module_name = tok_text(&p->previous);
        n->import.module_name_len = tok_len(&p->previous);
        consume(p, TOK_SEMICOLON, "expected ';' after import");
        return n;
    }

    /* cinclude "header.h"; */
    if (match(p, TOK_CINCLUDE)) {
        consume(p, TOK_STRING, "expected string path after 'cinclude'");
        Node *n = new_node(p, NODE_CINCLUDE);
        /* strip surrounding quotes from string token */
        n->cinclude.path = tok_text(&p->previous) + 1;
        n->cinclude.path_len = tok_len(&p->previous) - 2;
        consume(p, TOK_SEMICOLON, "expected ';' after cinclude");
        return n;
    }

    /* struct / packed struct */
    if (match(p, TOK_PACKED)) {
        consume(p, TOK_STRUCT, "expected 'struct' after 'packed'");
        return parse_struct_decl(p, true);
    }
    if (match(p, TOK_STRUCT)) {
        return parse_struct_decl(p, false);
    }

    /* enum */
    if (match(p, TOK_ENUM))
        return parse_enum_decl(p);

    /* union */
    if (match(p, TOK_UNION))
        return parse_union_decl(p);

    /* typedef / distinct typedef */
    if (match(p, TOK_DISTINCT)) {
        consume(p, TOK_TYPEDEF, "expected 'typedef' after 'distinct'");
        TypeNode *type = parse_type(p);
        /* function pointer distinct typedef: distinct typedef rettype (*Name)(params); */
        if (is_func_ptr_start(p)) {
            advance(p); /* consume '(' */
            advance(p); /* consume '*' */
            bool is_opt_fp = (type->kind == TYNODE_OPTIONAL);
            TypeNode *fp_ret = is_opt_fp ? type->optional.inner : type;
            const char *fpname = NULL;
            size_t fpname_len = 0;
            type = parse_func_ptr_after_ret(p, fp_ret, &fpname, &fpname_len);
            if (is_opt_fp) {
                TypeNode *opt = new_type_node(p, TYNODE_OPTIONAL);
                opt->optional.inner = type;
                type = opt;
            }
            Node *n = new_node(p, NODE_TYPEDEF);
            n->typedef_decl.type = type;
            n->typedef_decl.name = fpname;
            n->typedef_decl.name_len = fpname_len;
            n->typedef_decl.is_distinct = true;
            consume(p, TOK_SEMICOLON, "expected ';' after typedef");
            return n;
        }
        consume(p, TOK_IDENT, "expected type alias name");
        Node *n = new_node(p, NODE_TYPEDEF);
        n->typedef_decl.type = type;
        n->typedef_decl.name = tok_text(&p->previous);
        n->typedef_decl.name_len = tok_len(&p->previous);
        n->typedef_decl.is_distinct = true;
        consume(p, TOK_SEMICOLON, "expected ';' after typedef");
        return n;
    }
    if (match(p, TOK_TYPEDEF)) {
        TypeNode *type = parse_type(p);
        /* function pointer typedef: typedef rettype (*Name)(params); */
        if (is_func_ptr_start(p)) {
            advance(p); /* consume '(' */
            advance(p); /* consume '*' */
            bool is_opt_fp = (type->kind == TYNODE_OPTIONAL);
            TypeNode *fp_ret = is_opt_fp ? type->optional.inner : type;
            const char *fpname = NULL;
            size_t fpname_len = 0;
            type = parse_func_ptr_after_ret(p, fp_ret, &fpname, &fpname_len);
            if (is_opt_fp) {
                TypeNode *opt = new_type_node(p, TYNODE_OPTIONAL);
                opt->optional.inner = type;
                type = opt;
            }
            Node *n = new_node(p, NODE_TYPEDEF);
            n->typedef_decl.type = type;
            n->typedef_decl.name = fpname;
            n->typedef_decl.name_len = fpname_len;
            n->typedef_decl.is_distinct = false;
            consume(p, TOK_SEMICOLON, "expected ';' after typedef");
            return n;
        }
        consume(p, TOK_IDENT, "expected type alias name");
        Node *n = new_node(p, NODE_TYPEDEF);
        n->typedef_decl.type = type;
        n->typedef_decl.name = tok_text(&p->previous);
        n->typedef_decl.name_len = tok_len(&p->previous);
        n->typedef_decl.is_distinct = false;
        consume(p, TOK_SEMICOLON, "expected ';' after typedef");
        return n;
    }

    /* mmio range declaration: mmio 0x40020000..0x40020FFF; */
    if (match(p, TOK_MMIO)) {
        consume(p, TOK_NUMBER_INT, "expected start address after 'mmio'");
        Node *n = new_node(p, NODE_MMIO);
        /* parse start address from previous token */
        {
            const char *text = p->previous.start;
            size_t len = p->previous.length;
            uint64_t val = 0;
            if (len > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
                for (size_t i = 2; i < len; i++) {
                    if (text[i] == '_') continue;
                    val *= 16;
                    if (text[i] >= '0' && text[i] <= '9') val += text[i] - '0';
                    else if (text[i] >= 'a' && text[i] <= 'f') val += text[i] - 'a' + 10;
                    else if (text[i] >= 'A' && text[i] <= 'F') val += text[i] - 'A' + 10;
                }
            } else {
                for (size_t i = 0; i < len; i++) {
                    if (text[i] == '_') continue;
                    val = val * 10 + (text[i] - '0');
                }
            }
            n->mmio_decl.range_start = val;
        }
        consume(p, TOK_DOTDOT, "expected '..' in mmio range");
        consume(p, TOK_NUMBER_INT, "expected end address in mmio range");
        /* parse end address */
        {
            const char *text = p->previous.start;
            size_t len = p->previous.length;
            uint64_t val = 0;
            if (len > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
                for (size_t i = 2; i < len; i++) {
                    if (text[i] == '_') continue;
                    val *= 16;
                    if (text[i] >= '0' && text[i] <= '9') val += text[i] - '0';
                    else if (text[i] >= 'a' && text[i] <= 'f') val += text[i] - 'a' + 10;
                    else if (text[i] >= 'A' && text[i] <= 'F') val += text[i] - 'A' + 10;
                }
            } else {
                for (size_t i = 0; i < len; i++) {
                    if (text[i] == '_') continue;
                    val = val * 10 + (text[i] - '0');
                }
            }
            n->mmio_decl.range_end = val;
        }
        consume(p, TOK_SEMICOLON, "expected ';' after mmio declaration");
        return n;
    }

    /* interrupt */
    if (match(p, TOK_INTERRUPT)) {
        consume(p, TOK_IDENT, "expected interrupt name");
        Node *n = new_node(p, NODE_INTERRUPT);
        n->interrupt.name = tok_text(&p->previous);
        n->interrupt.name_len = tok_len(&p->previous);

        /* optional: as "handler_name" */
        if (match(p, TOK_AS)) {
            consume(p, TOK_STRING, "expected string after 'as'");
            n->interrupt.as_name = p->previous.start + 1;
            n->interrupt.as_name_len = p->previous.length - 2;
        }

        n->interrupt.body = parse_block(p);
        return n;
    }

    /* comptime — compile-time evaluated function */
    if (match(p, TOK_COMPTIME)) {
        Node *n = parse_func_or_var(p, false);
        if (n->kind != NODE_FUNC_DECL) {
            error_at(p, &p->previous, "comptime can only be applied to functions");
        } else {
            n->func_decl.is_comptime = true;
        }
        return n;
    }

    /* static — modifier for function or variable */
    if (match(p, TOK_STATIC)) {
        return parse_func_or_var(p, true);
    }

    /* const global variable or function with const return type */
    if (check(p, TOK_CONST)) {
        /* peek ahead: const TYPE NAME '(' → function with const return type */
        Scanner saved_sc = *p->scanner;
        Token saved_cur = p->current;
        Token saved_prev = p->previous;
        advance(p); /* consume const */
        int depth = 0;
        bool found_func = false;
        while (!check(p, TOK_EOF)) {
            if (check(p, TOK_LBRACKET)) { depth += 1; advance(p); continue; }
            if (check(p, TOK_RBRACKET)) { if (depth > 0) depth -= 1; advance(p); continue; }
            if (depth == 0 && check(p, TOK_IDENT)) {
                advance(p);
                found_func = check(p, TOK_LPAREN);
                break;
            }
            if (check(p, TOK_STAR) || check(p, TOK_QUESTION)) { advance(p); continue; }
            if (p->current.type >= TOK_U8 && p->current.type <= TOK_VOID) { advance(p); continue; }
            break;
        }
        *p->scanner = saved_sc;
        p->current = saved_cur;
        p->previous = saved_prev;
        if (found_func) {
            return parse_func_or_var(p, false);
        } else {
            advance(p); /* re-consume const */
            Node *n = parse_var_decl(p, true, false, false);
            n->kind = NODE_GLOBAL_VAR;
            return n;
        }
    }

    /* volatile global variable or function with volatile return type */
    if (check(p, TOK_VOLATILE)) {
        /* peek ahead: volatile TYPE NAME '(' → function with volatile return type.
         * Otherwise → volatile variable declaration. */
        Scanner saved_sc = *p->scanner;
        Token saved_cur = p->current;
        Token saved_prev = p->previous;
        advance(p); /* consume volatile */
        /* skip the type tokens */
        int depth = 0;
        while (!check(p, TOK_EOF)) {
            if (check(p, TOK_LBRACKET)) { depth += 1; advance(p); continue; }
            if (check(p, TOK_RBRACKET)) { if (depth > 0) depth -= 1; advance(p); continue; }
            if (depth == 0 && check(p, TOK_IDENT)) {
                advance(p); /* consume the name */
                bool is_func = check(p, TOK_LPAREN);
                /* restore scanner */
                *p->scanner = saved_sc;
                p->current = saved_cur;
                p->previous = saved_prev;
                if (is_func) {
                    return parse_func_or_var(p, false);
                } else {
                    advance(p); /* re-consume volatile */
                    Node *n = parse_var_decl(p, false, false, true);
                    n->kind = NODE_GLOBAL_VAR;
                    return n;
                }
            }
            if (check(p, TOK_STAR) || check(p, TOK_QUESTION)) {
                advance(p); continue;
            }
            /* type keyword — skip */
            if (p->current.type >= TOK_U8 && p->current.type <= TOK_VOID) {
                advance(p); continue;
            }
            break;
        }
        /* fallback: restore and treat as var decl */
        *p->scanner = saved_sc;
        p->current = saved_cur;
        p->previous = saved_prev;
        advance(p); /* re-consume volatile */
        Node *n = parse_var_decl(p, false, false, true);
        n->kind = NODE_GLOBAL_VAR;
        return n;
    }

    /* function or global variable */
    return parse_func_or_var(p, false);
}

/* ================================================================
 * ENTRY POINT — parse entire file
 * ================================================================ */

void parser_init(Parser *p, Scanner *scanner, Arena *arena, const char *file_name) {
    p->scanner = scanner;
    p->arena = arena;
    p->had_error = false;
    p->panic_mode = false;
    p->oom = false;
    p->file_name = file_name;
    p->depth = 0;
    advance(p); /* prime the first token */
}

Node *parse_file(Parser *p) {
    Node *file_node = new_node(p, NODE_FILE);

    Node *stack_decls[64];
    Node **decls = stack_decls;
    int cap = 64;
    int count = 0;

    while (!check(p, TOK_EOF) && !p->oom) {
        if (count >= cap) {
            int new_cap = cap * 2;
            Node **new_decls = (Node **)parser_alloc(p, new_cap * sizeof(Node *));
            if (!new_decls) break;
            memcpy(new_decls, decls, count * sizeof(Node *));
            decls = new_decls;
            cap = new_cap;
        }
        p->panic_mode = false;
        Token before = p->current;
        decls[count++] = parse_declaration(p);
        if (p->current.start == before.start && p->current.type == before.type) {
            advance(p);
        }
    }

    file_node->file.decl_count = count;
    if (count > 0) {
        file_node->file.decls = (Node **)arena_alloc(p->arena, count * sizeof(Node *));
        memcpy(file_node->file.decls, decls, count * sizeof(Node *));
    }
    return file_node;
}
