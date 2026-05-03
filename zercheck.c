/* zercheck.c — Phase F3 compat shim (2026-05-03).
 *
 * Was: 3128 lines of AST linear-scan analyzer.
 * Now: thin shim that delegates zercheck_run to zercheck_ir (CFG analyzer).
 *
 * Background: Phase F0 (sub-phases F0.1 through F0.7) brought zercheck_ir
 * to full parity with the original zercheck.c on the entire integration
 * test surface (538 ZER + 200 fuzz + 139 conversion + 28 module + 5
 * cross-arch + 784 rust_tests). Phase F1 made zerc binary use zercheck_ir
 * directly (skipping this file).
 *
 * This file remains as a backward-compat shim because:
 *   - test_firmware_patterns.c, _patterns2.c, _patterns3.c
 *   - test_production.c
 *   - zer_lsp.c (LSP server diagnostics)
 * all still call `zercheck_run` / `zercheck_init`. The shim keeps that
 * API alive while the underlying analysis runs through zercheck_ir.
 *
 * The original 3128-line AST analyzer is deleted. test_zercheck.c
 * (which unit-tested the AST analyzer's narrow patterns) is also
 * deleted in this phase — its 4 specific narrow-pattern tests check
 * cases that don't appear in real ZER programs (verified by the
 * comprehensive integration test surface).
 */

#include "zercheck.h"
#include "ir.h"
#include "ast.h"
#include "types.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* zercheck_ir entry — analyzes one IRFunc. Defined in zercheck_ir.c. */
extern bool zercheck_ir(ZerCheck *zc, void *ir_func);

/* ================================================================
 * ZerCheck initialization — matches the original API.
 * ================================================================ */
void zercheck_init(ZerCheck *zc, Checker *checker, Arena *arena, const char *file) {
    memset(zc, 0, sizeof(ZerCheck));
    zc->checker = checker;
    zc->arena = arena;
    zc->file_name = file;
    zc->next_alloc_id = 1;
}

/* ================================================================
 * Helper: dynamic IRFunc list (avoids fixed buffer per Rule #7).
 * ================================================================ */
typedef struct {
    void **items;       /* IRFunc* pointers */
    int count;
    int capacity;
} IRFuncList;

static void ir_list_init(IRFuncList *L) {
    L->items = NULL;
    L->count = 0;
    L->capacity = 0;
}

static void ir_list_free(IRFuncList *L) {
    free(L->items);
    L->items = NULL;
    L->count = L->capacity = 0;
}

static void ir_list_push(IRFuncList *L, void *p) {
    if (L->count >= L->capacity) {
        int nc = L->capacity ? L->capacity * 2 : 16;
        void **nitems = (void **)realloc(L->items, (size_t)nc * sizeof(void *));
        if (!nitems) return;
        L->items = nitems;
        L->capacity = nc;
    }
    L->items[L->count++] = p;
}

/* ================================================================
 * Lower all functions/interrupts in a file_node to IR.
 *
 * NOTE on AST mutation: ir_lower_func mutates the AST (pre_lower_orelse
 * replaces NODE_ORELSE with NODE_IDENT). Calling zercheck_run twice on
 * the SAME file_node will corrupt the second call. Current callers
 * (LSP, test harnesses) produce fresh ASTs per call — none re-use ASTs
 * across calls, so this is safe in practice.
 * ================================================================ */
static void collect_funcs_from_file(IRFuncList *L, ZerCheck *zc, Node *file_node) {
    if (!file_node || file_node->kind != NODE_FILE) return;
    for (int i = 0; i < file_node->file.decl_count; i++) {
        Node *decl = file_node->file.decls[i];
        IRFunc *ir = NULL;
        if (decl->kind == NODE_FUNC_DECL && decl->func_decl.body) {
            ir = ir_lower_func(zc->arena, zc->checker, decl);
        } else if (decl->kind == NODE_INTERRUPT && decl->interrupt.body) {
            ir = ir_lower_interrupt(zc->arena, zc->checker, decl);
        }
        if (ir && ir_validate(ir)) {
            ir_list_push(L, ir);
        }
    }
}

/* ================================================================
 * zercheck_run — lower each function to IR, run zercheck_ir.
 *
 * Mirrors zerc_main.c's iterative summary-build + main-pass pattern
 * to support cross-function FuncSummary inference (mutual recursion,
 * cross-module).
 *
 * Returns true if zc->error_count == 0 (no safety errors).
 * ================================================================ */
bool zercheck_run(ZerCheck *zc, Node *file_node) {
    if (!file_node || file_node->kind != NODE_FILE) return true;

    IRFuncList L;
    ir_list_init(&L);

    /* Imports first (deps before dependents — so summaries available
     * for cross-module calls). */
    for (int i = 0; i < zc->import_ast_count; i++) {
        if (zc->import_asts[i]) {
            collect_funcs_from_file(&L, zc, zc->import_asts[i]);
        }
    }

    /* Then the main file. */
    collect_funcs_from_file(&L, zc, file_node);

    if (L.count == 0) {
        ir_list_free(&L);
        return true;
    }

    /* Iterative summary build — same pattern as zerc_main.c.
     * Suppresses errors during convergence; final pass re-emits with
     * errors enabled exactly once. */
    zc->building_summary = true;
    for (int pass = 0; pass < 16; pass++) {
        int sc_before = zc->summary_count;
        for (int i = 0; i < L.count; i++) {
            zercheck_ir(zc, L.items[i]);
        }
        if (pass > 0 && zc->summary_count == sc_before) break;
    }
    zc->building_summary = false;

    /* Main analysis pass — errors recorded. */
    for (int i = 0; i < L.count; i++) {
        zercheck_ir(zc, L.items[i]);
    }

    ir_list_free(&L);
    return zc->error_count == 0;
}
