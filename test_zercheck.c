#include <stdio.h>
#include <string.h>
#include "lexer.h"
#include "ast.h"
#include "parser.h"
#include "types.h"
#include "checker.h"
#include "zercheck.h"

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

static bool run_zercheck(const char *source, Arena *arena) {
    Scanner s; scanner_init(&s, source);
    Parser p; parser_init(&p, &s, arena, "test");
    Node *f = parse_file(&p);
    if (p.had_error) return true; /* parse error = not zercheck's problem */

    Checker c; checker_init(&c, arena, "test");
    if (!checker_check(&c, f)) return true; /* type error = not zercheck's problem */

    ZerCheck zc;
    zercheck_init(&zc, &c, arena, "test");
    return zercheck_run(&zc, f);
}

static void ok(const char *src, const char *name) {
    Arena a; arena_init(&a, 128*1024); tests_run++;
    if (run_zercheck(src, &a)) { tests_passed++; }
    else { printf("  FAIL(ok): %s\n", name); tests_failed++; }
    arena_free(&a);
}

static void err(const char *src, const char *name) {
    Arena a; arena_init(&a, 128*1024); tests_run++;
    if (!run_zercheck(src, &a)) { tests_passed++; }
    else { printf("  FAIL(err): %s\n", name); tests_failed++; }
    arena_free(&a);
}

/* ================================================================ */

static void test_basic_pool_lifecycle(void) {
    printf("[basic pool lifecycle]\n");
    ok("struct T { u32 x; }\n"
       "Pool(T, 4) pool;\n"
       "void f() {\n"
       "    Handle(T) h = pool.alloc() orelse return;\n"
       "    pool.get(h).x = 5;\n"
       "    pool.free(h);\n"
       "}\n",
       "alloc → get → free: clean lifecycle");
}

static void test_use_after_free(void) {
    printf("[use-after-free detection]\n");
    err("struct T { u32 x; }\n"
        "Pool(T, 4) pool;\n"
        "void f() {\n"
        "    Handle(T) h = pool.alloc() orelse return;\n"
        "    pool.free(h);\n"
        "    pool.get(h).x = 5;\n"
        "}\n",
        "get after free detected");
}

static void test_double_free(void) {
    printf("[double free detection]\n");
    err("struct T { u32 x; }\n"
        "Pool(T, 4) pool;\n"
        "void f() {\n"
        "    Handle(T) h = pool.alloc() orelse return;\n"
        "    pool.free(h);\n"
        "    pool.free(h);\n"
        "}\n",
        "double free detected");
}

static void test_wrong_pool(void) {
    printf("[wrong pool detection]\n");
    err("struct T { u32 x; }\n"
        "Pool(T, 4) pool_a;\n"
        "Pool(T, 4) pool_b;\n"
        "void f() {\n"
        "    Handle(T) h = pool_a.alloc() orelse return;\n"
        "    pool_b.get(h).x = 5;\n"
        "    pool_a.free(h);\n"
        "}\n",
        "handle from pool_a used on pool_b");
}

static void test_handle_freed_in_branch(void) {
    printf("[handle freed in branch]\n");
    ok("struct T { u32 x; }\n"
       "Pool(T, 4) pool;\n"
       "void f(bool cond) {\n"
       "    Handle(T) h = pool.alloc() orelse return;\n"
       "    if (cond) {\n"
       "        pool.free(h);\n"
       "    } else {\n"
       "        pool.free(h);\n"
       "    }\n"
       "}\n",
       "freed on both branches: no error after merge");
}

static void test_clean_program(void) {
    printf("[clean program — no pool]\n");
    ok("u32 add(u32 a, u32 b) { return a + b; }\n"
       "u32 main() { return add(1, 2); }\n",
       "program without pools passes cleanly");
}

static void test_multiple_handles(void) {
    printf("[multiple handles]\n");
    ok("struct T { u32 x; }\n"
       "Pool(T, 8) pool;\n"
       "void f() {\n"
       "    Handle(T) h1 = pool.alloc() orelse return;\n"
       "    Handle(T) h2 = pool.alloc() orelse return;\n"
       "    pool.get(h1).x = 1;\n"
       "    pool.get(h2).x = 2;\n"
       "    pool.free(h1);\n"
       "    pool.free(h2);\n"
       "}\n",
       "two handles, both properly freed");
}

static void test_use_after_free_second_handle(void) {
    printf("[use-after-free on second handle]\n");
    err("struct T { u32 x; }\n"
        "Pool(T, 8) pool;\n"
        "void f() {\n"
        "    Handle(T) h1 = pool.alloc() orelse return;\n"
        "    Handle(T) h2 = pool.alloc() orelse return;\n"
        "    pool.free(h2);\n"
        "    pool.get(h2).x = 5;\n"
        "    pool.free(h1);\n"
        "}\n",
        "h2 used after free, h1 still alive");
}

/* ================================================================ */

int main(void) {
    printf("=== ZER-CHECK Tests ===\n\n");

    test_basic_pool_lifecycle();
    test_use_after_free();
    test_double_free();
    test_wrong_pool();
    test_handle_freed_in_branch();
    test_clean_program();
    test_multiple_handles();
    test_use_after_free_second_handle();

    /* ---- BUG-035: if/else merge — only mark freed if BOTH paths free ---- */
    printf("\n[if/else merge: freed on one branch only → not freed]\n");
    /* Key regression test: then-branch frees, else doesn't — use after if
     * must be OK. Old code (|| merge) would false-positive here. */
    ok("struct T { u32 x; }\n"
       "Pool(T, 4) pool;\n"
       "void f(bool cond) {\n"
       "    Handle(T) h = pool.alloc() orelse return;\n"
       "    if (cond) {\n"
       "        pool.free(h);\n"
       "    } else {\n"
       "        pool.get(h).x = 1;\n"
       "    }\n"
       "    pool.get(h).x = 2;\n"
       "}\n",
       "one-branch free with else: no false positive on post-if use");

    printf("[if/else merge: freed on both branches → use after = error]\n");
    err("struct T { u32 x; }\n"
        "Pool(T, 4) pool;\n"
        "void f(bool cond) {\n"
        "    Handle(T) h = pool.alloc() orelse return;\n"
        "    if (cond) {\n"
        "        pool.free(h);\n"
        "    } else {\n"
        "        pool.free(h);\n"
        "    }\n"
        "    pool.get(h).x = 1;\n"
        "}\n",
        "both-branch free then use = error");

    /* ---- BUG-036: switch arm merge — only freed if ALL arms free ---- */
    printf("\n[switch merge: freed in all arms → use after = error]\n");
    err("struct T { u32 x; }\n"
        "Pool(T, 4) pool;\n"
        "enum Mode { a, b }\n"
        "void f(Mode m) {\n"
        "    Handle(T) h = pool.alloc() orelse return;\n"
        "    switch (m) {\n"
        "        .a => { pool.free(h); }\n"
        "        .b => { pool.free(h); }\n"
        "    }\n"
        "    pool.get(h).x = 1;\n"
        "}\n",
        "all switch arms free then use = error");

    printf("[switch merge: freed in some arms → no false positive]\n");
    /* Key regression test: only one arm frees — use after switch must be OK */
    ok("struct T { u32 x; }\n"
       "Pool(T, 4) pool;\n"
       "enum Mode { a, b }\n"
       "void f(Mode m) {\n"
       "    Handle(T) h = pool.alloc() orelse return;\n"
       "    switch (m) {\n"
       "        .a => { pool.free(h); }\n"
       "        .b => { }\n"
       "    }\n"
       "    pool.get(h).x = 1;\n"
       "}\n",
       "partial switch free: no false positive on post-switch use");

    /* ---- Loop + Pool patterns ---- */
    printf("\n[loop: pool.free inside loop — caught]\n");
    err("struct T { u32 x; }\n"
        "Pool(T, 4) pool;\n"
        "void f() {\n"
        "    Handle(T) h = pool.alloc() orelse return;\n"
        "    for (u32 i = 0; i < 3; i += 1) {\n"
        "        pool.free(h);\n"
        "    }\n"
        "}\n",
        "pool.free inside loop — use-after-free on next iteration");

    printf("[loop: alloc+use+free in same iteration — OK]\n");
    ok("struct T { u32 x; }\n"
       "Pool(T, 4) pool;\n"
       "void f() {\n"
       "    for (u32 i = 0; i < 3; i += 1) {\n"
       "        Handle(T) h = pool.alloc() orelse return;\n"
       "        pool.get(h).x = i;\n"
       "        pool.free(h);\n"
       "    }\n"
       "}\n",
       "alloc+use+free in same iteration — OK");

    printf("[loop: conditional free in loop — not caught]\n");
    /* NOTE: zercheck under-approximation — conditional free inside
       loop doesn't propagate freed state outside the loop body */
    ok("struct T { u32 x; }\n"
       "Pool(T, 4) pool;\n"
       "void f(bool cond) {\n"
       "    Handle(T) h = pool.alloc() orelse return;\n"
       "    for (u32 i = 0; i < 3; i += 1) {\n"
       "        if (cond) { pool.free(h); }\n"
       "    }\n"
       "    pool.get(h).x = 1;\n"
       "}\n",
       "conditional free in loop then use after — not caught (under-approx)");

    printf("[loop: alloc before loop, use in loop, free after — OK]\n");
    ok("struct T { u32 x; }\n"
       "Pool(T, 4) pool;\n"
       "void f() {\n"
       "    Handle(T) h = pool.alloc() orelse return;\n"
       "    for (u32 i = 0; i < 3; i += 1) {\n"
       "        pool.get(h).x = i;\n"
       "    }\n"
       "    pool.free(h);\n"
       "}\n",
       "alloc before loop, use in loop, free after — OK");

    printf("[double free — error]\n");
    err("struct T { u32 x; }\n"
        "Pool(T, 4) pool;\n"
        "void f() {\n"
        "    Handle(T) h = pool.alloc() orelse return;\n"
        "    pool.free(h);\n"
        "    pool.free(h);\n"
        "}\n",
        "double free — error");

    printf("\n=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) printf(", %d FAILED", tests_failed);
    printf(" ===\n");

    return tests_failed > 0 ? 1 : 0;
}
