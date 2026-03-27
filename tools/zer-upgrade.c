/*
 * zer-upgrade — Phase 2: Replace compat builtins with safe ZER (source-to-source)
 *
 * Reads .zer files containing compat.zer imports and replaces unsafe
 * compat builtins with safe ZER equivalents.
 *
 * Layer 1 (this version): Simple token-level replacements
 *   zer_strlen(x)       → x.len
 *   zer_strcmp(a, b)     → bytes_equal(a, b)
 *   zer_strncmp(a,b,n)  → bytes_equal(a[0..n], b[0..n])
 *   zer_memcpy(d, s, n) → bytes_copy(d, s)
 *   zer_memset(d, v, n) → bytes_fill(d, v)
 *   zer_memcmp(a, b, n) → bytes_equal(a[0..n], b[0..n])
 *   zer_exit(n)          → @trap()
 *
 * Layer 2 (future): malloc/free analysis → Slab(T)
 *   zer_malloc_bytes(@size(T)) + zer_free(p) → Slab(T) + alloc/free
 *
 * Usage: zer-upgrade input.zer [-o output.zer]
 * Build: gcc -std=c99 -O2 -o zer-upgrade tools/zer-upgrade.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

/* ================================================================
 * Source buffer — work on raw text with string matching
 * ================================================================ */

static char *read_file(const char *path, int *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "zer-upgrade: cannot open '%s'\n", path); return NULL; }
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

/* ================================================================
 * Argument extractor — parse function call args from source text
 * ================================================================ */

/* find matching closing paren, respecting nesting */
static int find_close_paren(const char *src, int start, int len) {
    int depth = 0;
    for (int i = start; i < len; i++) {
        if (src[i] == '(') depth++;
        if (src[i] == ')') { depth--; if (depth == 0) return i; }
        if (src[i] == '"') { i++; while (i < len && src[i] != '"') { if (src[i] == '\\') i++; i++; } }
        if (src[i] == '\'') { i++; while (i < len && src[i] != '\'') { if (src[i] == '\\') i++; i++; } }
    }
    return -1;
}

/* extract comma-separated args between ( and ) */
typedef struct { int start; int len; } Span;

static int extract_args(const char *src, int open, int close, Span *args, int max_args) {
    int count = 0;
    int pos = open + 1;
    int depth = 0;

    /* skip leading whitespace */
    while (pos < close && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n')) pos++;

    int arg_start = pos;
    for (int i = pos; i < close; i++) {
        if (src[i] == '(' || src[i] == '[') depth++;
        if (src[i] == ')' || src[i] == ']') depth--;
        if (src[i] == '"') { i++; while (i < close && src[i] != '"') { if (src[i] == '\\') i++; i++; } }
        if (depth == 0 && src[i] == ',') {
            if (count < max_args) {
                /* trim whitespace from arg */
                int end = i;
                while (end > arg_start && (src[end - 1] == ' ' || src[end - 1] == '\t')) end--;
                while (arg_start < end && (src[arg_start] == ' ' || src[arg_start] == '\t')) arg_start++;
                args[count].start = arg_start;
                args[count].len = end - arg_start;
                count++;
            }
            arg_start = i + 1;
            while (arg_start < close && (src[arg_start] == ' ' || src[arg_start] == '\t')) arg_start++;
        }
    }
    /* last arg */
    if (arg_start < close && count < max_args) {
        int end = close;
        while (end > arg_start && (src[end - 1] == ' ' || src[end - 1] == '\t')) end--;
        while (arg_start < end && (src[arg_start] == ' ' || src[arg_start] == '\t')) arg_start++;
        args[count].start = arg_start;
        args[count].len = end - arg_start;
        count++;
    }
    return count;
}

/* ================================================================
 * Replacement engine
 * ================================================================ */

/* output buffer (dynamic) */
static char *out_buf = NULL;
static int out_len = 0;
static int out_cap = 0;

static void out_init(void) {
    out_cap = 1024 * 64;
    out_buf = (char *)malloc(out_cap);
    out_len = 0;
}

static void out_write(const char *s, int len) {
    while (out_len + len >= out_cap) {
        out_cap *= 2;
        out_buf = (char *)realloc(out_buf, out_cap);
    }
    memcpy(out_buf + out_len, s, len);
    out_len += len;
}

static void out_str(const char *s) {
    out_write(s, (int)strlen(s));
}

/* statistics */
static int upgrades = 0;
static int kept = 0;
static bool needs_str_import = false;

/* check if position starts with a specific string */
static bool starts_with(const char *src, int pos, int len, const char *prefix) {
    int plen = (int)strlen(prefix);
    if (pos + plen > len) return false;
    return memcmp(src + pos, prefix, plen) == 0;
}

/* check if character before pos is NOT alphanumeric (word boundary) */
static bool word_boundary_before(const char *src, int pos) {
    if (pos <= 0) return true;
    char c = src[pos - 1];
    return !isalnum(c) && c != '_';
}

static void upgrade(const char *src, int src_len) {
    int i = 0;

    while (i < src_len) {

        /* ---- zer_strlen(expr) → expr.len ---- */
        if (starts_with(src, i, src_len, "zer_strlen(") && word_boundary_before(src, i)) {
            int open = i + 10; /* position of ( */
            int close = find_close_paren(src, open, src_len);
            if (close > 0) {
                Span args[1];
                int argc = extract_args(src, open, close, args, 1);
                if (argc == 1) {
                    out_write(src + args[0].start, args[0].len);
                    out_str(".len");
                    i = close + 1;
                    upgrades++;
                    needs_str_import = true;
                    continue;
                }
            }
        }

        /* ---- zer_strcmp(a, b) → bytes_equal(a, b) ---- */
        /* strcmp returns 0 on match, bytes_equal returns true.
         * Strip trailing " == 0" or " != 0" and invert if needed. */
        if (starts_with(src, i, src_len, "zer_strcmp(") && word_boundary_before(src, i)) {
            int open = i + 10;
            int close = find_close_paren(src, open, src_len);
            if (close > 0) {
                /* check for trailing == 0 or != 0 */
                int after = close + 1;
                while (after < src_len && (src[after] == ' ' || src[after] == '\t')) after++;
                bool has_eq0 = (after + 3 < src_len && src[after] == '=' && src[after+1] == '=' &&
                               src[after+2] == ' ' && src[after+3] == '0');
                bool has_neq0 = (after + 3 < src_len && src[after] == '!' && src[after+1] == '=' &&
                                src[after+2] == ' ' && src[after+3] == '0');
                /* also handle without space: ==0, !=0 */
                if (!has_eq0 && after + 2 < src_len && src[after] == '=' && src[after+1] == '=' && src[after+2] == '0')
                    has_eq0 = true;
                if (!has_neq0 && after + 2 < src_len && src[after] == '!' && src[after+1] == '=' && src[after+2] == '0')
                    has_neq0 = true;

                if (has_neq0) out_str("!");
                out_str("bytes_equal(");
                out_write(src + open + 1, close - open - 1);
                out_str(")");
                if (has_eq0 || has_neq0) {
                    /* skip past the == 0 / != 0 */
                    i = after;
                    while (i < src_len && (src[i] == '=' || src[i] == '!' || src[i] == ' ' || src[i] == '0')) i++;
                } else {
                    i = close + 1;
                }
                upgrades++;
                needs_str_import = true;
                continue;
            }
        }

        /* ---- zer_strncmp(a, b, n) → bytes_equal(a[0..n], b[0..n]) ---- */
        if (starts_with(src, i, src_len, "zer_strncmp(") && word_boundary_before(src, i)) {
            int open = i + 11;
            int close = find_close_paren(src, open, src_len);
            if (close > 0) {
                Span args[3];
                int argc = extract_args(src, open, close, args, 3);
                if (argc == 3) {
                    /* check for == 0 / != 0 after close paren */
                    int after = close + 1;
                    while (after < src_len && (src[after] == ' ' || src[after] == '\t')) after++;
                    bool has_eq0 = (after + 3 < src_len && src[after]=='=' && src[after+1]=='=' && src[after+2]==' ' && src[after+3]=='0');
                    bool has_neq0 = (after + 3 < src_len && src[after]=='!' && src[after+1]=='=' && src[after+2]==' ' && src[after+3]=='0');
                    if (!has_eq0 && after+2 < src_len && src[after]=='=' && src[after+1]=='=' && src[after+2]=='0') has_eq0 = true;
                    if (!has_neq0 && after+2 < src_len && src[after]=='!' && src[after+1]=='=' && src[after+2]=='0') has_neq0 = true;
                    if (has_neq0) out_str("!");
                    out_str("bytes_equal(");
                    out_write(src + args[0].start, args[0].len);
                    out_str("[0..");
                    out_write(src + args[2].start, args[2].len);
                    out_str("], ");
                    out_write(src + args[1].start, args[1].len);
                    out_str("[0..");
                    out_write(src + args[2].start, args[2].len);
                    out_str("])");
                    if (has_eq0 || has_neq0) {
                        i = after;
                        while (i < src_len && (src[i]=='='||src[i]=='!'||src[i]==' '||src[i]=='0')) i++;
                    } else {
                        i = close + 1;
                    }
                    upgrades++;
                    needs_str_import = true;
                    continue;
                }
            }
        }

        /* ---- zer_memcpy(dst, src, n) → bytes_copy(dst, src) ---- */
        if (starts_with(src, i, src_len, "zer_memcpy(") && word_boundary_before(src, i)) {
            int open = i + 10;
            int close = find_close_paren(src, open, src_len);
            if (close > 0) {
                Span args[3];
                int argc = extract_args(src, open, close, args, 3);
                if (argc == 3) {
                    out_str("bytes_copy(");
                    out_write(src + args[0].start, args[0].len);
                    out_str(", ");
                    out_write(src + args[1].start, args[1].len);
                    out_str(")");
                    i = close + 1;
                    upgrades++;
                    needs_str_import = true;
                    continue;
                }
            }
        }

        /* ---- zer_memmove(dst, src, n) → bytes_copy(dst, src) ---- */
        if (starts_with(src, i, src_len, "zer_memmove(") && word_boundary_before(src, i)) {
            int open = i + 11;
            int close = find_close_paren(src, open, src_len);
            if (close > 0) {
                Span args[3];
                int argc = extract_args(src, open, close, args, 3);
                if (argc == 3) {
                    out_str("bytes_copy(");
                    out_write(src + args[0].start, args[0].len);
                    out_str(", ");
                    out_write(src + args[1].start, args[1].len);
                    out_str(")");
                    i = close + 1;
                    upgrades++;
                    needs_str_import = true;
                    continue;
                }
            }
        }

        /* ---- zer_memset(dst, val, n) → bytes_fill(dst, val) ---- */
        if (starts_with(src, i, src_len, "zer_memset(") && word_boundary_before(src, i)) {
            int open = i + 10;
            int close = find_close_paren(src, open, src_len);
            if (close > 0) {
                Span args[3];
                int argc = extract_args(src, open, close, args, 3);
                if (argc == 3) {
                    /* check if val is 0 → bytes_zero */
                    if (args[1].len == 1 && src[args[1].start] == '0') {
                        out_str("bytes_zero(");
                        out_write(src + args[0].start, args[0].len);
                        out_str(")");
                    } else {
                        out_str("bytes_fill(");
                        out_write(src + args[0].start, args[0].len);
                        out_str(", ");
                        out_write(src + args[1].start, args[1].len);
                        out_str(")");
                    }
                    i = close + 1;
                    upgrades++;
                    needs_str_import = true;
                    continue;
                }
            }
        }

        /* ---- zer_memcmp(a, b, n) → bytes_equal(a[0..n], b[0..n]) ---- */
        if (starts_with(src, i, src_len, "zer_memcmp(") && word_boundary_before(src, i)) {
            int open = i + 10;
            int close = find_close_paren(src, open, src_len);
            if (close > 0) {
                Span args[3];
                int argc = extract_args(src, open, close, args, 3);
                if (argc == 3) {
                    out_str("bytes_equal(");
                    out_write(src + args[0].start, args[0].len);
                    out_str("[0..");
                    out_write(src + args[2].start, args[2].len);
                    out_str("], ");
                    out_write(src + args[1].start, args[1].len);
                    out_str("[0..");
                    out_write(src + args[2].start, args[2].len);
                    out_str("])");
                    i = close + 1;
                    upgrades++;
                    needs_str_import = true;
                    continue;
                }
            }
        }

        /* ---- zer_exit(n) → @trap() ---- */
        if (starts_with(src, i, src_len, "zer_exit(") && word_boundary_before(src, i)) {
            int open = i + 8;
            int close = find_close_paren(src, open, src_len);
            if (close > 0) {
                out_str("@trap()");
                i = close + 1;
                upgrades++;
                continue;
            }
        }

        /* ---- Compat calls we can't upgrade yet — count them ---- */
        if (starts_with(src, i, src_len, "zer_malloc_bytes(") && word_boundary_before(src, i)) {
            /* pass through — Layer 2 handles this */
            kept++;
        }
        if (starts_with(src, i, src_len, "zer_calloc_bytes(") && word_boundary_before(src, i)) {
            kept++;
        }
        if (starts_with(src, i, src_len, "zer_realloc_bytes(") && word_boundary_before(src, i)) {
            kept++;
        }
        if (starts_with(src, i, src_len, "zer_free(") && word_boundary_before(src, i)) {
            kept++;
        }
        /* I/O: printf, fopen, etc. — stay as compat until stdlib covers them */
        if (starts_with(src, i, src_len, "zer_printf(") && word_boundary_before(src, i)) {
            kept++;
        }
        if (starts_with(src, i, src_len, "zer_fprintf(") && word_boundary_before(src, i)) {
            kept++;
        }

        /* ---- Remove "import compat;" if no compat calls remain ---- */
        /* (handled in post-processing) */

        /* ---- Default: copy character ---- */
        out_write(src + i, 1);
        i++;
    }
}

/* ================================================================
 * Post-processing
 * ================================================================ */

/* remove "import compat;" line if no compat calls remain */
static void remove_compat_import_if_clean(void) {
    if (kept > 0) return; /* still has compat calls */

    const char *needle = "import compat;";
    int nlen = (int)strlen(needle);
    for (int i = 0; i <= out_len - nlen; i++) {
        if (memcmp(out_buf + i, needle, nlen) == 0) {
            /* remove the line (including trailing newline) */
            int end = i + nlen;
            while (end < out_len && (out_buf[end] == '\n' || out_buf[end] == '\r')) end++;
            int removed = end - i;
            memmove(out_buf + i, out_buf + end, out_len - end);
            out_len -= removed;
            break;
        }
    }
}

/* add "import str;" if str.zer functions were used */
static void add_str_import_if_needed(void) {
    if (!needs_str_import) return;
    /* check if already has import str; */
    if (strstr(out_buf, "import str;")) return;

    /* insert after the last import line or at the top */
    const char *import_line = "import str;\n";
    int ilen = (int)strlen(import_line);

    /* find a good insertion point — after last "import" or "cinclude" line */
    int insert_at = 0;
    for (int i = 0; i < out_len; i++) {
        if (out_buf[i] == '\n') {
            /* check if next line starts with import or cinclude */
            int j = i + 1;
            while (j < out_len && (out_buf[j] == ' ' || out_buf[j] == '\t')) j++;
            if (j + 6 < out_len && memcmp(out_buf + j, "import", 6) == 0) insert_at = i + 1;
            if (j + 8 < out_len && memcmp(out_buf + j, "cinclude", 8) == 0) insert_at = i + 1;
        }
    }
    /* find end of that line */
    while (insert_at < out_len && out_buf[insert_at] != '\n') insert_at++;
    if (insert_at < out_len) insert_at++; /* past the newline */

    /* insert */
    while (out_len + ilen >= out_cap) { out_cap *= 2; out_buf = (char *)realloc(out_buf, out_cap); }
    memmove(out_buf + insert_at + ilen, out_buf + insert_at, out_len - insert_at);
    memcpy(out_buf + insert_at, import_line, ilen);
    out_len += ilen;
}

/* ================================================================
 * Main
 * ================================================================ */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: zer-upgrade <input.zer> [-o output.zer]\n");
        fprintf(stderr, "  Phase 2: Replace compat builtins with safe ZER\n");
        return 1;
    }

    const char *input_path = argv[1];
    const char *output_path = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        }
    }

    /* default: overwrite in place */
    if (!output_path) output_path = input_path;

    int src_len;
    char *src = read_file(input_path, &src_len);
    if (!src) return 1;

    out_init();
    upgrade(src, src_len);

    /* post-processing */
    remove_compat_import_if_clean();
    add_str_import_if_needed();

    /* write output */
    FILE *f = fopen(output_path, "wb");
    if (!f) {
        fprintf(stderr, "zer-upgrade: cannot write '%s'\n", output_path);
        free(src);
        free(out_buf);
        return 1;
    }
    fwrite(out_buf, 1, out_len, f);
    fclose(f);

    printf("zer-upgrade: %s\n", output_path);
    printf("  upgraded: %d compat calls → safe ZER\n", upgrades);
    if (kept > 0) {
        printf("  kept:     %d compat calls (malloc/free/IO — Layer 2 needed)\n", kept);
    } else {
        printf("  result:   fully safe — no compat dependency\n");
    }

    free(src);
    free(out_buf);
    return 0;
}
