#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lexer.h"
#include "ast.h"
#include "parser.h"
#include "types.h"
#include "checker.h"
#include "emitter.h"

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
    fread(buf, 1, size, f);
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

/* register all declarations from a module into the checker */
static void register_module_decls(Compiler *cc, Checker *checker, Module *m) {
    if (m->checked) return;
    m->checked = true;

    /* first register imports */
    for (int i = 0; i < m->ast->file.decl_count; i++) {
        Node *decl = m->ast->file.decls[i];
        if (decl->kind == NODE_IMPORT) {
            Module *imported = find_or_create_module(cc,
                decl->import.module_name, decl->import.module_name_len);
            if (imported) {
                register_module_decls(cc, checker, imported);
            }
        }
    }
}

/* ================================================================ */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: zerc <input.zer> [-o output.c] [--run]\n");
        return 1;
    }

    const char *input_path = argv[1];
    const char *output_path = NULL;
    bool do_run = false;
    bool no_preamble = false;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (strcmp(argv[i], "--run") == 0) {
            do_run = true;
        } else if (strcmp(argv[i], "--lib") == 0) {
            no_preamble = true;
        }
    }

    /* default output: input with .c extension */
    char default_output[256];
    if (!output_path) {
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

    /* type check — register all imported module declarations first */
    Checker checker;
    checker_init(&checker, &cc.arena, input_path);

    /* register imported modules first (so their types are in scope for main) */
    for (int i = 1; i < cc.module_count; i++) {
        Module *m = &cc.modules[i];
        if (m->ast) checker_register_file(&checker, m->ast);
    }
    /* register main module last */
    if (main_mod->ast) checker_register_file(&checker, main_mod->ast);

    /* type-check imported module bodies (declarations already registered) */
    for (int i = 1; i < cc.module_count; i++) {
        Module *m = &cc.modules[i];
        if (m->ast) {
            checker_check_bodies(&checker, m->ast);
        }
    }

    /* type-check main file (full check — its decls were registered above) */
    if (!checker_check_bodies(&checker, main_mod->ast)) {
        fprintf(stderr, "error: type check failed\n");
        free(cc.modules);
        arena_free(&cc.arena);
        return 1;
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

    /* emit: preamble (from main) → imported modules → main declarations
     * This ensures imported functions are declared before main uses them. */
    /* We emit main's file which includes preamble + main declarations,
     * but imported module code needs to come between preamble and main body.
     * Simplest: emit preamble via main, then imported, then main body.
     * Since emit_file does both, just emit imported first with preamble,
     * then main without. But only first file needs preamble. */
    /* Emit with topological ordering: dependencies before dependents.
     * Recursive: emit a module's imports first, then the module itself.
     * emitted flag prevents double emission in diamond dependencies. */
    {
        /* first, emit preamble (only once) via a dummy main emit */
        /* we need preamble before any module code */
        /* hack: emit main file (which has preamble), but we also need
         * imported modules before main's functions. So: emit preamble
         * separately by emitting an empty-ish main first... */
        /* Actually simpler: just emit preamble manually, then topo-order all */
    }
    /* For clean solution: emit main file (has preamble + runtime),
     * then topo-emit imports, then... no, main needs imports first.
     *
     * Cleanest: build emit order array via DFS, emit first gets preamble. */
    {
        int *emit_order = (int *)malloc(cc.module_count * sizeof(int));
        int emit_count = 0;
        bool *visited = (bool *)calloc(cc.module_count, sizeof(bool));

        /* recursive topo sort via function pointer... or just inline loops.
         * For simplicity, iterate: repeatedly find modules with all deps emitted */
        bool progress = true;
        while (progress && emit_count < cc.module_count) {
            progress = false;
            for (int i = 0; i < cc.module_count; i++) {
                if (visited[i]) continue;
                Module *m = &cc.modules[i];
                if (!m->ast) { visited[i] = true; continue; }

                /* check if all imports of this module are already emitted */
                bool deps_met = true;
                for (int j = 0; j < m->ast->file.decl_count; j++) {
                    Node *d = m->ast->file.decls[j];
                    if (d->kind == NODE_IMPORT) {
                        /* find the imported module index */
                        for (int k = 0; k < cc.module_count; k++) {
                            if (strlen(cc.modules[k].name) == d->import.module_name_len &&
                                memcmp(cc.modules[k].name, d->import.module_name,
                                       d->import.module_name_len) == 0) {
                                if (!visited[k]) { deps_met = false; break; }
                            }
                        }
                        if (!deps_met) break;
                    }
                }

                if (deps_met) {
                    emit_order[emit_count++] = i;
                    visited[i] = true;
                    progress = true;
                }
            }
        }

        /* emit in topological order: first module gets preamble (unless --lib) */
        for (int i = 0; i < emit_count; i++) {
            Module *m = &cc.modules[emit_order[i]];
            if (i == 0 && !no_preamble) {
                emit_file(&emitter, m->ast);
            } else {
                emit_file_no_preamble(&emitter, m->ast);
            }
        }
        free(emit_order);
        free(visited);
    }

    fclose(out);

    printf("zerc: %s → %s\n", input_path, output_path);

    /* --run: compile with GCC and execute */
    if (do_run) {
        /* build exe path: replace .c with .exe (or append .exe) */
        char exe_path[512];
        size_t clen = strlen(output_path);
        if (clen > 2 && strcmp(output_path + clen - 2, ".c") == 0) {
            memcpy(exe_path, output_path, clen - 2);
            strcpy(exe_path + clen - 2, ".exe");
        } else {
            snprintf(exe_path, sizeof(exe_path), "%s.exe", output_path);
        }

        char gcc_cmd[1024];
        snprintf(gcc_cmd, sizeof(gcc_cmd), "gcc -std=c99 -O2 -o \"%s\" \"%s\"",
                 exe_path, output_path);
        printf("zerc: %s\n", gcc_cmd);
        int gcc_ret = system(gcc_cmd);
        if (gcc_ret != 0) {
            fprintf(stderr, "zerc: gcc failed\n");
            free(cc.modules);
            arena_free(&cc.arena);
            return 1;
        }

        char run_cmd[1024];
        snprintf(run_cmd, sizeof(run_cmd), ".\\\"%s\"", exe_path);
        printf("zerc: running %s\n", exe_path);
        int run_ret = system(run_cmd);
        free(cc.modules);
        arena_free(&cc.arena);
        return run_ret;
    }

    free(cc.modules);
    arena_free(&cc.arena);
    return 0;
}
