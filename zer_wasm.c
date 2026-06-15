/* ================================================================
 * ZER WebAssembly bridge
 *
 * Exposes the ZER compiler frontend (lexer -> parser -> checker ->
 * zercheck -> emitter) as a set of C entry points callable from
 * JavaScript once compiled to WASM via emscripten.
 *
 * WHY: shipping an unsigned native zer-lsp.exe / zerc.exe trips
 * Windows Defender's `Wacatac.B!ml` ML false positive (unsigned mingw
 * PE, no reputation). Running the SAME compiler code as a .wasm module
 * loaded by node (Microsoft-signed, trusted) removes the native binary
 * from the spawn path entirely — there is nothing for Defender to flag.
 *
 * Design: string in, string out. The compiler is the single source of
 * truth (same .c files as the native build); JS only marshals text.
 *
 * Memory model: each entry point returns a pointer into a single static
 * result buffer that is reused on the next call. Callers (single-thread,
 * sequential) must read the result before the next call. No per-call
 * free dance from JS.
 *
 * Build (emscripten):
 *   emcc -O2 -I. zer_wasm.c lexer.c parser.c ast.c types.c checker.c \
 *        emitter.c zercheck.c zercheck_ir.c ir.c ir_lower.c $SAFETY_SRCS \
 *        -sEXPORTED_FUNCTIONS=_zer_diagnostics_json,_zer_emit_c,_zer_free,_zer_version,_malloc,_free \
 *        -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8 \
 *        -sMODULARIZE -sEXPORT_NAME=ZerModule -sALLOW_MEMORY_GROWTH \
 *        -o zer.js
 * ================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define WASM_EXPORT
#endif

#include "lexer.h"
#include "ast.h"
#include "parser.h"
#include "types.h"
#include "checker.h"
#include "zercheck.h"
#include "emitter.h"

/* ----------------------------------------------------------------
 * Tiny dynamic string builder for assembling JSON / capturing output
 * ---------------------------------------------------------------- */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} SB;

static void sb_init(SB *sb) {
    sb->cap = 4096;
    sb->buf = (char *)malloc(sb->cap);
    sb->len = 0;
    if (sb->buf) sb->buf[0] = '\0';
}

static void sb_ensure(SB *sb, size_t extra) {
    if (!sb->buf) return;
    while (sb->len + extra + 1 > sb->cap) {
        sb->cap *= 2;
        sb->buf = (char *)realloc(sb->buf, sb->cap);
        if (!sb->buf) return;
    }
}

static void sb_putc(SB *sb, char c) {
    sb_ensure(sb, 1);
    if (!sb->buf) return;
    sb->buf[sb->len++] = c;
    sb->buf[sb->len] = '\0';
}

static void sb_puts(SB *sb, const char *s) {
    if (!s) return;
    size_t n = strlen(s);
    sb_ensure(sb, n);
    if (!sb->buf) return;
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

/* Append a JSON-escaped string value (with surrounding quotes). */
static void sb_json_str(SB *sb, const char *s) {
    sb_putc(sb, '"');
    for (const char *p = s; p && *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"':  sb_puts(sb, "\\\""); break;
        case '\\': sb_puts(sb, "\\\\"); break;
        case '\n': sb_puts(sb, "\\n"); break;
        case '\r': sb_puts(sb, "\\r"); break;
        case '\t': sb_puts(sb, "\\t"); break;
        default:
            if (c < 0x20) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", c);
                sb_puts(sb, esc);
            } else {
                sb_putc(sb, (char)c);
            }
        }
    }
    sb_putc(sb, '"');
}

/* ----------------------------------------------------------------
 * Single static result buffer returned to JS (reused per call).
 * ---------------------------------------------------------------- */
static char *g_result = NULL;

static const char *publish_result(SB *sb) {
    if (g_result) { free(g_result); g_result = NULL; }
    g_result = sb->buf ? sb->buf : NULL;   /* take ownership of SB buffer */
    return g_result ? g_result : "";
}

/* ----------------------------------------------------------------
 * Append the checker's structured diagnostics + parser error as a
 * JSON array of {line,col,severity,message}. Lines are the compiler's
 * native 1-based line numbers; the JS LSP layer converts to 0-based.
 * ---------------------------------------------------------------- */
static void append_diag_array(SB *sb, Parser *parser, Checker *checker) {
    sb_putc(sb, '[');
    int emitted = 0;

    if (parser && parser->had_error) {
        sb_puts(sb, "{\"line\":");
        char num[16];
        snprintf(num, sizeof(num), "%d", parser->previous.line > 0 ? parser->previous.line : 1);
        sb_puts(sb, num);
        sb_puts(sb, ",\"col\":0,\"severity\":1,\"message\":");
        sb_json_str(sb, "parse error");
        sb_putc(sb, '}');
        emitted++;
    }

    if (checker) {
        for (int i = 0; i < checker->diag_count; i++) {
            if (emitted > 0) sb_putc(sb, ',');
            sb_puts(sb, "{\"line\":");
            char num[16];
            snprintf(num, sizeof(num), "%d", checker->diagnostics[i].line > 0 ? checker->diagnostics[i].line : 1);
            sb_puts(sb, num);
            sb_puts(sb, ",\"col\":0,\"severity\":");
            snprintf(num, sizeof(num), "%d", checker->diagnostics[i].severity);
            sb_puts(sb, num);
            sb_puts(sb, ",\"message\":");
            sb_json_str(sb, checker->diagnostics[i].message);
            sb_putc(sb, '}');
            emitted++;
        }
    }

    sb_putc(sb, ']');
}

/* ----------------------------------------------------------------
 * Production safety analyzer wiring (mirrors zerc_main.c).
 *
 * zercheck_ir is the SOLE safety driver (CFG UAF / double-free / leak /
 * move). It runs via the emitter's ir_hook on the lowered IR, then an
 * iterative summary build + main pass. zercheck_ir has no public header
 * decl — mirror zerc_main.c's extern (void* IRFunc to avoid pulling ir.h).
 * ---------------------------------------------------------------- */
extern bool zercheck_ir(ZerCheck *zc_ir, void *ir_func);

static void **g_ir_funcs = NULL;
static int g_ir_count = 0;
static int g_ir_cap = 0;

static void wasm_ir_hook(void *ctx, void *ir_func) {
    (void)ctx;
    if (g_ir_count >= g_ir_cap) {
        g_ir_cap = g_ir_cap < 16 ? 16 : g_ir_cap * 2;
        g_ir_funcs = (void **)realloc(g_ir_funcs, (size_t)g_ir_cap * sizeof(void *));
    }
    if (g_ir_funcs) g_ir_funcs[g_ir_count++] = ir_func;
}

/* Run the full zercheck_ir analysis over the collected IRFuncs, exactly as
 * zerc_main.c does after emit. Returns the safety error count. */
static int wasm_run_zercheck_ir(ZerCheck *zc_ir) {
    if (g_ir_count <= 0) return 0;
    /* Iterative FuncSummary build (mutual-recursion convergence). */
    zc_ir->building_summary = true;
    for (int pass = 0; pass < 16; pass++) {
        int sc_before = zc_ir->summary_count;
        for (int i = 0; i < g_ir_count; i++) zercheck_ir(zc_ir, g_ir_funcs[i]);
        if (pass > 0 && zc_ir->summary_count == sc_before) break;
    }
    zc_ir->building_summary = false;
    /* Main pass — errors recorded now. */
    for (int i = 0; i < g_ir_count; i++) zercheck_ir(zc_ir, g_ir_funcs[i]);
    return zc_ir->error_count;
}

/* Apply the same checker target config the native driver sets (zerc_main.c),
 * so asm/intrinsic validation matches. Pointer width comes from the global
 * zer_target_ptr_bits (default 32) inside checker_init. */
static void wasm_config_checker(Checker *c, const char *src) {
    c->source = src;
    c->target_features = (1u << 1) | (1u << 2); /* SSE | SSE2 — native baseline */
    c->target_arch = 1;                          /* ZER_ARCH_X86_64 — native default */
    /* The wasm CLI/LSP target the bundled desktop gcc (mingw-w64 on win-x64,
     * gcc on linux-x64) — both LP64/LLP64 with 8-byte size_t. Native zerc
     * probes gcc and sets 64 here; the global default (32, for embedded) would
     * make the checker model usize as 32-bit while gcc compiles it 64-bit,
     * over-rejecting u64<->usize and miscomputing @size(usize). Override to 64.
     * (Embedded cross-compile width is a future --target-bits plumb.) */
    c->target_ptr_bits = 64;
}

/* ================================================================
 * Entry point 1 (Phase 1 — LSP diagnostics)
 *
 * Run lexer -> parser -> checker -> zercheck and return a JSON array
 * of diagnostics. Mirrors zer_lsp.c run_diagnostics exactly, so the
 * editor experience is identical to the native LSP.
 * ================================================================ */
WASM_EXPORT const char *zer_diagnostics_json(const char *src, const char *fname) {
    SB sb; sb_init(&sb);
    if (!src) { sb_puts(&sb, "[]"); return publish_result(&sb); }
    if (!fname || !*fname) fname = "input.zer";

    Arena arena;
    arena_init(&arena, 256 * 1024);

    Scanner scanner;
    scanner_init(&scanner, src);

    Parser parser;
    parser_init(&parser, &scanner, &arena, fname);
    Node *file_node = parse_file(&parser);

    Checker checker;
    bool checked = false;
    if (!parser.had_error && file_node) {
        checker_init(&checker, &arena, fname);
        wasm_config_checker(&checker, src);
        checker_check(&checker, file_node);
        checked = true;

        if (checker.error_count == 0) {
            /* LSP diagnostics use the lighter zercheck shim, same as the
             * former native zer_lsp.c. The full zercheck_ir CFG analysis runs
             * on the compile path (zer_emit_c) which gates the produced binary. */
            ZerCheck zc;
            zercheck_init(&zc, &checker, &arena, fname);
            zercheck_run(&zc, file_node);
        }
    }

    append_diag_array(&sb, &parser, checked ? &checker : NULL);

    if (checked && checker.diagnostics) free(checker.diagnostics);
    arena_free(&arena);
    return publish_result(&sb);
}

/* ================================================================
 * Entry point 2 (Phase 2 — replace zerc.exe frontend)
 *
 * Run the full pipeline including the C emitter. On success returns
 *   {"ok":true,"c":"<emitted C source>"}
 * On any error returns
 *   {"ok":false,"diagnostics":[...]}
 * The JS driver writes the emitted C to a temp file and invokes the
 * bundled GCC on it — no native zerc.exe in the path.
 *
 * Emitter output is captured to memory via open_memstream (musl/
 * emscripten support it) — no temp files inside WASM.
 * ================================================================ */
WASM_EXPORT const char *zer_emit_c(const char *src, const char *fname, int track_cptrs) {
    SB sb; sb_init(&sb);
    if (!src) { sb_puts(&sb, "{\"ok\":false,\"diagnostics\":[]}"); return publish_result(&sb); }
    if (!fname || !*fname) fname = "input.zer";

    Arena arena;
    arena_init(&arena, 256 * 1024);

    Scanner scanner;
    scanner_init(&scanner, src);

    Parser parser;
    parser_init(&parser, &scanner, &arena, fname);
    Node *file_node = parse_file(&parser);

    bool ok = false;
    Checker checker;
    bool checked = false;

    char *c_buf = NULL;
    size_t c_size = 0;

    if (!parser.had_error && file_node) {
        checker_init(&checker, &arena, fname);
        wasm_config_checker(&checker, src);
        checker_check(&checker, file_node);
        checked = true;

        if (checker.error_count == 0) {
            FILE *mem = open_memstream(&c_buf, &c_size);
            if (mem) {
                Emitter emitter;
                emitter_init(&emitter, mem, &arena, &checker);
                /* Mirror the native driver (zerc_main.c:661):
                 *   emitter.track_cptrs = track_cptrs || do_run;
                 * Enables Level 3/4/5 *opaque inline-header tracking AND the
                 * __wrap_malloc/free/calloc/realloc definitions the --wrap=malloc
                 * linker interception requires. The CLI passes it on for
                 * --run / --track-cptrs. */
                emitter.track_cptrs = (track_cptrs != 0);
                emitter.source_file = fname;

                /* Wire the SOLE production safety analyzer exactly as
                 * zerc_main.c does: zercheck_ir runs via the emitter ir_hook on
                 * the same lowered IR (collected here), then an iterative
                 * summary build + main pass after emit. WITHOUT this the wasm
                 * compile path would emit binaries containing UAF / double-free
                 * / leaks that native zerc rejects. */
                ZerCheck zc_ir;
                zercheck_init(&zc_ir, &checker, &arena, fname);
                zc_ir.import_asts = NULL;
                zc_ir.import_ast_count = 0;
                emitter.ir_hook_ctx = &zc_ir;
                emitter.ir_hook = wasm_ir_hook;
                g_ir_count = 0;

                emit_file(&emitter, file_node);
                fflush(mem);
                fclose(mem);

                int safety_errors = wasm_run_zercheck_ir(&zc_ir);

                /* Gate the compile on BOTH the type checker and the IR safety
                 * analyzer (zerc_main.c:744 removes the output on zercheck
                 * errors — here we just report ok:false). zercheck_ir messages
                 * go to stderr (ir_zc_error); the JS CLI captures and prints
                 * them. */
                ok = (checker.error_count == 0 && safety_errors == 0);

                if (g_ir_funcs) { free(g_ir_funcs); g_ir_funcs = NULL; }
                g_ir_count = 0;
                g_ir_cap = 0;
            }
        }
    }

    if (ok && c_buf) {
        sb_puts(&sb, "{\"ok\":true,\"c\":");
        sb_json_str(&sb, c_buf);
        sb_putc(&sb, '}');
    } else {
        sb_puts(&sb, "{\"ok\":false,\"diagnostics\":");
        append_diag_array(&sb, &parser, checked ? &checker : NULL);
        sb_putc(&sb, '}');
    }

    if (c_buf) free(c_buf);
    if (checked && checker.diagnostics) free(checker.diagnostics);
    arena_free(&arena);
    return publish_result(&sb);
}

/* Free the static result buffer (optional — JS can ignore; reused per call). */
WASM_EXPORT void zer_free(void) {
    if (g_result) { free(g_result); g_result = NULL; }
}

WASM_EXPORT const char *zer_version(void) {
    return "0.5.0";
}
