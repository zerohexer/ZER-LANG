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

    /* ---- MAYBE_FREED: if/else merge — one branch frees → MAYBE_FREED ---- */
    printf("\n[if/else merge: freed on one branch only → MAYBE_FREED]\n");
    /* After MAYBE_FREED change: then-branch frees, else doesn't → h is
     * MAYBE_FREED. Use after the if is now an error. */
    err("struct T { u32 x; }\n"
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
        "one-branch free with else: use after MAYBE_FREED = error");

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

    printf("[switch merge: freed in some arms → MAYBE_FREED]\n");
    /* After MAYBE_FREED: one arm frees → MAYBE_FREED. Use after switch = error. */
    err("struct T { u32 x; }\n"
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
        "partial switch free: use after MAYBE_FREED = error");

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

    printf("[loop: conditional free in loop — now caught]\n");
    /* After MAYBE_FREED + loop second pass: conditional free inside loop
     * marks h as MAYBE_FREED. Use after loop is now caught. */
    err("struct T { u32 x; }\n"
        "Pool(T, 4) pool;\n"
        "void f(bool cond) {\n"
        "    Handle(T) h = pool.alloc() orelse return;\n"
        "    for (u32 i = 0; i < 3; i += 1) {\n"
        "        if (cond) { pool.free(h); }\n"
        "    }\n"
        "    pool.get(h).x = 1;\n"
        "}\n",
        "conditional free in loop then use after — caught via MAYBE_FREED");

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

    /* ---- Handle aliasing ---- */
    printf("\n[alias: use-after-free via alias — error]\n");
    err("struct T { u32 x; }\n"
        "Pool(T, 4) pool;\n"
        "void f() {\n"
        "    Handle(T) h1 = pool.alloc() orelse return;\n"
        "    Handle(T) alias = h1;\n"
        "    pool.free(h1);\n"
        "    pool.get(alias).x = 5;\n"
        "}\n",
        "alias: free h1, use alias — use-after-free");

    printf("[alias: use-after-free via assignment — error]\n");
    err("struct T { u32 x; }\n"
        "Pool(T, 4) pool;\n"
        "void f() {\n"
        "    Handle(T) h1 = pool.alloc() orelse return;\n"
        "    Handle(T) h2 = pool.alloc() orelse return;\n"
        "    h2 = h1;\n"
        "    pool.free(h1);\n"
        "    pool.get(h2).x = 5;\n"
        "}\n",
        "alias via assignment: free h1, use h2 — use-after-free");

    printf("[alias: valid alias use — OK]\n");
    ok("struct T { u32 x; }\n"
       "Pool(T, 4) pool;\n"
       "void f() {\n"
       "    Handle(T) h1 = pool.alloc() orelse return;\n"
       "    Handle(T) alias = h1;\n"
       "    pool.get(alias).x = 5;\n"
       "    pool.free(h1);\n"
       "}\n",
       "alias: use before free — OK");

    /* BUG-113: zercheck must check conditions */
    printf("\n[zercheck: use-after-free in if condition]\n");
    err("struct T { u32 x; }\n"
        "Pool(T, 4) pool;\n"
        "void f() {\n"
        "    Handle(T) h = pool.alloc() orelse return;\n"
        "    pool.free(h);\n"
        "    if (pool.get(h).x == 5) { }\n"
        "}\n",
        "use-after-free in if condition");

    printf("[zercheck: use-after-free in while condition]\n");
    err("struct T { u32 x; }\n"
        "Pool(T, 4) pool;\n"
        "void f() {\n"
        "    Handle(T) h = pool.alloc() orelse return;\n"
        "    pool.free(h);\n"
        "    while (pool.get(h).x < 10) { }\n"
        "}\n",
        "use-after-free in while condition");

    /* BUG-117: handle parameters tracked */
    printf("\n[handle param: use-after-free — error]\n");
    err("struct T { u32 x; }\n"
        "Pool(T, 4) pool;\n"
        "void use_handle(Handle(T) h) {\n"
        "    pool.free(h);\n"
        "    pool.get(h).x = 5;\n"
        "}\n",
        "handle param: free then use — use-after-free");

    printf("[handle param: valid use — OK]\n");
    ok("struct T { u32 x; }\n"
       "Pool(T, 4) pool;\n"
       "void use_handle(Handle(T) h) {\n"
       "    pool.get(h).x = 5;\n"
       "    pool.free(h);\n"
       "}\n",
       "handle param: use then free — OK");

    /* BUG-357: handle tracking for array elements and struct fields */
    printf("[BUG-357: array handle UAF]\n");
    err("struct T { u32 x; }\n"
        "Pool(T, 4) pool;\n"
        "void f() {\n"
        "    Handle(T)[2] arr;\n"
        "    arr[0] = pool.alloc() orelse return;\n"
        "    pool.free(arr[0]);\n"
        "    pool.get(arr[0]).x = 5;\n"
        "}\n",
        "BUG-357: array handle use-after-free");

    printf("[BUG-357: array handle double free]\n");
    err("struct T { u32 x; }\n"
        "Pool(T, 4) pool;\n"
        "void f() {\n"
        "    Handle(T)[2] arr;\n"
        "    arr[0] = pool.alloc() orelse return;\n"
        "    pool.free(arr[0]);\n"
        "    pool.free(arr[0]);\n"
        "}\n",
        "BUG-357: array handle double free");

    printf("[BUG-357: struct field handle UAF]\n");
    err("struct T { u32 x; }\n"
        "struct State { Handle(T) h; }\n"
        "Pool(T, 4) pool;\n"
        "void f() {\n"
        "    State s;\n"
        "    s.h = pool.alloc() orelse return;\n"
        "    pool.free(s.h);\n"
        "    pool.get(s.h).x = 5;\n"
        "}\n",
        "BUG-357: struct field handle use-after-free");

    printf("[BUG-357: array handle valid use]\n");
    ok("struct T { u32 x; }\n"
       "Pool(T, 4) pool;\n"
       "void f() {\n"
       "    Handle(T)[2] arr;\n"
       "    arr[0] = pool.alloc() orelse return;\n"
       "    arr[1] = pool.alloc() orelse return;\n"
       "    pool.get(arr[0]).x = 5;\n"
       "    pool.get(arr[1]).x = 10;\n"
       "    pool.free(arr[0]);\n"
       "    pool.free(arr[1]);\n"
       "}\n",
       "BUG-357: array handle valid lifecycle");

    printf("[BUG-357: different array indices independent]\n");
    ok("struct T { u32 x; }\n"
       "Pool(T, 4) pool;\n"
       "void f() {\n"
       "    Handle(T)[2] arr;\n"
       "    arr[0] = pool.alloc() orelse return;\n"
       "    arr[1] = pool.alloc() orelse return;\n"
       "    pool.free(arr[0]);\n"
       "    pool.get(arr[1]).x = 10;\n"
       "    pool.free(arr[1]);\n"
       "}\n",
       "BUG-357: arr[0] freed, arr[1] still valid");

    /* BUG-380: alias via struct field — freeing h should also mark s.h as freed */
    printf("[BUG-380: alias double-free via struct field]\n");
    err("struct T { u32 x; }\n"
        "struct State { Handle(T) h; }\n"
        "Pool(T, 4) pool;\n"
        "void f() {\n"
        "    Handle(T) h = pool.alloc() orelse return;\n"
        "    State s;\n"
        "    s.h = h;\n"
        "    pool.free(h);\n"
        "    pool.free(s.h);\n"
        "}\n",
        "BUG-380: free(h) then free(s.h) — double free via alias");

    printf("[BUG-380: alias UAF via struct field]\n");
    err("struct T { u32 x; }\n"
        "struct State { Handle(T) h; }\n"
        "Pool(T, 4) pool;\n"
        "void f() {\n"
        "    Handle(T) h = pool.alloc() orelse return;\n"
        "    State s;\n"
        "    s.h = h;\n"
        "    pool.free(h);\n"
        "    pool.get(s.h).x = 5;\n"
        "}\n",
        "BUG-380: free(h) then get(s.h) — use-after-free via alias");

    /* BUG-385: struct param with Handle field — UAF via s.h */
    printf("[BUG-385: struct param handle UAF]\n");
    err("struct T { u32 x; }\n"
        "struct State { Handle(T) h; }\n"
        "Pool(T, 4) pool;\n"
        "void f(State s) {\n"
        "    pool.free(s.h);\n"
        "    pool.get(s.h).x = 5;\n"
        "}\n",
        "BUG-385: struct param handle use-after-free");

    printf("[BUG-385: struct param handle double free]\n");
    err("struct T { u32 x; }\n"
        "struct State { Handle(T) h; }\n"
        "Pool(T, 4) pool;\n"
        "void f(State s) {\n"
        "    pool.free(s.h);\n"
        "    pool.free(s.h);\n"
        "}\n",
        "BUG-385: struct param handle double free");

    printf("[BUG-385: struct param handle valid use]\n");
    ok("struct T { u32 x; }\n"
       "struct State { Handle(T) h; }\n"
       "Pool(T, 4) pool;\n"
       "void f(State s) {\n"
       "    pool.get(s.h).x = 5;\n"
       "    pool.free(s.h);\n"
       "}\n",
       "BUG-385: struct param handle valid lifecycle");

    /* ---- MAYBE_FREED: new tests ---- */
    printf("\n[MAYBE_FREED: if-without-else, then frees → use after = error]\n");
    err("struct T { u32 x; }\n"
        "Pool(T, 4) pool;\n"
        "void f(bool cond) {\n"
        "    Handle(T) h = pool.alloc() orelse return;\n"
        "    if (cond) { pool.free(h); }\n"
        "    pool.get(h).x = 1;\n"
        "}\n",
        "if-no-else free: use after = error");

    printf("[MAYBE_FREED: if-without-else, then frees → free after = error]\n");
    err("struct T { u32 x; }\n"
        "Pool(T, 4) pool;\n"
        "void f(bool cond) {\n"
        "    Handle(T) h = pool.alloc() orelse return;\n"
        "    if (cond) { pool.free(h); }\n"
        "    pool.free(h);\n"
        "}\n",
        "if-no-else free: double free after = error");

    printf("[MAYBE_FREED: both branches free, no use after → OK]\n");
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
       "both branches free, no use after — OK (FREED, not MAYBE)");

    printf("[MAYBE_FREED: one branch frees, else doesn't, no use after → leak warning]\n");
    ok("struct T { u32 x; }\n"
       "Pool(T, 4) pool;\n"
       "void f(bool cond) {\n"
       "    Handle(T) h = pool.alloc() orelse return;\n"
       "    if (cond) {\n"
       "        pool.free(h);\n"
       "    } else {\n"
       "        pool.get(h).x = 1;\n"
       "    }\n"
       "}\n",
       "one branch free, else doesn't — warning only (maybe leaked)");

    /* ---- Leak detection tests ---- */
    printf("\n[leak: alloc without free → warning (not error)]\n");
    ok("struct T { u32 x; }\n"
       "Pool(T, 4) pool;\n"
       "void f() {\n"
       "    Handle(T) h = pool.alloc() orelse return;\n"
       "    pool.get(h).x = 5;\n"
       "}\n",
       "alloc without free — warning only, not compile error");

    printf("[leak: overwrite alive handle → error]\n");
    err("struct T { u32 x; }\n"
        "Pool(T, 4) pool;\n"
        "void f() {\n"
        "    Handle(T) h = pool.alloc() orelse return;\n"
        "    h = pool.alloc() orelse return;\n"
        "    pool.free(h);\n"
        "}\n",
        "overwrite alive handle — first handle leaked");

    printf("[leak: alloc + free → OK (no leak)]\n");
    ok("struct T { u32 x; }\n"
       "Pool(T, 4) pool;\n"
       "void f() {\n"
       "    Handle(T) h = pool.alloc() orelse return;\n"
       "    pool.get(h).x = 5;\n"
       "    pool.free(h);\n"
       "}\n",
       "alloc + use + free — clean, no leak");

    printf("[leak: param handle not freed → OK (caller responsibility)]\n");
    ok("struct T { u32 x; }\n"
       "Pool(T, 4) pool;\n"
       "void f(Handle(T) h) {\n"
       "    pool.get(h).x = 5;\n"
       "}\n",
       "param handle not freed — OK, caller's responsibility");

    /* ---- Loop second pass tests ---- */
    printf("\n[loop2: conditional free then alloc in loop → OK]\n");
    ok("struct T { u32 x; }\n"
       "Pool(T, 4) pool;\n"
       "void f() {\n"
       "    Handle(T) h = pool.alloc() orelse return;\n"
       "    for (u32 i = 0; i < 3; i += 1) {\n"
       "        pool.free(h);\n"
       "        h = pool.alloc() orelse return;\n"
       "    }\n"
       "    pool.free(h);\n"
       "}\n",
       "free-then-realloc in loop — valid cycling pattern");

    /* ---- Cross-function analysis (change 4) ---- */
    printf("\n[cross-func: wrapper frees handle → use-after-free caught]\n");
    err("struct T { u32 x; }\n"
        "Pool(T, 4) pool;\n"
        "void free_handle(Handle(T) h) {\n"
        "    pool.free(h);\n"
        "}\n"
        "void f() {\n"
        "    Handle(T) h = pool.alloc() orelse return;\n"
        "    free_handle(h);\n"
        "    pool.get(h).x = 5;\n"
        "}\n",
        "cross-func: wrapper frees → use-after-free");

    printf("[cross-func: wrapper frees handle → double free caught]\n");
    err("struct T { u32 x; }\n"
        "Pool(T, 4) pool;\n"
        "void free_handle(Handle(T) h) {\n"
        "    pool.free(h);\n"
        "}\n"
        "void f() {\n"
        "    Handle(T) h = pool.alloc() orelse return;\n"
        "    free_handle(h);\n"
        "    pool.free(h);\n"
        "}\n",
        "cross-func: wrapper frees → double free");

    printf("[cross-func: wrapper uses handle (no free) → OK]\n");
    ok("struct T { u32 x; }\n"
       "Pool(T, 4) pool;\n"
       "void use_handle(Handle(T) h) {\n"
       "    pool.get(h).x = 42;\n"
       "}\n"
       "void f() {\n"
       "    Handle(T) h = pool.alloc() orelse return;\n"
       "    use_handle(h);\n"
       "    pool.free(h);\n"
       "}\n",
       "cross-func: wrapper uses (no free) — OK");

    printf("[cross-func: conditional free wrapper → MAYBE_FREED]\n");
    err("struct T { u32 x; }\n"
        "Pool(T, 4) pool;\n"
        "void maybe_free(Handle(T) h, bool cond) {\n"
        "    if (cond) { pool.free(h); }\n"
        "}\n"
        "void f(bool c) {\n"
        "    Handle(T) h = pool.alloc() orelse return;\n"
        "    maybe_free(h, c);\n"
        "    pool.get(h).x = 5;\n"
        "}\n",
        "cross-func: conditional free → use after MAYBE_FREED");

    printf("[cross-func: non-handle param → no effect]\n");
    ok("struct T { u32 x; }\n"
       "Pool(T, 4) pool;\n"
       "void helper(u32 n) {\n"
       "    u32 x = n;\n"
       "}\n"
       "void f() {\n"
       "    Handle(T) h = pool.alloc() orelse return;\n"
       "    helper(42);\n"
       "    pool.free(h);\n"
       "}\n",
       "cross-func: non-handle param → no effect on handle");

    printf("[cross-func: free then use inside wrapper → caller OK]\n");
    ok("struct T { u32 x; }\n"
       "Pool(T, 4) pool;\n"
       "void process_and_free(Handle(T) h) {\n"
       "    pool.get(h).x = 99;\n"
       "    pool.free(h);\n"
       "}\n"
       "void f() {\n"
       "    Handle(T) h = pool.alloc() orelse return;\n"
       "    process_and_free(h);\n"
       "}\n",
       "cross-func: wrapper frees — caller clean (no leak, no UAF)");

    /* ---- Struct copy aliasing ---- */
    printf("\n[struct copy: UAF via copied struct]\n");
    err("struct T { u32 x; }\n"
        "struct State { Handle(T) h; }\n"
        "Pool(T, 4) tasks;\n"
        "void f() {\n"
        "    State s1;\n"
        "    s1.h = tasks.alloc() orelse return;\n"
        "    State s2 = s1;\n"
        "    tasks.free(s1.h);\n"
        "    tasks.get(s2.h).x = 5;\n"
        "}\n",
        "struct copy: free s1.h then use s2.h — UAF via alias");

    /* ---- Level 1: *opaque malloc/free tracking ---- */
    printf("\n[*opaque Level 1: malloc/free tracking]\n");

    err("*opaque malloc(u32 size);\n"
        "void free(*opaque ptr);\n"
        "void f() {\n"
        "    *opaque p = malloc(64);\n"
        "    free(p);\n"
        "    free(p);\n"
        "}\n",
        "*opaque double free — error");

    ok("*opaque malloc(u32 size);\n"
       "void free(*opaque ptr);\n"
       "void f() {\n"
       "    *opaque p = malloc(64);\n"
       "    free(p);\n"
       "}\n",
       "*opaque alloc + free — valid (no leak)");

    ok("*opaque malloc(u32 size);\n"
       "void free(*opaque ptr);\n"
       "void f() {\n"
       "    *opaque p = malloc(64);\n"
       "}\n",
       "*opaque leak — warning only, not compile error");

    ok("*opaque malloc(u32 size);\n"
       "void free(*opaque ptr);\n"
       "void f() {\n"
       "    *opaque p = malloc(64);\n"
       "    free(p);\n"
       "}\n",
       "*opaque alloc then free — valid");

    printf("\n=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) printf(", %d FAILED", tests_failed);
    printf(" ===\n");

    return tests_failed > 0 ? 1 : 0;
}
