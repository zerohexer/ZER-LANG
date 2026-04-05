#include <stdio.h>
#include <string.h>
#include "lexer.h"
#include "ast.h"
#include "parser.h"
#include "types.h"
#include "checker.h"

static int pass = 0, fail = 0, total = 0;

static bool run_check(const char *source, Arena *arena) {
    Scanner s; scanner_init(&s, source);
    Parser p; parser_init(&p, &s, arena, "test");
    Node *f = parse_file(&p);
    if (p.had_error) return false;
    Checker c; checker_init(&c, arena, "test");
    return checker_check(&c, f);
}

static void ok(const char *src, const char *name) {
    Arena a; arena_init(&a, 128*1024); total++;
    if (run_check(src, &a)) { pass++; } else { printf("  FAIL(ok): %s\n", name); fail++; }
    arena_free(&a);
}
static void err(const char *src, const char *name) {
    Arena a; arena_init(&a, 128*1024); total++;
    if (!run_check(src, &a)) { pass++; } else { printf("  FAIL(err): %s\n", name); fail++; }
    arena_free(&a);
}

int main(void) {
    printf("=== Extra Edge Case Tests ===\n\n");

    /* negative integer literal */
    ok("void f() { i32 a = -5; }", "i32 a = -5 (negative literal)");
    ok("void f() { i8 a = -1; }", "i8 a = -1");

    /* const capture immutability */
    err("void f() { ?u32 m = 5; if (m) |val| { val = 10; } }",
        "assign to const capture rejected");

    /* nested field on pointer */
    ok("struct Inner { u32 x; }\n"
       "struct Outer { Inner inner; }\n"
       "void f(Outer o) { u32 v = o.inner.x; }",
       "nested struct field access");

    /* chained orelse */
    ok("?u32 a() { return null; }\n"
       "?u32 b() { return null; }\n"
       "void f() { u32 x = a() orelse b() orelse 0; }",
       "chained orelse");

    /* array to slice coercion in function arg */
    ok("void process([]u8 data) { }\n"
       "void f() { u8[64] buf; process(buf); }",
       "array → slice coercion in call");

    /* optional pointer: ?*T with capture */
    ok("struct Task { u32 pid; }\n"
       "void f() {\n"
       "    ?*Task maybe = null;\n"
       "    if (maybe) |t| { t.pid = 5; }\n"
       "}",
       "?*Task if-unwrap capture gives *Task");

    /* switch with bool */
    ok("void a() {} void b() {}\n"
       "void f() { switch (true) { true => a(), false => b(), } }",
       "bool switch exhaustive");

    /* string literal is const []u8 */
    ok("void f() { const []u8 msg = \"hello\"; }",
       "string literal as const []u8");

    /* multiple return paths */
    ok("u32 f(bool cond) { if (cond) { return 1; } return 0; }",
       "multiple return paths");

    /* void function with no return */
    ok("void f() { u32 x = 5; }",
       "void function without return");

    /* compound assignment on non-numeric */
    err("void f() { bool b = true; b += true; }",
        "bool += rejected");

    /* orelse on non-optional */
    err("void f() { u32 x = 5; u32 y = x orelse 0; }",
        "orelse on u32 rejected");

    /* address of non-lvalue (literal) — parser might not support this */
    /* skip — parser creates different AST */

    /* deref non-pointer */
    err("void f() { u32 x = 5; u32 y = *x; }",
        "dereference non-pointer rejected");

    /* bitwise NOT on bool */
    err("void f() { bool b = true; bool c = ~b; }",
        "~bool rejected");

    /* logical NOT on integer — BUG-426: now allowed (common C idiom) */
    ok("void f() { u32 x = 5; bool b = !x; }",
        "!u32 OK");

    /* index non-indexable */
    err("void f() { u32 x = 5; u32 y = x[0]; }",
        "index u32 rejected");

    /* slice non-sliceable */
    err("void f() { u32 x = 5; []u32 s = x[0..1]; }",
        "slice u32 rejected");

    printf("\n=== Results: %d/%d passed", pass, total);
    if (fail > 0) printf(", %d FAILED", fail);
    printf(" ===\n");
    return fail > 0 ? 1 : 0;
}
