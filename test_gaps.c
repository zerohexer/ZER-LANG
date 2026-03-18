#include <stdio.h>
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
    if (run_check(src, &a)) { pass++; }
    else { printf("  FAIL(ok): %s\n", name); fail++; }
    arena_free(&a);
}

static void err(const char *src, const char *name) {
    Arena a; arena_init(&a, 128*1024); total++;
    if (!run_check(src, &a)) { pass++; }
    else { printf("  FAIL(err): %s\n", name); fail++; }
    arena_free(&a);
}

int main(void) {
    printf("=== Gap Verification Tests ===\n\n");

    /* Gap 3: null as function argument to ?T param */
    printf("[Gap 3: null as function argument]\n");
    ok("void take(?u32 x) { }\nvoid f() { take(null); }",
       "null passed to ?u32 param");
    ok("void take(?bool x) { }\nvoid f() { take(null); }",
       "null passed to ?bool param");
    ok("void take(?u32 x) { }\nvoid f() { take(5); }",
       "u32 literal passed to ?u32 param (T→?T coerce)");

    /* Gap 4: orelse return in wrong context */
    printf("[Gap 4: orelse return context]\n");
    ok("?u32 read() { return null; }\n"
       "u32 f() { u32 x = read() orelse return; return x; }",
       "orelse return in u32 function (valid — bare return exits)");

    printf("\n=== Results: %d/%d passed", pass, total);
    if (fail > 0) printf(", %d FAILED", fail);
    printf(" ===\n");
    return fail > 0 ? 1 : 0;
}
