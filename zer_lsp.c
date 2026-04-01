/* ================================================================
 * ZER Language Server Protocol (LSP) Implementation
 *
 * Single-file LSP server for ZER-LANG. Reuses the compiler
 * pipeline (lexer → parser → checker → zercheck) for:
 *   - Real-time diagnostics (errors/warnings)
 *   - Hover (show symbol type)
 *   - Go-to-definition
 *   - Document symbols
 *
 * Communicates over stdin/stdout using JSON-RPC 2.0.
 * Build: gcc -std=c99 -O2 -o zer-lsp zer_lsp.c lexer.c parser.c
 *        ast.c types.c checker.c zercheck.c
 * ================================================================ */

#define _POSIX_C_SOURCE 200809L /* for strdup, fileno */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#define dup    _dup
#define dup2   _dup2
#define fileno _fileno
#define close  _close
#else
#include <unistd.h>
#endif

#include "lexer.h"
#include "ast.h"
#include "parser.h"
#include "types.h"
#include "checker.h"
#include "zercheck.h"

/* ================================================================
 * Logging (to stderr — never stdout, that's the LSP channel)
 * ================================================================ */

static FILE *log_file = NULL;

static void lsp_log(const char *fmt, ...) {
    if (!log_file) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(log_file, fmt, args);
    va_end(args);
    fprintf(log_file, "\n");
    fflush(log_file);
}

/* ================================================================
 * String Builder — for constructing JSON responses
 * ================================================================ */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} StringBuilder;

static void sb_init(StringBuilder *sb) {
    sb->cap = 4096;
    sb->buf = (char *)malloc(sb->cap);
    sb->len = 0;
    sb->buf[0] = '\0';
}

static void sb_ensure(StringBuilder *sb, size_t extra) {
    while (sb->len + extra + 1 > sb->cap) {
        sb->cap *= 2;
        sb->buf = (char *)realloc(sb->buf, sb->cap);
    }
}

static void sb_append(StringBuilder *sb, const char *str) {
    size_t slen = strlen(str);
    sb_ensure(sb, slen);
    memcpy(sb->buf + sb->len, str, slen);
    sb->len += slen;
    sb->buf[sb->len] = '\0';
}

static void sb_appendf(StringBuilder *sb, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    sb_ensure(sb, (size_t)needed);
    va_start(args, fmt);
    vsnprintf(sb->buf + sb->len, (size_t)needed + 1, fmt, args);
    va_end(args);
    sb->len += (size_t)needed;
}

/* Append a JSON-escaped string (escapes \ " and control chars) */
static void sb_append_json_string(StringBuilder *sb, const char *str) {
    sb_append(sb, "\"");
    for (const char *p = str; *p; p++) {
        switch (*p) {
        case '"':  sb_append(sb, "\\\""); break;
        case '\\': sb_append(sb, "\\\\"); break;
        case '\n': sb_append(sb, "\\n"); break;
        case '\r': sb_append(sb, "\\r"); break;
        case '\t': sb_append(sb, "\\t"); break;
        default:
            if ((unsigned char)*p < 0x20) {
                sb_appendf(sb, "\\u%04x", (unsigned char)*p);
            } else {
                char c[2] = { *p, 0 };
                sb_append(sb, c);
            }
        }
    }
    sb_append(sb, "\"");
}

static void sb_free(StringBuilder *sb) {
    free(sb->buf);
    sb->buf = NULL;
    sb->len = sb->cap = 0;
}

/* ================================================================
 * Minimal JSON Parser — extracts values from LSP messages
 *
 * Not a general-purpose JSON parser. Handles the specific
 * patterns used by LSP (nested objects, string/int values).
 * ================================================================ */

/* Skip whitespace */
static const char *json_skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* Skip a JSON value (string, number, object, array, bool, null) */
static const char *json_skip_value(const char *p) {
    p = json_skip_ws(p);
    switch (*p) {
    case '"': {
        p++; /* skip opening " */
        while (*p && *p != '"') {
            if (*p == '\\') p++; /* skip escaped char */
            if (*p) p++;
        }
        if (*p == '"') p++; /* skip closing " */
        return p;
    }
    case '{': {
        p++; int depth = 1;
        while (*p && depth > 0) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            else if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\') p++;
                    if (*p) p++;
                }
            }
            if (*p) p++;
        }
        return p;
    }
    case '[': {
        p++; int depth = 1;
        while (*p && depth > 0) {
            if (*p == '[') depth++;
            else if (*p == ']') depth--;
            else if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\') p++;
                    if (*p) p++;
                }
            }
            if (*p) p++;
        }
        return p;
    }
    case 't': return p + 4; /* true */
    case 'f': return p + 5; /* false */
    case 'n': return p + 4; /* null */
    default: /* number */
        if (*p == '-') p++;
        while (isdigit((unsigned char)*p)) p++;
        if (*p == '.') { p++; while (isdigit((unsigned char)*p)) p++; }
        if (*p == 'e' || *p == 'E') {
            p++; if (*p == '+' || *p == '-') p++;
            while (isdigit((unsigned char)*p)) p++;
        }
        return p;
    }
}

/* Find a key in a JSON object. Returns pointer to the value, or NULL. */
static const char *json_find_key(const char *json, const char *key) {
    if (!json) return NULL;
    json = json_skip_ws(json);
    if (*json != '{') return NULL;
    json++; /* skip { */

    size_t klen = strlen(key);
    while (*json && *json != '}') {
        json = json_skip_ws(json);
        if (*json != '"') break;
        json++; /* skip " */
        const char *kstart = json;
        while (*json && *json != '"') {
            if (*json == '\\') json++;
            json++;
        }
        size_t found_len = (size_t)(json - kstart);
        if (*json == '"') json++; /* skip closing " */
        json = json_skip_ws(json);
        if (*json == ':') json++; /* skip : */
        json = json_skip_ws(json);

        if (found_len == klen && memcmp(kstart, key, klen) == 0) {
            return json; /* pointer to value */
        }

        json = json_skip_value(json);
        json = json_skip_ws(json);
        if (*json == ',') json++;
    }
    return NULL;
}

/* Read a JSON string value. Returns malloc'd string, or NULL. */
static char *json_read_string(const char *val) {
    if (!val) return NULL;
    val = json_skip_ws(val);
    if (*val != '"') return NULL;
    val++; /* skip " */

    /* first pass: compute length */
    size_t len = 0;
    const char *p = val;
    while (*p && *p != '"') {
        if (*p == '\\') { p++; }
        p++; len++;
    }

    char *result = (char *)malloc(len + 1);
    size_t i = 0;
    p = val;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'n': result[i++] = '\n'; break;
            case 'r': result[i++] = '\r'; break;
            case 't': result[i++] = '\t'; break;
            case '"': result[i++] = '"'; break;
            case '\\': result[i++] = '\\'; break;
            case '/': result[i++] = '/'; break;
            default: result[i++] = *p; break;
            }
            p++;
        } else {
            result[i++] = *p++;
        }
    }
    result[i] = '\0';
    return result;
}

/* Read a JSON integer value. Returns the integer, or -1 on error. */
static int json_read_int(const char *val) {
    if (!val) return -1;
    val = json_skip_ws(val);
    return atoi(val);
}

/* Read a JSON boolean value. */
static bool json_read_bool(const char *val) {
    if (!val) return false;
    val = json_skip_ws(val);
    return *val == 't';
}

/* ================================================================
 * LSP Protocol — read/write JSON-RPC messages
 * ================================================================ */

/* Read a single LSP message from stdin. Returns malloc'd JSON string. */
static char *lsp_read_message(void) {
    /* Read headers (Content-Length: N\r\n ... \r\n) */
    int content_length = -1;
    char header_line[1024];

    for (;;) {
        /* Read a line */
        int i = 0;
        int c;
        while ((c = fgetc(stdin)) != EOF) {
            if (c == '\n') break;
            if (i < (int)sizeof(header_line) - 1)
                header_line[i++] = (char)c;
        }
        if (c == EOF) return NULL;
        /* strip \r */
        if (i > 0 && header_line[i-1] == '\r') i--;
        header_line[i] = '\0';

        if (i == 0) break; /* empty line = end of headers */

        if (strncmp(header_line, "Content-Length:", 15) == 0) {
            content_length = atoi(header_line + 15);
        }
    }

    if (content_length <= 0) return NULL;

    /* Read body */
    char *body = (char *)malloc((size_t)content_length + 1);
    size_t total_read = 0;
    while (total_read < (size_t)content_length) {
        size_t n = fread(body + total_read, 1,
                         (size_t)content_length - total_read, stdin);
        if (n == 0) { free(body); return NULL; }
        total_read += n;
    }
    body[content_length] = '\0';

    lsp_log("← %s", body);
    return body;
}

/* Send a JSON-RPC message to stdout */
static void lsp_send(const char *json) {
    size_t len = strlen(json);
    lsp_log("→ %s", json);
    fprintf(stdout, "Content-Length: %zu\r\n\r\n%s", len, json);
    fflush(stdout);
}

/* Send a JSON-RPC response */
static void lsp_respond(int id, const char *result_json) {
    StringBuilder sb;
    sb_init(&sb);
    sb_appendf(&sb, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}", id, result_json);
    lsp_send(sb.buf);
    sb_free(&sb);
}

/* Send a JSON-RPC error response */
static void lsp_respond_error(int id, int code, const char *message) {
    StringBuilder sb;
    sb_init(&sb);
    sb_appendf(&sb, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":{\"code\":%d,\"message\":",
               id, code);
    sb_append_json_string(&sb, message);
    sb_append(&sb, "}}");
    lsp_send(sb.buf);
    sb_free(&sb);
}

/* Send a notification (no id) */
static void lsp_notify(const char *method, const char *params_json) {
    StringBuilder sb;
    sb_init(&sb);
    sb_appendf(&sb, "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}",
               method, params_json);
    lsp_send(sb.buf);
    sb_free(&sb);
}

/* ================================================================
 * Document Store — keeps open document text in memory
 * ================================================================ */

#define MAX_DOCUMENTS 64

typedef struct {
    char *uri;              /* document URI */
    char *text;             /* full document text */
    char *file_path;        /* file path extracted from URI */
    bool is_open;
} Document;

static Document documents[MAX_DOCUMENTS];
static int document_count = 0;

static Document *doc_find(const char *uri) {
    for (int i = 0; i < document_count; i++) {
        if (documents[i].is_open && strcmp(documents[i].uri, uri) == 0)
            return &documents[i];
    }
    return NULL;
}

/* Extract file path from file:// URI */
static char *uri_to_path(const char *uri) {
    /* file:///C:/Users/... → C:/Users/... */
    /* file:///home/... → /home/... */
    if (strncmp(uri, "file:///", 8) == 0) {
        const char *path = uri + 7; /* keep one leading / */
#ifdef _WIN32
        path = uri + 8; /* skip all three slashes on Windows */
#endif
        size_t len = strlen(path);
        char *result = (char *)malloc(len + 1);
        /* decode %XX sequences */
        size_t j = 0;
        for (size_t i = 0; i < len; i++) {
            if (path[i] == '%' && i + 2 < len) {
                char hex[3] = { path[i+1], path[i+2], 0 };
                result[j++] = (char)strtol(hex, NULL, 16);
                i += 2;
            } else {
                result[j++] = path[i];
            }
        }
        result[j] = '\0';
        return result;
    }
    return strdup(uri);
}

static Document *doc_open(const char *uri, const char *text) {
    Document *doc = doc_find(uri);
    if (!doc) {
        if (document_count >= MAX_DOCUMENTS) return NULL;
        doc = &documents[document_count++];
    }
    if (doc->uri) free(doc->uri);
    if (doc->text) free(doc->text);
    if (doc->file_path) free(doc->file_path);
    doc->uri = strdup(uri);
    doc->text = strdup(text);
    doc->file_path = uri_to_path(uri);
    doc->is_open = true;
    return doc;
}

static void doc_update(Document *doc, const char *text) {
    if (doc->text) free(doc->text);
    doc->text = strdup(text);
}

static void doc_close(const char *uri) {
    Document *doc = doc_find(uri);
    if (doc) {
        free(doc->uri); doc->uri = NULL;
        free(doc->text); doc->text = NULL;
        free(doc->file_path); doc->file_path = NULL;
        doc->is_open = false;
    }
}

/* ================================================================
 * Diagnostic Capture — run compiler pipeline, collect errors
 *
 * Strategy: redirect stderr to a temp file, run pipeline, parse
 * the error messages back into structured diagnostics.
 * ================================================================ */

#define MAX_DIAGS 256

typedef struct {
    int line;           /* 0-based for LSP */
    int col;            /* 0-based for LSP */
    int severity;       /* 1=error, 2=warning, 3=info, 4=hint */
    char message[512];
} LspDiagnostic;

static LspDiagnostic diag_buf[MAX_DIAGS];
static int diag_count = 0;

/* Parse stderr output: "file:line: error: message" or "file:line: warning: message" */
static void parse_stderr_diagnostics(const char *stderr_output) {
    const char *p = stderr_output;
    while (*p && diag_count < MAX_DIAGS) {
        /* find start of a diagnostic line */
        const char *line_start = p;

        /* skip to end of line */
        const char *line_end = p;
        while (*line_end && *line_end != '\n') line_end++;

        /* try to parse: file:line: error/warning/zercheck: message */
        /* format: "filename:123: error: whatever" */
        const char *colon1 = strchr(line_start, ':');
        if (colon1 && colon1 < line_end) {
            const char *after_colon1 = colon1 + 1;
            int line_num = 0;
            bool has_line = false;
            while (after_colon1 < line_end && isdigit((unsigned char)*after_colon1)) {
                line_num = line_num * 10 + (*after_colon1 - '0');
                after_colon1++;
                has_line = true;
            }
            if (has_line && *after_colon1 == ':') {
                after_colon1++; /* skip second : */
                while (after_colon1 < line_end && *after_colon1 == ' ') after_colon1++;

                int severity = 1; /* error */
                if (strncmp(after_colon1, "warning:", 8) == 0) {
                    severity = 2;
                    after_colon1 += 8;
                } else if (strncmp(after_colon1, "error:", 6) == 0) {
                    severity = 1;
                    after_colon1 += 6;
                } else if (strncmp(after_colon1, "zercheck:", 9) == 0) {
                    severity = 1;
                    after_colon1 += 9;
                }
                while (after_colon1 < line_end && *after_colon1 == ' ') after_colon1++;

                LspDiagnostic *d = &diag_buf[diag_count++];
                d->line = line_num > 0 ? line_num - 1 : 0; /* 1-based → 0-based */
                d->col = 0;
                d->severity = severity;
                size_t msg_len = (size_t)(line_end - after_colon1);
                if (msg_len >= sizeof(d->message)) msg_len = sizeof(d->message) - 1;
                memcpy(d->message, after_colon1, msg_len);
                d->message[msg_len] = '\0';
            }
        }

        p = line_end;
        if (*p == '\n') p++;
    }
}

/* Run the full compiler pipeline and collect diagnostics */
static void run_diagnostics(Document *doc) {
    diag_count = 0;
    if (!doc || !doc->text) return;

    Arena arena;
    arena_init(&arena, 256 * 1024);

    const char *fname = doc->file_path ? doc->file_path : "input.zer";

    Scanner scanner;
    scanner_init(&scanner, doc->text);

    Parser parser;
    parser_init(&parser, &scanner, &arena, fname);
    Node *file_node = parse_file(&parser);

    if (parser.had_error) {
        /* parser error — add a generic diagnostic */
        if (diag_count < MAX_DIAGS) {
            LspDiagnostic *d = &diag_buf[diag_count++];
            d->line = parser.previous.line > 0 ? parser.previous.line - 1 : 0;
            d->col = 0;
            d->severity = 1;
            snprintf(d->message, sizeof(d->message), "parse error");
        }
    }

    if (!parser.had_error && file_node) {
        Checker checker;
        checker_init(&checker, &arena, fname);
        checker_check(&checker, file_node);

        /* read diagnostics directly from checker's list */
        for (int i = 0; i < checker.diag_count && diag_count < MAX_DIAGS; i++) {
            LspDiagnostic *d = &diag_buf[diag_count++];
            d->line = checker.diagnostics[i].line > 0 ? checker.diagnostics[i].line - 1 : 0;
            d->col = 0;
            d->severity = checker.diagnostics[i].severity;
            strncpy(d->message, checker.diagnostics[i].message, sizeof(d->message) - 1);
            d->message[sizeof(d->message) - 1] = '\0';
        }

        /* free checker diagnostics */
        free(checker.diagnostics);

        if (checker.error_count == 0) {
            ZerCheck zc;
            zercheck_init(&zc, &checker, &arena, fname);
            zercheck_run(&zc, file_node);
        }
    }

    arena_free(&arena);
}

/* Build and send publishDiagnostics notification */
static void publish_diagnostics(Document *doc) {
    run_diagnostics(doc);

    StringBuilder sb;
    sb_init(&sb);
    sb_append(&sb, "{\"uri\":");
    sb_append_json_string(&sb, doc->uri);
    sb_append(&sb, ",\"diagnostics\":[");

    for (int i = 0; i < diag_count; i++) {
        if (i > 0) sb_append(&sb, ",");
        LspDiagnostic *d = &diag_buf[i];
        sb_appendf(&sb,
            "{\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
            "\"end\":{\"line\":%d,\"character\":%d}},"
            "\"severity\":%d,\"source\":\"zerc\",\"message\":",
            d->line, d->col, d->line, 999, d->severity);
        sb_append_json_string(&sb, d->message);
        sb_append(&sb, "}");
    }

    sb_append(&sb, "]}");
    lsp_notify("textDocument/publishDiagnostics", sb.buf);
    sb_free(&sb);
}

/* ================================================================
 * Symbol Lookup — find symbol at cursor position
 * ================================================================ */

/* Compute 0-based column of a pointer within source text */
static int column_of(const char *source, const char *ptr) {
    const char *line_start = ptr;
    while (line_start > source && *(line_start - 1) != '\n') line_start--;
    return (int)(ptr - line_start);
}

/* Find the token at a given (0-based line, 0-based character) position */
static Token find_token_at(const char *source, int line, int character) {
    Scanner s;
    scanner_init(&s, source);
    Token best;
    memset(&best, 0, sizeof(best));
    best.type = TOK_EOF;

    Token tok;
    do {
        tok = next_token(&s);
        int tok_line = tok.line - 1; /* convert to 0-based */
        if (tok_line == line && tok.start) {
            int tok_col = column_of(source, tok.start);
            if (character >= tok_col && character < tok_col + (int)tok.length) {
                return tok;
            }
        }
    } while (tok.type != TOK_EOF);

    return best; /* not found */
}

/* Get type description as a string */
static const char *type_to_string(Type *t) {
    if (!t) return "unknown";
    switch (t->kind) {
    case TYPE_VOID: return "void";
    case TYPE_U8: return "u8";
    case TYPE_U16: return "u16";
    case TYPE_U32: return "u32";
    case TYPE_U64: return "u64";
    case TYPE_I8: return "i8";
    case TYPE_I16: return "i16";
    case TYPE_I32: return "i32";
    case TYPE_I64: return "i64";
    case TYPE_USIZE: return "usize";
    case TYPE_F32: return "f32";
    case TYPE_F64: return "f64";
    case TYPE_BOOL: return "bool";
    case TYPE_STRUCT: return t->struct_type.name ? t->struct_type.name : "struct";
    case TYPE_ENUM: return t->enum_type.name ? t->enum_type.name : "enum";
    case TYPE_UNION: return t->union_type.name ? t->union_type.name : "union";
    case TYPE_POINTER: return "*T";
    case TYPE_OPTIONAL: return "?T";
    case TYPE_ARRAY: return "T[N]";
    case TYPE_SLICE: return "[]T";
    case TYPE_POOL: return "Pool(T, N)";
    case TYPE_RING: return "Ring(T, N)";
    case TYPE_HANDLE: return "Handle(T)";
    case TYPE_FUNC_PTR: return "function";
    case TYPE_OPAQUE: return "opaque";
    case TYPE_DISTINCT: return "distinct";
    case TYPE_ARENA: return "Arena";
    default: return "unknown";
    }
}

/* Format a full type description into a buffer */
static void format_type(Type *t, char *buf, size_t buf_size) {
    if (!t) { snprintf(buf, buf_size, "unknown"); return; }

    switch (t->kind) {
    case TYPE_POINTER:
        if (t->pointer.inner) {
            char inner[256];
            format_type(t->pointer.inner, inner, sizeof(inner));
            snprintf(buf, buf_size, "*%s", inner);
        } else {
            snprintf(buf, buf_size, "*unknown");
        }
        break;
    case TYPE_OPTIONAL:
        if (t->optional.inner) {
            char inner[256];
            format_type(t->optional.inner, inner, sizeof(inner));
            snprintf(buf, buf_size, "?%s", inner);
        } else {
            snprintf(buf, buf_size, "?unknown");
        }
        break;
    case TYPE_ARRAY:
        if (t->array.inner) {
            char inner[256];
            format_type(t->array.inner, inner, sizeof(inner));
            snprintf(buf, buf_size, "%s[%llu]", inner, (unsigned long long)t->array.size);
        } else {
            snprintf(buf, buf_size, "T[%llu]", (unsigned long long)t->array.size);
        }
        break;
    case TYPE_SLICE:
        if (t->slice.inner) {
            char inner[256];
            format_type(t->slice.inner, inner, sizeof(inner));
            snprintf(buf, buf_size, "[]%s", inner);
        } else {
            snprintf(buf, buf_size, "[]T");
        }
        break;
    case TYPE_STRUCT:
        snprintf(buf, buf_size, "struct %s",
                 t->struct_type.name ? t->struct_type.name : "<anon>");
        break;
    case TYPE_ENUM:
        snprintf(buf, buf_size, "enum %s",
                 t->enum_type.name ? t->enum_type.name : "<anon>");
        break;
    case TYPE_UNION:
        snprintf(buf, buf_size, "union %s",
                 t->union_type.name ? t->union_type.name : "<anon>");
        break;
    case TYPE_FUNC_PTR: {
        char ret[256];
        format_type(t->func_ptr.ret, ret, sizeof(ret));
        snprintf(buf, buf_size, "%s(", ret);
        size_t pos = strlen(buf);
        for (uint32_t i = 0; i < t->func_ptr.param_count && pos < buf_size - 10; i++) {
            if (i > 0) { buf[pos++] = ','; buf[pos++] = ' '; }
            char param[128];
            format_type(t->func_ptr.params[i], param, sizeof(param));
            size_t plen = strlen(param);
            if (pos + plen < buf_size - 2) {
                memcpy(buf + pos, param, plen);
                pos += plen;
            }
        }
        if (pos < buf_size - 2) { buf[pos++] = ')'; buf[pos] = '\0'; }
        break;
    }
    case TYPE_POOL: {
        char elem[256];
        format_type(t->pool.elem, elem, sizeof(elem));
        snprintf(buf, buf_size, "Pool(%s, %llu)", elem, (unsigned long long)t->pool.count);
        break;
    }
    case TYPE_RING: {
        char elem[256];
        format_type(t->ring.elem, elem, sizeof(elem));
        snprintf(buf, buf_size, "Ring(%s, %llu)", elem, (unsigned long long)t->ring.count);
        break;
    }
    case TYPE_HANDLE: {
        char elem[256];
        format_type(t->handle.elem, elem, sizeof(elem));
        snprintf(buf, buf_size, "Handle(%s)", elem);
        break;
    }
    default:
        snprintf(buf, buf_size, "%s", type_to_string(t));
        break;
    }
}

/* Run pipeline and look up symbol. Returns symbol or NULL.
 * Caller must arena_free after use. */
static Symbol *lookup_symbol_at(Document *doc, int line, int character,
                                 Arena *arena, Checker *checker) {
    if (!doc || !doc->text) return NULL;

    /* Find token at position */
    Token tok = find_token_at(doc->text, line, character);
    if (tok.type != TOK_IDENT && tok.type < TOK_STRUCT) return NULL;
    if (tok.type != TOK_IDENT) return NULL; /* only look up identifiers */

    /* Run pipeline to get scope info */
    const char *fname = doc->file_path ? doc->file_path : "input.zer";

    /* suppress stderr during lookup */
    fflush(stderr);
    int saved = dup(fileno(stderr));
#ifdef _WIN32
    FILE *devnull = fopen("NUL", "w");
#else
    FILE *devnull = fopen("/dev/null", "w");
#endif
    if (devnull) {
        dup2(fileno(devnull), fileno(stderr));
        fclose(devnull);
    }

    Scanner scanner;
    scanner_init(&scanner, doc->text);
    Parser parser;
    parser_init(&parser, &scanner, arena, fname);
    Node *file_node = parse_file(&parser);

    Symbol *result = NULL;
    if (!parser.had_error && file_node) {
        checker_init(checker, arena, fname);
        checker_check(checker, file_node);

        /* look up token in scope */
        result = scope_lookup(checker->global_scope,
                              tok.start, (uint32_t)tok.length);

        /* also check current_scope if different */
        if (!result && checker->current_scope != checker->global_scope) {
            result = scope_lookup(checker->current_scope,
                                  tok.start, (uint32_t)tok.length);
        }
    }

    fflush(stderr);
    dup2(saved, fileno(stderr));
    close(saved);

    return result;
}

/* ================================================================
 * LSP Handlers
 * ================================================================ */

static bool initialized = false;
static bool shutdown_requested = false;

/* Handle initialize request */
static void handle_initialize(int id) {
    const char *result =
        "{"
        "\"capabilities\":{"
            "\"textDocumentSync\":{"
                "\"openClose\":true,"
                "\"change\":1"  /* Full sync */
            "},"
            "\"hoverProvider\":true,"
            "\"definitionProvider\":true,"
            "\"completionProvider\":{"
                "\"triggerCharacters\":[\".\",\"@\"]"
            "},"
            "\"documentSymbolProvider\":true"
        "},"
        "\"serverInfo\":{"
            "\"name\":\"zer-lsp\","
            "\"version\":\"0.1.0\""
        "}"
        "}";
    lsp_respond(id, result);
    initialized = true;
    lsp_log("Server initialized");
}

/* Handle textDocument/didOpen */
static void handle_did_open(const char *params) {
    const char *td = json_find_key(params, "textDocument");
    if (!td) return;
    char *uri = json_read_string(json_find_key(td, "uri"));
    char *text = json_read_string(json_find_key(td, "text"));
    if (!uri || !text) { free(uri); free(text); return; }

    Document *doc = doc_open(uri, text);
    lsp_log("Opened: %s", uri);
    if (doc) publish_diagnostics(doc);

    free(uri);
    free(text);
}

/* Handle textDocument/didChange */
static void handle_did_change(const char *params) {
    const char *td = json_find_key(params, "textDocument");
    if (!td) return;
    char *uri = json_read_string(json_find_key(td, "uri"));
    if (!uri) return;

    /* Find contentChanges array */
    const char *changes = json_find_key(params, "contentChanges");
    if (changes) {
        changes = json_skip_ws(changes);
        if (*changes == '[') {
            changes++; /* skip [ */
            changes = json_skip_ws(changes);
            /* first element of array */
            if (*changes == '{') {
                char *text = json_read_string(json_find_key(changes, "text"));
                if (text) {
                    Document *doc = doc_find(uri);
                    if (doc) {
                        doc_update(doc, text);
                        publish_diagnostics(doc);
                    }
                    free(text);
                }
            }
        }
    }

    free(uri);
}

/* Handle textDocument/didClose */
static void handle_did_close(const char *params) {
    const char *td = json_find_key(params, "textDocument");
    if (!td) return;
    char *uri = json_read_string(json_find_key(td, "uri"));
    if (uri) {
        /* clear diagnostics */
        StringBuilder sb;
        sb_init(&sb);
        sb_append(&sb, "{\"uri\":");
        sb_append_json_string(&sb, uri);
        sb_append(&sb, ",\"diagnostics\":[]}");
        lsp_notify("textDocument/publishDiagnostics", sb.buf);
        sb_free(&sb);

        doc_close(uri);
        lsp_log("Closed: %s", uri);
        free(uri);
    }
}

/* Handle textDocument/hover */
static void handle_hover(int id, const char *params) {
    const char *td = json_find_key(params, "textDocument");
    const char *pos = json_find_key(params, "position");
    if (!td || !pos) { lsp_respond(id, "null"); return; }

    char *uri = json_read_string(json_find_key(td, "uri"));
    int line = json_read_int(json_find_key(pos, "line"));
    int character = json_read_int(json_find_key(pos, "character"));

    Document *doc = uri ? doc_find(uri) : NULL;
    free(uri);

    if (!doc) { lsp_respond(id, "null"); return; }

    Arena arena;
    arena_init(&arena, 256 * 1024);
    Checker checker;
    Symbol *sym = lookup_symbol_at(doc, line, character, &arena, &checker);

    if (sym && sym->type) {
        char type_str[512];
        format_type(sym->type, type_str, sizeof(type_str));

        char hover_text[1024];
        if (sym->is_function) {
            snprintf(hover_text, sizeof(hover_text), "%.*s: %s",
                     (int)sym->name_len, sym->name, type_str);
        } else if (sym->is_const) {
            snprintf(hover_text, sizeof(hover_text), "const %.*s: %s",
                     (int)sym->name_len, sym->name, type_str);
        } else {
            snprintf(hover_text, sizeof(hover_text), "%.*s: %s",
                     (int)sym->name_len, sym->name, type_str);
        }

        StringBuilder sb;
        sb_init(&sb);
        /* format as markdown code block for syntax highlighting in editor */
        char md_hover[1200];
        snprintf(md_hover, sizeof(md_hover), "```zer\n%s\n```", hover_text);
        sb_append(&sb, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
        sb_append_json_string(&sb, md_hover);
        sb_append(&sb, "}}");
        lsp_respond(id, sb.buf);
        sb_free(&sb);
    } else {
        lsp_respond(id, "null");
    }

    arena_free(&arena);
}

/* Handle textDocument/definition */
static void handle_definition(int id, const char *params) {
    const char *td = json_find_key(params, "textDocument");
    const char *pos = json_find_key(params, "position");
    if (!td || !pos) { lsp_respond(id, "null"); return; }

    char *uri = json_read_string(json_find_key(td, "uri"));
    int line = json_read_int(json_find_key(pos, "line"));
    int character = json_read_int(json_find_key(pos, "character"));

    Document *doc = uri ? doc_find(uri) : NULL;

    if (!doc) { free(uri); lsp_respond(id, "null"); return; }

    Arena arena;
    arena_init(&arena, 256 * 1024);
    Checker checker;
    Symbol *sym = lookup_symbol_at(doc, line, character, &arena, &checker);

    if (sym && sym->line > 0) {
        int def_line = (int)sym->line - 1; /* 1-based → 0-based */

        /* Use the current document URI if symbol is in same file */
        const char *target_uri = uri;

        StringBuilder sb;
        sb_init(&sb);
        sb_append(&sb, "{\"uri\":");
        sb_append_json_string(&sb, target_uri);
        sb_appendf(&sb,
            ",\"range\":{\"start\":{\"line\":%d,\"character\":0},"
            "\"end\":{\"line\":%d,\"character\":0}}}",
            def_line, def_line);
        lsp_respond(id, sb.buf);
        sb_free(&sb);
    } else {
        lsp_respond(id, "null");
    }

    free(uri);
    arena_free(&arena);
}

/* Handle textDocument/completion */
static void handle_completion(int id, const char *params) {
    const char *td = json_find_key(params, "textDocument");
    if (!td) { lsp_respond(id, "[]"); return; }

    char *uri = json_read_string(json_find_key(td, "uri"));
    Document *doc = uri ? doc_find(uri) : NULL;
    free(uri);

    StringBuilder sb;
    sb_init(&sb);
    sb_append(&sb, "[");

    /* ZER keywords */
    static const char *keywords[] = {
        "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64",
        "usize", "f32", "f64", "bool", "void", "opaque",
        "struct", "packed", "enum", "union", "const", "typedef", "distinct",
        "if", "else", "for", "while", "switch", "break", "continue", "return",
        "default", "orelse", "null", "true", "false",
        "Pool", "Ring", "Slab", "Arena", "Handle",
        "defer", "import", "cinclude", "volatile", "interrupt", "asm", "static",
        "keep", "as", "comptime", "mmio", "naked", "section",
        NULL
    };

    int count = 0;
    for (int i = 0; keywords[i]; i++) {
        if (count > 0) sb_append(&sb, ",");
        sb_append(&sb, "{\"label\":");
        sb_append_json_string(&sb, keywords[i]);
        sb_appendf(&sb, ",\"kind\":14}"); /* 14 = Keyword */
        count++;
    }

    /* ZER intrinsics */
    static const char *intrinsics[] = {
        "@size", "@truncate", "@saturate", "@bitcast", "@cast",
        "@ptrcast", "@ptrtoint", "@inttoptr",
        "@barrier", "@barrier_store", "@barrier_load",
        "@offset", "@container",
        "@trap", "@probe", "@cstr",
        "@critical", "@atomic_add", "@atomic_sub", "@atomic_or",
        "@atomic_and", "@atomic_xor", "@atomic_cas",
        "@atomic_load", "@atomic_store",
        NULL
    };

    for (int i = 0; intrinsics[i]; i++) {
        if (count > 0) sb_append(&sb, ",");
        sb_append(&sb, "{\"label\":");
        sb_append_json_string(&sb, intrinsics[i]);
        sb_appendf(&sb, ",\"kind\":3}"); /* 3 = Function */
        count++;
    }

    /* If we have a document, add symbols from the checker */
    if (doc && doc->text) {
        Arena arena;
        arena_init(&arena, 256 * 1024);
        Checker checker;

        /* suppress stderr */
        fflush(stderr);
        int saved = dup(fileno(stderr));
#ifdef _WIN32
        FILE *devnull = fopen("NUL", "w");
#else
        FILE *devnull = fopen("/dev/null", "w");
#endif
        if (devnull) { dup2(fileno(devnull), fileno(stderr)); fclose(devnull); }

        Scanner scanner;
        scanner_init(&scanner, doc->text);
        Parser parser;
        parser_init(&parser, &scanner, &arena,
                    doc->file_path ? doc->file_path : "input.zer");
        Node *file_node = parse_file(&parser);

        if (!parser.had_error && file_node) {
            checker_init(&checker, &arena,
                         doc->file_path ? doc->file_path : "input.zer");
            checker_check(&checker, file_node);

            /* add symbols from global scope */
            Scope *scope = checker.global_scope;
            for (uint32_t i = 0; i < scope->symbol_count; i++) {
                Symbol *sym = &scope->symbols[i];
                if (count > 0) sb_append(&sb, ",");
                sb_append(&sb, "{\"label\":");
                /* need to null-terminate the name */
                char name[256];
                size_t nlen = sym->name_len < 255 ? sym->name_len : 255;
                memcpy(name, sym->name, nlen);
                name[nlen] = '\0';
                sb_append_json_string(&sb, name);
                int kind = sym->is_function ? 3 : 6; /* Function : Variable */
                if (sym->type) {
                    if (sym->type->kind == TYPE_STRUCT) kind = 22; /* Struct */
                    else if (sym->type->kind == TYPE_ENUM) kind = 13; /* Enum */
                    else if (sym->type->kind == TYPE_UNION) kind = 22;
                }
                sb_appendf(&sb, ",\"kind\":%d}", kind);
                count++;
            }
        }

        fflush(stderr);
        dup2(saved, fileno(stderr));
        close(saved);
        arena_free(&arena);
    }

    sb_append(&sb, "]");
    lsp_respond(id, sb.buf);
    sb_free(&sb);
}

/* Handle textDocument/documentSymbol */
static void handle_document_symbols(int id, const char *params) {
    const char *td = json_find_key(params, "textDocument");
    if (!td) { lsp_respond(id, "[]"); return; }

    char *uri = json_read_string(json_find_key(td, "uri"));
    Document *doc = uri ? doc_find(uri) : NULL;
    free(uri);

    if (!doc || !doc->text) { lsp_respond(id, "[]"); return; }

    Arena arena;
    arena_init(&arena, 256 * 1024);

    /* suppress stderr */
    fflush(stderr);
    int saved = dup(fileno(stderr));
#ifdef _WIN32
    FILE *devnull = fopen("NUL", "w");
#else
    FILE *devnull = fopen("/dev/null", "w");
#endif
    if (devnull) { dup2(fileno(devnull), fileno(stderr)); fclose(devnull); }

    Scanner scanner;
    scanner_init(&scanner, doc->text);
    Parser parser;
    parser_init(&parser, &scanner, &arena,
                doc->file_path ? doc->file_path : "input.zer");
    Node *file_node = parse_file(&parser);

    StringBuilder sb;
    sb_init(&sb);
    sb_append(&sb, "[");

    if (!parser.had_error && file_node && file_node->kind == NODE_FILE) {
        int count = 0;
        for (int i = 0; i < file_node->file.decl_count; i++) {
            Node *decl = file_node->file.decls[i];
            const char *name = NULL;
            size_t name_len = 0;
            int kind = 0;

            switch (decl->kind) {
            case NODE_FUNC_DECL:
                name = decl->func_decl.name;
                name_len = decl->func_decl.name_len;
                kind = 12; /* Function */
                break;
            case NODE_STRUCT_DECL:
                name = decl->struct_decl.name;
                name_len = decl->struct_decl.name_len;
                kind = 23; /* Struct */
                break;
            case NODE_ENUM_DECL:
                name = decl->enum_decl.name;
                name_len = decl->enum_decl.name_len;
                kind = 10; /* Enum */
                break;
            case NODE_UNION_DECL:
                name = decl->union_decl.name;
                name_len = decl->union_decl.name_len;
                kind = 23; /* Struct */
                break;
            case NODE_TYPEDEF:
                name = decl->typedef_decl.name;
                name_len = decl->typedef_decl.name_len;
                kind = 26; /* TypeParameter */
                break;
            case NODE_GLOBAL_VAR:
                name = decl->var_decl.name;
                name_len = decl->var_decl.name_len;
                kind = 13; /* Variable */
                break;
            default: break;
            }

            if (name && name_len > 0) {
                if (count > 0) sb_append(&sb, ",");
                char sym_name[256];
                size_t nlen = name_len < 255 ? name_len : 255;
                memcpy(sym_name, name, nlen);
                sym_name[nlen] = '\0';
                int line = decl->loc.line > 0 ? decl->loc.line - 1 : 0;

                sb_append(&sb, "{\"name\":");
                sb_append_json_string(&sb, sym_name);
                sb_appendf(&sb,
                    ",\"kind\":%d,"
                    "\"range\":{\"start\":{\"line\":%d,\"character\":0},"
                    "\"end\":{\"line\":%d,\"character\":0}},"
                    "\"selectionRange\":{\"start\":{\"line\":%d,\"character\":0},"
                    "\"end\":{\"line\":%d,\"character\":0}}}",
                    kind, line, line, line, line);
                count++;
            }
        }
    }

    sb_append(&sb, "]");
    lsp_respond(id, sb.buf);
    sb_free(&sb);

    fflush(stderr);
    dup2(saved, fileno(stderr));
    close(saved);
    arena_free(&arena);
}

/* Handle shutdown request */
static void handle_shutdown(int id) {
    shutdown_requested = true;
    lsp_respond(id, "null");
    lsp_log("Shutdown requested");
}

/* ================================================================
 * Message Dispatch
 * ================================================================ */

static void dispatch_message(const char *json) {
    const char *method_val = json_find_key(json, "method");
    const char *id_val = json_find_key(json, "id");
    const char *params_val = json_find_key(json, "params");

    char *method = json_read_string(method_val);
    int id = id_val ? json_read_int(id_val) : -1;

    if (!method) {
        free(method);
        return;
    }

    lsp_log("Method: %s (id=%d)", method, id);

    /* ---- Lifecycle ---- */
    if (strcmp(method, "initialize") == 0) {
        handle_initialize(id);
    }
    else if (strcmp(method, "initialized") == 0) {
        /* notification — no response needed */
    }
    else if (strcmp(method, "shutdown") == 0) {
        handle_shutdown(id);
    }
    else if (strcmp(method, "exit") == 0) {
        free(method);
        exit(shutdown_requested ? 0 : 1);
    }

    /* ---- Document sync ---- */
    else if (strcmp(method, "textDocument/didOpen") == 0) {
        handle_did_open(params_val);
    }
    else if (strcmp(method, "textDocument/didChange") == 0) {
        handle_did_change(params_val);
    }
    else if (strcmp(method, "textDocument/didClose") == 0) {
        handle_did_close(params_val);
    }

    /* ---- Language features ---- */
    else if (strcmp(method, "textDocument/hover") == 0) {
        handle_hover(id, params_val);
    }
    else if (strcmp(method, "textDocument/definition") == 0) {
        handle_definition(id, params_val);
    }
    else if (strcmp(method, "textDocument/completion") == 0) {
        handle_completion(id, params_val);
    }
    else if (strcmp(method, "textDocument/documentSymbol") == 0) {
        handle_document_symbols(id, params_val);
    }

    /* ---- Unknown request (respond with error if it has an id) ---- */
    else if (id >= 0) {
        lsp_respond_error(id, -32601, "Method not found");
    }

    free(method);
}

/* ================================================================
 * Main — LSP server entry point
 * ================================================================ */

int main(int argc, char **argv) {
    /* Set stdin/stdout to binary mode on Windows */
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    /* Open log file if --log flag is given */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            log_file = fopen(argv[i + 1], "w");
            i++;
        }
    }

    lsp_log("ZER Language Server starting...");

    /* Main loop: read messages, dispatch */
    for (;;) {
        char *msg = lsp_read_message();
        if (!msg) break; /* stdin closed */
        dispatch_message(msg);
        free(msg);
    }

    lsp_log("ZER Language Server exiting");
    if (log_file) fclose(log_file);
    return 0;
}
