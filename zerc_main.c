/* _POSIX_C_SOURCE needed for popen/pclose declarations on Linux/macOS.
 * Without this, popen returns implicit int → truncates 64-bit pointer → SIGSEGV. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/wait.h>  /* WEXITSTATUS, WIFEXITED, WIFSIGNALED */
#include <signal.h>    /* WTERMSIG */
#endif
#include "lexer.h"
#include "ast.h"
#include "parser.h"
#include "types.h"
#include "checker.h"
#include "emitter.h"
#include "zercheck.h"
#include "ir.h"

/* ================================================================
 * ZER Compiler Driver — zerc
 *
 * Usage: zerc <input.zer> [-o output.c]
 *
 * Pipeline:
 *   1. Read main source file
 *   2. Parse → AST
 *   3. Resolve imports (parse imported files recursively)
 *   4. Type check (all files in combined scope)
 *   5. Emit C
 * ================================================================ */

typedef struct {
    const char *name;       /* module name (e.g., "uart") */
    const char *path;       /* file path (e.g., "uart.zer") */
    char *source;           /* source text */
    Node *ast;              /* parsed AST */
    bool parsed;
    bool checking;          /* for circular import detection */
    bool checked;
    bool emitted;           /* prevent double emission in diamond deps */
} Module;

typedef struct {
    Module *modules;        /* dynamic array */
    int module_count;
    int module_capacity;
    Arena arena;
    const char *source_dir; /* directory of main source file */
} Compiler;

/* read file into malloc'd string */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char *buf = (char *)malloc(size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t bytes_read = fread(buf, 1, size, f);
    if (bytes_read != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[size] = '\0';
    fclose(f);
    return buf;
}

/* extract directory from file path */
static const char *get_dir(const char *path, char *buf, size_t buf_size) {
    const char *last_sep = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') last_sep = p + 1;
    }
    size_t dir_len = last_sep - path;
    if (dir_len == 0) {
        buf[0] = '.';
        buf[1] = '/';
        buf[2] = '\0';
        return buf;
    }
    if (dir_len >= buf_size) dir_len = buf_size - 1;
    memcpy(buf, path, dir_len);
    buf[dir_len] = '\0';
    return buf;
}

/* find or create a module by name */
static Module *find_or_create_module(Compiler *cc, const char *name, size_t name_len) {
    /* check if already registered */
    for (int i = 0; i < cc->module_count; i++) {
        if (strlen(cc->modules[i].name) == name_len &&
            memcmp(cc->modules[i].name, name, name_len) == 0) {
            return &cc->modules[i];
        }
    }

    /* grow modules array if needed */
    if (cc->module_count >= cc->module_capacity) {
        int new_cap = cc->module_capacity * 2;
        if (new_cap < 16) new_cap = 16;
        Module *new_mods = (Module *)realloc(cc->modules, new_cap * sizeof(Module));
        if (!new_mods) {
            fprintf(stderr, "error: module array realloc failed\n");
            return NULL;
        }
        cc->modules = new_mods;
        cc->module_capacity = new_cap;
    }

    /* create new module */
    Module *m = &cc->modules[cc->module_count++];
    memset(m, 0, sizeof(Module));

    /* copy name */
    char *name_copy = (char *)arena_alloc(&cc->arena, name_len + 1);
    memcpy(name_copy, name, name_len);
    name_copy[name_len] = '\0';
    m->name = name_copy;

    /* build file path: source_dir/name.zer */
    size_t dir_len = strlen(cc->source_dir);
    char *path = (char *)arena_alloc(&cc->arena, dir_len + name_len + 5);
    sprintf(path, "%s%.*s.zer", cc->source_dir, (int)name_len, name);
    m->path = path;

    return m;
}

/* parse a module (recursively resolves imports) */
static bool parse_module(Compiler *cc, Module *m) {
    if (m->checking) {
        fprintf(stderr, "error: circular import detected: '%s' imports itself (directly or indirectly)\n", m->name);
        return false;
    }
    if (m->parsed) return true;
    m->checking = true;

    /* read source */
    m->source = read_file(m->path);
    if (!m->source) {
        fprintf(stderr, "error: cannot open module '%s' at '%s'\n", m->name, m->path);
        return false;
    }

    /* parse */
    Scanner scanner;
    scanner_init(&scanner, m->source);
    Parser parser;
    parser_init(&parser, &scanner, &cc->arena, m->path);
    parser.source = m->source;
    m->ast = parse_file(&parser);
    if (parser.had_error) {
        fprintf(stderr, "error: parse errors in module '%s'\n", m->name);
        return false;
    }

    m->parsed = true;
    /* keep checking=true until imports resolved (circular detection) */

    /* resolve imports in this module */
    for (int i = 0; i < m->ast->file.decl_count; i++) {
        Node *decl = m->ast->file.decls[i];
        if (decl->kind == NODE_IMPORT) {
            Module *imported = find_or_create_module(cc,
                decl->import.module_name, decl->import.module_name_len);
            if (!imported || !parse_module(cc, imported)) {
                return false;
            }
        }
    }

    m->checking = false; /* imports resolved, safe to clear */
    return true;
}

/* ================================================================ */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: zerc <input.zer> [-o output] [--run] [--emit-c] [--emit-ir]\n");
        return 1;
    }

    const char *input_path = argv[1];
    const char *output_path = NULL;
    bool do_run = false;
    bool emit_c = false;
    bool emit_ir = false;
    bool no_preamble = false;
    bool no_strict_mmio = false;
    bool track_cptrs = false;
    bool release_mode = false;
    const char *gcc_override = NULL;
    bool target_bits_explicit = false;
    uint32_t zer_stack_limit = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (strcmp(argv[i], "--run") == 0) {
            do_run = true;
        } else if (strcmp(argv[i], "--emit-c") == 0) {
            emit_c = true;
        } else if (strcmp(argv[i], "--emit-ir") == 0) {
            emit_ir = true;
        } else if (strcmp(argv[i], "--lib") == 0) {
            no_preamble = true;
        } else if (strcmp(argv[i], "--no-strict-mmio") == 0) {
            no_strict_mmio = true;
        } else if (strcmp(argv[i], "--track-cptrs") == 0) {
            track_cptrs = true;
        } else if (strcmp(argv[i], "--release") == 0) {
            release_mode = true;
        } else if (strcmp(argv[i], "--target-bits") == 0 && i + 1 < argc) {
            zer_target_ptr_bits = atoi(argv[++i]);
            target_bits_explicit = true;
        } else if (strcmp(argv[i], "--gcc") == 0 && i + 1 < argc) {
            gcc_override = argv[++i];
        } else if (strcmp(argv[i], "--stack-limit") == 0 && i + 1 < argc) {
            zer_stack_limit = (uint32_t)atoi(argv[++i]);
        }
    }

    /* auto-detect target pointer width from GCC if not explicitly set */
    if (!target_bits_explicit) {
        const char *probe_gcc = gcc_override ? gcc_override : "gcc";
        char probe_cmd[512];
#ifdef _WIN32
        snprintf(probe_cmd, sizeof(probe_cmd),
            "echo. | \"%s\" -dM -E - 2>NUL", probe_gcc);
#else
        snprintf(probe_cmd, sizeof(probe_cmd),
            "echo '' | \"%s\" -dM -E - 2>/dev/null", probe_gcc);
#endif
        FILE *probe = popen(probe_cmd, "r");
        if (probe) {
            char line[256];
            while (fgets(line, sizeof(line), probe)) {
                /* look for: #define __SIZEOF_SIZE_T__ N */
                if (strstr(line, "__SIZEOF_SIZE_T__")) {
                    int sz = 0;
                    char *val = strstr(line, "__SIZEOF_SIZE_T__");
                    if (val) {
                        val += 17; /* skip "__SIZEOF_SIZE_T__" */
                        while (*val == ' ') val++;
                        sz = atoi(val);
                    }
                    if (sz > 0) zer_target_ptr_bits = sz * 8;
                    break;
                }
            }
            pclose(probe);
        }
        /* if probe failed, keep default 32 */
    }

    /* determine output mode:
     * zerc main.zer              → compile to main.exe (temp .c, deleted)
     * zerc main.zer --run        → compile + run (temp .c, deleted)
     * zerc main.zer --emit-c     → emit C to main.c (kept)
     * zerc main.zer -o out.c     → emit C to out.c (kept)
     * zerc main.zer -o out.exe   → compile to out.exe (temp .c, deleted) */
    char default_output[256];
    bool use_temp_c = false;

    if (output_path) {
        /* -o specified: check if target is .c (emit C) or not (compile to exe) */
        size_t olen = strlen(output_path);
        if (olen > 2 && strcmp(output_path + olen - 2, ".c") == 0) {
            /* -o file.c → emit C, keep file */
            emit_c = true;
        } else {
            /* -o file.exe or -o file → compile to that exe via temp .c */
            use_temp_c = true;
        }
    } else if (emit_c) {
        /* --emit-c with no -o: default to input.c */
        size_t len = strlen(input_path);
        if (len > 4 && strcmp(input_path + len - 4, ".zer") == 0) {
            memcpy(default_output, input_path, len - 4);
            strcpy(default_output + len - 4, ".c");
        } else {
            snprintf(default_output, sizeof(default_output), "%s.c", input_path);
        }
        output_path = default_output;
    } else {
        /* default: compile to exe via temp .c */
        use_temp_c = true;
    }

    /* for temp .c mode, create temp path and set up exe path */
    char temp_c_path[512];
    char exe_from_input[512];
    if (use_temp_c) {
        size_t len = strlen(input_path);
        if (len > 4 && strcmp(input_path + len - 4, ".zer") == 0) {
            memcpy(default_output, input_path, len - 4);
            strcpy(default_output + len - 4, ".c");
        } else {
            snprintf(default_output, sizeof(default_output), "%s.c", input_path);
        }
        output_path = default_output;
    }

    /* init compiler */
    Compiler cc;
    memset(&cc, 0, sizeof(cc));
    arena_init(&cc.arena, 1024 * 1024); /* 1MB arena */

    char dir_buf[256];
    cc.source_dir = get_dir(input_path, dir_buf, sizeof(dir_buf));

    /* init module array */
    cc.module_capacity = 16;
    cc.modules = (Module *)malloc(cc.module_capacity * sizeof(Module));
    memset(cc.modules, 0, cc.module_capacity * sizeof(Module));

    /* create main module */
    Module *main_mod = &cc.modules[cc.module_count++];
    main_mod->name = "<main>";
    main_mod->path = input_path;

    /* read main source */
    main_mod->source = read_file(input_path);
    if (!main_mod->source) {
        fprintf(stderr, "error: cannot open '%s'\n", input_path);
        return 1;
    }

    /* parse main + imports */
    Scanner scanner;
    scanner_init(&scanner, main_mod->source);
    Parser parser;
    parser_init(&parser, &scanner, &cc.arena, input_path);
    parser.source = main_mod->source;
    main_mod->ast = parse_file(&parser);
    if (parser.had_error) {
        fprintf(stderr, "error: parse failed\n");
        arena_free(&cc.arena);
        return 1;
    }
    main_mod->parsed = true;
    main_mod->checking = true; /* for circular import detection */

    /* resolve imports */
    for (int i = 0; i < main_mod->ast->file.decl_count; i++) {
        Node *decl = main_mod->ast->file.decls[i];
        if (decl->kind == NODE_IMPORT) {
            Module *imported = find_or_create_module(&cc,
                decl->import.module_name, decl->import.module_name_len);
            if (!imported || !parse_module(&cc, imported)) {
                arena_free(&cc.arena);
                return 1;
            }
        }
    }

    /* BUG-349: compute topological order for registration AND body checking,
     * not just emission. Dependencies must be registered before dependents. */
    int *topo_order = (int *)malloc(cc.module_count * sizeof(int));
    int topo_count = 0;
    {
        bool *tvisited = (bool *)calloc(cc.module_count, sizeof(bool));
        bool tprogress = true;
        while (tprogress && topo_count < cc.module_count) {
            tprogress = false;
            for (int i = 0; i < cc.module_count; i++) {
                if (tvisited[i]) continue;
                Module *m = &cc.modules[i];
                if (!m->ast) { tvisited[i] = true; continue; }
                bool deps_met = true;
                for (int j = 0; j < m->ast->file.decl_count; j++) {
                    Node *d = m->ast->file.decls[j];
                    if (d->kind == NODE_IMPORT) {
                        for (int k = 0; k < cc.module_count; k++) {
                            if (strlen(cc.modules[k].name) == d->import.module_name_len &&
                                memcmp(cc.modules[k].name, d->import.module_name,
                                       d->import.module_name_len) == 0) {
                                if (!tvisited[k]) { deps_met = false; break; }
                            }
                        }
                        if (!deps_met) break;
                    }
                }
                if (deps_met) {
                    topo_order[topo_count++] = i;
                    tvisited[i] = true;
                    tprogress = true;
                }
            }
        }
        free(tvisited);
    }

    /* type check — register all modules in topological order */
    Checker checker;
    checker_init(&checker, &cc.arena, input_path);
    checker.source = main_mod->source;
    checker.no_strict_mmio = no_strict_mmio;
    checker.stack_limit = zer_stack_limit;

    /* register in topo order: dependencies first, main last */
    for (int ti = 0; ti < topo_count; ti++) {
        int idx = topo_order[ti];
        Module *m = &cc.modules[idx];
        if (!m->ast) continue;
        if (idx == 0) {
            /* main module — no prefix */
            checker.current_module = NULL;
            checker.current_module_len = 0;
        } else {
            checker.current_module = m->name;
            checker.current_module_len = (uint32_t)strlen(m->name);
        }
        checker_register_file(&checker, m->ast);
    }

    /* type-check bodies in topo order — each imported module gets its own scope
     * so same-named types in different modules resolve correctly */
    for (int ti = 0; ti < topo_count; ti++) {
        int idx = topo_order[ti];
        if (idx == 0) continue; /* main checked separately below */
        Module *m = &cc.modules[idx];
        if (!m->ast) continue;
        checker.current_module = m->name;
        checker.current_module_len = (uint32_t)strlen(m->name);
        checker.file_name = m->path;
        checker.source = m->source;
        checker_push_module_scope(&checker, m->ast);
        checker_check_bodies(&checker, m->ast);
        checker_pop_module_scope(&checker);
    }
    checker.current_module = NULL;
    checker.current_module_len = 0;
    checker.file_name = input_path;
    checker.source = main_mod->source;

    /* type-check main file (full check — its decls were registered above) */
    if (!checker_check_bodies(&checker, main_mod->ast)) {
        fprintf(stderr, "error: type check failed\n");
        free(cc.modules);
        arena_free(&cc.arena);
        return 1;
    }

    /* Post-passes on main file: stack depth + interrupt safety + lock ordering */
    checker_post_passes(&checker, main_mod->ast);
    if (checker.error_count > 0) {
        fprintf(stderr, "error: type check failed\n");
        free(cc.modules);
        arena_free(&cc.arena);
        return 1;
    }

    /* IR lowering: AST → flat IR (for --emit-ir debugging + future analysis) */
    if (emit_ir) {
        Node *file = main_mod->ast;
        if (file && file->kind == NODE_FILE) {
            for (int i = 0; i < file->file.decl_count; i++) {
                Node *decl = file->file.decls[i];
                IRFunc *ir = NULL;
                if (decl->kind == NODE_FUNC_DECL && decl->func_decl.body)
                    ir = ir_lower_func(&cc.arena, &checker, decl);
                else if (decl->kind == NODE_INTERRUPT && decl->interrupt.body)
                    ir = ir_lower_interrupt(&cc.arena, &checker, decl);
                if (ir) {
                    if (!ir_validate(ir)) {
                        fprintf(stderr, "INTERNAL ERROR: IR validation failed\n");
                        free(topo_order);
                        free(cc.modules);
                        arena_free(&cc.arena);
                        return 1;
                    }
                    ir_print(stdout, ir);
                    fprintf(stdout, "\n");
                }
            }
        }
        free(topo_order);
        free(cc.modules);
        arena_free(&cc.arena);
        return 0;
    }

    /* ZER-CHECK: path-sensitive handle + *opaque tracking */
    {
        ZerCheck zc;
        zercheck_init(&zc, &checker, &cc.arena, input_path);
        /* Feed imported module ASTs in topological order (dependencies first)
         * for cross-module summary building. topo_order[0] is main module,
         * rest are imports in dependency order. */
        Node *import_asts[64];
        int import_ast_count = 0;
        for (int ti = 0; ti < topo_count && import_ast_count < 64; ti++) {
            int mi = topo_order[ti];
            if (mi != 0 && cc.modules[mi].ast)
                import_asts[import_ast_count++] = cc.modules[mi].ast;
        }
        zc.import_asts = import_asts;
        zc.import_ast_count = import_ast_count;
        bool ast_ok = zercheck_run(&zc, main_mod->ast);

        /* Phase E: optional dual-run. Runs BEFORE ast_ok short-circuits
         * so negative tests can be verified too. When ZER_DUAL_RUN=1 is
         * set, also run zercheck_ir.c (CFG-based) on each function and
         * log diagnostic-count disagreements to stderr.
         *
         * Parity semantics for negative tests:
         *   - ast_err > 0 AND ir_err > 0  → both reject, no disagreement
         *   - ast_err > 0 AND ir_err == 0 → zercheck_ir MISSED a case
         *   - ast_err == 0 AND ir_err > 0 → zercheck_ir FALSE POSITIVE
         *
         * zercheck.c remains the source of truth for the compile's
         * success/failure; zercheck_ir errors are comparison-only. */
        extern bool zercheck_ir(ZerCheck *zc_ir, IRFunc *func);
        const char *dual = getenv("ZER_DUAL_RUN");
        if (dual && dual[0] != '\0' && dual[0] != '0') {
            ZerCheck zc_ir;
            zercheck_init(&zc_ir, &checker, &cc.arena, input_path);
            zc_ir.import_asts = import_asts;
            zc_ir.import_ast_count = import_ast_count;
            int ir_func_count = 0;
            int ir_error_count_start = zc_ir.error_count;
            for (int i = 0; i < main_mod->ast->file.decl_count; i++) {
                Node *decl = main_mod->ast->file.decls[i];
                if (decl->kind != NODE_FUNC_DECL) continue;
                if (!decl->func_decl.body) continue;
                IRFunc *ir = ir_lower_func(&cc.arena, &checker, decl);
                if (!ir) continue;
                ir->module_prefix = NULL;
                ir->module_prefix_len = 0;
                zercheck_ir(&zc_ir, ir);
                ir_func_count++;
            }
            int ast_err = zc.error_count;
            int ir_err = zc_ir.error_count - ir_error_count_start;
            /* Loose parity: for negative tests we don't require exact
             * error counts — only that BOTH analyzers reject. Exact-count
             * match required only when both agree on zero errors. */
            bool agree;
            if (ast_err == 0 && ir_err == 0) agree = true;
            else if (ast_err > 0 && ir_err > 0) agree = true;
            else agree = false;
            if (!agree) {
                fprintf(stderr,
                    "zercheck DUAL-RUN disagreement in %s: ast=%d ir=%d "
                    "(functions analyzed: %d)\n",
                    input_path, ast_err, ir_err, ir_func_count);
            } else if (dual[0] == '2') {
                fprintf(stderr,
                    "zercheck DUAL-RUN agree in %s: ast=%d ir=%d "
                    "(functions analyzed: %d)\n",
                    input_path, ast_err, ir_err, ir_func_count);
            }
        }

        if (!ast_ok) {
            fprintf(stderr, "error: zercheck failed\n");
            free(cc.modules);
            arena_free(&cc.arena);
            return 1;
        }
    }

    /* emit C */
    FILE *out = fopen(output_path, "w");
    if (!out) {
        fprintf(stderr, "error: cannot open output '%s'\n", output_path);
        arena_free(&cc.arena);
        return 1;
    }

    Emitter emitter;
    emitter_init(&emitter, out, &cc.arena, &checker);
    emitter.lib_mode = no_preamble;
    /* track_cptrs: _zer_opaque wrapping + _zer_check_alive + --wrap=malloc.
     * Always on for --run (including --release) — compiled-in safety, not debug.
     * Only disabled without explicit --track-cptrs when emitting C library (--lib). */
    emitter.track_cptrs = track_cptrs || do_run;
    emitter.source_file = input_path;

    /* emit in topological order — dependencies first, main last.
     * When IR is active, emit ALL modules' structs+globals first,
     * then ALL modules' functions. This ensures cross-module global
     * references are declared before use (C forward-declaration requirement). */
    for (int ti = 0; ti < topo_count; ti++) {
        Module *m = &cc.modules[topo_order[ti]];
        emitter.source_file = m->path;
        if (topo_order[ti] == 0) {
            emitter.current_module = NULL;
            emitter.current_module_len = 0;
        } else {
            emitter.current_module = m->name;
            emitter.current_module_len = (uint32_t)strlen(m->name);
        }
        if (ti == 0 && !no_preamble) {
            emit_file(&emitter, m->ast);
        } else {
            emit_file_no_preamble(&emitter, m->ast);
        }
    }
    free(topo_order);

    fclose(out);

    if (emit_c) {
        printf("zerc: %s -> %s\n", input_path, output_path);
    }

    /* compile to exe: when --run or use_temp_c (default compile mode) */
    if (do_run || use_temp_c) {
        /* build exe path */
        char exe_path[512];
#ifdef _WIN32
        const char *exe_ext = ".exe";
#else
        const char *exe_ext = "";
#endif
        /* derive exe name from input .zer file */
        size_t ilen = strlen(input_path);
        if (ilen > 4 && strcmp(input_path + ilen - 4, ".zer") == 0) {
            memcpy(exe_path, input_path, ilen - 4);
            strcpy(exe_path + ilen - 4, exe_ext);
        } else {
            snprintf(exe_path, sizeof(exe_path), "%s%s", input_path, exe_ext);
        }

        /* find GCC: check for bundled gcc relative to zerc binary first,
         * then fall back to system PATH.
         * Bundled layout: zerc[.exe] + gcc/bin/gcc[.exe] in same directory. */
        char gcc_path[512];
        int found_bundled = 0;
        {
            /* extract directory from argv[0] */
            char zerc_dir[512];
            strncpy(zerc_dir, argv[0], sizeof(zerc_dir) - 1);
            zerc_dir[sizeof(zerc_dir) - 1] = '\0';
            /* find last slash or backslash */
            char *last_sep = NULL;
            for (char *p = zerc_dir; *p; p++) {
                if (*p == '/' || *p == '\\') last_sep = p;
            }
            if (last_sep) {
                *(last_sep + 1) = '\0';
            } else {
                zerc_dir[0] = '\0'; /* zerc in current dir */
            }
#ifdef _WIN32
            snprintf(gcc_path, sizeof(gcc_path), "%sgcc\\bin\\gcc.exe", zerc_dir);
#else
            snprintf(gcc_path, sizeof(gcc_path), "%sgcc/bin/gcc", zerc_dir);
#endif
            FILE *test = fopen(gcc_path, "r");
            if (test) {
                fclose(test);
                found_bundled = 1;
            }
        }
        if (!found_bundled) {
            strcpy(gcc_path, "gcc"); /* fall back to system PATH */
        }

        char gcc_cmd[2048];
        /* Only quote gcc path if it contains spaces (bundled path).
         * Plain "gcc" with quotes breaks Windows cmd.exe system(). */
        bool need_quote = (strchr(gcc_path, ' ') != NULL);
        const char *wrap_flags = (emitter.track_cptrs) ?
            " -Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc" : "";
#ifdef _WIN32
        const char *platform_flags = " -mconsole";
#else
        const char *platform_flags = "";
#endif
        snprintf(gcc_cmd, sizeof(gcc_cmd),
                 need_quote ? "\"%s\" -std=c99 -O2 -fwrapv -fno-strict-aliasing%s%s -o \"%s\" \"%s\""
                            : "%s -std=c99 -O2 -fwrapv -fno-strict-aliasing%s%s -o \"%s\" \"%s\"",
                 gcc_path, wrap_flags, platform_flags, exe_path, output_path);
        printf("zerc: %s\n", gcc_cmd);
        int gcc_ret = system(gcc_cmd);

        /* delete temp .c file after GCC (success or fail) */
        if (use_temp_c) {
            remove(output_path);
        }

        if (gcc_ret != 0) {
            fprintf(stderr, "zerc: gcc failed (tried: %s)\n", gcc_path);
            free(cc.modules);
            arena_free(&cc.arena);
            return 1;
        }

        printf("zerc: %s -> %s\n", input_path, exe_path);

        /* only run if --run was specified */
        if (do_run) {
            char run_cmd[1024];
#ifdef _WIN32
            /* absolute paths (C:\...) don't need .\ prefix */
            if (exe_path[0] && exe_path[1] == ':') {
                snprintf(run_cmd, sizeof(run_cmd), "%s", exe_path);
            } else {
                snprintf(run_cmd, sizeof(run_cmd), ".\\%s", exe_path);
            }
#else
            /* absolute paths (/...) don't need ./ prefix */
            if (exe_path[0] == '/') {
                snprintf(run_cmd, sizeof(run_cmd), "%s", exe_path);
            } else {
                snprintf(run_cmd, sizeof(run_cmd), "./%s", exe_path);
            }
#endif
            printf("zerc: running %s\n", exe_path);
            int run_ret = system(run_cmd);
            free(cc.modules);
            arena_free(&cc.arena);
            /* BUG-581: system() returns wait status on POSIX (encoded with
             * high bits for signals). Returning raw status → shell sees
             * `status & 255` so exit code 3 shows as 0, silently masking
             * test failures. Extract the real exit code.
             * On Windows MSVCRT system() already returns the exit code directly. */
#ifdef _WIN32
            return run_ret;
#else
            if (WIFEXITED(run_ret)) return WEXITSTATUS(run_ret);
            if (WIFSIGNALED(run_ret)) return 128 + WTERMSIG(run_ret);
            return run_ret ? 1 : 0;
#endif
        }
    }

    free(cc.modules);
    arena_free(&cc.arena);
    return 0;
}
