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
    n->kind = kind;
    n->loc.line = p->previous.line;
    n->loc.file = p->file_name;
    return n;
}

static TypeNode *new_type_node(Parser *p, TypeNodeKind kind) {
    TypeNode *t = (TypeNode *)arena_alloc(p->arena, sizeof(TypeNode));
    t->kind = kind;
    t->loc.line = p->previous.line;
    t->loc.file = p->file_name;
    return t;
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
    case TOK_POOL: case TOK_RING: case TOK_ARENA: case TOK_HANDLE:
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

    /* check for array suffix: T[N] */
    if (match(p, TOK_LBRACKET)) {
        TypeNode *arr = new_type_node(p, TYNODE_ARRAY);
        arr->array.elem = base;
        arr->array.size_expr = parse_expression(p);
        consume(p, TOK_RBRACKET, "expected ']' after array size");
        return arr;
    }

    return base;
}

/* ================================================================
 * EXPRESSION PARSING — Pratt parser (precedence climbing)
 * ================================================================ */

typedef enum {
    PREC_NONE,
    PREC_ORELSE,        /* orelse */
    PREC_ASSIGN,        /* = += -= etc. */
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
        if (is_type_token(p->current.type) && p->current.type != TOK_IDENT) {
            n->intrinsic.type_arg = parse_type(p);
            if (match(p, TOK_COMMA)) {
                /* more args after type */
            }
        }

        /* parse expression arguments */
        Node *args[16];
        int arg_count = 0;
        if (!check(p, TOK_RPAREN)) {
            do {
                if (arg_count >= 16) {
                    error(p, "too many intrinsic arguments");
                    break;
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
            Node *args[64];
            int arg_count = 0;
            if (!check(p, TOK_RPAREN)) {
                do {
                    if (arg_count >= 64) {
                        error(p, "too many function arguments");
                        break;
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
    return parse_precedence(p, PREC_ORELSE);
}

/* ================================================================
 * STATEMENT PARSING
 * ================================================================ */

static Node *parse_block(Parser *p) {
    consume(p, TOK_LBRACE, "expected '{'");
    Node *n = new_node(p, NODE_BLOCK);

    Node *stmts[1024];
    int count = 0;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        if (count >= 1024) {
            error(p, "too many statements in block");
            break;
        }
        stmts[count++] = parse_statement(p);
    }

    consume(p, TOK_RBRACE, "expected '}'");

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

    SwitchArm arms[128];
    int arm_count = 0;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        if (arm_count >= 128) {
            error(p, "too many switch arms");
            break;
        }
        SwitchArm *arm = &arms[arm_count++];
        memset(arm, 0, sizeof(SwitchArm));
        arm->loc.line = p->current.line;

        if (match(p, TOK_DEFAULT)) {
            arm->is_default = true;
        } else {
            /* parse match values: .variant or value, possibly comma-separated */
            Node *values[16];
            int val_count = 0;
            do {
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
     * Strategy: save scanner state, try to parse a type, check if
     * followed by an identifier (which means var decl). Restore if not. */
    if (is_type_token(p->current.type)) {
        Scanner saved_scanner = *p->scanner;
        Token saved_cur = p->current;
        Token saved_prev = p->previous;
        bool saved_error = p->had_error;
        bool saved_panic = p->panic_mode;

        /* suppress errors during lookahead */
        p->had_error = false;
        p->panic_mode = true;

        /* try to parse a type */
        TypeNode *try_type = parse_type(p);
        (void)try_type;

        /* if next token is an identifier, this is a var decl */
        bool is_var_decl = !p->had_error && check(p, TOK_IDENT);

        /* restore scanner state completely */
        *p->scanner = saved_scanner;
        p->current = saved_cur;
        p->previous = saved_prev;
        p->had_error = saved_error;
        p->panic_mode = saved_panic;

        if (is_var_decl) {
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

    FieldDecl fields[128];
    int count = 0;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        if (count >= 128) {
            error(p, "too many struct fields");
            break;
        }
        FieldDecl *f = &fields[count++];
        memset(f, 0, sizeof(FieldDecl));
        f->loc.line = p->current.line;

        f->is_keep = match(p, TOK_KEEP);
        f->type = parse_type(p);
        consume(p, TOK_IDENT, "expected field name");
        f->name = tok_text(&p->previous);
        f->name_len = tok_len(&p->previous);
        consume(p, TOK_SEMICOLON, "expected ';' after field");
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

    EnumVariant variants[256];
    int count = 0;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        if (count >= 256) {
            error(p, "too many enum variants");
            break;
        }
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

    UnionVariant variants[128];
    int count = 0;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        if (count >= 128) {
            error(p, "too many union variants");
            break;
        }
        UnionVariant *v = &variants[count++];
        memset(v, 0, sizeof(UnionVariant));
        v->loc.line = p->current.line;

        v->type = parse_type(p);
        consume(p, TOK_IDENT, "expected variant name");
        v->name = tok_text(&p->previous);
        v->name_len = tok_len(&p->previous);
        consume(p, TOK_SEMICOLON, "expected ';' after union variant");
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

    /* check for function pointer: type (*name)(params) */
    /* TODO: function pointer declarations */

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

        ParamDecl params[32];
        int param_count = 0;

        if (!check(p, TOK_RPAREN)) {
            do {
                if (param_count >= 32) {
                    error(p, "too many function parameters");
                    break;
                }
                ParamDecl *param = &params[param_count++];
                memset(param, 0, sizeof(ParamDecl));
                param->loc.line = p->current.line;
                param->is_keep = match(p, TOK_KEEP);
                param->type = parse_type(p);
                consume(p, TOK_IDENT, "expected parameter name");
                param->name = tok_text(&p->previous);
                param->name_len = tok_len(&p->previous);
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
        consume(p, TOK_IDENT, "expected type alias name");
        Node *n = new_node(p, NODE_TYPEDEF);
        n->typedef_decl.type = type;
        n->typedef_decl.name = tok_text(&p->previous);
        n->typedef_decl.name_len = tok_len(&p->previous);
        n->typedef_decl.is_distinct = false;
        consume(p, TOK_SEMICOLON, "expected ';' after typedef");
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

    /* static — modifier for function or variable */
    if (match(p, TOK_STATIC)) {
        return parse_func_or_var(p, true);
    }

    /* const global variable */
    if (match(p, TOK_CONST)) {
        Node *n = parse_var_decl(p, true, false, false);
        n->kind = NODE_GLOBAL_VAR;
        return n;
    }

    /* volatile global variable */
    if (match(p, TOK_VOLATILE)) {
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
    p->file_name = file_name;
    advance(p); /* prime the first token */
}

Node *parse_file(Parser *p) {
    Node *file_node = new_node(p, NODE_FILE);

    Node *decls[1024];
    int count = 0;

    while (!check(p, TOK_EOF)) {
        if (count >= 1024) {
            error(p, "too many top-level declarations");
            break;
        }
        p->panic_mode = false;
        decls[count++] = parse_declaration(p);
    }

    file_node->file.decl_count = count;
    if (count > 0) {
        file_node->file.decls = (Node **)arena_alloc(p->arena, count * sizeof(Node *));
        memcpy(file_node->file.decls, decls, count * sizeof(Node *));
    }
    return file_node;
}
