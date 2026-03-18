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

#define MAX_MODULES 64

typedef struct {
    const char *name;       /* module name (e.g., "uart") */
    const char *path;       /* file path (e.g., "uart.zer") */
    char *source;           /* source text */
    Node *ast;              /* parsed AST */
    bool parsed;
    bool checking;          /* for circular import detection */
    bool checked;
} Module;

typedef struct {
    Module modules[MAX_MODULES];
    int module_count;
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

    if (cc->module_count >= MAX_MODULES) {
        fprintf(stderr, "error: too many modules (max %d)\n", MAX_MODULES);
        return NULL;
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
    if (m->parsed) return true;

    if (m->checking) {
        fprintf(stderr, "error: circular import detected: '%s'\n", m->name);
        return false;
    }
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
    m->checking = false;

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
        fprintf(stderr, "Usage: zerc <input.zer> [-o output.c]\n");
        return 1;
    }

    const char *input_path = argv[1];
    const char *output_path = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
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

    /* create main module */
    Module *main_mod = &cc.modules[cc.module_count++];
    memset(main_mod, 0, sizeof(Module));
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

    /* register imported module declarations into global scope */
    for (int i = 1; i < cc.module_count; i++) {
        Module *m = &cc.modules[i];
        if (m->ast) {
            checker_register_file(&checker, m->ast);
        }
    }

    /* check main file (imports already in scope) */
    if (!checker_check(&checker, main_mod->ast)) {
        fprintf(stderr, "error: type check failed\n");
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

    /* emit: preamble (from main) → imported modules → main declarations
     * This ensures imported functions are declared before main uses them. */
    /* We emit main's file which includes preamble + main declarations,
     * but imported module code needs to come between preamble and main body.
     * Simplest: emit preamble via main, then imported, then main body.
     * Since emit_file does both, just emit imported first with preamble,
     * then main without. But only first file needs preamble. */
    if (cc.module_count > 1) {
        /* Emit in reverse dependency order: deepest dependency first.
         * Modules are added depth-first during recursive import resolution,
         * so the last module in the array is the deepest dependency.
         * Reverse order ensures C declaration-before-use. */

        /* deepest dependency gets preamble */
        emit_file(&emitter, cc.modules[cc.module_count - 1].ast);

        /* remaining imports in reverse order (no preamble) */
        for (int i = cc.module_count - 2; i >= 1; i--) {
            if (cc.modules[i].ast)
                emit_file_no_preamble(&emitter, cc.modules[i].ast);
        }

        /* main last (no preamble) */
        emit_file_no_preamble(&emitter, main_mod->ast);
    } else {
        /* no imports: just emit main with preamble */
        emit_file(&emitter, main_mod->ast);
    }

    fclose(out);

    printf("zerc: %s → %s\n", input_path, output_path);

    arena_free(&cc.arena);
    return 0;
}
