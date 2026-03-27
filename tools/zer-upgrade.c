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
    char func_name[64];     /* function where malloc occurs */
    int malloc_pos;         /* byte position of malloc in source */
    int func_start;         /* byte position of function start */
    int func_end;           /* byte position of function end (closing }) */
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

/* check if a type name is a primitive (should NOT become a Slab type) */
static bool is_primitive_type(const char *name) {
    static const char *prims[] = {
        "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64",
        "usize", "f32", "f64", "bool", "void", "opaque",
        "uint8_t", "uint16_t", "uint32_t", "uint64_t",
        "int8_t", "int16_t", "int32_t", "int64_t",
        "char", "int", "float", "double", "short", "long",
        "size_t", "ssize_t", "ptrdiff_t",
        NULL
    };
    for (int i = 0; prims[i]; i++) {
        if (strcmp(name, prims[i]) == 0) return true;
    }
    return false;
}

/* check if a type name has a Slab allocator */
static bool is_slab_type(const char *type_name) {
    for (int i = 0; i < slab_type_count; i++) {
        if (strcmp(slab_types[i].name, type_name) == 0)
            return true;
    }
    return false;
}

/* Track handle parameters: function params of Slab types get rewritten */
#define MAX_HANDLE_PARAMS 32
typedef struct {
    char var_name[64];
    char type_name[64];
    int func_start;
    int func_end;
} HandleParam;

static HandleParam handle_params[MAX_HANDLE_PARAMS * 16]; /* per scan */
static int handle_param_count = 0;

/* check if a variable is a handle parameter in the current function scope */
static HandleParam *find_handle_param(const char *var_name, int pos) {
    for (int i = 0; i < handle_param_count; i++) {
        if (strcmp(handle_params[i].var_name, var_name) == 0 &&
            pos >= handle_params[i].func_start && pos <= handle_params[i].func_end)
            return &handle_params[i];
    }
    return NULL;
}

/* check if a variable name was malloc'd — only within the same function scope */
static AllocInfo *find_alloc(const char *var_name, int pos) {
    for (int i = 0; i < alloc_count; i++) {
        if (strcmp(allocs[i].var_name, var_name) == 0 &&
            pos >= allocs[i].func_start && pos <= allocs[i].func_end)
            return &allocs[i];
    }
    return NULL;
}

/* find the enclosing function boundaries for a position */
static void find_func_bounds(const char *src, int src_len, int pos, int *out_start, int *out_end) {
    /* scan backward for opening { at depth 0 (function body start) */
    int depth = 0;
    int fstart = pos;
    for (int i = pos; i >= 0; i--) {
        if (src[i] == '}') depth++;
        if (src[i] == '{') {
            if (depth == 0) { fstart = i; break; }
            depth--;
        }
    }
    /* scan forward for matching closing } */
    depth = 0;
    int fend = src_len;
    for (int i = fstart; i < src_len; i++) {
        if (src[i] == '{') depth++;
        if (src[i] == '}') {
            depth--;
            if (depth == 0) { fend = i; break; }
        }
    }
    *out_start = fstart;
    *out_end = fend;
}

/* Pre-scan: find all malloc/free patterns in source */
static void scan_allocs(const char *src, int src_len) {
    alloc_count = 0;
    slab_type_count = 0;

    for (int i = 0; i < src_len; i++) {
        /* Pattern: TYPE *VAR = @ptrcast(*TYPE, zer_malloc_bytes(@size(TYPE))); */
        if (starts_with(src, i, src_len, "@ptrcast(*") && word_boundary_before(src, i)) {
            int j = i + 10;
            int type_start = j;
            while (j < src_len && src[j] != ',' && src[j] != ')') j++;
            int type_len = j - type_start;
            if (type_len > 0 && type_len < 63) {
                char type_name[64];
                memcpy(type_name, src + type_start, type_len);
                type_name[type_len] = '\0';
                while (type_len > 0 && type_name[type_len - 1] == ' ') type_name[--type_len] = '\0';

                /* skip primitive types — they don't make sense as Slab(u8) etc. */
                if (is_primitive_type(type_name)) continue;

                if (strstr(src + j, "zer_malloc_bytes") && (strstr(src + j, "zer_malloc_bytes") - (src + j)) < 30) {
                    int k = i - 1;
                    while (k > 0 && (src[k] == ' ' || src[k] == '\t')) k--;
                    if (k > 0 && src[k] == '=') {
                        k--;
                        while (k > 0 && (src[k] == ' ' || src[k] == '\t')) k--;
                        int var_end = k + 1;
                        while (k > 0 && (isalnum(src[k]) || src[k] == '_')) k--;
                        k++;
                        int var_len = var_end - k;
                        if (var_len > 0 && var_len < 63 && alloc_count < MAX_ALLOCS) {
                            AllocInfo *ai = &allocs[alloc_count++];
                            memcpy(ai->var_name, src + k, var_len);
                            ai->var_name[var_len] = '\0';
                            strncpy(ai->type_name, type_name, 63);
                            ai->malloc_pos = i;
                            ai->has_free = false;
                            find_func_bounds(src, src_len, i, &ai->func_start, &ai->func_end);
                            get_slab_name(type_name);
                        }
                    }
                }
            }
        }

        /* Pattern: zer_free(VAR); — match to alloc */
        if (starts_with(src, i, src_len, "zer_free(") && word_boundary_before(src, i)) {
            int open = i + 8;
            int close = find_close_paren(src, open, src_len);
            if (close > 0) {
                int vs = open + 1;
                while (vs < close && (src[vs] == ' ' || src[vs] == '\t')) vs++;
                int ve = close;
                while (ve > vs && (src[ve - 1] == ' ' || src[ve - 1] == '\t')) ve--;
                int vlen = ve - vs;
                if (vlen > 0 && vlen < 63) {
                    char var_name[64];
                    memcpy(var_name, src + vs, vlen);
                    var_name[vlen] = '\0';
                    AllocInfo *ai = find_alloc(var_name, i);
                    if (ai) ai->has_free = true;
                }
            }
        }
    }
}

/* Second scan (a): find local variables of Slab types assigned from function returns.
 * Pattern: SlabType *var = func_call(...)
 * These are handle variables even though they weren't malloc'd directly.
 * Must run AFTER scan_allocs so slab_types is populated. */
static void scan_local_slab_vars(const char *src, int src_len) {
    if (slab_type_count == 0) return;

    for (int i = 0; i < src_len; i++) {
        /* skip if not an identifier start */
        if (!isalpha(src[i]) && src[i] != '_') continue;

        /* read identifier */
        int id_start = i;
        while (i < src_len && (isalnum(src[i]) || src[i] == '_')) i++;
        int id_len = i - id_start;
        if (id_len <= 0 || id_len >= 63) continue;

        char type_name[64] = {0};
        memcpy(type_name, src + id_start, id_len);

        if (!is_slab_type(type_name)) continue;

        /* skip spaces */
        int p = i;
        while (p < src_len && src[p] == ' ') p++;

        /* check for * */
        if (p >= src_len || src[p] != '*') continue;
        p++;
        while (p < src_len && src[p] == ' ') p++;

        /* read var name */
        int vn_start = p;
        while (p < src_len && (isalnum(src[p]) || src[p] == '_')) p++;
        int vn_len = p - vn_start;
        if (vn_len <= 0 || vn_len >= 63) continue;

        /* check for = (assignment, not just declaration) */
        while (p < src_len && src[p] == ' ') p++;
        if (p >= src_len || src[p] != '=') continue;

        /* check it's NOT already a malloc alloc (avoid double-registration) */
        char var_name[64] = {0};
        memcpy(var_name, src + vn_start, vn_len);

        if (find_alloc(var_name, vn_start)) continue; /* already tracked */

        /* register as handle param (reuses HandleParam for simplicity) */
        if (handle_param_count < MAX_HANDLE_PARAMS * 16) {
            HandleParam *hp = &handle_params[handle_param_count++];
            strncpy(hp->var_name, var_name, 63);
            strncpy(hp->type_name, type_name, 63);
            find_func_bounds(src, src_len, vn_start, &hp->func_start, &hp->func_end);
        }
    }
}

/* Second scan (b): find function params of Slab types.
 * Pattern: TYPE *NAME in function signature → Handle(TYPE) NAME_h
 * Must run AFTER scan_allocs so slab_types is populated. */
static void scan_handle_params(const char *src, int src_len) {
    /* don't reset handle_param_count — scan_local_slab_vars may have added entries */
    if (slab_type_count == 0) return;

    for (int i = 0; i < src_len; i++) {
        /* look for function signatures: IDENT IDENT( or IDENT *IDENT( */
        /* We detect: TYPE *PARAM inside function param lists.
         * A param list starts with ( and we're inside a top-level declaration. */
        /* Simple heuristic: find ( that follows IDENT IDENT pattern (return_type func_name) */

        /* Find lines that look like function declarations:
         * word word( or word *word( at start of line */
        if (src[i] == '(' && i > 2) {
            /* check if this could be a function decl paren */
            int k = i - 1;
            while (k > 0 && (isalnum(src[k]) || src[k] == '_')) k--;
            if (k >= 0 && k < i - 1) {
                /* scan inside the parens for SlabType *name patterns */
                int close = find_close_paren(src, i, src_len);
                if (close < 0 || close - i > 500) continue; /* not a short param list */

                int func_start, func_end;
                /* find function body bounds — look for { after ) */
                int body = close + 1;
                while (body < src_len && (src[body] == ' ' || src[body] == '\n' || src[body] == '\t')) body++;
                if (body >= src_len || src[body] != '{') continue; /* not a function def */
                find_func_bounds(src, src_len, body, &func_start, &func_end);

                /* scan params */
                int p = i + 1;
                while (p < close) {
                    /* skip whitespace */
                    while (p < close && (src[p] == ' ' || src[p] == '\t' || src[p] == '\n' || src[p] == ',')) p++;
                    if (p >= close) break;

                    /* skip const */
                    if (p + 5 < close && memcmp(src + p, "const", 5) == 0 && !isalnum(src[p + 5])) p += 5;
                    while (p < close && src[p] == ' ') p++;

                    /* read type name */
                    int tn_start = p;
                    while (p < close && (isalnum(src[p]) || src[p] == '_')) p++;
                    int tn_len = p - tn_start;
                    if (tn_len == 0) { p++; continue; }

                    char type_name[64] = {0};
                    if (tn_len < 63) memcpy(type_name, src + tn_start, tn_len);

                    /* skip spaces */
                    while (p < close && src[p] == ' ') p++;

                    /* check for * */
                    if (p < close && src[p] == '*') {
                        p++;
                        while (p < close && src[p] == ' ') p++;

                        /* read param name */
                        int pn_start = p;
                        while (p < close && (isalnum(src[p]) || src[p] == '_')) p++;
                        int pn_len = p - pn_start;

                        /* check if type is a Slab type */
                        if (pn_len > 0 && pn_len < 63 && is_slab_type(type_name) &&
                            handle_param_count < MAX_HANDLE_PARAMS * 16) {
                            HandleParam *hp = &handle_params[handle_param_count++];
                            memcpy(hp->var_name, src + pn_start, pn_len);
                            hp->var_name[pn_len] = '\0';
                            strncpy(hp->type_name, type_name, 63);
                            hp->func_start = func_start;
                            hp->func_end = func_end;
                        }
                    }

                    /* skip to next comma or close paren */
                    while (p < close && src[p] != ',') p++;
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

        /* ---- skip string literals — don't replace identifiers inside strings ---- */
        if (src[i] == '"') {
            out_write(src + i, 1); i++;
            while (i < src_len && src[i] != '"') {
                if (src[i] == '\\' && i + 1 < src_len) { out_write(src + i, 2); i += 2; continue; }
                out_write(src + i, 1); i++;
            }
            if (i < src_len) { out_write(src + i, 1); i++; } /* closing " */
            continue;
        }

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
         * Strategy: DON'T rollback output. Instead, detect at start of line
         * and replace the entire line before any chars are emitted. */
        /* We detect this at the LINE level: scan the current line for the pattern,
         * and if found, emit the replacement and skip the whole line. */
        if (src[i] == '\n' || i == 0) {
            /* look at the NEXT line */
            int line_start = (src[i] == '\n') ? i + 1 : i;
            if (line_start < src_len) {
                /* skip indentation */
                int ls = line_start;
                while (ls < src_len && (src[ls] == ' ' || src[ls] == '\t')) ls++;

                /* check for: IDENT *IDENT = @ptrcast(*IDENT, zer_malloc_bytes( */
                int p = ls;
                /* skip type name */
                int tn_start = p;
                while (p < src_len && (isalnum(src[p]) || src[p] == '_')) p++;
                int tn_len = p - tn_start;
                /* skip spaces + * */
                while (p < src_len && src[p] == ' ') p++;
                if (p < src_len && src[p] == '*') {
                    p++;
                    /* skip var name */
                    while (p < src_len && src[p] == ' ') p++;
                    int vn_start = p;
                    while (p < src_len && (isalnum(src[p]) || src[p] == '_')) p++;
                    int vn_len = p - vn_start;
                    /* skip = */
                    while (p < src_len && src[p] == ' ') p++;
                    if (p < src_len && src[p] == '=' && vn_len > 0 && tn_len > 0) {
                        p++;
                        while (p < src_len && src[p] == ' ') p++;
                        /* check for @ptrcast(* */
                        if (starts_with(src, p, src_len, "@ptrcast(*")) {
                            int cast_start = p + 10;
                            int ct = cast_start;
                            while (ct < src_len && src[ct] != ',' && src[ct] != ')') ct++;
                            /* check zer_malloc_bytes after comma */
                            int rest = ct;
                            while (rest < src_len && (src[rest] == ',' || src[rest] == ' ')) rest++;
                            if (starts_with(src, rest, src_len, "zer_malloc_bytes(")) {
                                /* MATCH! Replace the entire line. */
                                char type_buf[64] = {0}, var_buf[64] = {0};
                                if (tn_len < 63) memcpy(type_buf, src + tn_start, tn_len);
                                if (vn_len < 63) memcpy(var_buf, src + vn_start, vn_len);

                                /* skip primitive types — keep as compat call */
                                if (is_primitive_type(type_buf)) goto emit_line_asis;

                                const char *slab = get_slab_name(type_buf);

                                /* emit newline if we're not at start */
                                if (i > 0 && src[i] == '\n') out_str("\n");

                                /* emit indentation */
                                out_write(src + line_start, ls - line_start);

                                /* emit Handle alloc pattern */
                                out_str("?Handle(");
                                out_str(type_buf);
                                out_str(") ");
                                out_str(var_buf);
                                out_str("_maybe = ");
                                out_str(slab);
                                out_str(".alloc();\n");
                                out_write(src + line_start, ls - line_start);
                                out_str("Handle(");
                                out_str(type_buf);
                                out_str(") ");
                                out_str(var_buf);
                                out_str("_h = ");
                                out_str(var_buf);
                                out_str("_maybe orelse return;");

                                /* skip to end of this line */
                                i = line_start;
                                while (i < src_len && src[i] != '\n') i++;
                                /* skip the next line if it's a null check: if (!var) ... */
                                if (i < src_len && src[i] == '\n') {
                                    int peek = i + 1;
                                    while (peek < src_len && (src[peek] == ' ' || src[peek] == '\t')) peek++;
                                    if (starts_with(src, peek, src_len, "if (!") &&
                                        strncmp(src + peek + 5, var_buf, strlen(var_buf)) == 0) {
                                        /* skip null check line */
                                        i++;
                                        while (i < src_len && src[i] != '\n') i++;
                                    }
                                }
                                upgrades++;
                                continue;
                            }
                        }
                    }
                }
            }
        }

        emit_line_asis: /* label for primitive type skip — emit line as-is */

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
                    AllocInfo *ai = find_alloc(var_name, i);
                    HandleParam *hp = ai ? NULL : find_handle_param(var_name, i);
                    if (ai || hp) {
                        const char *type = ai ? ai->type_name : hp->type_name;
                        const char *slab = get_slab_name(type);
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
        /* skip if preceded by . — it's a field name, not a variable reference */
        if (src[i] != '.' && (isalpha(src[i]) || src[i] == '_') &&
            !(i > 0 && src[i - 1] == '.')) {
            /* scan identifier */
            int id_start = i;
            while (i < src_len && (isalnum(src[i]) || src[i] == '_')) i++;
            int id_len = i - id_start;
            char ident[64] = {0};
            if (id_len < 63) memcpy(ident, src + id_start, id_len);
            ident[id_len < 63 ? id_len : 63] = '\0';

            /* check if this ident is a malloc'd or handle-param variable */
            AllocInfo *ai = find_alloc(ident, id_start);
            HandleParam *hp = ai ? NULL : find_handle_param(ident, id_start);

            /* skip if this is a var declaration (Type *name = ...) — rewrite_signatures handles it */
            if (hp && !ai) {
                int check_after = i;
                while (check_after < src_len && src[check_after] == ' ') check_after++;
                if (check_after < src_len && src[check_after] == '=') {
                    /* check backward for * (pointer decl) */
                    int check_before = id_start - 1;
                    while (check_before > 0 && src[check_before] == ' ') check_before--;
                    if (check_before > 0 && src[check_before] == '*') {
                        /* this is a declaration — don't add _h, rewrite_signatures does it */
                        out_write(src + id_start, id_len);
                        continue;
                    }
                }
            }

            if (ai || hp) {
                const char *type = ai ? ai->type_name : hp->type_name;
                const char *slab = get_slab_name(type);
                if (i < src_len && src[i] == '.') {
                    /* var.field → slab.get(var_h).field */
                    out_str(slab);
                    out_str(".get(");
                    out_str(ident);
                    out_str("_h)");
                    /* don't consume the . — it'll be emitted next */
                } else {
                    /* bare reference: return var, func(var), x = var
                     * → var_h (the handle replaces the pointer) */
                    out_str(ident);
                    out_str("_h");
                }
                continue;
            }

            /* emit the identifier as-is */
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

/* rewrite function signatures: SlabType *param → Handle(SlabType) param_h
 * and SlabType *func_name( → ?Handle(SlabType) func_name( */
static void rewrite_signatures(void) {
    if (slab_type_count == 0) return;
    /* rewrite function signatures containing Slab-type params */

    /* work on a copy — build new buffer with replacements */
    char *new_buf = (char *)malloc(out_cap * 2);
    int new_len = 0;
    int new_cap = out_cap * 2;

    #define NB_WRITE(s, l) do { \
        while (new_len + (l) >= new_cap) { new_cap *= 2; new_buf = (char *)realloc(new_buf, new_cap); } \
        memcpy(new_buf + new_len, (s), (l)); new_len += (l); \
    } while(0)
    #define NB_STR(s) NB_WRITE(s, (int)strlen(s))

    int i = 0;
    while (i < out_len) {
        /* find start of each line */
        int line_start = i;
        int line_end = i;
        while (line_end < out_len && out_buf[line_end] != '\n') line_end++;

        /* check if this line is a function declaration:
         * must contain ( and end with ) { or have , for multi-line */
        bool has_paren = false;
        for (int j = line_start; j < line_end; j++) {
            if (out_buf[j] == '(') { has_paren = true; break; }
        }

        bool is_func_decl = false;
        if (has_paren) {
            /* check if line ends with { or ) — skip trailing whitespace and \r */
            for (int j = line_end - 1; j >= line_start; j--) {
                if (out_buf[j] == '{') { is_func_decl = true; break; }
                if (out_buf[j] == ')') { is_func_decl = true; break; }
                if (out_buf[j] != ' ' && out_buf[j] != '\t' && out_buf[j] != '\r') break;
            }
        }

        if (is_func_decl) {
            /* process this line character by character, replacing Slab type patterns */
            int j = line_start;

            /* check for return type: SlabType *func_name( → ?Handle(SlabType) func_name( */
            int ls = j;
            while (ls < line_end && (out_buf[ls] == ' ' || out_buf[ls] == '\t')) ls++;
            /* skip 'static' if present */
            bool has_static = false;
            if (ls + 7 < line_end && memcmp(out_buf + ls, "static ", 7) == 0) {
                has_static = true;
                NB_WRITE(out_buf + j, ls + 7 - j);
                j = ls + 7;
                ls = j;
            }

            /* check if return type is SlabType * */
            int rt_start = ls;
            while (ls < line_end && (isalnum(out_buf[ls]) || out_buf[ls] == '_')) ls++;
            int rt_len = ls - rt_start;
            if (rt_len > 0) {
                char rt_name[64] = {0};
                if (rt_len < 63) memcpy(rt_name, out_buf + rt_start, rt_len);
                int rs = ls;
                while (rs < line_end && out_buf[rs] == ' ') rs++;
                if (rs < line_end && out_buf[rs] == '*' && is_slab_type(rt_name)) {
                    /* SlabType *func_name → ?Handle(SlabType) func_name */
                    NB_WRITE(out_buf + j, rt_start - j); /* emit indent */
                    NB_STR("?Handle(");
                    NB_STR(rt_name);
                    NB_STR(") ");
                    j = rs + 1; /* skip past * */
                    while (j < line_end && out_buf[j] == ' ') j++; /* skip spaces */
                }
            }

            /* now process the rest, looking for SlabType *param inside parens */
            bool in_params = false;
            while (j < line_end) {
                if (out_buf[j] == '(') in_params = true;
                if (out_buf[j] == ')') in_params = false;

                if (in_params && (isalpha(out_buf[j]) || out_buf[j] == '_')) {
                    /* read identifier */
                    int id_start = j;
                    while (j < line_end && (isalnum(out_buf[j]) || out_buf[j] == '_')) j++;
                    int id_len = j - id_start;
                    char id_name[64] = {0};
                    if (id_len < 63) memcpy(id_name, out_buf + id_start, id_len);

                    /* skip spaces */
                    int after = j;
                    while (after < line_end && out_buf[after] == ' ') after++;

                    /* check: is this SlabType * param_name ? */
                    if (after < line_end && out_buf[after] == '*' && is_slab_type(id_name)) {
                        after++; /* skip * */
                        while (after < line_end && out_buf[after] == ' ') after++;
                        /* read param name */
                        int pn_start = after;
                        while (after < line_end && (isalnum(out_buf[after]) || out_buf[after] == '_')) after++;
                        int pn_len = after - pn_start;

                        if (pn_len > 0) {
                            /* emit: Handle(Type) name_h */
                            NB_STR("Handle(");
                            NB_STR(id_name);
                            NB_STR(") ");
                            NB_WRITE(out_buf + pn_start, pn_len);
                            NB_STR("_h");
                            j = after;
                            continue;
                        }
                    }

                    /* not a slab type — emit as-is */
                    NB_WRITE(out_buf + id_start, id_len);
                    continue;
                }

                NB_WRITE(out_buf + j, 1);
                j++;
            }
        } else {
            /* not a function declaration — check for struct field: SlabType *field; */
            bool did_struct_field = false;
            {
                int ls = line_start;
                while (ls < line_end && (out_buf[ls] == ' ' || out_buf[ls] == '\t')) ls++;
                /* read potential type name */
                int ft_start = ls;
                while (ls < line_end && (isalnum(out_buf[ls]) || out_buf[ls] == '_')) ls++;
                int ft_len = ls - ft_start;
                if (ft_len > 0 && ft_len < 63) {
                    char ft_name[64] = {0};
                    memcpy(ft_name, out_buf + ft_start, ft_len);
                    /* skip spaces */
                    while (ls < line_end && out_buf[ls] == ' ') ls++;
                    /* check for * */
                    if (ls < line_end && out_buf[ls] == '*' && is_slab_type(ft_name)) {
                        ls++;
                        while (ls < line_end && out_buf[ls] == ' ') ls++;
                        /* read field name */
                        int fn_start = ls;
                        while (ls < line_end && (isalnum(out_buf[ls]) || out_buf[ls] == '_')) ls++;
                        int fn_len = ls - fn_start;
                        /* check what follows: ; (struct field) or = (local var decl) */
                        int check = ls;
                        while (check < line_end && (out_buf[check] == ' ' || out_buf[check] == '\t' || out_buf[check] == '\r')) check++;
                        if (fn_len > 0 && check < line_end && out_buf[check] == ';') {
                            /* struct field: emit ?Handle(Type) field_name; */
                            NB_WRITE(out_buf + line_start, ft_start - line_start);
                            NB_STR("?Handle(");
                            NB_STR(ft_name);
                            NB_STR(") ");
                            NB_WRITE(out_buf + fn_start, fn_len);
                            NB_STR(";");
                            did_struct_field = true;
                        } else if (fn_len > 0 && check < line_end && out_buf[check] == '=') {
                            /* local var decl: Task *a = expr → ?Handle(Task) a_h = expr */
                            NB_WRITE(out_buf + line_start, ft_start - line_start);
                            NB_STR("?Handle(");
                            NB_STR(ft_name);
                            NB_STR(") ");
                            NB_WRITE(out_buf + fn_start, fn_len);
                            NB_STR("_h ");
                            /* emit the rest of the line (= expr;) */
                            NB_WRITE(out_buf + check, line_end - check);
                            did_struct_field = true;
                        }
                    }
                }
            }
            if (!did_struct_field) {
                NB_WRITE(out_buf + line_start, line_end - line_start);
            }
        }

        /* copy newline */
        if (line_end < out_len) {
            NB_WRITE(out_buf + line_end, 1);
        }
        i = line_end + 1;
    }

    /* swap buffers */
    free(out_buf);
    out_buf = new_buf;
    out_len = new_len;
    out_cap = new_cap;

    #undef NB_WRITE
    #undef NB_STR
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
    scan_local_slab_vars(src, src_len);
    scan_handle_params(src, src_len);

    out_init();
    upgrade(src, src_len);

    /* post-processing */
    add_slab_declarations();
    rewrite_signatures();
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
