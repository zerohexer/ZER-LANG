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
    { "int",      3, "i32" },
    { "short",    5, "i16" },
    { "long",     4, "i64" },
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
 * Transform engine — output to growable buffer
 * ================================================================ */

static char *out_buf = NULL;
static int out_len = 0;
static int out_cap = 0;
static bool needs_compat = false; /* set true if malloc/free/ptr arith used */

static void emit_raw(const char *s, int len) {
    if (out_len + len > out_cap) {
        if (out_cap == 0) out_cap = 65536;
        while (out_len + len > out_cap) out_cap *= 2;
        out_buf = realloc(out_buf, out_cap);
    }
    memcpy(out_buf + out_len, s, len);
    out_len += len;
}

static void emit_str(const char *s) {
    emit_raw(s, (int)strlen(s));
}

static void emit_tok(CToken *t) {
    emit_raw(t->start, t->len);
}

/* Trim trailing whitespace/newlines from output buffer */
static void emit_trim_trailing_ws(void) {
    while (out_len > 0 && (out_buf[out_len - 1] == ' ' || out_buf[out_len - 1] == '\t' ||
           out_buf[out_len - 1] == '\n' || out_buf[out_len - 1] == '\r'))
        out_len--;
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

/* Try to rearrange pointer declaration: type *name → *type name
 * Emits stars + zer_type + space if successful.
 * Returns token index to continue from (the variable name), or -1 if no rearrangement. */
static int try_ptr_rearrange(int type_idx, const char *zer_type) {
    int j = type_idx + 1;
    while (j < token_count && tokens[j].type == CT_WHITESPACE) j++;

    int star_count = 0;
    while (j < token_count && tokens[j].type == CT_STAR) {
        star_count++;
        j++;
        while (j < token_count && tokens[j].type == CT_WHITESPACE) j++;
    }

    if (star_count == 0) return -1;
    if (j >= token_count || tokens[j].type != CT_IDENT) return -1;

    /* Verify declaration context: after name should be ; = , ) ( [ */
    int after = j + 1;
    while (after < token_count && tokens[after].type == CT_WHITESPACE) after++;
    if (after >= token_count) return -1;
    CTokenType at = tokens[after].type;
    if (at != CT_SEMICOLON && at != CT_EQ && at != CT_COMMA &&
        at != CT_RPAREN && at != CT_LPAREN && at != CT_LBRACKET) return -1;

    for (int s = 0; s < star_count; s++) emit_str("*");
    emit_str(zer_type);
    emit_str(" ");
    return j; /* continue from variable name */
}

static void transform(void) {
    int i = 0;

    /* State tracking for switch/case and loop transforms */
    int brace_depth = 0;
    int switch_depth = 0;
    struct { int brace; bool arm_open; const char *indent; int indent_len; } sw_stack[32];
    int loop_depth = 0;
    int loop_brace[32];
    bool next_brace_is_switch = false;
    bool next_brace_is_loop = false;
    /* For typedef struct { } Name; — skip trailing name after } */
    bool skip_typedef_trailer = false;
    int typedef_body_depth = -1; /* brace_depth when typedef body { was seen */

    while (i < token_count && tokens[i].type != CT_EOF) {
        CToken *t = &tokens[i];

        /* ---- Skip trailing Name ; after typedef struct {} Name; ---- */
        if (skip_typedef_trailer && typedef_body_depth < 0) {
            /* Past the body } — skip trailing tokens until ; */
            if (t->type == CT_SEMICOLON) {
                skip_typedef_trailer = false;
            }
            i++;
            continue;
        }

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

            /* ---- switch keyword: track for case/break transform ---- */
            if (tok_eq(t, "switch")) {
                next_brace_is_switch = true;
                emit_str("switch");
                i++;
                continue;
            }

            /* ---- case VALUE: → VALUE => { ---- */
            if (tok_eq(t, "case") && switch_depth > 0) {
                /* Find preceding indentation for proper formatting */
                const char *indent = "";
                int indent_len = 0;
                for (int p = i - 1; p >= 0; p--) {
                    if (tokens[p].type == CT_WHITESPACE) {
                        indent = tokens[p].start;
                        indent_len = tokens[p].len;
                        break;
                    }
                    if (tokens[p].type == CT_NEWLINE) break;
                }
                /* Close previous arm if open */
                if (sw_stack[switch_depth - 1].arm_open) {
                    emit_str("}\n");
                    emit_raw(indent, indent_len);
                    sw_stack[switch_depth - 1].arm_open = false;
                }
                i++; /* skip 'case' */
                int j = skip_spaces(i);
                /* Collect case values, handling fallthrough (case A: case B: → A, B =>) */
                while (j < token_count) {
                    int val_start = j;
                    /* collect tokens until ':' */
                    while (j < token_count && tokens[j].type != CT_COLON) j++;
                    /* emit value tokens with type mapping */
                    for (int v = val_start; v < j; v++) {
                        if (tokens[v].type == CT_WHITESPACE) continue; /* trim spaces in value */
                        if (tokens[v].type == CT_IDENT) {
                            const char *mt = map_type(&tokens[v]);
                            if (mt) { emit_str(mt); continue; }
                        }
                        emit_tok(&tokens[v]);
                    }
                    if (j < token_count && tokens[j].type == CT_COLON) j++; /* skip ':' */
                    /* Check for fallthrough: next meaningful token is 'case' */
                    int next = skip_ws(j);
                    if (next < token_count && tokens[next].type == CT_IDENT &&
                        tok_eq(&tokens[next], "case")) {
                        emit_str(", ");
                        j = next + 1; /* skip 'case' */
                        j = skip_spaces(j);
                    } else {
                        break;
                    }
                }
                emit_str(" => {");
                sw_stack[switch_depth - 1].arm_open = true;
                sw_stack[switch_depth - 1].indent = indent;
                sw_stack[switch_depth - 1].indent_len = indent_len;
                i = j;
                continue;
            }

            /* ---- default: → default => { ---- */
            if (tok_eq(t, "default") && switch_depth > 0) {
                int j = skip_spaces(i + 1);
                if (j < token_count && tokens[j].type == CT_COLON) {
                    if (sw_stack[switch_depth - 1].arm_open) {
                        const char *indent = "";
                        int indent_len = 0;
                        for (int p = i - 1; p >= 0; p--) {
                            if (tokens[p].type == CT_WHITESPACE) {
                                indent = tokens[p].start;
                                indent_len = tokens[p].len;
                                break;
                            }
                            if (tokens[p].type == CT_NEWLINE) break;
                        }
                        emit_str("}\n");
                        emit_raw(indent, indent_len);
                        sw_stack[switch_depth - 1].arm_open = false;
                    }
                    emit_str("default => {");
                    sw_stack[switch_depth - 1].arm_open = true;
                    /* Save indent for this arm (reuse case-level indent from backward scan) */
                    {
                        const char *di = "";
                        int dil = 0;
                        for (int p = i - 1; p >= 0; p--) {
                            if (tokens[p].type == CT_WHITESPACE) {
                                di = tokens[p].start;
                                dil = tokens[p].len;
                                break;
                            }
                            if (tokens[p].type == CT_NEWLINE) break;
                        }
                        sw_stack[switch_depth - 1].indent = di;
                        sw_stack[switch_depth - 1].indent_len = dil;
                    }
                    i = j + 1;
                    continue;
                }
            }

            /* ---- break; inside switch → } (close arm) ---- */
            if (tok_eq(t, "break")) {
                int j = skip_spaces(i + 1);
                if (j < token_count && tokens[j].type == CT_SEMICOLON && switch_depth > 0) {
                    /* Check if break belongs to switch or nested loop */
                    bool is_switch_break = true;
                    if (loop_depth > 0 &&
                        loop_brace[loop_depth - 1] > sw_stack[switch_depth - 1].brace) {
                        is_switch_break = false;
                    }
                    if (is_switch_break) {
                        /* Trim trailing whitespace (the indent before 'break' was already emitted) */
                        emit_trim_trailing_ws();
                        emit_str("\n");
                        emit_raw(sw_stack[switch_depth - 1].indent,
                                 sw_stack[switch_depth - 1].indent_len);
                        emit_str("}\n");
                        sw_stack[switch_depth - 1].arm_open = false;
                        i = j + 1; /* skip break; */
                        /* Skip trailing newline after break; (we already emitted one) */
                        if (i < token_count && tokens[i].type == CT_NEWLINE) i++;
                        continue;
                    }
                }
                emit_str("break");
                i++;
                continue;
            }

            /* ---- for/while/do — track loop depth ---- */
            if (tok_eq(t, "for") || tok_eq(t, "while") || tok_eq(t, "do")) {
                next_brace_is_loop = true;
                emit_tok(t);
                i++;
                continue;
            }

            /* ---- goto LABEL; → // MANUAL: goto LABEL; ---- */
            if (tok_eq(t, "goto")) {
                emit_str("// MANUAL: ");
                while (i < token_count && tokens[i].type != CT_SEMICOLON &&
                       tokens[i].type != CT_NEWLINE && tokens[i].type != CT_EOF) {
                    emit_tok(&tokens[i]);
                    i++;
                }
                if (i < token_count && tokens[i].type == CT_SEMICOLON) {
                    emit_str(";");
                    i++;
                }
                continue;
            }

            /* ---- enum keyword: strip in usage, keep in declaration ---- */
            if (tok_eq(t, "enum")) {
                int j = skip_spaces(i + 1);
                if (j < token_count && tokens[j].type == CT_IDENT) {
                    int k = skip_ws(j + 1);
                    if (k < token_count && tokens[k].type == CT_LBRACE) {
                        /* enum declaration — keep enum keyword */
                        emit_str("enum");
                        i++;
                        continue;
                    }
                    /* enum usage: enum Color → Color */
                    i = j;
                    continue;
                }
                if (j < token_count && tokens[j].type == CT_LBRACE) {
                    /* anonymous enum { ... } — keep */
                    emit_str("enum");
                    i++;
                    continue;
                }
                emit_str("enum");
                i++;
                continue;
            }

            /* ---- union keyword: strip in usage, keep in declaration ---- */
            if (tok_eq(t, "union")) {
                int j = skip_spaces(i + 1);
                if (j < token_count && tokens[j].type == CT_IDENT) {
                    int k = skip_ws(j + 1);
                    if (k < token_count && tokens[k].type == CT_LBRACE) {
                        emit_str("union");
                        i++;
                        continue;
                    }
                    /* union usage: union Data → Data */
                    i = j;
                    continue;
                }
                emit_str("union");
                i++;
                continue;
            }

            /* ---- typedef handling ---- */
            if (tok_eq(t, "typedef")) {
                int j = skip_spaces(i + 1);

                /* typedef struct/enum/union ... */
                if (j < token_count && tokens[j].type == CT_IDENT &&
                    (tok_eq(&tokens[j], "struct") || tok_eq(&tokens[j], "enum") ||
                     tok_eq(&tokens[j], "union"))) {
                    int k = skip_spaces(j + 1);

                    /* typedef struct Name Name; → drop (forward decl) */
                    if (k < token_count && tokens[k].type == CT_IDENT) {
                        int m = skip_spaces(k + 1);
                        if (m < token_count && tokens[m].type == CT_IDENT) {
                            int n = skip_spaces(m + 1);
                            if (n < token_count && tokens[n].type == CT_SEMICOLON) {
                                /* typedef struct X X; → skip */
                                i = n + 1;
                                continue;
                            }
                        }
                        /* typedef struct Name { ... } Alias; → struct Name { ... } */
                        if (m < token_count && tokens[m].type == CT_LBRACE) {
                            emit_tok(&tokens[j]); /* struct/enum/union */
                            emit_str(" ");
                            emit_tok(&tokens[k]); /* Name */
                            emit_str(" ");
                            i = m; /* point to { — main loop processes body */
                            /* After }, skip trailing Alias ; */
                            skip_typedef_trailer = true;
                            typedef_body_depth = 0;
                            continue;
                        }
                    }

                    /* typedef struct { ... } Name; → struct Name { ... } */
                    if (k < token_count && tokens[k].type == CT_LBRACE) {
                        /* Find closing } to get the Name */
                        int depth = 0, m = k;
                        while (m < token_count) {
                            if (tokens[m].type == CT_LBRACE) depth++;
                            if (tokens[m].type == CT_RBRACE) { depth--; if (depth == 0) break; }
                            m++;
                        }
                        int name_idx = skip_spaces(m + 1);
                        if (name_idx < token_count && tokens[name_idx].type == CT_IDENT) {
                            emit_tok(&tokens[j]); /* keyword */
                            emit_str(" ");
                            emit_tok(&tokens[name_idx]); /* Name */
                            emit_str(" ");
                            i = k; /* point to { — main loop processes body */
                            /* After }, skip trailing Name ; */
                            skip_typedef_trailer = true;
                            typedef_body_depth = 0; /* next { will increment to 1 */
                            continue;
                        }
                    }
                }

                /* typedef with function pointer: typedef ... (*Name)(...); → keep with mapping */
                {
                    int scan = j;
                    bool has_fptr = false;
                    while (scan < token_count && tokens[scan].type != CT_SEMICOLON &&
                           tokens[scan].type != CT_EOF) {
                        if (tokens[scan].type == CT_LPAREN) {
                            int next = skip_spaces(scan + 1);
                            if (next < token_count && tokens[next].type == CT_STAR) {
                                has_fptr = true;
                                break;
                            }
                        }
                        scan++;
                    }
                    if (has_fptr) {
                        /* Emit typedef, let main loop handle rest with type mapping */
                        emit_str("typedef ");
                        i = j;
                        continue;
                    }
                }

                /* Simple typedef alias: typedef TYPE NAME; → drop as comment */
                if (j < token_count && tokens[j].type == CT_IDENT) {
                    emit_str("// typedef (dropped): ");
                    while (i < token_count && tokens[i].type != CT_SEMICOLON &&
                           tokens[i].type != CT_EOF) {
                        emit_tok(&tokens[i]);
                        i++;
                    }
                    if (i < token_count && tokens[i].type == CT_SEMICOLON) {
                        emit_str(";");
                        i++;
                    }
                    emit_str("\n");
                    continue;
                }

                /* Fallback: mark as manual */
                emit_str("// MANUAL: ");
                while (i < token_count && tokens[i].type != CT_SEMICOLON &&
                       tokens[i].type != CT_EOF) {
                    emit_tok(&tokens[i]);
                    i++;
                }
                if (i < token_count && tokens[i].type == CT_SEMICOLON) {
                    emit_str(";");
                    i++;
                }
                emit_str("\n");
                continue;
            }

            /* Two-token type combos: unsigned int → u32, etc.
             * Must check BEFORE single-token map to avoid double-mapping. */
            {
                bool found_combo = false;
                for (int ci = 0; type_combos[ci].first; ci++) {
                    if (tok_eq(t, type_combos[ci].first)) {
                        int j = skip_spaces(i + 1);
                        if (j < token_count && tokens[j].type == CT_IDENT &&
                            tok_eq(&tokens[j], type_combos[ci].second)) {
                            /* Check for pointer rearrangement */
                            int next = try_ptr_rearrange(j, type_combos[ci].zer);
                            if (next >= 0) {
                                i = next;
                            } else {
                                emit_str(type_combos[ci].zer);
                                i = j + 1;
                            }
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
                    int next = try_ptr_rearrange(i, "u32");
                    if (next >= 0) { i = next; continue; }
                    emit_str("u32");
                    i++;
                    continue;
                }
            }

            /* standalone 'int' (not part of a combo already handled) → i32 */
            if (tok_eq(t, "int")) {
                int next = try_ptr_rearrange(i, "i32");
                if (next >= 0) { i = next; continue; }
                emit_str("i32");
                i++;
                continue;
            }

            /* standalone 'long' (not part of combo) → i64 */
            if (tok_eq(t, "long")) {
                int next = try_ptr_rearrange(i, "i64");
                if (next >= 0) { i = next; continue; }
                emit_str("i64");
                i++;
                continue;
            }

            /* standalone 'short' (not part of combo) → i16 */
            if (tok_eq(t, "short")) {
                int next = try_ptr_rearrange(i, "i16");
                if (next >= 0) { i = next; continue; }
                emit_str("i16");
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
                    /* Check for struct Name *var → *Name var */
                    {
                        int m = j + 1;
                        while (m < token_count && tokens[m].type == CT_WHITESPACE) m++;
                        int star_count = 0;
                        while (m < token_count && tokens[m].type == CT_STAR) {
                            star_count++;
                            m++;
                            while (m < token_count && tokens[m].type == CT_WHITESPACE) m++;
                        }
                        if (star_count > 0 && m < token_count && tokens[m].type == CT_IDENT) {
                            int after = m + 1;
                            while (after < token_count && tokens[after].type == CT_WHITESPACE) after++;
                            CTokenType at = after < token_count ? tokens[after].type : CT_EOF;
                            if (at == CT_SEMICOLON || at == CT_EQ || at == CT_COMMA ||
                                at == CT_RPAREN || at == CT_LPAREN || at == CT_LBRACKET) {
                                for (int s = 0; s < star_count; s++) emit_str("*");
                                emit_tok(&tokens[j]); /* Name */
                                emit_str(" ");
                                i = m; /* continue from var name */
                                continue;
                            }
                        }
                    }
                    /* struct usage without pointer: struct Node → Node */
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

            /* I/O functions — keep as cinclude calls but tag them */
            if (tok_eq(t, "printf")) { emit_str("zer_printf"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "fprintf")) { emit_str("zer_fprintf"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "sprintf")) { emit_str("zer_sprintf"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "snprintf")) { emit_str("zer_snprintf"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "puts")) { emit_str("zer_puts"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "fputs")) { emit_str("zer_fputs"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "fopen")) { emit_str("zer_fopen"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "fclose")) { emit_str("zer_fclose"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "fread")) { emit_str("zer_fread"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "fwrite")) { emit_str("zer_fwrite"); needs_compat = true; i++; continue; }

            /* exit() → zer_exit() */
            if (tok_eq(t, "exit")) { emit_str("zer_exit"); needs_compat = true; i++; continue; }

            /* Type-mapped identifier */
            if (mapped) {
                int next = try_ptr_rearrange(i, mapped);
                if (next >= 0) { i = next; continue; }
                emit_str(mapped);
                i++;
                continue;
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
            /* check for (type *) pattern — exclude (void *) in func ptr params */
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
                            /* strip 'struct' keyword inside args */
                            if (tokens[i].type == CT_IDENT && tok_eq(&tokens[i], "struct")) {
                                i++;
                                while (i < token_count && tokens[i].type == CT_WHITESPACE) i++;
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

        /* ---- Brace tracking for switch/loop depth ---- */
        if (t->type == CT_LBRACE) {
            brace_depth++;
            if (skip_typedef_trailer && typedef_body_depth >= 0) {
                typedef_body_depth++;
            }
            if (next_brace_is_switch && switch_depth < 32) {
                sw_stack[switch_depth].brace = brace_depth;
                sw_stack[switch_depth].arm_open = false;
                switch_depth++;
                next_brace_is_switch = false;
                next_brace_is_loop = false;
            } else if (next_brace_is_loop && loop_depth < 32) {
                loop_brace[loop_depth] = brace_depth;
                loop_depth++;
                next_brace_is_loop = false;
            }
            emit_str("{");
            i++;
            continue;
        }
        if (t->type == CT_RBRACE) {
            /* Check if closing a typedef body */
            if (skip_typedef_trailer && typedef_body_depth > 0) {
                typedef_body_depth--;
                if (typedef_body_depth == 0) {
                    /* Body closed — emit } and start skipping trailer */
                    typedef_body_depth = -1;
                    brace_depth--;
                    emit_str("}\n");
                    i++;
                    continue;
                }
            }
            /* Check if closing a switch body */
            if (switch_depth > 0 && brace_depth == sw_stack[switch_depth - 1].brace) {
                if (sw_stack[switch_depth - 1].arm_open) {
                    /* Trim trailing whitespace, emit arm close at case-level indent */
                    emit_trim_trailing_ws();
                    emit_str("\n");
                    emit_raw(sw_stack[switch_depth - 1].indent,
                             sw_stack[switch_depth - 1].indent_len);
                    emit_str("}\n");
                    sw_stack[switch_depth - 1].arm_open = false;
                    /* Re-emit the switch-level indent (was trimmed above) */
                    const char *sw_indent = "";
                    int sw_indent_len = 0;
                    for (int p = i - 1; p >= 0; p--) {
                        if (tokens[p].type == CT_WHITESPACE) {
                            sw_indent = tokens[p].start;
                            sw_indent_len = tokens[p].len;
                            break;
                        }
                        if (tokens[p].type == CT_NEWLINE) break;
                    }
                    emit_raw(sw_indent, sw_indent_len);
                }
                switch_depth--;
            }
            /* Check if closing a loop body */
            if (loop_depth > 0 && brace_depth == loop_brace[loop_depth - 1]) {
                loop_depth--;
            }
            brace_depth--;
            emit_str("}");
            i++;
            continue;
        }

        /* ---- Ternary operator: mark as MANUAL ---- */
        if (t->type == CT_QUESTION) {
            emit_str("/* MANUAL: rewrite ternary as if/else */ ?");
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

static char *read_file(const char *path, int *file_len) {
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
    *file_len = (int)len;
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

    /* Transform to buffer */
    transform();

    /* Write output file */
    FILE *outf = fopen(output_path, "w");
    if (!outf) {
        fprintf(stderr, "zer-convert: cannot write '%s'\n", output_path);
        free(src);
        return 1;
    }

    if (needs_compat) {
        fprintf(outf, "// Converted from %s by zer-convert\n", input_path);
        fprintf(outf, "// Uses compat.zer — run 'zerc --safe-upgrade' to replace with safe ZER\n\n");
        fprintf(outf, "import compat;\n\n");
    } else {
        fprintf(outf, "// Converted from %s by zer-convert\n", input_path);
        fprintf(outf, "// Review MANUAL: comments for items needing attention\n\n");
    }

    fwrite(out_buf, 1, out_len, outf);
    fclose(outf);
    free(out_buf);
    free(src);

    printf("zer-convert: %s -> %s", input_path, output_path);
    if (needs_compat) printf(" (uses compat.zer)");
    printf("\n");

    return 0;
}
