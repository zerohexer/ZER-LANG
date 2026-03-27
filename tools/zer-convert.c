/*
 * zer-convert — Automated C to ZER syntax transformer (Phase 1)
 *
 * Reads a C source file and emits equivalent ZER code.
 * Uses compat.zer for unsafe patterns (malloc, pointer arithmetic).
 * Token-level transform — no full C AST needed.
 *
 * Usage: zer-convert input.c [-o output.zer]
 *
 * Build: gcc -std=c99 -O2 -o zer-convert tools/zer-convert.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

/* ================================================================
 * Simple C tokenizer — just enough to transform C to ZER
 * ================================================================ */

typedef enum {
    CT_EOF, CT_IDENT, CT_NUMBER, CT_STRING, CT_CHAR_LIT,
    CT_LPAREN, CT_RPAREN, CT_LBRACE, CT_RBRACE,
    CT_LBRACKET, CT_RBRACKET, CT_SEMICOLON, CT_COMMA,
    CT_DOT, CT_ARROW, CT_STAR, CT_AMP, CT_PLUS, CT_MINUS,
    CT_SLASH, CT_PERCENT, CT_TILDE, CT_BANG, CT_QUESTION,
    CT_COLON, CT_EQ, CT_EQEQ, CT_BANGEQ, CT_LT, CT_GT,
    CT_LTEQ, CT_GTEQ, CT_PLUSPLUS, CT_MINUSMINUS,
    CT_PLUSEQ, CT_MINUSEQ, CT_STAREQ, CT_SLASHEQ, CT_PERCENTEQ,
    CT_AMPEQ, CT_PIPEEQ, CT_CARETEQ, CT_LTLTEQ, CT_GTGTEQ,
    CT_AMPAMP, CT_PIPEPIPE, CT_CARET, CT_PIPE,
    CT_LTLT, CT_GTGT, CT_HASH, CT_NEWLINE,
    CT_WHITESPACE, CT_COMMENT,
    CT_UNKNOWN,
} CTokenType;

typedef struct {
    CTokenType type;
    const char *start;
    int len;
    int line;
} CToken;

typedef struct {
    const char *src;
    int pos;
    int len;
    int line;
} CScanner;

static void cs_init(CScanner *s, const char *src, int len) {
    s->src = src; s->pos = 0; s->len = len; s->line = 1;
}

static char cs_peek(CScanner *s) {
    return s->pos < s->len ? s->src[s->pos] : '\0';
}

static char cs_advance(CScanner *s) {
    char c = s->src[s->pos++];
    if (c == '\n') s->line++;
    return c;
}

static bool cs_match(CScanner *s, char c) {
    if (s->pos < s->len && s->src[s->pos] == c) {
        cs_advance(s);
        return true;
    }
    return false;
}

static CToken cs_make(CScanner *s, CTokenType type, const char *start, int len, int line) {
    return (CToken){ type, start, len, line };
}

static CToken cs_next(CScanner *s) {
    /* emit whitespace as token (preserves formatting) */
    if (s->pos < s->len) {
        char c = s->src[s->pos];
        if (c == ' ' || c == '\t' || c == '\r') {
            const char *start = s->src + s->pos;
            while (s->pos < s->len && (s->src[s->pos] == ' ' || s->src[s->pos] == '\t' || s->src[s->pos] == '\r'))
                s->pos++;
            return cs_make(s, CT_WHITESPACE, start, (int)(s->src + s->pos - start), s->line);
        }
        if (c == '\n') {
            int line = s->line;
            cs_advance(s);
            return cs_make(s, CT_NEWLINE, s->src + s->pos - 1, 1, line);
        }
    }

    if (s->pos >= s->len) return cs_make(s, CT_EOF, NULL, 0, s->line);

    const char *start = s->src + s->pos;
    int line = s->line;
    char c = cs_advance(s);

    /* line comments */
    if (c == '/' && cs_peek(s) == '/') {
        while (s->pos < s->len && s->src[s->pos] != '\n') s->pos++;
        return cs_make(s, CT_COMMENT, start, (int)(s->src + s->pos - start), line);
    }
    /* block comments */
    if (c == '/' && cs_peek(s) == '*') {
        cs_advance(s);
        while (s->pos < s->len - 1) {
            if (s->src[s->pos] == '*' && s->src[s->pos + 1] == '/') {
                s->pos += 2; break;
            }
            if (s->src[s->pos] == '\n') s->line++;
            s->pos++;
        }
        return cs_make(s, CT_COMMENT, start, (int)(s->src + s->pos - start), line);
    }

    /* strings */
    if (c == '"') {
        while (s->pos < s->len && s->src[s->pos] != '"') {
            if (s->src[s->pos] == '\\') s->pos++;
            s->pos++;
        }
        if (s->pos < s->len) s->pos++; /* closing " */
        return cs_make(s, CT_STRING, start, (int)(s->src + s->pos - start), line);
    }

    /* char literals */
    if (c == '\'') {
        while (s->pos < s->len && s->src[s->pos] != '\'') {
            if (s->src[s->pos] == '\\') s->pos++;
            s->pos++;
        }
        if (s->pos < s->len) s->pos++;
        return cs_make(s, CT_CHAR_LIT, start, (int)(s->src + s->pos - start), line);
    }

    /* numbers */
    if (isdigit(c) || (c == '0' && (cs_peek(s) == 'x' || cs_peek(s) == 'X' || cs_peek(s) == 'b'))) {
        while (s->pos < s->len && (isalnum(s->src[s->pos]) || s->src[s->pos] == '_' || s->src[s->pos] == '.'))
            s->pos++;
        /* skip suffixes like ULL, u, L */
        while (s->pos < s->len && (s->src[s->pos] == 'u' || s->src[s->pos] == 'U' ||
               s->src[s->pos] == 'l' || s->src[s->pos] == 'L'))
            s->pos++;
        return cs_make(s, CT_NUMBER, start, (int)(s->src + s->pos - start), line);
    }

    /* identifiers and keywords */
    if (isalpha(c) || c == '_') {
        while (s->pos < s->len && (isalnum(s->src[s->pos]) || s->src[s->pos] == '_'))
            s->pos++;
        return cs_make(s, CT_IDENT, start, (int)(s->src + s->pos - start), line);
    }

    /* multi-char operators */
    switch (c) {
    case '(': return cs_make(s, CT_LPAREN, start, 1, line);
    case ')': return cs_make(s, CT_RPAREN, start, 1, line);
    case '{': return cs_make(s, CT_LBRACE, start, 1, line);
    case '}': return cs_make(s, CT_RBRACE, start, 1, line);
    case '[': return cs_make(s, CT_LBRACKET, start, 1, line);
    case ']': return cs_make(s, CT_RBRACKET, start, 1, line);
    case ';': return cs_make(s, CT_SEMICOLON, start, 1, line);
    case ',': return cs_make(s, CT_COMMA, start, 1, line);
    case '~': return cs_make(s, CT_TILDE, start, 1, line);
    case '?': return cs_make(s, CT_QUESTION, start, 1, line);
    case ':': return cs_make(s, CT_COLON, start, 1, line);
    case '#': return cs_make(s, CT_HASH, start, 1, line);
    case '.':
        if (cs_peek(s) == '.' && s->pos + 1 < s->len && s->src[s->pos + 1] == '.') {
            s->pos += 2; return cs_make(s, CT_DOT, start, 3, line); /* ... */
        }
        return cs_make(s, CT_DOT, start, 1, line);
    case '+':
        if (cs_match(s, '+')) return cs_make(s, CT_PLUSPLUS, start, 2, line);
        if (cs_match(s, '=')) return cs_make(s, CT_PLUSEQ, start, 2, line);
        return cs_make(s, CT_PLUS, start, 1, line);
    case '-':
        if (cs_match(s, '-')) return cs_make(s, CT_MINUSMINUS, start, 2, line);
        if (cs_match(s, '=')) return cs_make(s, CT_MINUSEQ, start, 2, line);
        if (cs_match(s, '>')) return cs_make(s, CT_ARROW, start, 2, line);
        return cs_make(s, CT_MINUS, start, 1, line);
    case '*':
        if (cs_match(s, '=')) return cs_make(s, CT_STAREQ, start, 2, line);
        return cs_make(s, CT_STAR, start, 1, line);
    case '/':
        if (cs_match(s, '=')) return cs_make(s, CT_SLASHEQ, start, 2, line);
        return cs_make(s, CT_SLASH, start, 1, line);
    case '%':
        if (cs_match(s, '=')) return cs_make(s, CT_PERCENTEQ, start, 2, line);
        return cs_make(s, CT_PERCENT, start, 1, line);
    case '&':
        if (cs_match(s, '&')) return cs_make(s, CT_AMPAMP, start, 2, line);
        if (cs_match(s, '=')) return cs_make(s, CT_AMPEQ, start, 2, line);
        return cs_make(s, CT_AMP, start, 1, line);
    case '|':
        if (cs_match(s, '|')) return cs_make(s, CT_PIPEPIPE, start, 2, line);
        if (cs_match(s, '=')) return cs_make(s, CT_PIPEEQ, start, 2, line);
        return cs_make(s, CT_PIPE, start, 1, line);
    case '^':
        if (cs_match(s, '=')) return cs_make(s, CT_CARETEQ, start, 2, line);
        return cs_make(s, CT_CARET, start, 1, line);
    case '=':
        if (cs_match(s, '=')) return cs_make(s, CT_EQEQ, start, 2, line);
        return cs_make(s, CT_EQ, start, 1, line);
    case '!':
        if (cs_match(s, '=')) return cs_make(s, CT_BANGEQ, start, 2, line);
        return cs_make(s, CT_BANG, start, 1, line);
    case '<':
        if (cs_match(s, '<')) {
            if (cs_match(s, '=')) return cs_make(s, CT_LTLTEQ, start, 3, line);
            return cs_make(s, CT_LTLT, start, 2, line);
        }
        if (cs_match(s, '=')) return cs_make(s, CT_LTEQ, start, 2, line);
        return cs_make(s, CT_LT, start, 1, line);
    case '>':
        if (cs_match(s, '>')) {
            if (cs_match(s, '=')) return cs_make(s, CT_GTGTEQ, start, 3, line);
            return cs_make(s, CT_GTGT, start, 2, line);
        }
        if (cs_match(s, '=')) return cs_make(s, CT_GTEQ, start, 2, line);
        return cs_make(s, CT_GT, start, 1, line);
    }

    return cs_make(s, CT_UNKNOWN, start, 1, line);
}

/* ================================================================
 * Type mapping — C types to ZER types
 * ================================================================ */

typedef struct { const char *c_name; int c_len; const char *zer_name; } TypeMap;

static const TypeMap type_map[] = {
    { "uint8_t",  7, "u8" },
    { "uint16_t", 8, "u16" },
    { "uint32_t", 8, "u32" },
    { "uint64_t", 8, "u64" },
    { "int8_t",   6, "i8" },
    { "int16_t",  7, "i16" },
    { "int32_t",  7, "i32" },
    { "int64_t",  7, "i64" },
    { "size_t",   6, "usize" },
    { "float",    5, "f32" },
    { "double",   6, "f64" },
    { "char",     4, "u8" },
    { "void",     4, "void" },
    { "bool",     4, "bool" },
    { "_Bool",    5, "bool" },
    { "NULL",     4, "null" },
    { "true",     4, "true" },
    { "false",    5, "false" },
    { NULL, 0, NULL }
};

/* two-token type combos that must be checked BEFORE single-token map */
typedef struct { const char *first; const char *second; const char *zer; } TypeCombo;
static const TypeCombo type_combos[] = {
    { "unsigned", "int",   "u32" },
    { "unsigned", "char",  "u8" },
    { "unsigned", "short", "u16" },
    { "unsigned", "long",  "u64" },
    { "signed",   "int",   "i32" },
    { "signed",   "char",  "i8" },
    { "signed",   "short", "i16" },
    { "signed",   "long",  "i64" },
    { "long",     "long",  "i64" },
    { "long",     "int",   "i64" },
    { "short",    "int",   "i16" },
    { NULL, NULL, NULL }
};

/* is_c_type and next_is_star defined after token buffer */

static bool tok_eq(CToken *t, const char *s) {
    int len = (int)strlen(s);
    return t->len == len && memcmp(t->start, s, len) == 0;
}

static const char *map_type(CToken *t) {
    for (int i = 0; type_map[i].c_name; i++) {
        if (t->len == type_map[i].c_len &&
            memcmp(t->start, type_map[i].c_name, t->len) == 0) {
            return type_map[i].zer_name;
        }
    }
    /* also check standalone keywords not in the table */
    if (tok_eq(t, "int")) return "i32";
    if (tok_eq(t, "long")) return "i64";
    if (tok_eq(t, "short")) return "i16";
    if (tok_eq(t, "unsigned")) return "u32";
    return NULL;
}

/* ================================================================
 * Token buffer — read all tokens, then transform
 * ================================================================ */

#define MAX_TOKENS 1000000

static CToken tokens[MAX_TOKENS];
static int token_count = 0;

static void tokenize(const char *src, int len) {
    CScanner sc;
    cs_init(&sc, src, len);
    token_count = 0;
    while (token_count < MAX_TOKENS) {
        CToken t = cs_next(&sc);
        tokens[token_count++] = t;
        if (t.type == CT_EOF) break;
    }
}

/* ================================================================
 * Transform engine
 * ================================================================ */

static FILE *out;
static bool needs_compat = false; /* set true if malloc/free/ptr arith used */
static bool in_switch_arm = false; /* track open switch arm for auto-close */

/* typedef tag→name mapping: typedef struct node { ... } Node; → node maps to Node */
#define MAX_TAG_MAPS 64
static struct { char tag[64]; char name[64]; } tag_maps[MAX_TAG_MAPS];
static int tag_map_count = 0;

static void add_tag_map(const char *tag, int tag_len, const char *name, int name_len) {
    if (tag_map_count >= MAX_TAG_MAPS) return;
    if (tag_len >= 63 || name_len >= 63) return;
    memcpy(tag_maps[tag_map_count].tag, tag, tag_len);
    tag_maps[tag_map_count].tag[tag_len] = '\0';
    memcpy(tag_maps[tag_map_count].name, name, name_len);
    tag_maps[tag_map_count].name[name_len] = '\0';
    tag_map_count++;
}

static const char *lookup_tag(const char *ident, int len) {
    for (int i = 0; i < tag_map_count; i++) {
        if ((int)strlen(tag_maps[i].tag) == len &&
            memcmp(tag_maps[i].tag, ident, len) == 0)
            return tag_maps[i].name;
    }
    return NULL;
}

static void emit_raw(const char *s, int len) {
    fwrite(s, 1, len, out);
}

static void emit_str(const char *s) {
    fputs(s, out);
}

static void emit_tok(CToken *t) {
    fwrite(t->start, 1, t->len, out);
}

/* check if token at index i is a specific identifier */
static bool is_ident_at(int i, const char *name) {
    if (i < 0 || i >= token_count) return false;
    return tokens[i].type == CT_IDENT && tok_eq(&tokens[i], name);
}

/* skip to next meaningful token index */
static int skip_ws(int i) {
    while (i < token_count && (tokens[i].type == CT_NEWLINE ||
           tokens[i].type == CT_COMMENT || tokens[i].type == CT_WHITESPACE))
        i++;
    return i;
}

/* skip whitespace only (not newlines/comments) */
static int skip_spaces(int i) {
    while (i < token_count && tokens[i].type == CT_WHITESPACE)
        i++;
    return i;
}

/* check if position i is followed by: WS IDENT [ expr ] (C array decl pattern)
 * If so, emit [expr] SPACE IDENT and return new position after ].
 * Otherwise return -1. */
static int try_reorder_array(int i) {
    int j = skip_spaces(i);
    if (j >= token_count || tokens[j].type != CT_IDENT) return -1;
    int name_idx = j;
    int k = skip_spaces(j + 1);
    if (k >= token_count || tokens[k].type != CT_LBRACKET) return -1;
    /* find matching ] — collect everything inside */
    int depth = 1;
    int m = k + 1;
    while (m < token_count && depth > 0) {
        if (tokens[m].type == CT_LBRACKET) depth++;
        if (tokens[m].type == CT_RBRACKET) { depth--; if (depth == 0) break; }
        m++;
    }
    if (m >= token_count || tokens[m].type != CT_RBRACKET) return -1;
    /* check what follows ] — must NOT be = or ( (those indicate indexing/function, not decl) */
    int after = skip_spaces(m + 1);
    /* array decl: type name[N]; or type name[N] = ... (but with preceding type context)
     * vs indexing: arr[i] = ... (but arr was already emitted as ident, not as type)
     * Since we only call this after type emission, it's always a declaration context. */
    /* emit [size] name */
    emit_str("[");
    for (int x = k + 1; x < m; x++) emit_tok(&tokens[x]);
    emit_str("] ");
    emit_tok(&tokens[name_idx]);
    return m + 1;
}

static void transform(void) {
    int i = 0;

    while (i < token_count && tokens[i].type != CT_EOF) {
        CToken *t = &tokens[i];

        /* ---- Preprocessor lines: #include, #define ---- */
        if (t->type == CT_HASH) {
            int j = i + 1;
            j = skip_ws(j);
            if (j < token_count && tokens[j].type == CT_IDENT) {
                if (tok_eq(&tokens[j], "include")) {
                    /* #include "foo.h" → cinclude "foo.h"; */
                    /* #include <foo.h> → cinclude "foo.h"; */
                    emit_str("cinclude ");
                    j++;
                    /* skip whitespace between 'include' and the path */
                    while (j < token_count && (tokens[j].type == CT_WHITESPACE || tokens[j].type == CT_NEWLINE)) j++;
                    if (j < token_count && tokens[j].type == CT_STRING) {
                        emit_tok(&tokens[j]);
                        j++;
                    } else if (j < token_count && tokens[j].type == CT_LT) {
                        /* <header.h> → "header.h" */
                        emit_str("\"");
                        j++;
                        while (j < token_count && tokens[j].type != CT_GT && tokens[j].type != CT_NEWLINE) {
                            emit_tok(&tokens[j]);
                            j++;
                        }
                        emit_str("\"");
                        if (j < token_count && tokens[j].type == CT_GT) j++;
                    }
                    emit_str(";\n");
                    /* skip to end of line */
                    while (j < token_count && tokens[j].type != CT_NEWLINE && tokens[j].type != CT_EOF) j++;
                    if (j < token_count && tokens[j].type == CT_NEWLINE) j++;
                    i = j;
                    continue;
                }
                if (tok_eq(&tokens[j], "define")) {
                    /* #define NAME VALUE → const ... NAME = VALUE; (simple cases) */
                    /* Complex macros: emit as comment + cinclude extraction */
                    j++;
                    j = skip_ws(j);
                    if (j < token_count && tokens[j].type == CT_IDENT) {
                        CToken *name = &tokens[j];
                        j++;
                        /* Check if it's a function-like macro: NAME( */
                        if (j < token_count && tokens[j].type == CT_LPAREN) {
                            /* function-like macro — emit as comment, too complex */
                            emit_str("// MANUAL: ");
                            while (i < token_count && tokens[i].type != CT_NEWLINE && tokens[i].type != CT_EOF) {
                                emit_tok(&tokens[i]);
                                i++;
                            }
                            emit_str("\n");
                            if (i < token_count && tokens[i].type == CT_NEWLINE) i++;
                            continue;
                        }
                        /* simple constant: #define FOO 42 */
                        j = skip_ws(j);
                        if (j < token_count && tokens[j].type == CT_NUMBER) {
                            /* check if it looks like an integer */
                            emit_str("const u32 ");
                            emit_tok(name);
                            emit_str(" = ");
                            emit_tok(&tokens[j]);
                            emit_str(";\n");
                            j++;
                            while (j < token_count && tokens[j].type != CT_NEWLINE && tokens[j].type != CT_EOF) j++;
                            if (j < token_count && tokens[j].type == CT_NEWLINE) j++;
                            i = j;
                            continue;
                        }
                    }
                    /* fallback: emit as comment */
                    emit_str("// MANUAL: ");
                    while (i < token_count && tokens[i].type != CT_NEWLINE && tokens[i].type != CT_EOF) {
                        emit_tok(&tokens[i]);
                        i++;
                    }
                    emit_str("\n");
                    if (i < token_count && tokens[i].type == CT_NEWLINE) i++;
                    continue;
                }
                /* other preprocessor: #ifdef, #endif, etc → emit as comment */
                emit_str("// MANUAL: ");
                while (i < token_count && tokens[i].type != CT_NEWLINE && tokens[i].type != CT_EOF) {
                    emit_tok(&tokens[i]);
                    i++;
                }
                emit_str("\n");
                if (i < token_count && tokens[i].type == CT_NEWLINE) i++;
                continue;
            }
        }

        /* ---- switch/case/break → ZER switch syntax ---- */
        /* case VALUE: → .VALUE => { and default: → default => { */
        if (t->type == CT_IDENT && tok_eq(t, "case")) {
            /* close previous arm if one was open (case without break) */
            if (in_switch_arm) {
                emit_str("}\n");
                in_switch_arm = false;
                /* re-emit indentation */
                for (int w = i - 1; w >= 0 && tokens[w].type == CT_WHITESPACE; w--) {
                    emit_tok(&tokens[w]);
                }
            }
            i++;
            int j = skip_spaces(i);
            /* collect everything up to : as the case value */
            emit_str(".");
            while (j < token_count && tokens[j].type != CT_COLON) {
                emit_tok(&tokens[j]);
                j++;
            }
            if (j < token_count && tokens[j].type == CT_COLON) j++; /* skip : */
            emit_str(" => {");
            in_switch_arm = true;
            i = j;
            continue;
        }
        if (t->type == CT_IDENT && tok_eq(t, "default")) {
            int j = skip_spaces(i + 1);
            if (j < token_count && tokens[j].type == CT_COLON) {
                /* close previous arm if open */
                if (in_switch_arm) {
                    emit_str("}\n");
                    in_switch_arm = false;
                    /* re-emit indentation (whitespace before default was already emitted) */
                    for (int w = i - 1; w >= 0 && tokens[w].type == CT_WHITESPACE; w--) {
                        emit_tok(&tokens[w]);
                    }
                }
                emit_str("default => {");
                in_switch_arm = true;
                i = j + 1;
                continue;
            }
        }
        /* break; inside switch → } (close arm block) */
        if (t->type == CT_IDENT && tok_eq(t, "break") && in_switch_arm) {
            int j = skip_spaces(i + 1);
            if (j < token_count && tokens[j].type == CT_SEMICOLON) {
                emit_str("}");
                in_switch_arm = false;
                i = j + 1; /* skip break + ; */
                continue;
            }
        }
        /* closing } of switch body — close any open arm first */
        if (t->type == CT_RBRACE && in_switch_arm) {
            emit_str("}\n"); /* close the arm */
            in_switch_arm = false;
            /* don't consume the } — let it emit normally for the switch body close */
        }

        /* ---- do { body } while (cond); → while (true) { body if (!(cond)) { break; } } ---- */
        if (t->type == CT_IDENT && tok_eq(t, "do")) {
            int j = skip_ws(i + 1);
            if (j < token_count && tokens[j].type == CT_LBRACE) {
                /* find matching } */
                int depth = 1;
                int body_end = j + 1;
                while (body_end < token_count && depth > 0) {
                    if (tokens[body_end].type == CT_LBRACE) depth++;
                    if (tokens[body_end].type == CT_RBRACE) depth--;
                    body_end++;
                }
                /* body_end is past }. Check for while (cond); */
                int w = skip_ws(body_end);
                if (w < token_count && tokens[w].type == CT_IDENT && tok_eq(&tokens[w], "while")) {
                    int p = skip_spaces(w + 1);
                    if (p < token_count && tokens[p].type == CT_LPAREN) {
                        int close = -1;
                        int pd = 1;
                        int q = p + 1;
                        while (q < token_count && pd > 0) {
                            if (tokens[q].type == CT_LPAREN) pd++;
                            if (tokens[q].type == CT_RPAREN) { pd--; if (pd == 0) { close = q; break; } }
                            q++;
                        }
                        if (close > 0) {
                            int semi = skip_spaces(close + 1);
                            /* Emit: while (true) { [body with transforms] if (!(cond)) { break; } } */
                            emit_str("while (true) {\n");
                            /* emit body contents with basic transforms */
                            int rbrace_idx = body_end - 1;
                            for (int x = j + 1; x < rbrace_idx; x++) {
                                if (tokens[x].type == CT_PLUSPLUS) { emit_str(" += 1"); continue; }
                                if (tokens[x].type == CT_MINUSMINUS) { emit_str(" -= 1"); continue; }
                                if (tokens[x].type == CT_ARROW) { emit_str("."); continue; }
                                if (tokens[x].type == CT_IDENT && tok_eq(&tokens[x], "NULL")) { emit_str("null"); continue; }
                                if (tokens[x].type == CT_IDENT) {
                                    const char *mt = map_type(&tokens[x]);
                                    if (mt) { emit_str(mt); continue; }
                                }
                                emit_tok(&tokens[x]);
                            }
                            /* emit the condition check */
                            emit_str("    if (!(");
                            for (int x = p + 1; x < close; x++) {
                                if (tokens[x].type == CT_IDENT) {
                                    const char *mt = map_type(&tokens[x]);
                                    if (mt) { emit_str(mt); continue; }
                                }
                                emit_tok(&tokens[x]);
                            }
                            emit_str(")) { break; }\n");
                            emit_str("}");
                            if (semi < token_count && tokens[semi].type == CT_SEMICOLON)
                                i = semi + 1;
                            else
                                i = close + 1;
                            continue;
                        }
                    }
                }
            }
        }

        /* ---- i++ / i-- → i += 1 / i -= 1 ---- */
        if (t->type == CT_PLUSPLUS) {
            emit_str(" += 1");
            i++;
            continue;
        }
        if (t->type == CT_MINUSMINUS) {
            emit_str(" -= 1");
            i++;
            continue;
        }

        /* ---- Type mapping: int → i32, uint8_t → u8, etc. ---- */
        if (t->type == CT_IDENT) {

            /* Two-token type combos: unsigned int → u32, etc.
             * Must check BEFORE single-token map to avoid double-mapping. */
            {
                bool found_combo = false;
                for (int ci = 0; type_combos[ci].first; ci++) {
                    if (tok_eq(t, type_combos[ci].first)) {
                        int j = skip_spaces(i + 1);
                        if (j < token_count && tokens[j].type == CT_IDENT &&
                            tok_eq(&tokens[j], type_combos[ci].second)) {
                            emit_str(type_combos[ci].zer);
                            i = j + 1; /* skip both tokens */
                            /* check for C-style array: type name[N] → type[N] name */
                            { int ar = try_reorder_array(i); if (ar >= 0) i = ar; }
                            found_combo = true;
                            break;
                        }
                    }
                }
                if (found_combo) continue;
            }

            /* standalone 'unsigned' without type after it → u32 */
            if (tok_eq(t, "unsigned")) {
                int j = skip_spaces(i + 1);
                /* if next meaningful token is NOT a type keyword, emit u32 */
                if (j >= token_count || tokens[j].type != CT_IDENT ||
                    (!tok_eq(&tokens[j], "int") && !tok_eq(&tokens[j], "char") &&
                     !tok_eq(&tokens[j], "short") && !tok_eq(&tokens[j], "long"))) {
                    emit_str("u32");
                    i++;
                    { int ar = try_reorder_array(i); if (ar >= 0) i = ar; }
                    continue;
                }
            }

            /* standalone 'int' (not part of a combo already handled) → i32 */
            if (tok_eq(t, "int")) {
                emit_str("i32");
                i++;
                { int ar = try_reorder_array(i); if (ar >= 0) i = ar; }
                continue;
            }

            /* standalone 'long' (not part of combo) → i64 */
            if (tok_eq(t, "long")) {
                emit_str("i64");
                i++;
                { int ar = try_reorder_array(i); if (ar >= 0) i = ar; }
                continue;
            }

            /* standalone 'short' (not part of combo) → i16 */
            if (tok_eq(t, "short")) {
                emit_str("i16");
                i++;
                { int ar = try_reorder_array(i); if (ar >= 0) i = ar; }
                continue;
            }

            /* void * → *opaque (type-erased pointer) */
            if (tok_eq(t, "void")) {
                int j = skip_spaces(i + 1);
                if (j < token_count && tokens[j].type == CT_STAR) {
                    /* void * → *opaque */
                    emit_str("*opaque ");
                    i = j + 1; /* skip void + * */
                    continue;
                }
                /* standalone void — keep as void */
                emit_str("void");
                i++;
                continue;
            }

            const char *mapped = map_type(t);

            /* NULL → null */
            if (tok_eq(t, "NULL")) {
                emit_str("null");
                i++;
                continue;
            }

            /* typedef struct → struct Name { ... } (ZER doesn't use typedef for structs) */
            if (tok_eq(t, "typedef")) {
                int j = skip_spaces(i + 1);
                if (j < token_count && tokens[j].type == CT_IDENT &&
                    (tok_eq(&tokens[j], "struct") || tok_eq(&tokens[j], "union") || tok_eq(&tokens[j], "enum"))) {
                    /* typedef struct [tag] { ... } Name;
                     * → struct Name { ... }
                     * Strategy: find the }, then the Name before ;.
                     * Emit "struct Name " then jump i to { so normal transform handles body. */
                    const char *kw = tok_eq(&tokens[j], "struct") ? "struct" :
                                     tok_eq(&tokens[j], "union") ? "union" : "enum";
                    int k = skip_spaces(j + 1);
                    int tag_idx = -1; /* optional struct tag */
                    /* skip optional tag name (e.g., typedef struct node { ... } Node;) */
                    if (k < token_count && tokens[k].type == CT_IDENT) {
                        int m = skip_ws(k + 1);
                        if (m < token_count && tokens[m].type == CT_LBRACE) {
                            tag_idx = k;
                            k = m; /* skip tag, jump to { */
                        }
                    }
                    if (k < token_count && tokens[k].type == CT_LBRACE) {
                        /* find matching } to locate the typedef name after it */
                        int depth = 1;
                        int m = k + 1;
                        while (m < token_count && depth > 0) {
                            if (tokens[m].type == CT_LBRACE) depth++;
                            if (tokens[m].type == CT_RBRACE) depth--;
                            m++;
                        }
                        /* m is past }. Find the typedef name before ; */
                        int name_idx = -1;
                        int n = skip_spaces(m);
                        if (n < token_count && tokens[n].type == CT_IDENT) {
                            name_idx = n;
                        }
                        if (name_idx >= 0) {
                            /* register tag→name mapping if tag exists */
                            if (tag_idx >= 0) {
                                add_tag_map(tokens[tag_idx].start, tokens[tag_idx].len,
                                           tokens[name_idx].start, tokens[name_idx].len);
                            }
                            /* emit "struct Name " and mark the post-} name+; for skipping */
                            emit_str(kw);
                            emit_str(" ");
                            emit_tok(&tokens[name_idx]);
                            emit_str(" ");
                            /* Set i to { — let normal transform loop handle body contents.
                             * We need to mark the typedef-name + ; after } for skipping.
                             * Use a simple approach: replace the name token with a skip marker. */
                            tokens[name_idx].type = CT_WHITESPACE; /* neutralize — will emit as space */
                            tokens[name_idx].start = " ";
                            tokens[name_idx].len = 0;
                            /* also neutralize the ; if present */
                            int semi = skip_spaces(name_idx + 1);
                            if (semi < token_count && tokens[semi].type == CT_SEMICOLON) {
                                tokens[semi].type = CT_WHITESPACE;
                                tokens[semi].start = "";
                                tokens[semi].len = 0;
                            }
                            i = k; /* jump to {, normal loop handles contents */
                            continue;
                        }
                    }
                }
                /* non-struct typedef — pass through */
                emit_str("typedef");
                i++;
                continue;
            }

            /* struct keyword in usage (not declaration) — drop it + trailing whitespace */
            if (tok_eq(t, "struct")) {
                int j = skip_spaces(i + 1);
                if (j < token_count && tokens[j].type == CT_IDENT) {
                    /* check if next-next is { → keep 'struct' (declaration) */
                    int k = skip_ws(j + 1);
                    if (k < token_count && tokens[k].type == CT_LBRACE) {
                        /* struct declaration — keep struct keyword */
                        emit_str("struct");
                        i++;
                        continue;
                    }
                    /* struct usage: struct<ws>Name → Name (skip struct + whitespace) */
                    /* also apply tag mapping: struct node → Node (if node maps to Node) */
                    const char *tag_name = lookup_tag(tokens[j].start, tokens[j].len);
                    if (tag_name) {
                        emit_str(tag_name);
                        i = j + 1;
                    } else {
                        i = j; /* jump to the name, skip struct + spaces */
                    }
                    continue;
                }
            }

            /* enum keyword in usage (not declaration) — drop it + trailing whitespace */
            if (tok_eq(t, "enum")) {
                int j = skip_spaces(i + 1);
                if (j < token_count && tokens[j].type == CT_IDENT) {
                    /* check if next-next is { → keep 'enum' (declaration) */
                    int k = skip_ws(j + 1);
                    if (k < token_count && tokens[k].type == CT_LBRACE) {
                        /* enum declaration — keep enum keyword */
                        emit_str("enum");
                        i++;
                        continue;
                    }
                    /* enum usage: enum State → State (skip enum + whitespace) */
                    i = j;
                    continue;
                }
            }

            /* sizeof(T) → @size(T) — must handle type args to prevent
             * (type *) inside sizeof from being detected as a cast.
             * sizeof(dict_entry *) → @size(*dict_entry) not @size@ptrcast(...) */
            if (tok_eq(t, "sizeof")) {
                i++;
                int j = skip_spaces(i);
                if (j < token_count && tokens[j].type == CT_LPAREN) {
                    /* sizeof(something) — check if the content is a type */
                    int k = skip_spaces(j + 1);
                    /* skip 'struct' keyword if present */
                    bool had_struct = false;
                    if (k < token_count && tokens[k].type == CT_IDENT && tok_eq(&tokens[k], "struct")) {
                        had_struct = true;
                        k = skip_spaces(k + 1);
                    }
                    if (k < token_count && tokens[k].type == CT_IDENT) {
                        const char *mt = map_type(&tokens[k]);
                        int m = skip_spaces(k + 1);
                        /* sizeof(type *) → @size(*type) */
                        if (m < token_count && tokens[m].type == CT_STAR) {
                            int n = skip_spaces(m + 1);
                            if (n < token_count && tokens[n].type == CT_RPAREN) {
                                emit_str("@size(*");
                                if (mt) emit_str(mt);
                                else emit_raw(tokens[k].start, tokens[k].len);
                                emit_str(")");
                                i = n + 1;
                                continue;
                            }
                        }
                        /* sizeof(type) → @size(type) */
                        if (m < token_count && tokens[m].type == CT_RPAREN) {
                            emit_str("@size(");
                            if (mt) emit_str(mt);
                            else emit_raw(tokens[k].start, tokens[k].len);
                            emit_str(")");
                            i = m + 1;
                            continue;
                        }
                    }
                }
                /* fallback: emit @size and let normal parsing handle the rest */
                emit_str("@size");
                continue;
            }

            /* malloc/calloc/realloc/free → compat wrappers */
            if (tok_eq(t, "malloc")) { emit_str("zer_malloc_bytes"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "calloc")) { emit_str("zer_calloc_bytes"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "realloc")) { emit_str("zer_realloc_bytes"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "free")) { emit_str("zer_free"); needs_compat = true; i++; continue; }

            /* string/memory functions → compat wrappers */
            if (tok_eq(t, "strlen")) { emit_str("zer_strlen"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "strcmp")) { emit_str("zer_strcmp"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "strncmp")) { emit_str("zer_strncmp"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "strdup")) { emit_str("zer_strdup"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "strndup")) { emit_str("zer_strndup"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "strcpy")) { emit_str("zer_strcpy"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "strncpy")) { emit_str("zer_strncpy"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "strcat")) { emit_str("zer_strcat"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "memcmp")) { emit_str("zer_memcmp"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "memcpy")) { emit_str("zer_memcpy"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "memmove")) { emit_str("zer_memmove"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "memset")) { emit_str("zer_memset"); needs_compat = true; i++; continue; }

            /* I/O functions — keep as-is (accessed via cinclude "stdio.h") */
            /* printf, fprintf, etc. are C functions used directly via cinclude.
             * They don't need compat wrappers — just declare them in ZER. */

            /* exit() → zer_exit() */
            if (tok_eq(t, "exit")) { emit_str("zer_exit"); needs_compat = true; i++; continue; }

            /* Type-mapped identifier */
            if (mapped) {
                emit_str(mapped);
                i++;
                { int ar = try_reorder_array(i); if (ar >= 0) i = ar; }
                continue;
            }

            /* Tag→name mapping: bare 'node' → 'Node' (from typedef struct node {} Node;) */
            {
                const char *tag_name = lookup_tag(t->start, t->len);
                if (tag_name) {
                    emit_str(tag_name);
                    i++;
                    continue;
                }
            }
        }

        /* ---- C-style cast: (int)x → @truncate(i32, x), (Node *)p → @ptrcast(*Node, p) ---- */
        if (t->type == CT_LPAREN) {
            /* look ahead: ( TYPE ) or ( TYPE * ) or ( struct TYPE * ) */
            int j = skip_spaces(i + 1);
            bool is_cast = false;
            bool is_ptr_cast = false;
            const char *cast_type = NULL;
            char cast_buf[128];
            int cast_end = j;

            /* check for (struct Name *) pattern */
            if (j < token_count && tokens[j].type == CT_IDENT && tok_eq(&tokens[j], "struct")) {
                int k = skip_spaces(j + 1);
                if (k < token_count && tokens[k].type == CT_IDENT) {
                    int m = skip_spaces(k + 1);
                    if (m < token_count && tokens[m].type == CT_STAR) {
                        int n = skip_spaces(m + 1);
                        if (n < token_count && tokens[n].type == CT_RPAREN) {
                            snprintf(cast_buf, sizeof(cast_buf), "*%.*s", tokens[k].len, tokens[k].start);
                            cast_type = cast_buf;
                            is_ptr_cast = true;
                            is_cast = true;
                            cast_end = n + 1;
                        }
                    }
                }
            }
            /* check for (void *) cast → @ptrcast(*opaque, ...) */
            if (!is_cast && j < token_count && tokens[j].type == CT_IDENT &&
                tok_eq(&tokens[j], "void")) {
                int k = skip_spaces(j + 1);
                if (k < token_count && tokens[k].type == CT_STAR) {
                    int m = skip_spaces(k + 1);
                    if (m < token_count && tokens[m].type == CT_RPAREN) {
                        cast_type = "*opaque";
                        is_ptr_cast = true;
                        is_cast = true;
                        cast_end = m + 1;
                    }
                }
            }
            /* check for (type *) pattern */
            if (!is_cast && j < token_count && tokens[j].type == CT_IDENT &&
                !tok_eq(&tokens[j], "void")) {
                const char *mapped_cast = map_type(&tokens[j]);
                int k = skip_spaces(j + 1);
                if (k < token_count && tokens[k].type == CT_STAR) {
                    int m = skip_spaces(k + 1);
                    if (m < token_count && tokens[m].type == CT_RPAREN) {
                        const char *tname = mapped_cast ? mapped_cast : "";
                        if (!mapped_cast) {
                            snprintf(cast_buf, sizeof(cast_buf), "*%.*s", tokens[j].len, tokens[j].start);
                        } else {
                            snprintf(cast_buf, sizeof(cast_buf), "*%s", tname);
                        }
                        cast_type = cast_buf;
                        is_ptr_cast = true;
                        is_cast = true;
                        cast_end = m + 1;
                    }
                }
                /* check for (type) pattern — value cast.
                 * Exclude (void) — that's a function param list, not a cast. */
                if (!is_cast && mapped_cast && !tok_eq(&tokens[j], "void")) {
                    if (k < token_count && tokens[k].type == CT_RPAREN) {
                        cast_type = mapped_cast;
                        is_cast = true;
                        cast_end = k + 1;
                    }
                }
            }

            if (is_cast && cast_type) {
                if (is_ptr_cast) {
                    emit_str("@ptrcast(");
                    emit_str(cast_type);
                    emit_str(", ");
                } else {
                    emit_str("@truncate(");
                    emit_str(cast_type);
                    emit_str(", ");
                }
                /* emit the cast operand expression.
                 * (int)x → @truncate(i32, x)
                 * (int)(a + b) → @truncate(i32, (a + b))
                 * (dict *)malloc(sizeof(dict)) → @ptrcast(*dict, zer_malloc_bytes(@size(dict)))
                 * Key: if operand is ident followed by (, it's a function call — include the args. */
                i = cast_end;
                /* skip whitespace */
                while (i < token_count && tokens[i].type == CT_WHITESPACE) i++;

                if (i < token_count && tokens[i].type == CT_LPAREN) {
                    /* (int)(expr) — emit matching parens */
                    int depth = 0;
                    while (i < token_count) {
                        if (tokens[i].type == CT_LPAREN) depth++;
                        if (tokens[i].type == CT_RPAREN) {
                            depth--;
                            if (depth == 0) { emit_tok(&tokens[i]); i++; break; }
                        }
                        emit_tok(&tokens[i]);
                        i++;
                    }
                } else if (i < token_count && tokens[i].type == CT_IDENT) {
                    /* check if it's a function call: ident( */
                    int fn = i;
                    int after_fn = skip_spaces(fn + 1);
                    if (after_fn < token_count && tokens[after_fn].type == CT_LPAREN) {
                        /* function call — emit ident + (args) as the full cast operand.
                         * Also apply compat mapping to the function name. */
                        CToken *fntok = &tokens[fn];
                        /* map function name through compat */
                        if (tok_eq(fntok, "malloc")) { emit_str("zer_malloc_bytes"); needs_compat = true; }
                        else if (tok_eq(fntok, "calloc")) { emit_str("zer_calloc_bytes"); needs_compat = true; }
                        else if (tok_eq(fntok, "realloc")) { emit_str("zer_realloc_bytes"); needs_compat = true; }
                        else { emit_tok(fntok); }
                        i = after_fn;
                        /* emit the (args) — transform sizeof inside */
                        int depth = 0;
                        while (i < token_count) {
                            if (tokens[i].type == CT_LPAREN) depth++;
                            if (tokens[i].type == CT_RPAREN) {
                                depth--;
                                if (depth == 0) { emit_tok(&tokens[i]); i++; break; }
                            }
                            /* transform sizeof inside args */
                            if (tokens[i].type == CT_IDENT && tok_eq(&tokens[i], "sizeof")) {
                                emit_str("@size");
                                i++;
                                continue;
                            }
                            /* transform NULL inside args */
                            if (tokens[i].type == CT_IDENT && tok_eq(&tokens[i], "NULL")) {
                                emit_str("null");
                                i++;
                                continue;
                            }
                            /* transform type names inside args */
                            if (tokens[i].type == CT_IDENT) {
                                const char *mt = map_type(&tokens[i]);
                                if (mt) { emit_str(mt); i++; continue; }
                            }
                            emit_tok(&tokens[i]);
                            i++;
                        }
                    } else {
                        /* single identifier — just emit it */
                        if (tokens[i].type == CT_STAR || tokens[i].type == CT_AMP) {
                            emit_tok(&tokens[i]); i++;
                        }
                        if (i < token_count && (tokens[i].type == CT_IDENT || tokens[i].type == CT_NUMBER)) {
                            emit_tok(&tokens[i]); i++;
                        }
                    }
                } else if (i < token_count) {
                    /* prefix operator like * or & */
                    if (tokens[i].type == CT_STAR || tokens[i].type == CT_AMP) {
                        emit_tok(&tokens[i]); i++;
                    }
                    if (i < token_count && (tokens[i].type == CT_IDENT || tokens[i].type == CT_NUMBER)) {
                        emit_tok(&tokens[i]); i++;
                    }
                }
                emit_str(")");
                continue;
            }
        }

        /* ---- Arrow operator: -> stays as . (ZER auto-derefs) ---- */
        if (t->type == CT_ARROW) {
            emit_str(".");
            i++;
            continue;
        }

        /* ---- Whitespace: pass through ---- */
        if (t->type == CT_WHITESPACE) {
            emit_tok(t);
            i++;
            continue;
        }

        /* ---- Comments: pass through ---- */
        if (t->type == CT_COMMENT) {
            emit_tok(t);
            i++;
            continue;
        }

        /* ---- Newlines: pass through ---- */
        if (t->type == CT_NEWLINE) {
            emit_str("\n");
            i++;
            continue;
        }

        /* ---- Everything else: pass through unchanged ---- */
        emit_tok(t);
        i++;
    }
}

/* ================================================================
 * Main
 * ================================================================ */

static char *read_file(const char *path, int *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "zer-convert: cannot open '%s'\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);
    *out_len = (int)len;
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: zer-convert <input.c> [-o output.zer]\n");
        fprintf(stderr, "  Converts C source to ZER syntax (Phase 1)\n");
        return 1;
    }

    const char *input_path = argv[1];
    const char *output_path = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        }
    }

    /* default output: replace .c with .zer */
    char default_out[512];
    if (!output_path) {
        strncpy(default_out, input_path, sizeof(default_out) - 5);
        int len = (int)strlen(default_out);
        if (len > 2 && strcmp(default_out + len - 2, ".c") == 0) {
            strcpy(default_out + len - 2, ".zer");
        } else {
            strcat(default_out, ".zer");
        }
        output_path = default_out;
    }

    int src_len;
    char *src = read_file(input_path, &src_len);
    if (!src) return 1;

    tokenize(src, src_len);

    out = fopen(output_path, "w");
    if (!out) {
        fprintf(stderr, "zer-convert: cannot write '%s'\n", output_path);
        free(src);
        return 1;
    }

    /* header */
    fprintf(out, "// Converted from %s by zer-convert\n", input_path);
    fprintf(out, "// Review MANUAL: comments for items needing attention\n\n");

    transform();

    /* add compat import if needed */
    if (needs_compat) {
        /* prepend compat import — rewrite file */
        fclose(out);

        /* read what we wrote */
        int zer_len;
        char *zer_content = read_file(output_path, &zer_len);

        out = fopen(output_path, "w");
        fprintf(out, "// Converted from %s by zer-convert\n", input_path);
        fprintf(out, "// Uses compat.zer — run 'zerc --safe-upgrade' to replace with safe ZER\n\n");
        fprintf(out, "import compat;\n\n");
        /* skip the old header lines */
        char *body = zer_content;
        /* skip first two comment lines + blank */
        for (int skip = 0; skip < 3 && body && *body; skip++) {
            char *nl = strchr(body, '\n');
            if (nl) body = nl + 1; else break;
        }
        if (body) fwrite(body, 1, strlen(body), out);
        free(zer_content);
    }

    fclose(out);
    free(src);

    printf("zer-convert: %s -> %s", input_path, output_path);
    if (needs_compat) printf(" (uses compat.zer)");
    printf("\n");

    return 0;
}
