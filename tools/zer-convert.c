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
    { "uintptr_t", 9, "usize" },
    { "intptr_t",  8, "usize" },
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
static bool needs_extract = false; /* set true if cinclude extraction used */

/* Extraction buffer — collects C code that can't be converted to ZER */
#define EXTRACT_CAP (64 * 1024)
static char extract_buf[EXTRACT_CAP];
static int extract_len = 0;

static void extract_write(const char *s, int len) {
    if (extract_len + len < EXTRACT_CAP) {
        memcpy(extract_buf + extract_len, s, len);
        extract_len += len;
    }
}
static void extract_str(const char *s) { extract_write(s, (int)strlen(s)); }
static void extract_tok(CToken *t) { extract_write(t->start, t->len); }
static int switch_depth = 0;       /* nesting depth of switch statements */
static int switch_arm_depth[16];   /* which depth levels have an open arm */
static int switch_arm_top = 0;     /* stack top for open arm tracking */

static bool in_switch_arm_at_current_depth(void) {
    return switch_arm_top > 0 && switch_arm_depth[switch_arm_top - 1] == switch_depth;
}
static void push_switch_arm(void) {
    if (switch_arm_top < 16) switch_arm_depth[switch_arm_top++] = switch_depth;
}
static void pop_switch_arm(void) {
    if (switch_arm_top > 0 && switch_arm_depth[switch_arm_top - 1] == switch_depth)
        switch_arm_top--;
}

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

/* forward declarations for helpers used in classify_params */
static int skip_ws(int i);
static int skip_spaces(int i);

/* ================================================================
 * Pre-scan: classify char* params/vars as slice vs pointer
 * Also detect nullable pointers and pointer arithmetic
 * ================================================================ */

#define MAX_PARAM_INFO 256

typedef struct {
    char name[64];       /* param/var name */
    int func_idx;        /* token index of function body { */
    int func_end;        /* token index of function body } */
    bool is_const;       /* was const char * */
    bool is_slice;       /* classified as []u8 */
    bool is_nullable;    /* should be ?*T (uses null checks/assigns) */
    bool is_ambiguous;   /* zero usage clues — extract function to .h */
} ParamInfo;

static ParamInfo param_infos[MAX_PARAM_INFO];
static int param_info_count = 0;

/* check if param at given name should be a slice in a function scope.
 * tok_idx can be in the param list (before func_idx) or in the body. */
static ParamInfo *find_param_info(const char *name, int name_len, int tok_idx) {
    for (int i = 0; i < param_info_count; i++) {
        if ((int)strlen(param_infos[i].name) == name_len &&
            memcmp(param_infos[i].name, name, name_len) == 0 &&
            tok_idx <= param_infos[i].func_end)
            return &param_infos[i];
    }
    return NULL;
}

/* check if token i..i+N matches a name */
static bool tokens_match_name(int idx, const char *name) {
    if (idx < 0 || idx >= token_count) return false;
    return tok_eq(&tokens[idx], name);
}

/* Pre-scan pass: find char-ptr and const-char-ptr params, analyze usage, classify */
static void classify_params(void) {
    param_info_count = 0;

    for (int i = 0; i < token_count; i++) {
        /* find function declarations: type name ( params ) { */
        /* look for ( that could start a param list */
        if (tokens[i].type != CT_LPAREN) continue;
        if (i < 2) continue;

        /* check if preceded by: IDENT IDENT( or IDENT *IDENT( pattern */
        int fname = -1;
        { int k = i - 1;
          while (k > 0 && tokens[k].type == CT_WHITESPACE) k--;
          if (k >= 0 && tokens[k].type == CT_IDENT) fname = k;
        }
        if (fname < 0) continue;

        /* find matching ) and then { */
        int depth = 1, close = i + 1;
        while (close < token_count && depth > 0) {
            if (tokens[close].type == CT_LPAREN) depth++;
            if (tokens[close].type == CT_RPAREN) depth--;
            close++;
        }
        /* close is past ). Find { */
        int brace = skip_ws(close);
        if (brace >= token_count || tokens[brace].type != CT_LBRACE) continue;

        /* find matching } for function body */
        int body_end = brace + 1;
        depth = 1;
        while (body_end < token_count && depth > 0) {
            if (tokens[body_end].type == CT_LBRACE) depth++;
            if (tokens[body_end].type == CT_RBRACE) depth--;
            body_end++;
        }

        /* scan params for char-ptr and const-char-ptr patterns */
        int p = i + 1;
        while (p < close - 1) {
            if (tokens[p].type == CT_WHITESPACE || tokens[p].type == CT_COMMA) { p++; continue; }

            /* detect const */
            bool is_const = false;
            if (tokens[p].type == CT_IDENT && tok_eq(&tokens[p], "const")) {
                is_const = true;
                p++; while (p < close && tokens[p].type == CT_WHITESPACE) p++;
            }

            /* detect char-ptr or void-ptr type */
            bool is_char_ptr = false;
            if (p < close && tokens[p].type == CT_IDENT &&
                (tok_eq(&tokens[p], "char") || tok_eq(&tokens[p], "void"))) {
                int k = skip_spaces(p + 1);
                if (k < close && tokens[k].type == CT_STAR) {
                    is_char_ptr = true;
                    p = k + 1;
                    while (p < close && tokens[p].type == CT_WHITESPACE) p++;
                }
            }

            if (!is_char_ptr) { p++; continue; }

            /* read param name */
            if (p >= close || tokens[p].type != CT_IDENT) { p++; continue; }
            if (param_info_count >= MAX_PARAM_INFO) break;

            ParamInfo *pi = &param_infos[param_info_count];
            memset(pi, 0, sizeof(*pi));
            int nlen = tokens[p].len < 63 ? tokens[p].len : 63;
            memcpy(pi->name, tokens[p].start, nlen);
            pi->name[nlen] = '\0';
            pi->is_const = is_const;
            pi->func_idx = brace;
            pi->func_end = body_end;
            pi->is_slice = false;
            pi->is_nullable = false;

            /* scan function body for usage patterns */
            for (int b = brace + 1; b < body_end - 1; b++) {
                if (tokens[b].type != CT_IDENT) continue;
                if (!tok_eq(&tokens[b], pi->name)) continue;

                /* check what follows the name */
                int after = skip_spaces(b + 1);

                /* name[...] → indexing → slice */
                if (after < body_end && tokens[after].type == CT_LBRACKET) {
                    pi->is_slice = true;
                }
                /* name == NULL / name != NULL → nullable */
                if (after + 1 < body_end && tokens[after].type == CT_EQEQ) {
                    int val = skip_spaces(after + 1);
                    if (val < body_end && tokens[val].type == CT_IDENT && tok_eq(&tokens[val], "NULL"))
                        pi->is_nullable = true;
                }
                if (after + 1 < body_end && tokens[after].type == CT_BANGEQ) {
                    int val = skip_spaces(after + 1);
                    if (val < body_end && tokens[val].type == CT_IDENT && tok_eq(&tokens[val], "NULL"))
                        pi->is_nullable = true;
                }
                /* name = NULL → nullable */
                if (after < body_end && tokens[after].type == CT_EQ) {
                    int val = skip_spaces(after + 1);
                    if (val < body_end && tokens[val].type == CT_IDENT && tok_eq(&tokens[val], "NULL"))
                        pi->is_nullable = true;
                }

                /* check what precedes — if(!name) or if(name) → nullable */
                int before = b - 1;
                while (before > brace && tokens[before].type == CT_WHITESPACE) before--;
                if (before > brace && tokens[before].type == CT_BANG)
                    pi->is_nullable = true;
                if (before > brace && tokens[before].type == CT_LPAREN) {
                    /* if(name) pattern — check if preceded by if/while */
                    int kw = before - 1;
                    while (kw > brace && tokens[kw].type == CT_WHITESPACE) kw--;
                    if (kw > brace && tokens[kw].type == CT_IDENT &&
                        (tok_eq(&tokens[kw], "if") || tok_eq(&tokens[kw], "while")))
                        pi->is_nullable = true;
                }

                /* check if name is arg to strlen/strcmp/printf → string → slice */
                if (before > brace && tokens[before].type == CT_LPAREN) {
                    int fn = before - 1;
                    while (fn > brace && tokens[fn].type == CT_WHITESPACE) fn--;
                    if (fn > brace && tokens[fn].type == CT_IDENT) {
                        if (tok_eq(&tokens[fn], "strlen") || tok_eq(&tokens[fn], "strcmp") ||
                            tok_eq(&tokens[fn], "strncmp") || tok_eq(&tokens[fn], "strcpy") ||
                            tok_eq(&tokens[fn], "strcat") || tok_eq(&tokens[fn], "puts") ||
                            tok_eq(&tokens[fn], "printf") || tok_eq(&tokens[fn], "fputs"))
                            pi->is_slice = true;
                    }
                }
                /* also as second arg: strcmp(other, name) */
                if (before > brace && tokens[before].type == CT_COMMA) {
                    /* walk back to find function name before ( */
                    int scan = before - 1;
                    int pdepth = 0;
                    while (scan > brace) {
                        if (tokens[scan].type == CT_RPAREN) pdepth++;
                        if (tokens[scan].type == CT_LPAREN) {
                            if (pdepth == 0) break;
                            pdepth--;
                        }
                        scan--;
                    }
                    /* scan is at (. Check token before it. */
                    int fn = scan - 1;
                    while (fn > brace && tokens[fn].type == CT_WHITESPACE) fn--;
                    if (fn > brace && tokens[fn].type == CT_IDENT) {
                        if (tok_eq(&tokens[fn], "strcmp") || tok_eq(&tokens[fn], "strncmp") ||
                            tok_eq(&tokens[fn], "memcpy") || tok_eq(&tokens[fn], "memcmp"))
                            pi->is_slice = true;
                    }
                }
            }

            /* const char * without counter-evidence → slice (convention) */
            if (is_const && !pi->is_slice) {
                pi->is_slice = true; /* const char* is almost always a string */
            }

            /* non-const, no usage clues at all → ambiguous, extract function */
            if (!is_const && !pi->is_slice && !pi->is_nullable) {
                pi->is_ambiguous = true;
            }

            param_info_count++;
            p++;
        }
    }
}

/* ================================================================
 * Pre-scan: mark functions/structs needing cinclude extraction
 * (contain goto, ternary, inline asm, or bit fields)
 * ================================================================ */

#define MAX_EXTRACT 64
typedef struct {
    int start;      /* token index of the start (function return type or struct keyword) */
    int end;        /* token index past the closing } */
    bool is_func;   /* true = function, false = struct */
    char name[64];  /* function or struct name */
} ExtractRange;

static ExtractRange extracts[MAX_EXTRACT];
static int extract_count = 0;

static bool is_in_extract(int tok_idx) {
    for (int i = 0; i < extract_count; i++) {
        if (tok_idx >= extracts[i].start && tok_idx < extracts[i].end)
            return true;
    }
    return false;
}

static void scan_for_extractions(void) {
    extract_count = 0;

    for (int i = 0; i < token_count; i++) {
        /* find struct definitions with bit fields: struct Name { ... : N ... } */
        if (tokens[i].type == CT_IDENT &&
            (tok_eq(&tokens[i], "struct") || tok_eq(&tokens[i], "union"))) {
            int j = skip_spaces(i + 1);
            /* skip optional name */
            if (j < token_count && tokens[j].type == CT_IDENT) j = skip_spaces(j + 1);
            if (j >= token_count || tokens[j].type != CT_LBRACE) continue;
            /* scan body for : N (bit field) */
            int depth = 1;
            int k = j + 1;
            bool has_bitfield = false;
            while (k < token_count && depth > 0) {
                if (tokens[k].type == CT_LBRACE) depth++;
                if (tokens[k].type == CT_RBRACE) depth--;
                if (tokens[k].type == CT_COLON && depth == 1) {
                    /* check if followed by a number (bit width) */
                    int m = skip_spaces(k + 1);
                    if (m < token_count && tokens[m].type == CT_NUMBER)
                        has_bitfield = true;
                }
                k++;
            }
            if (has_bitfield && extract_count < MAX_EXTRACT) {
                /* find the start — could be preceded by typedef */
                int start = i;
                if (start > 0) {
                    int prev = start - 1;
                    while (prev > 0 && tokens[prev].type == CT_WHITESPACE) prev--;
                    if (prev >= 0 && tokens[prev].type == CT_IDENT && tok_eq(&tokens[prev], "typedef"))
                        start = prev;
                }
                ExtractRange *er = &extracts[extract_count++];
                er->start = start;
                er->end = k; /* past } */
                /* skip ; after } */
                int semi = skip_spaces(k);
                if (semi < token_count && tokens[semi].type == CT_SEMICOLON) er->end = semi + 1;
                /* skip typedef name + ; after } */
                if (semi < token_count && tokens[semi].type == CT_IDENT) {
                    int semi2 = skip_spaces(semi + 1);
                    if (semi2 < token_count && tokens[semi2].type == CT_SEMICOLON)
                        er->end = semi2 + 1;
                }
                er->is_func = false;
                /* extract struct name */
                int nj = skip_spaces(i + 1);
                if (nj < token_count && tokens[nj].type == CT_IDENT &&
                    skip_spaces(nj + 1) < token_count && tokens[skip_spaces(nj + 1)].type == CT_LBRACE) {
                    int nl = tokens[nj].len < 63 ? tokens[nj].len : 63;
                    memcpy(er->name, tokens[nj].start, nl);
                    er->name[nl] = '\0';
                } else {
                    strcpy(er->name, "anonymous");
                }
            }
        }

        /* find function definitions containing goto, ternary, or inline asm */
        if (tokens[i].type == CT_LBRACE) {
            /* check if this { is a function body (preceded by ) ) */
            int prev = i - 1;
            while (prev > 0 && (tokens[prev].type == CT_WHITESPACE || tokens[prev].type == CT_NEWLINE)) prev--;
            if (prev < 0 || tokens[prev].type != CT_RPAREN) continue;

            /* scan body for goto, ?, asm */
            int depth = 1;
            int k = i + 1;
            bool has_goto = false, has_ternary = false, has_asm = false;
            bool has_ambiguous_param = false;
            while (k < token_count && depth > 0) {
                if (tokens[k].type == CT_LBRACE) depth++;
                if (tokens[k].type == CT_RBRACE) depth--;
                if (tokens[k].type == CT_IDENT && tok_eq(&tokens[k], "goto")) has_goto = true;
                if (tokens[k].type == CT_QUESTION) has_ternary = true;
                if (tokens[k].type == CT_IDENT &&
                    (tok_eq(&tokens[k], "asm") || tok_eq(&tokens[k], "__asm__") || tok_eq(&tokens[k], "__asm")))
                    has_asm = true;
                k++;
            }
            /* check if any param in this function is ambiguous */
            for (int pi = 0; pi < param_info_count; pi++) {
                if (param_infos[pi].is_ambiguous &&
                    param_infos[pi].func_idx == i)
                    has_ambiguous_param = true;
            }
            if ((has_goto || has_ternary || has_asm || has_ambiguous_param) && extract_count < MAX_EXTRACT) {
                /* walk backward to find function start (return type) */
                /* from the ), walk back past params to ( */
                int paren = prev;
                int pdepth = 1;
                paren--;
                while (paren > 0 && pdepth > 0) {
                    if (tokens[paren].type == CT_RPAREN) pdepth++;
                    if (tokens[paren].type == CT_LPAREN) pdepth--;
                    paren--;
                }
                paren++; /* at ( */
                /* function name is before ( */
                int fname = paren - 1;
                while (fname > 0 && tokens[fname].type == CT_WHITESPACE) fname--;
                /* return type is before function name */
                int rtype = fname - 1;
                while (rtype > 0 && tokens[rtype].type == CT_WHITESPACE) rtype--;
                /* walk back to start of return type (could be multi-token: unsigned long) */
                while (rtype > 0 && (tokens[rtype].type == CT_IDENT || tokens[rtype].type == CT_STAR ||
                       tokens[rtype].type == CT_WHITESPACE)) rtype--;
                rtype++; /* first token of return type */
                /* skip leading whitespace */
                while (rtype < fname && tokens[rtype].type == CT_WHITESPACE) rtype++;

                ExtractRange *er = &extracts[extract_count++];
                er->start = rtype;
                er->end = k; /* past } */
                er->is_func = true;
                int nl = tokens[fname].len < 63 ? tokens[fname].len : 63;
                memcpy(er->name, tokens[fname].start, nl);
                er->name[nl] = '\0';
            }
        }
    }
}

static void emit_raw(const char *s, int len) {
    fwrite(s, 1, len, out);
}

static void emit_str(const char *s) {
    fputs(s, out);
}

static void emit_tok(CToken *t) {
    /* strip C number suffixes (U, L, UL, ULL, LL, u, l) from numeric literals */
    if (t->type == CT_NUMBER) {
        int len = t->len;
        while (len > 0 && (t->start[len-1] == 'u' || t->start[len-1] == 'U' ||
               t->start[len-1] == 'l' || t->start[len-1] == 'L'))
            len--;
        if (len > 0) fwrite(t->start, 1, len, out);
        else fwrite(t->start, 1, t->len, out); /* safety: don't emit empty */
        return;
    }
    fwrite(t->start, 1, t->len, out);
}

/* try to rearrange pointer declaration: after emitting a type like "i32",
 * check if followed by * name (declaration context). If so, emit *type name
 * and return new position. Otherwise return -1.
 * Declaration context: name followed by =, ;, ,, ), [ */
static int try_ptr_rearrange(int pos, const char *type_str) {
    int j = pos;
    /* collect all * (handles **ptr, ***ptr) */
    int star_count = 0;
    while (j < token_count) {
        int k = skip_spaces(j);
        if (k < token_count && tokens[k].type == CT_STAR) {
            star_count++;
            j = k + 1;
        } else break;
    }
    if (star_count == 0) return -1;
    /* check for identifier after the stars */
    int name_pos = skip_spaces(j);
    if (name_pos >= token_count || tokens[name_pos].type != CT_IDENT) return -1;
    /* check what follows the name — must be declaration context */
    int after_name = skip_spaces(name_pos + 1);
    if (after_name >= token_count) return -1;
    CTokenType at = tokens[after_name].type;
    if (at != CT_EQ && at != CT_SEMICOLON && at != CT_COMMA &&
        at != CT_RPAREN && at != CT_LBRACKET && at != CT_LPAREN) return -1;
    /* rearrange: emit *...*type_str name */
    for (int s = 0; s < star_count; s++) emit_str("*");
    emit_str(type_str);
    emit_str(" ");
    emit_tok(&tokens[name_pos]);
    return after_name; /* caller continues from here */
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

        /* ---- Extract functions/structs that can't be converted to ZER ---- */
        /* Copy them verbatim to the .h extraction file, emit declaration in .zer */
        {
            for (int ei = 0; ei < extract_count; ei++) {
                if (i == extracts[ei].start) {
                    needs_extract = true;
                    /* write original C tokens to extract buffer */
                    for (int x = extracts[ei].start; x < extracts[ei].end; x++) {
                        extract_tok(&tokens[x]);
                    }
                    extract_str("\n");

                    if (extracts[ei].is_func) {
                        /* emit ZER declaration: return_type func_name(params); */
                        /* find ( and ) to extract the signature */
                        int x = extracts[ei].start;
                        while (x < extracts[ei].end && tokens[x].type != CT_LPAREN) {
                            /* map types in the return type + name */
                            if (tokens[x].type == CT_IDENT) {
                                const char *mt = map_type(&tokens[x]);
                                if (mt) emit_str(mt);
                                else emit_tok(&tokens[x]);
                            } else {
                                emit_tok(&tokens[x]);
                            }
                            x++;
                        }
                        /* emit (params) with type mapping */
                        if (x < extracts[ei].end) {
                            emit_str("(");
                            x++; /* skip ( */
                            while (x < extracts[ei].end && tokens[x].type != CT_RPAREN) {
                                if (tokens[x].type == CT_IDENT) {
                                    const char *mt = map_type(&tokens[x]);
                                    if (mt) emit_str(mt);
                                    else emit_tok(&tokens[x]);
                                } else if (tokens[x].type == CT_ARROW) {
                                    emit_str(".");
                                } else {
                                    emit_tok(&tokens[x]);
                                }
                                x++;
                            }
                            emit_str(")");
                        }
                        emit_str(";\n");
                    } else {
                        /* struct with bit fields — emit comment that it's in .h */
                        emit_str("// struct ");
                        emit_str(extracts[ei].name);
                        emit_str(" — in extracted .h (has bit fields)\n");
                    }

                    i = extracts[ei].end;
                    goto next_token;
                }
            }
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
                            /* Check for stringify (#) or token paste (##) in macro body.
                             * These can't be expressed as comptime — extract to .h */
                            {
                                bool has_unconvertible = false;
                                int scan = j;
                                /* scan params for ... (variadic) */
                                int pdepth = 1;
                                scan++;
                                while (scan < token_count && pdepth > 0) {
                                    if (tokens[scan].type == CT_LPAREN) pdepth++;
                                    if (tokens[scan].type == CT_RPAREN) pdepth--;
                                    if (tokens[scan].type == CT_DOT) {
                                        /* check for ... (three dots) */
                                        if (scan + 2 < token_count &&
                                            tokens[scan+1].type == CT_DOT &&
                                            tokens[scan+2].type == CT_DOT)
                                            has_unconvertible = true;
                                    }
                                    if (tokens[scan].type == CT_IDENT &&
                                        tok_eq(&tokens[scan], "__VA_ARGS__"))
                                        has_unconvertible = true;
                                    scan++;
                                }
                                /* scan body for # (stringify) or ## (token paste) or __VA_ARGS__ */
                                int bscan = scan;
                                while (bscan < token_count && tokens[bscan].type != CT_NEWLINE &&
                                       tokens[bscan].type != CT_EOF) {
                                    if (tokens[bscan].type == CT_HASH) { has_unconvertible = true; break; }
                                    if (tokens[bscan].type == CT_IDENT &&
                                        tok_eq(&tokens[bscan], "__VA_ARGS__"))
                                        { has_unconvertible = true; break; }
                                    bscan++;
                                }
                                if (has_unconvertible) {
                                    /* extract to companion .h — can't convert to comptime */
                                    needs_extract = true;
                                    extract_str("#define ");
                                    extract_tok(name);
                                    /* extract rest of line (params + body) */
                                    while (j < token_count && tokens[j].type != CT_NEWLINE &&
                                           tokens[j].type != CT_EOF) {
                                        /* handle line continuation */
                                        if (tokens[j].type == CT_UNKNOWN && tokens[j].len == 1 &&
                                            tokens[j].start[0] == '\\') {
                                            extract_str("\\\n");
                                            j++;
                                            if (j < token_count && tokens[j].type == CT_NEWLINE) j++;
                                            continue;
                                        }
                                        extract_tok(&tokens[j]); j++;
                                    }
                                    extract_str("\n");
                                    /* emit declaration in .zer so the macro name is usable */
                                    emit_str("// extracted to .h: ");
                                    emit_tok(name);
                                    emit_str(" (stringify/variadic macro)\n");
                                    if (j < token_count && tokens[j].type == CT_NEWLINE) j++;
                                    i = j;
                                    continue;
                                }
                            }
                            /* function-like macro → comptime function
                             * #define MAX(a, b) ((a) > (b) ? (a) : (b))
                             * → comptime u32 MAX(u32 a, u32 b) { return (a) > (b) ? (a) : (b); } */
                            emit_str("comptime u32 ");
                            emit_tok(name);
                            emit_str("(");
                            j++; /* skip ( */
                            /* collect param names */
                            bool first_param = true;
                            while (j < token_count && tokens[j].type != CT_RPAREN &&
                                   tokens[j].type != CT_NEWLINE && tokens[j].type != CT_EOF) {
                                if (tokens[j].type == CT_COMMA) {
                                    emit_str(", ");
                                    j++;
                                    continue;
                                }
                                if (tokens[j].type == CT_WHITESPACE) { j++; continue; }
                                if (tokens[j].type == CT_IDENT) {
                                    if (!first_param) { /* already emitted comma */ }
                                    emit_str("u32 ");
                                    emit_tok(&tokens[j]);
                                    first_param = false;
                                }
                                j++;
                            }
                            if (j < token_count && tokens[j].type == CT_RPAREN) j++;
                            emit_str(") {\n    return ");
                            /* skip whitespace after ) */
                            while (j < token_count && tokens[j].type == CT_WHITESPACE) j++;
                            /* emit the macro body expression */
                            while (j < token_count && tokens[j].type != CT_NEWLINE && tokens[j].type != CT_EOF) {
                                /* handle line continuation \ */
                                if (tokens[j].type == CT_UNKNOWN && tokens[j].len == 1 && tokens[j].start[0] == '\\') {
                                    j++;
                                    if (j < token_count && tokens[j].type == CT_NEWLINE) j++;
                                    continue;
                                }
                                emit_tok(&tokens[j]);
                                j++;
                            }
                            emit_str(";\n}\n");
                            if (j < token_count && tokens[j].type == CT_NEWLINE) j++;
                            i = j;
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
                        /* fallback: #define NAME expr (non-numeric expression)
                         * → comptime u32 NAME() { return expr; }
                         * If empty body (guard macro), emit as const */
                        j = skip_ws(j);
                        if (j >= token_count || tokens[j].type == CT_NEWLINE || tokens[j].type == CT_EOF) {
                            /* empty #define — guard macro or flag, emit as const bool */
                            emit_str("const bool ");
                            emit_tok(name);
                            emit_str(" = true;\n");
                            if (j < token_count && tokens[j].type == CT_NEWLINE) j++;
                            i = j;
                            continue;
                        }
                        /* has expression body — emit as comptime */
                        emit_str("comptime u32 ");
                        emit_tok(name);
                        emit_str("() {\n    return ");
                        while (j < token_count && tokens[j].type != CT_NEWLINE && tokens[j].type != CT_EOF) {
                            if (tokens[j].type == CT_UNKNOWN && tokens[j].len == 1 && tokens[j].start[0] == '\\') {
                                j++;
                                if (j < token_count && tokens[j].type == CT_NEWLINE) j++;
                                continue;
                            }
                            emit_tok(&tokens[j]);
                            j++;
                        }
                        emit_str(";\n}\n");
                        if (j < token_count && tokens[j].type == CT_NEWLINE) j++;
                        i = j;
                        continue;
                    }
                }
                /* #ifdef NAME → comptime if (NAME) {
                 * Include guard detection: #ifndef NAME followed by #define NAME
                 * on the next preprocessor line → strip both (ZER uses import) */
                if (tok_eq(&tokens[j], "ifndef")) {
                    int nj = skip_ws(j + 1);
                    if (nj < token_count && tokens[nj].type == CT_IDENT) {
                        /* look ahead for #define SAME_NAME on next line */
                        int guard_name = nj;
                        int peek = nj + 1;
                        /* skip to next line */
                        while (peek < token_count && tokens[peek].type != CT_NEWLINE) peek++;
                        if (peek < token_count) peek++; /* skip newline */
                        /* skip whitespace */
                        while (peek < token_count && tokens[peek].type == CT_WHITESPACE) peek++;
                        /* check for # define SAME_NAME */
                        if (peek < token_count && tokens[peek].type == CT_HASH) {
                            int dp = skip_ws(peek + 1);
                            if (dp < token_count && tok_eq(&tokens[dp], "define")) {
                                int dn = skip_ws(dp + 1);
                                if (dn < token_count && tokens[dn].type == CT_IDENT &&
                                    tokens[dn].len == tokens[guard_name].len &&
                                    memcmp(tokens[dn].start, tokens[guard_name].start, tokens[dn].len) == 0) {
                                    /* check that #define has empty body (guard macro) */
                                    int after_def = dn + 1;
                                    while (after_def < token_count && tokens[after_def].type == CT_WHITESPACE) after_def++;
                                    if (after_def >= token_count || tokens[after_def].type == CT_NEWLINE ||
                                        tokens[after_def].type == CT_EOF) {
                                        /* include guard pattern detected — strip both lines */
                                        emit_str("// include guard: ");
                                        emit_tok(&tokens[guard_name]);
                                        emit_str(" (stripped)\n");
                                        /* skip past #define line */
                                        int skip = after_def;
                                        if (skip < token_count && tokens[skip].type == CT_NEWLINE) skip++;
                                        i = skip;
                                        continue;
                                    }
                                }
                            }
                        }
                    }
                    /* not a guard — fall through to regular #ifndef handling below */
                }
                /* #ifdef NAME → comptime if (NAME) { */
                if (tok_eq(&tokens[j], "ifdef")) {
                    j++;
                    j = skip_ws(j);
                    if (j < token_count && tokens[j].type == CT_IDENT) {
                        emit_str("comptime if (");
                        emit_tok(&tokens[j]);
                        emit_str(") {\n");
                        j++;
                    }
                    while (j < token_count && tokens[j].type != CT_NEWLINE && tokens[j].type != CT_EOF) j++;
                    if (j < token_count && tokens[j].type == CT_NEWLINE) j++;
                    i = j;
                    continue;
                }
                /* #ifndef NAME → comptime if (!NAME) { */
                if (tok_eq(&tokens[j], "ifndef")) {
                    j++;
                    j = skip_ws(j);
                    if (j < token_count && tokens[j].type == CT_IDENT) {
                        emit_str("comptime if (!");
                        emit_tok(&tokens[j]);
                        emit_str(") {\n");
                        j++;
                    }
                    while (j < token_count && tokens[j].type != CT_NEWLINE && tokens[j].type != CT_EOF) j++;
                    if (j < token_count && tokens[j].type == CT_NEWLINE) j++;
                    i = j;
                    continue;
                }
                /* #endif → } */
                if (tok_eq(&tokens[j], "endif")) {
                    emit_str("}\n");
                    j++;
                    while (j < token_count && tokens[j].type != CT_NEWLINE && tokens[j].type != CT_EOF) j++;
                    if (j < token_count && tokens[j].type == CT_NEWLINE) j++;
                    i = j;
                    continue;
                }
                /* #else → } else { */
                if (tok_eq(&tokens[j], "else")) {
                    emit_str("} else {\n");
                    j++;
                    while (j < token_count && tokens[j].type != CT_NEWLINE && tokens[j].type != CT_EOF) j++;
                    if (j < token_count && tokens[j].type == CT_NEWLINE) j++;
                    i = j;
                    continue;
                }
                /* #if expr → comptime if (expr) {
                 * Expand defined(X) → X and defined X → X in the expression */
                if (tok_eq(&tokens[j], "if")) {
                    emit_str("comptime if (");
                    j++;
                    j = skip_ws(j);
                    while (j < token_count && tokens[j].type != CT_NEWLINE && tokens[j].type != CT_EOF) {
                        /* expand defined(X) → X or defined X → X */
                        if (tokens[j].type == CT_IDENT && tok_eq(&tokens[j], "defined")) {
                            int d = skip_spaces(j + 1);
                            if (d < token_count && tokens[d].type == CT_LPAREN) {
                                /* defined(X) → X */
                                int e = skip_spaces(d + 1);
                                if (e < token_count && tokens[e].type == CT_IDENT) {
                                    emit_tok(&tokens[e]);
                                    int f = skip_spaces(e + 1);
                                    if (f < token_count && tokens[f].type == CT_RPAREN) j = f + 1;
                                    else j = e + 1;
                                    continue;
                                }
                            } else if (d < token_count && tokens[d].type == CT_IDENT) {
                                /* defined X → X */
                                emit_tok(&tokens[d]);
                                j = d + 1;
                                continue;
                            }
                        }
                        emit_tok(&tokens[j]);
                        j++;
                    }
                    emit_str(") {\n");
                    if (j < token_count && tokens[j].type == CT_NEWLINE) j++;
                    i = j;
                    continue;
                }
                /* #elif expr → } else comptime if (expr) {
                 * Same defined() expansion as #if */
                if (tok_eq(&tokens[j], "elif")) {
                    emit_str("} else {\ncomptime if (");
                    j++;
                    j = skip_ws(j);
                    while (j < token_count && tokens[j].type != CT_NEWLINE && tokens[j].type != CT_EOF) {
                        if (tokens[j].type == CT_IDENT && tok_eq(&tokens[j], "defined")) {
                            int d = skip_spaces(j + 1);
                            if (d < token_count && tokens[d].type == CT_LPAREN) {
                                int e = skip_spaces(d + 1);
                                if (e < token_count && tokens[e].type == CT_IDENT) {
                                    emit_tok(&tokens[e]);
                                    int f = skip_spaces(e + 1);
                                    if (f < token_count && tokens[f].type == CT_RPAREN) j = f + 1;
                                    else j = e + 1;
                                    continue;
                                }
                            } else if (d < token_count && tokens[d].type == CT_IDENT) {
                                emit_tok(&tokens[d]);
                                j = d + 1;
                                continue;
                            }
                        }
                        emit_tok(&tokens[j]);
                        j++;
                    }
                    emit_str(") {\n");
                    if (j < token_count && tokens[j].type == CT_NEWLINE) j++;
                    i = j;
                    continue;
                }
                /* #pragma, #error, #warning, #line, etc → emit as comment */
                emit_str("// ");
                while (i < token_count && tokens[i].type != CT_NEWLINE && tokens[i].type != CT_EOF) {
                    emit_tok(&tokens[i]);
                    i++;
                }
                emit_str("\n");
                if (i < token_count && tokens[i].type == CT_NEWLINE) i++;
                continue;
            }
        }

        /* ---- switch depth tracking ---- */
        if (t->type == CT_IDENT && tok_eq(t, "switch")) {
            /* emit "switch" and track depth — the { after switch(...) increments */
            emit_str("switch");
            i++;
            /* emit everything up to and including the { */
            while (i < token_count && tokens[i].type != CT_LBRACE && tokens[i].type != CT_EOF) {
                emit_tok(&tokens[i]); i++;
            }
            if (i < token_count && tokens[i].type == CT_LBRACE) {
                emit_tok(&tokens[i]); i++;
                switch_depth++;
            }
            continue;
        }

        /* ---- switch/case/break → ZER switch syntax ---- */
        if (t->type == CT_IDENT && tok_eq(t, "case") && switch_depth > 0) {
            if (in_switch_arm_at_current_depth()) {
                emit_str("}\n");
                pop_switch_arm();
                for (int w = i - 1; w >= 0 && tokens[w].type == CT_WHITESPACE; w--) {
                    emit_tok(&tokens[w]);
                }
            }
            i++;
            int j = skip_spaces(i);
            emit_str(".");
            while (j < token_count && tokens[j].type != CT_COLON) {
                emit_tok(&tokens[j]); j++;
            }
            if (j < token_count && tokens[j].type == CT_COLON) j++;
            /* merge consecutive case labels */
            while (1) {
                int nxt = skip_ws(j);
                if (nxt < token_count && tokens[nxt].type == CT_IDENT && tok_eq(&tokens[nxt], "case")) {
                    nxt++;
                    int k = skip_spaces(nxt);
                    emit_str(", .");
                    while (k < token_count && tokens[k].type != CT_COLON) {
                        emit_tok(&tokens[k]); k++;
                    }
                    if (k < token_count && tokens[k].type == CT_COLON) k++;
                    j = k;
                } else break;
            }
            emit_str(" => {");
            push_switch_arm();
            i = j;
            continue;
        }
        if (t->type == CT_IDENT && tok_eq(t, "default") && switch_depth > 0) {
            int j = skip_spaces(i + 1);
            if (j < token_count && tokens[j].type == CT_COLON) {
                if (in_switch_arm_at_current_depth()) {
                    emit_str("}\n");
                    pop_switch_arm();
                    for (int w = i - 1; w >= 0 && tokens[w].type == CT_WHITESPACE; w--) {
                        emit_tok(&tokens[w]);
                    }
                }
                emit_str("default => {");
                push_switch_arm();
                i = j + 1;
                continue;
            }
        }
        /* break; inside switch arm → } (close arm block) */
        if (t->type == CT_IDENT && tok_eq(t, "break") && in_switch_arm_at_current_depth()) {
            int j = skip_spaces(i + 1);
            if (j < token_count && tokens[j].type == CT_SEMICOLON) {
                emit_str("}");
                pop_switch_arm();
                i = j + 1;
                continue;
            }
        }
        /* closing } of switch body — close any open arm, decrement depth */
        if (t->type == CT_RBRACE && switch_depth > 0) {
            if (in_switch_arm_at_current_depth()) {
                emit_str("}\n");
                pop_switch_arm();
            }
            switch_depth--;
            /* emit the } normally */
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
        /* ++i (pre) → i += 1; i++ (post) → += 1 (ident already emitted) */
        if (t->type == CT_PLUSPLUS) {
            int j = skip_spaces(i + 1);
            if (j < token_count && tokens[j].type == CT_IDENT &&
                (i == 0 || tokens[i - 1].type != CT_IDENT)) {
                /* pre-increment: ++ident → ident += 1 */
                emit_tok(&tokens[j]);
                emit_str(" += 1");
                i = j + 1;
            } else {
                /* post-increment: ident++ → += 1 */
                emit_str(" += 1");
                i++;
            }
            continue;
        }
        if (t->type == CT_MINUSMINUS) {
            int j = skip_spaces(i + 1);
            if (j < token_count && tokens[j].type == CT_IDENT &&
                (i == 0 || tokens[i - 1].type != CT_IDENT)) {
                emit_tok(&tokens[j]);
                emit_str(" -= 1");
                i = j + 1;
            } else {
                emit_str(" -= 1");
                i++;
            }
            continue;
        }

        /* ---- Strip C keywords not in ZER: extern, inline, restrict, register ---- */
        if (t->type == CT_IDENT &&
            (tok_eq(t, "extern") || tok_eq(t, "inline") ||
             tok_eq(t, "restrict") || tok_eq(t, "__inline") ||
             tok_eq(t, "__inline__") || tok_eq(t, "__restrict") ||
             tok_eq(t, "__restrict__") || tok_eq(t, "__extension__") ||
             tok_eq(t, "register"))) {
            /* strip keyword, preserve trailing whitespace */
            i++;
            continue;
        }

        /* ---- volatile qualifier: preserve for ZER (volatile *u32, volatile i32) ---- */
        if (t->type == CT_IDENT && tok_eq(t, "volatile")) {
            /* volatile TYPE *name → volatile *TYPE name
             * volatile TYPE name  → volatile TYPE name
             * Check if followed by a type + pointer pattern for MMIO */
            int j = skip_spaces(i + 1);
            /* check for volatile TYPE * → volatile *TYPE (pointer reorder) */
            if (j < token_count && tokens[j].type == CT_IDENT) {
                const char *mapped = map_type(&tokens[j]);
                int k = skip_spaces(j + 1);
                if (k < token_count && tokens[k].type == CT_STAR && mapped) {
                    /* volatile uint32_t *name → volatile *u32 name */
                    emit_str("volatile *");
                    emit_str(mapped);
                    emit_str(" ");
                    i = k + 1; /* skip volatile + type + * */
                    /* skip trailing whitespace */
                    while (i < token_count && tokens[i].type == CT_WHITESPACE) i++;
                    continue;
                }
                /* volatile TYPE name (no pointer) */
                if (mapped) {
                    emit_str("volatile ");
                    emit_str(mapped);
                    i = j + 1;
                    { int ar = try_reorder_array(i); if (ar >= 0) i = ar; }
                    continue;
                }
            }
            /* volatile followed by non-mapped type — emit as-is */
            emit_str("volatile ");
            i++;
            continue;
        }

        /* ---- Type mapping: int → i32, uint8_t → u8, etc. ---- */
        if (t->type == CT_IDENT) {

            /* char * name → []u8 name or ?*u8 name (usage-based classification) */
            {
                bool had_const = false;
                int ci_pos = i;
                bool check_char = tok_eq(t, "char");
                if (!check_char && tok_eq(t, "const")) {
                    ci_pos = skip_spaces(i + 1);
                    if (ci_pos < token_count && tok_eq(&tokens[ci_pos], "char")) {
                        had_const = true;
                        check_char = true;
                    }
                }
                if (check_char) {
                    int star_pos = skip_spaces(ci_pos + 1);
                    if (star_pos < token_count && tokens[star_pos].type == CT_STAR) {
                        int name_pos = skip_spaces(star_pos + 1);
                        if (name_pos < token_count && tokens[name_pos].type == CT_IDENT) {
                            ParamInfo *pi = find_param_info(tokens[name_pos].start, tokens[name_pos].len, name_pos);
                            if (pi && pi->is_slice) {
                                if (had_const) emit_str("const ");
                                if (pi->is_nullable) emit_str("?");
                                emit_str("[]u8 ");
                                i = star_pos + 1;
                                while (i < token_count && tokens[i].type == CT_WHITESPACE) i++;
                                continue;
                            }
                            if (pi && pi->is_nullable && !pi->is_slice) {
                                if (had_const) emit_str("const ");
                                emit_str("?*u8 ");
                                i = star_pos + 1;
                                while (i < token_count && tokens[i].type == CT_WHITESPACE) i++;
                                continue;
                            }
                        }
                    }
                }
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
                            i = j + 1; /* skip both tokens */
                            /* check for trailing 'long' (unsigned long long → u64) */
                            { int nxt = skip_spaces(i);
                              if (nxt < token_count && tokens[nxt].type == CT_IDENT &&
                                  tok_eq(&tokens[nxt], "long")) {
                                  i = nxt + 1; /* skip the extra 'long' */
                              }
                            }
                            { int pr = try_ptr_rearrange(i, type_combos[ci].zer); if (pr >= 0) { i = pr; found_combo = true; break; } }
                            emit_str(type_combos[ci].zer);
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
                i++;
                { int pr = try_ptr_rearrange(i, "i32"); if (pr >= 0) { i = pr; continue; } }
                emit_str("i32");
                { int ar = try_reorder_array(i); if (ar >= 0) i = ar; }
                continue;
            }

            /* standalone 'long' (not part of combo) → i64 */
            if (tok_eq(t, "long")) {
                i++;
                { int pr = try_ptr_rearrange(i, "i64"); if (pr >= 0) { i = pr; continue; } }
                emit_str("i64");
                { int ar = try_reorder_array(i); if (ar >= 0) i = ar; }
                continue;
            }

            /* standalone 'short' (not part of combo) → i16 */
            if (tok_eq(t, "short")) {
                i++;
                { int pr = try_ptr_rearrange(i, "i16"); if (pr >= 0) { i = pr; continue; } }
                emit_str("i16");
                { int ar = try_reorder_array(i); if (ar >= 0) i = ar; }
                continue;
            }

            /* void * → *opaque, void ** → **opaque (type-erased pointer) */
            if (tok_eq(t, "void")) {
                int j = skip_spaces(i + 1);
                if (j < token_count && tokens[j].type == CT_STAR) {
                    int k = skip_spaces(j + 1);
                    if (k < token_count && tokens[k].type == CT_STAR) {
                        /* void ** → **opaque */
                        emit_str("**opaque ");
                        i = k + 1;
                        continue;
                    }
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

            /* union keyword in usage (not declaration) — drop it + trailing whitespace */
            if (tok_eq(t, "union")) {
                int j = skip_spaces(i + 1);
                if (j < token_count && tokens[j].type == CT_IDENT) {
                    int k = skip_ws(j + 1);
                    if (k < token_count && tokens[k].type == CT_LBRACE) {
                        emit_str("union");
                        i++;
                        continue;
                    }
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
                        /* sizeof(type) → @size(type), sizeof(var) → sizeof(var) */
                        if (m < token_count && tokens[m].type == CT_RPAREN) {
                            /* distinguish type vs variable:
                             * - mapped C type (int, char, etc.) → always @size
                             * - struct-prefixed → always @size
                             * - starts with uppercase → likely type → @size
                             * - starts with lowercase → likely variable → keep sizeof */
                            bool is_type = (mt != NULL) || had_struct ||
                                           (tokens[k].len > 0 && tokens[k].start[0] >= 'A' && tokens[k].start[0] <= 'Z');
                            if (is_type) {
                                emit_str("@size(");
                                if (mt) emit_str(mt);
                                else emit_raw(tokens[k].start, tokens[k].len);
                                emit_str(")");
                            } else {
                                /* variable — keep sizeof for GCC via cinclude */
                                emit_str("sizeof(");
                                emit_raw(tokens[k].start, tokens[k].len);
                                emit_str(")");
                            }
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
                i++;
                { int pr = try_ptr_rearrange(i, mapped); if (pr >= 0) { i = pr; continue; } }
                emit_str(mapped);
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

            /* Pointer arithmetic: ptr + N → ptr[N..] (sub-slice)
             * Only for classified char* params that became slices. */
            {
                ParamInfo *pi = find_param_info(t->start, t->len, i);
                if (pi && pi->is_slice) {
                    int j = skip_spaces(i + 1);
                    if (j < token_count && tokens[j].type == CT_PLUS) {
                        /* collect the offset expression up to ) or ; or , */
                        int k = skip_spaces(j + 1);
                        int expr_start = k;
                        int depth = 0;
                        while (k < token_count) {
                            if (tokens[k].type == CT_LPAREN) depth++;
                            if (tokens[k].type == CT_RPAREN) { if (depth == 0) break; depth--; }
                            if (depth == 0 && (tokens[k].type == CT_SEMICOLON ||
                                               tokens[k].type == CT_COMMA ||
                                               tokens[k].type == CT_RBRACKET)) break;
                            k++;
                        }
                        /* ptr + N → ptr[N..] */
                        emit_tok(t);
                        emit_str("[");
                        for (int x = expr_start; x < k; x++) {
                            if (tokens[x].type == CT_IDENT) {
                                const char *mt = map_type(&tokens[x]);
                                if (mt) { emit_str(mt); continue; }
                            }
                            emit_tok(&tokens[x]);
                        }
                        emit_str("..]");
                        i = k;
                        continue;
                    }
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
            /* check for (volatile TYPE *) cast → volatile *TYPE via @inttoptr if numeric arg */
            if (!is_cast && j < token_count && tokens[j].type == CT_IDENT &&
                tok_eq(&tokens[j], "volatile")) {
                int v = skip_spaces(j + 1);
                if (v < token_count && tokens[v].type == CT_IDENT) {
                    const char *vm = map_type(&tokens[v]);
                    int k = skip_spaces(v + 1);
                    if (k < token_count && tokens[k].type == CT_STAR) {
                        int m = skip_spaces(k + 1);
                        if (m < token_count && tokens[m].type == CT_RPAREN) {
                            /* (volatile uint32_t *) — check if arg is numeric → @inttoptr */
                            int after = skip_spaces(m + 1);
                            bool arg_is_numeric = (after < token_count &&
                                tokens[after].type == CT_NUMBER);
                            /* also check (volatile TYPE *)(0x...) with parens */
                            if (!arg_is_numeric && after < token_count &&
                                tokens[after].type == CT_LPAREN) {
                                int inner = skip_spaces(after + 1);
                                if (inner < token_count && tokens[inner].type == CT_NUMBER)
                                    arg_is_numeric = true;
                            }
                            if (arg_is_numeric) {
                                const char *tn = vm ? vm : "u32";
                                snprintf(cast_buf, sizeof(cast_buf), "*%s", tn);
                                cast_type = cast_buf;
                                is_ptr_cast = true;
                                is_cast = true;
                                cast_end = m + 1;
                                /* will emit @inttoptr instead of @ptrcast below */
                            } else {
                                const char *tn = vm ? vm : "u32";
                                snprintf(cast_buf, sizeof(cast_buf), "*%s", tn);
                                cast_type = cast_buf;
                                is_ptr_cast = true;
                                is_cast = true;
                                cast_end = m + 1;
                            }
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

            /* verify cast: what follows ) must be an expression, not , or ) or ; */
            if (is_cast) {
                int after = skip_spaces(cast_end);
                if (after < token_count) {
                    CTokenType at = tokens[after].type;
                    if (at == CT_COMMA || at == CT_RPAREN || at == CT_SEMICOLON || at == CT_EOF) {
                        is_cast = false; /* not a cast — it's a param type in funcptr decl */
                    }
                }
            }

            /* (uintptr_t)expr → @ptrtoint(expr) — almost always pointer-to-int */
            bool use_ptrtoint = false;
            if (is_cast && !is_ptr_cast && j < token_count &&
                tokens[j].type == CT_IDENT &&
                (tok_eq(&tokens[j], "uintptr_t") || tok_eq(&tokens[j], "intptr_t"))) {
                use_ptrtoint = true;
            }

            if (is_cast && cast_type) {
                /* detect if cast operand is a numeric literal → @inttoptr for MMIO */
                bool use_inttoptr = false;
                if (is_ptr_cast) {
                    int peek_i = skip_spaces(cast_end);
                    /* direct number: (type*)0x40020000 */
                    if (peek_i < token_count && tokens[peek_i].type == CT_NUMBER)
                        use_inttoptr = true;
                    /* parenthesized number: (type*)(0x40020000) */
                    if (!use_inttoptr && peek_i < token_count && tokens[peek_i].type == CT_LPAREN) {
                        int inner = skip_spaces(peek_i + 1);
                        if (inner < token_count && tokens[inner].type == CT_NUMBER)
                            use_inttoptr = true;
                    }
                }
                if (is_ptr_cast && use_inttoptr) {
                    emit_str("@inttoptr(");
                    emit_str(cast_type);
                    emit_str(", ");
                } else if (is_ptr_cast) {
                    emit_str("@ptrcast(");
                    emit_str(cast_type);
                    emit_str(", ");
                } else if (use_ptrtoint) {
                    emit_str("@ptrtoint(");
                    /* cast_type not needed — @ptrtoint has no type arg */
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

        /* ---- *(ptr + N) → ptr[N] for classified slice params ---- */
        if (t->type == CT_STAR) {
            int j = skip_spaces(i + 1);
            if (j < token_count && tokens[j].type == CT_LPAREN) {
                int k = skip_spaces(j + 1);
                if (k < token_count && tokens[k].type == CT_IDENT) {
                    ParamInfo *pi = find_param_info(tokens[k].start, tokens[k].len, k);
                    if (pi && pi->is_slice) {
                        int m = skip_spaces(k + 1);
                        if (m < token_count && tokens[m].type == CT_PLUS) {
                            /* *(ptr + expr) → ptr[expr] */
                            int n = skip_spaces(m + 1);
                            int expr_start = n;
                            int depth = 0;
                            while (n < token_count) {
                                if (tokens[n].type == CT_LPAREN) depth++;
                                if (tokens[n].type == CT_RPAREN) {
                                    if (depth == 0) break;
                                    depth--;
                                }
                                n++;
                            }
                            /* n is at closing ) */
                            emit_tok(&tokens[k]); /* ptr name */
                            emit_str("[");
                            for (int x = expr_start; x < n; x++) {
                                if (tokens[x].type == CT_IDENT) {
                                    const char *mt = map_type(&tokens[x]);
                                    if (mt) { emit_str(mt); continue; }
                                }
                                emit_tok(&tokens[x]);
                            }
                            emit_str("]");
                            i = n + 1; /* skip past ) */
                            continue;
                        }
                    }
                }
            }
        }

        /* ---- Everything else: pass through unchanged ---- */
        emit_tok(t);
        i++;
        next_token: ;
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
    classify_params();
    scan_for_extractions();

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

    /* write extracted .h file if any constructs needed extraction */
    if (needs_extract && extract_len > 0) {
        char h_path[512];
        strncpy(h_path, output_path, sizeof(h_path) - 10);
        int hlen = (int)strlen(h_path);
        /* replace .zer with _extract.h */
        if (hlen > 4 && strcmp(h_path + hlen - 4, ".zer") == 0) {
            strcpy(h_path + hlen - 4, "_extract.h");
        } else {
            strcat(h_path, "_extract.h");
        }

        FILE *hf = fopen(h_path, "w");
        if (hf) {
            fprintf(hf, "/* Extracted C code from %s — constructs that can't be expressed in ZER */\n", input_path);
            fprintf(hf, "/* (bit fields, goto, ternary, inline asm) */\n\n");
            fwrite(extract_buf, 1, extract_len, hf);
            fclose(hf);
        }

        /* prepend cinclude for the extracted .h into the .zer file */
        int zer_len2;
        char *zer_content2 = read_file(output_path, &zer_len2);
        if (zer_content2) {
            out = fopen(output_path, "w");
            /* find the basename of h_path for the cinclude */
            const char *h_basename = h_path;
            for (const char *p = h_path; *p; p++) {
                if (*p == '/' || *p == '\\') h_basename = p + 1;
            }
            /* find insertion point — after the header comments */
            char *insert_point = zer_content2;
            /* skip comment lines at top */
            while (*insert_point == '/' || *insert_point == '\n') {
                char *nl = strchr(insert_point, '\n');
                if (nl) insert_point = nl + 1; else break;
                if (*insert_point != '/' && *insert_point != '\n') break;
            }
            /* write: header comments, cinclude, rest */
            fwrite(zer_content2, 1, insert_point - zer_content2, out);
            fprintf(out, "cinclude \"%s\";\n", h_basename);
            fwrite(insert_point, 1, zer_len2 - (insert_point - zer_content2), out);
            fclose(out);
            free(zer_content2);
        }

        printf("zer-convert: extracted %d constructs to %s\n", extract_count, h_path);
    }

    free(src);

    printf("zer-convert: %s -> %s", input_path, output_path);
    if (needs_compat) printf(" (uses compat.zer)");
    if (needs_extract) printf(" (extracted .h)");
    printf("\n");

    return 0;
}
