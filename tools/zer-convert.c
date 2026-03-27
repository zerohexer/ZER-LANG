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
    { "unsigned",  8, "u32" },  /* standalone 'unsigned' = u32 */
    { "void",     4, "void" },
    { "bool",     4, "bool" },
    { "_Bool",    5, "bool" },
    { "NULL",     4, "null" },
    { "true",     4, "true" },
    { "false",    5, "false" },
    { NULL, 0, NULL }
};

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
 * Transform engine
 * ================================================================ */

static FILE *out;
static bool needs_compat = false; /* set true if malloc/free/ptr arith used */

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
                    /* skip whitespace */
                    while (j < token_count && tokens[j].type == CT_NEWLINE) j++;
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
            const char *mapped = map_type(t);

            /* NULL → null */
            if (tok_eq(t, "NULL")) {
                emit_str("null");
                i++;
                continue;
            }

            /* struct keyword in usage (not declaration) — drop it */
            if (tok_eq(t, "struct")) {
                int j = skip_ws(i + 1);
                if (j < token_count && tokens[j].type == CT_IDENT) {
                    /* check if next-next is { → keep 'struct' (declaration) */
                    int k = skip_ws(j + 1);
                    if (k < token_count && tokens[k].type == CT_LBRACE) {
                        /* struct declaration — keep struct keyword */
                        emit_str("struct");
                        i++;
                        continue;
                    }
                    /* struct usage: struct Node → Node */
                    i++; /* skip 'struct' */
                    continue;
                }
            }

            /* sizeof(T) → @size(T) */
            if (tok_eq(t, "sizeof")) {
                emit_str("@size");
                i++;
                continue;
            }

            /* malloc(sizeof(T)) → zer_malloc_bytes(@size(T)) */
            if (tok_eq(t, "malloc")) {
                emit_str("zer_malloc_bytes");
                needs_compat = true;
                i++;
                continue;
            }
            if (tok_eq(t, "calloc")) {
                emit_str("zer_calloc_bytes");
                needs_compat = true;
                i++;
                continue;
            }
            if (tok_eq(t, "realloc")) {
                emit_str("zer_realloc_bytes");
                needs_compat = true;
                i++;
                continue;
            }
            if (tok_eq(t, "free")) {
                emit_str("zer_free");
                needs_compat = true;
                i++;
                continue;
            }

            /* strlen, strcmp, memcmp, memcpy, memset */
            if (tok_eq(t, "strlen")) { emit_str("zer_strlen"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "strcmp")) { emit_str("zer_strcmp"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "strncmp")) { emit_str("zer_strncmp"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "memcmp")) { emit_str("zer_memcmp"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "memcpy")) { emit_str("zer_memcpy"); needs_compat = true; i++; continue; }
            if (tok_eq(t, "memset")) { emit_str("zer_memset"); needs_compat = true; i++; continue; }

            /* exit() → zer_exit() */
            if (tok_eq(t, "exit")) { emit_str("zer_exit"); needs_compat = true; i++; continue; }

            /* Type-mapped identifier */
            if (mapped) {
                emit_str(mapped);
                i++;
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
