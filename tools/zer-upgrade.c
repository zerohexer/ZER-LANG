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

/* forward declarations for helpers used in Layer 2 */
static bool starts_with(const char *src, int pos, int len, const char *prefix);
static bool word_boundary_before(const char *src, int pos);
static int find_close_paren(const char *src, int start, int len);

/* ================================================================
 * Layer 2: malloc/free → Slab(T) analysis
 * ================================================================ */

#define MAX_ALLOCS 256
#define MAX_TYPES 64

typedef struct {
    char type_name[64];     /* the allocated type: Node, dict, etc. */
    char var_name[64];      /* the variable: n, d, entry, etc. */
    int malloc_line;        /* line of malloc */
    bool has_free;          /* matched free found */
} AllocInfo;

typedef struct {
    char name[64];          /* unique type name */
    char slab_name[80];     /* generated slab variable name */
} SlabType;

static AllocInfo allocs[MAX_ALLOCS];
static int alloc_count = 0;
static SlabType slab_types[MAX_TYPES];
static int slab_type_count = 0;

/* find or add a slab type, return the slab variable name */
static const char *get_slab_name(const char *type_name) {
    for (int i = 0; i < slab_type_count; i++) {
        if (strcmp(slab_types[i].name, type_name) == 0)
            return slab_types[i].slab_name;
    }
    if (slab_type_count < MAX_TYPES) {
        SlabType *st = &slab_types[slab_type_count++];
        strncpy(st->name, type_name, 63);
        /* generate slab name: Node → node_slab, dict_entry → dict_entry_slab */
        snprintf(st->slab_name, 79, "%s_slab", type_name);
        /* lowercase first char */
        if (st->slab_name[0] >= 'A' && st->slab_name[0] <= 'Z')
            st->slab_name[0] += 32;
        return st->slab_name;
    }
    return "unknown_slab";
}

/* check if a variable name was malloc'd */
static AllocInfo *find_alloc(const char *var_name) {
    for (int i = 0; i < alloc_count; i++) {
        if (strcmp(allocs[i].var_name, var_name) == 0)
            return &allocs[i];
    }
    return NULL;
}

/* Pre-scan: find all malloc/free patterns in source */
static void scan_allocs(const char *src, int src_len) {
    alloc_count = 0;
    slab_type_count = 0;
    int line = 1;

    for (int i = 0; i < src_len; i++) {
        if (src[i] == '\n') line++;

        /* Pattern: TYPE *VAR = @ptrcast(*TYPE, zer_malloc_bytes(@size(TYPE))); */
        if (starts_with(src, i, src_len, "@ptrcast(*") && word_boundary_before(src, i)) {
            int j = i + 10; /* after @ptrcast(* */
            /* extract type name */
            int type_start = j;
            while (j < src_len && src[j] != ',' && src[j] != ')') j++;
            int type_len = j - type_start;
            if (type_len > 0 && type_len < 63) {
                char type_name[64];
                memcpy(type_name, src + type_start, type_len);
                type_name[type_len] = '\0';
                /* trim whitespace */
                while (type_len > 0 && type_name[type_len - 1] == ' ') type_name[--type_len] = '\0';

                /* check if this contains zer_malloc_bytes */
                if (strstr(src + j, "zer_malloc_bytes") && (strstr(src + j, "zer_malloc_bytes") - (src + j)) < 30) {
                    /* find variable name: scan backward from @ptrcast for "= " then the ident before it */
                    int k = i - 1;
                    while (k > 0 && (src[k] == ' ' || src[k] == '\t')) k--;
                    if (k > 0 && src[k] == '=') {
                        k--;
                        while (k > 0 && (src[k] == ' ' || src[k] == '\t')) k--;
                        /* now k points to end of var name */
                        int var_end = k + 1;
                        while (k > 0 && (isalnum(src[k]) || src[k] == '_')) k--;
                        k++; /* start of var name */
                        int var_len = var_end - k;
                        if (var_len > 0 && var_len < 63 && alloc_count < MAX_ALLOCS) {
                            AllocInfo *ai = &allocs[alloc_count++];
                            memcpy(ai->var_name, src + k, var_len);
                            ai->var_name[var_len] = '\0';
                            strncpy(ai->type_name, type_name, 63);
                            ai->malloc_line = line;
                            ai->has_free = false;
                            get_slab_name(type_name); /* register type */
                        }
                    }
                }
            }
        }

        /* Pattern: zer_free(VAR); — match to alloc */
        if (starts_with(src, i, src_len, "zer_free(") && word_boundary_before(src, i)) {
            int open = i + 9;
            int close = find_close_paren(src, open, src_len);
            if (close > 0) {
                char var_name[64];
                int vlen = close - open - 1;
                if (vlen > 0 && vlen < 63) {
                    /* trim whitespace */
                    int vs = open + 1;
                    while (vs < close && (src[vs] == ' ' || src[vs] == '\t')) vs++;
                    int ve = close;
                    while (ve > vs && (src[ve - 1] == ' ' || src[ve - 1] == '\t')) ve--;
                    vlen = ve - vs;
                    if (vlen > 0 && vlen < 63) {
                        memcpy(var_name, src + vs, vlen);
                        var_name[vlen] = '\0';
                        AllocInfo *ai = find_alloc(var_name);
                        if (ai) ai->has_free = true;
                    }
                }
            }
        }
    }
}

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

        /* ---- Layer 2: malloc/free → Slab(T) ---- */

        /* Pattern: TYPE *var = @ptrcast(*TYPE, zer_malloc_bytes(@size(TYPE)));
         * → ?Handle(TYPE) var_maybe = type_slab.alloc();
         *   Handle(TYPE) var_h = var_maybe orelse return; */
        if (starts_with(src, i, src_len, "@ptrcast(*") && word_boundary_before(src, i)) {
            /* check if this is a malloc ptrcast */
            int j = i + 10;
            int type_start = j;
            while (j < src_len && src[j] != ',' && src[j] != ')') j++;
            char type_buf[64] = {0};
            int tlen = j - type_start;
            if (tlen > 0 && tlen < 63) {
                memcpy(type_buf, src + type_start, tlen);
                type_buf[tlen] = '\0';
                while (tlen > 0 && type_buf[tlen - 1] == ' ') type_buf[--tlen] = '\0';
            }
            /* check if zer_malloc_bytes follows */
            int rest = j;
            while (rest < src_len && (src[rest] == ',' || src[rest] == ' ')) rest++;
            if (tlen > 0 && starts_with(src, rest, src_len, "zer_malloc_bytes(")) {
                /* find the outer closing paren of @ptrcast */
                int outer_close = find_close_paren(src, i + 9, src_len);
                if (outer_close > 0) {
                    const char *slab = get_slab_name(type_buf);
                    /* scan backward for variable name and type declaration */
                    int k = i - 1;
                    while (k > 0 && (src[k] == ' ' || src[k] == '\t')) k--;
                    if (k > 0 && src[k] == '=') {
                        k--;
                        while (k > 0 && (src[k] == ' ' || src[k] == '\t')) k--;
                        int var_end = k + 1;
                        while (k > 0 && (isalnum(src[k]) || src[k] == '_')) k--;
                        if (!isalnum(src[k]) && src[k] != '_') k++;
                        char var_name[64] = {0};
                        int vlen = var_end - k;
                        if (vlen > 0 && vlen < 63) {
                            memcpy(var_name, src + k, vlen);
                            var_name[vlen] = '\0';

                            /* scan further back to remove "TYPE *" declaration */
                            int decl_start = k - 1;
                            while (decl_start > 0 && (src[decl_start] == ' ' || src[decl_start] == '\t')) decl_start--;
                            if (decl_start > 0 && src[decl_start] == '*') {
                                decl_start--;
                                while (decl_start > 0 && (src[decl_start] == ' ' || src[decl_start] == '\t')) decl_start--;
                                /* now at end of type name — remove from output buffer */
                                int type_end = decl_start + 1;
                                while (decl_start > 0 && (isalnum(src[decl_start]) || src[decl_start] == '_')) decl_start--;
                                if (!isalnum(src[decl_start]) && src[decl_start] != '_') decl_start++;

                                /* truncate output to before the type declaration */
                                /* find how many chars to remove from out_buf */
                                int remove_len = (int)(src + i - (src + decl_start));
                                if (out_len >= remove_len) {
                                    out_len -= remove_len;
                                }
                            }

                            /* emit the Slab alloc pattern */
                            out_str("?Handle(");
                            out_str(type_buf);
                            out_str(") ");
                            out_str(var_name);
                            out_str("_maybe = ");
                            out_str(slab);
                            out_str(".alloc();\n");
                            /* find indentation */
                            int indent_pos = decl_start - 1;
                            while (indent_pos > 0 && src[indent_pos] != '\n') indent_pos--;
                            if (indent_pos > 0) indent_pos++;
                            int indent_len = 0;
                            while (indent_pos + indent_len < src_len &&
                                   (src[indent_pos + indent_len] == ' ' || src[indent_pos + indent_len] == '\t'))
                                indent_len++;
                            out_write(src + indent_pos, indent_len);
                            out_str("Handle(");
                            out_str(type_buf);
                            out_str(") ");
                            out_str(var_name);
                            out_str("_h = ");
                            out_str(var_name);
                            out_str("_maybe orelse return");

                            /* skip past the closing paren and semicolon */
                            i = outer_close + 1;
                            upgrades++;
                            continue;
                        }
                    }
                }
            }
        }

        /* ---- zer_free(var) → type_slab.free(var_h) ---- */
        if (starts_with(src, i, src_len, "zer_free(") && word_boundary_before(src, i)) {
            int open = i + 8;
            int close = find_close_paren(src, open, src_len);
            if (close > 0) {
                Span args[1];
                int argc = extract_args(src, open, close, args, 1);
                if (argc == 1) {
                    char var_name[64] = {0};
                    int vl = args[0].len < 63 ? args[0].len : 63;
                    memcpy(var_name, src + args[0].start, vl);
                    var_name[vl] = '\0';
                    AllocInfo *ai = find_alloc(var_name);
                    if (ai) {
                        const char *slab = get_slab_name(ai->type_name);
                        out_str(slab);
                        out_str(".free(");
                        out_str(var_name);
                        out_str("_h)");
                        i = close + 1;
                        upgrades++;
                        continue;
                    }
                }
            }
            /* unmatched free — keep as compat */
            kept++;
        }

        /* ---- var.field → slab.get(var_h).field for malloc'd variables ---- */
        if (src[i] != '.' && (isalpha(src[i]) || src[i] == '_')) {
            /* scan identifier */
            int id_start = i;
            while (i < src_len && (isalnum(src[i]) || src[i] == '_')) i++;
            int id_len = i - id_start;
            char ident[64] = {0};
            if (id_len < 63) memcpy(ident, src + id_start, id_len);
            ident[id_len < 63 ? id_len : 63] = '\0';

            /* check if this ident is a malloc'd variable followed by . */
            AllocInfo *ai = find_alloc(ident);
            if (ai && i < src_len && src[i] == '.') {
                const char *slab = get_slab_name(ai->type_name);
                out_str(slab);
                out_str(".get(");
                out_str(ident);
                out_str("_h)");
                /* don't consume the . — it'll be emitted next */
                continue;
            }

            /* check for !var (null check on malloc result) → !var_maybe */
            /* this is tricky — skip for now, emit as-is */

            /* emit the identifier (possibly mapped) */
            const char *mapped = NULL;
            /* re-check type maps and compat maps for the ident */
            /* but we already handled those above — just emit raw */
            out_write(src + id_start, id_len);
            continue;
        }

        /* ---- Compat calls still kept ---- */
        if (starts_with(src, i, src_len, "zer_calloc_bytes(") && word_boundary_before(src, i)) {
            kept++;
        }
        if (starts_with(src, i, src_len, "zer_realloc_bytes(") && word_boundary_before(src, i)) {
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

/* add Slab declarations at top of file */
static void add_slab_declarations(void) {
    if (slab_type_count == 0) return;

    /* find insertion point — after last import/cinclude line */
    int insert_at = 0;
    for (int i = 0; i < out_len; i++) {
        if (out_buf[i] == '\n') {
            int j = i + 1;
            while (j < out_len && (out_buf[j] == ' ' || out_buf[j] == '\t')) j++;
            if (j + 6 < out_len && memcmp(out_buf + j, "import", 6) == 0) insert_at = i + 1;
            if (j + 8 < out_len && memcmp(out_buf + j, "cinclude", 8) == 0) insert_at = i + 1;
        }
    }
    /* advance to end of that line */
    while (insert_at < out_len && out_buf[insert_at] != '\n') insert_at++;
    if (insert_at < out_len) insert_at++;

    /* build the declarations string */
    char decls[2048];
    int dlen = 0;
    dlen += snprintf(decls + dlen, sizeof(decls) - dlen, "\n");
    for (int i = 0; i < slab_type_count; i++) {
        dlen += snprintf(decls + dlen, sizeof(decls) - dlen,
            "static Slab(%s) %s;\n", slab_types[i].name, slab_types[i].slab_name);
    }

    /* insert into output */
    while (out_len + dlen >= out_cap) { out_cap *= 2; out_buf = (char *)realloc(out_buf, out_cap); }
    memmove(out_buf + insert_at + dlen, out_buf + insert_at, out_len - insert_at);
    memcpy(out_buf + insert_at, decls, dlen);
    out_len += dlen;
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

    /* Layer 2: pre-scan for malloc/free patterns */
    scan_allocs(src, src_len);

    out_init();
    upgrade(src, src_len);

    /* post-processing */
    add_slab_declarations();
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
