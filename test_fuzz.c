#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
#include <io.h>
#define dup _dup
#define dup2 _dup2
#define fileno _fileno
#define close _close
#else
#include <unistd.h>
#endif
#include "lexer.h"
#include "ast.h"
#include "parser.h"
#include "types.h"
#include "checker.h"

static int tests_run = 0;
static int tests_survived = 0;

/* Feed source to parser (and optionally checker). Returns 1 if no crash. */
static int fuzz_one(const char *source, const char *label, int run_check) {
    tests_run++;
    Arena arena;
    arena_init(&arena, 256 * 1024);

    Scanner scanner;
    scanner_init(&scanner, source);
    Parser parser;
    parser_init(&parser, &scanner, &arena, "fuzz");
    Node *file = parse_file(&parser);

    if (run_check && file && file->kind == NODE_FILE && !parser.oom) {
        Checker checker;
        checker_init(&checker, &arena, "fuzz");
        checker_check(&checker, file);
        /* We don't care about errors, just that it didn't crash */
    }

    arena_free(&arena);
    tests_survived++;
    (void)label;
    return 1;
}

/* Helper: build a string of repeated chars */
static char *repeat_char(char c, int count) {
    char *buf = (char *)malloc((size_t)count + 1);
    memset(buf, c, (size_t)count);
    buf[count] = '\0';
    return buf;
}

/* ======== Test categories ======== */

static void test_empty_and_whitespace(void) {
    printf("[empty / whitespace / null bytes]\n");
    fuzz_one("", "empty string", 1);
    fuzz_one(" ", "single space", 1);
    fuzz_one("\t", "single tab", 1);
    fuzz_one("\n", "single newline", 1);
    fuzz_one("\r\n", "crlf", 1);
    fuzz_one("   \t\t\t   \n\n\n   ", "mixed whitespace", 1);
    fuzz_one("\0", "null byte", 0);
    fuzz_one("a\0b", "embedded null", 0);
    fuzz_one("\0\0\0\0\0", "multiple nulls", 0);
}

static void test_random_ascii_garbage(void) {
    printf("[random ASCII garbage]\n");
    fuzz_one("@#$%^&*!~`", "special chars", 1);
    fuzz_one("!@#$%^&*()_+-=[]{}|;':\",./<>?", "all special", 1);
    fuzz_one("````````", "backticks", 1);
    fuzz_one("~~~~~~~", "tildes", 1);
    fuzz_one("$$$$$$$", "dollars", 1);
    fuzz_one("###", "hashes", 1);
    fuzz_one("\\\\\\\\", "backslashes", 1);
    fuzz_one("???", "question marks", 1);
    fuzz_one("!!!", "exclamation marks", 1);
    fuzz_one("@@@", "at signs", 1);
    fuzz_one("^^^", "carets", 1);
    fuzz_one("&&&", "ampersands", 1);
    fuzz_one("|||", "pipes", 1);
    fuzz_one("<<<>>>", "angle brackets", 1);
    fuzz_one("...", "dots", 1);
    fuzz_one(",,,", "commas", 1);
    fuzz_one(";;;", "semicolons", 1);
    fuzz_one(":::", "colons", 1);
    fuzz_one("\"\"\"", "triple double-quote", 0);
    fuzz_one("'''", "triple single-quote", 0);
}

static void test_incomplete_constructs(void) {
    printf("[incomplete / truncated constructs]\n");
    fuzz_one("struct {", "struct open brace", 1);
    fuzz_one("struct Foo {", "struct name open brace", 1);
    fuzz_one("struct Foo { u32", "struct field no semi", 1);
    fuzz_one("struct Foo { u32 x", "struct field no semi 2", 1);
    fuzz_one("u32 f(", "func open paren", 1);
    fuzz_one("u32 f(u32", "func param no close", 1);
    fuzz_one("u32 f(u32 x", "func param no close 2", 1);
    fuzz_one("u32 f(u32 x)", "func no body", 1);
    fuzz_one("u32 f(u32 x) {", "func open body", 1);
    fuzz_one("u32 f(u32 x) { return", "func return no val", 1);
    fuzz_one("u32 f(u32 x) { return 1", "func return no semi", 1);
    fuzz_one("if (", "if open paren", 1);
    fuzz_one("if (true", "if no close paren", 1);
    fuzz_one("if (true)", "if no body", 1);
    fuzz_one("if (true) {", "if open body", 1);
    fuzz_one("while (", "while open paren", 1);
    fuzz_one("while (true", "while no close", 1);
    fuzz_one("while (true) {", "while open body", 1);
    fuzz_one("for (", "for open paren", 1);
    fuzz_one("for (u32 i = 0", "for init only", 1);
    fuzz_one("for (u32 i = 0;", "for init semi", 1);
    fuzz_one("for (u32 i = 0; i < 10", "for no second semi", 1);
    fuzz_one("for (u32 i = 0; i < 10;", "for second semi", 1);
    fuzz_one("for (u32 i = 0; i < 10; i = i + 1", "for no close paren", 1);
    fuzz_one("enum {", "enum open brace", 1);
    fuzz_one("enum Color {", "enum name open brace", 1);
    fuzz_one("enum Color { red", "enum member no comma", 1);
    fuzz_one("union {", "union open brace", 1);
    fuzz_one("switch (", "switch open paren", 1);
    fuzz_one("switch (x) {", "switch open body", 1);
    fuzz_one("switch (x) { 1 =>", "switch arm no body", 1);
    fuzz_one("defer", "defer alone", 1);
    fuzz_one("defer {", "defer open brace", 1);
    fuzz_one("return", "return alone", 1);
    fuzz_one("break", "break alone", 1);
    fuzz_one("continue", "continue alone", 1);
    fuzz_one("import", "import alone", 1);
    fuzz_one("import \"", "import unterminated string", 0);
    fuzz_one("const", "const alone", 1);
    fuzz_one("const u32", "const type only", 1);
    fuzz_one("*", "star alone", 1);
    fuzz_one("?", "question alone", 1);
    fuzz_one("[]", "empty brackets", 1);
    fuzz_one("[", "open bracket", 1);
}

static void test_deeply_nested(void) {
    printf("[deeply nested expressions]\n");
    char *deep_parens;
    char *buf;
    int i;

    /* Nested parenthesized expression */
    buf = (char *)malloc(2048);
    buf[0] = '\0';
    for (i = 0; i < 100; i++) strcat(buf, "(");
    strcat(buf, "1");
    for (i = 0; i < 100; i++) strcat(buf, ")");
    fuzz_one(buf, "100 nested parens", 0);
    free(buf);

    /* Deeper nesting */
    buf = (char *)malloc(4096);
    buf[0] = '\0';
    for (i = 0; i < 500; i++) strcat(buf, "(");
    strcat(buf, "1");
    for (i = 0; i < 500; i++) strcat(buf, ")");
    fuzz_one(buf, "500 nested parens", 0);
    free(buf);

    /* Unmatched deep nesting */
    deep_parens = repeat_char('(', 200);
    fuzz_one(deep_parens, "200 open parens", 0);
    free(deep_parens);

    deep_parens = repeat_char(')', 200);
    fuzz_one(deep_parens, "200 close parens", 0);
    free(deep_parens);

    /* Nested blocks */
    buf = (char *)malloc(4096);
    buf[0] = '\0';
    strcat(buf, "void f() ");
    for (i = 0; i < 100; i++) strcat(buf, "{ ");
    for (i = 0; i < 100; i++) strcat(buf, "} ");
    fuzz_one(buf, "100 nested blocks", 1);
    free(buf);

    /* Nested if-else */
    buf = (char *)malloc(8192);
    buf[0] = '\0';
    strcat(buf, "void f() { ");
    for (i = 0; i < 50; i++) strcat(buf, "if (true) { ");
    strcat(buf, "return;");
    for (i = 0; i < 50; i++) strcat(buf, " }");
    strcat(buf, " }");
    fuzz_one(buf, "50 nested ifs", 1);
    free(buf);

    /* Long chain of binary ops */
    buf = (char *)malloc(8192);
    buf[0] = '\0';
    strcat(buf, "1");
    for (i = 0; i < 200; i++) strcat(buf, " + 1");
    fuzz_one(buf, "200 chained adds", 0);
    free(buf);
}

static void test_long_identifiers(void) {
    printf("[very long identifiers]\n");
    char *long_id;
    char *buf;

    long_id = repeat_char('a', 1000);
    buf = (char *)malloc(1100);
    sprintf(buf, "u32 %s = 1;", long_id);
    fuzz_one(buf, "1000-char identifier", 1);
    free(buf);
    free(long_id);

    long_id = repeat_char('x', 5000);
    buf = (char *)malloc(5100);
    sprintf(buf, "u32 %s = 1;", long_id);
    fuzz_one(buf, "5000-char identifier", 1);
    free(buf);
    free(long_id);

    long_id = repeat_char('_', 2000);
    buf = (char *)malloc(2100);
    sprintf(buf, "u32 %s = 1;", long_id);
    fuzz_one(buf, "2000 underscores identifier", 1);
    free(buf);
    free(long_id);
}

static void test_unterminated_strings(void) {
    printf("[unterminated strings / chars]\n");
    fuzz_one("\"hello", "unterminated string", 0);
    fuzz_one("\"", "lone double quote", 0);
    fuzz_one("\"\\", "string with lone backslash", 0);
    fuzz_one("\"\\n", "string with escape no close", 0);
    fuzz_one("'", "lone single quote", 0);
    fuzz_one("'a", "char no close", 0);
    fuzz_one("'\\n", "char escape no close", 0);
    fuzz_one("'ab'", "multi-char char literal", 0);
    fuzz_one("\"hello\\\"", "string escaped quote at end", 0);
    fuzz_one("u32 x = \"unterminated;", "string in decl", 0);

    /* Very long unterminated string */
    char *long_str = (char *)malloc(2048);
    long_str[0] = '"';
    memset(long_str + 1, 'A', 2000);
    long_str[2001] = '\0';
    fuzz_one(long_str, "2000-char unterminated string", 0);
    free(long_str);
}

static void test_binary_garbage(void) {
    printf("[binary garbage bytes]\n");
    char buf[256];
    int i;

    /* All byte values 1-255 (skip 0 since it terminates the string) */
    for (i = 1; i < 256; i++) {
        buf[0] = (char)i;
        buf[1] = '\0';
        char label[32];
        sprintf(label, "byte 0x%02x", i);
        fuzz_one(buf, label, 0);
    }

    /* Sequences of high bytes */
    memset(buf, 0xFF, 100);
    buf[100] = '\0';
    fuzz_one(buf, "100 x 0xFF bytes", 0);

    memset(buf, 0x80, 100);
    buf[100] = '\0';
    fuzz_one(buf, "100 x 0x80 bytes", 0);

    /* Mixed ASCII and high bytes */
    strcpy(buf, "u32 ");
    buf[4] = (char)0x80;
    buf[5] = (char)0xFF;
    buf[6] = (char)0xFE;
    strcpy(buf + 7, " = 1;");
    fuzz_one(buf, "high bytes in identifier", 0);
}

static void test_keywords_wrong_positions(void) {
    printf("[keywords in wrong positions]\n");
    fuzz_one("return struct while for", "keyword salad", 1);
    fuzz_one("struct struct struct", "triple struct", 1);
    fuzz_one("if if if", "triple if", 1);
    fuzz_one("for while if else", "control keywords", 1);
    fuzz_one("return return return;", "triple return", 1);
    fuzz_one("break continue break;", "break continue", 1);
    fuzz_one("const const const;", "triple const", 1);
    fuzz_one("enum struct union;", "type keywords as stmt", 1);
    fuzz_one("true false null;", "literal keywords", 1);
    fuzz_one("void void void;", "triple void", 1);
    fuzz_one("u32 if = 5;", "keyword as var name", 1);
    fuzz_one("struct if { u32 while; }", "keywords as names", 1);
    fuzz_one("import return;", "import return", 1);
    fuzz_one("defer defer defer;", "triple defer", 1);
    fuzz_one("switch switch switch;", "triple switch", 1);
    fuzz_one("as as as;", "triple as", 1);
    fuzz_one("orelse orelse orelse;", "triple orelse", 1);
    fuzz_one("volatile volatile volatile;", "triple volatile", 1);
}

static void test_mismatched_braces(void) {
    printf("[mismatched braces / parens / brackets]\n");
    fuzz_one("{{{{", "open braces", 1);
    fuzz_one("}}}}", "close braces", 1);
    fuzz_one("((((", "open parens", 1);
    fuzz_one("))))", "close parens", 1);
    fuzz_one("[[[[", "open brackets", 1);
    fuzz_one("]]]]", "close brackets", 1);
    fuzz_one("([{)]}", "interleaved mismatch", 1);
    fuzz_one("{(})", "brace paren mismatch", 1);
    fuzz_one("({[({[({[", "deeply mismatched open", 1);
    fuzz_one("}])}])}])", "deeply mismatched close", 1);
    fuzz_one("{ } } {", "extra close then open", 1);
    fuzz_one("void f() { { { } }", "missing close brace", 1);
    fuzz_one("void f() { } } }", "extra close braces", 1);
    fuzz_one("u32[10 x;", "unclosed bracket in type", 1);
    fuzz_one("u32 f(u32 x { }", "unclosed paren in func", 1);
}

static void test_huge_numbers(void) {
    printf("[huge numbers]\n");
    fuzz_one("u32 x = 99999999999999999999999999;", "huge decimal", 1);
    fuzz_one("u64 x = 999999999999999999999999999999999999999999;", "massive decimal", 1);
    fuzz_one("u32 x = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFF;", "huge hex", 1);
    fuzz_one("u32 x = 0b11111111111111111111111111111111111111111111;", "huge binary", 1);
    fuzz_one("u32 x = 00000000000000000000000000;", "huge zeros", 1);
    fuzz_one("f64 x = 1.79769313486231570e+99999;", "huge float exponent", 1);
    fuzz_one("f64 x = 0.000000000000000000000000001;", "tiny float", 1);
    fuzz_one("u32 x = 0x;", "hex no digits", 1);
    fuzz_one("u32 x = 0b;", "binary no digits", 1);
    fuzz_one("u32 x = 1_000_000;", "underscores in number", 1);
}

static void test_semantically_wrong(void) {
    printf("[valid-looking but semantically wrong]\n");
    fuzz_one("u32 u32 u32;", "type as identifier", 1);
    fuzz_one("struct struct;", "struct struct", 1);
    fuzz_one("enum enum;", "enum enum", 1);
    fuzz_one("union union;", "union union", 1);
    fuzz_one("u32 = 5;", "no var name", 1);
    fuzz_one("= 5;", "assignment no lhs", 1);
    fuzz_one("5 = x;", "number as lvalue", 1);
    fuzz_one("; ; ; ; ;", "just semicolons", 1);
    fuzz_one("u32 x = = 5;", "double equals", 1);
    fuzz_one("u32 x = ;", "no initializer", 1);
    fuzz_one("void f() {} void f() {}", "duplicate func", 1);
    fuzz_one("struct Foo { struct Bar { u32 x; } }", "nested struct def", 1);
    fuzz_one("u32 f() { return; } i32 g() { return; }", "two funcs missing vals", 1);
    fuzz_one("u32 x = 5; u32 x = 10;", "duplicate global var", 1);
    fuzz_one("*****u32 x;", "five-star pointer", 1);
    fuzz_one("?????u32 x;", "five-question optional", 1);
    fuzz_one("[][]u32 x;", "double slice", 1);
    fuzz_one("u32[10][20] x;", "2D array", 1);
    fuzz_one("const const u32 x = 1;", "double const", 1);
    fuzz_one("static static u32 x = 1;", "double static", 1);
}

static void test_mixed_random(void) {
    printf("[mixed random adversarial inputs]\n");
    fuzz_one("u32 x = \"hello\" + 5;", "string plus int", 1);
    fuzz_one("struct { } + struct { }", "struct add", 1);
    fuzz_one("if (struct {}) {}", "struct in condition", 1);
    fuzz_one("for (;;) for (;;) for (;;) {}", "triple nested for", 1);
    fuzz_one("u32 x = if (true) { 1 } else { 2 };", "if as expression", 1);
    fuzz_one("return return return;", "triple return stmt", 1);
    fuzz_one("u32 x = ();", "empty parens as value", 1);
    fuzz_one("u32 x = {};", "empty braces as value", 1);
    fuzz_one("u32 x = [];", "empty brackets as value", 1);
    fuzz_one(".field", "dot field at top level", 1);
    fuzz_one("->field", "arrow at top level", 1);
    fuzz_one("x.y.z.w.v.u.t.s.r.q.p", "long field chain", 0);
    fuzz_one("f()()()()()", "chained calls", 0);
    fuzz_one("x[0][1][2][3][4]", "chained indexing", 0);
    fuzz_one("+++++", "plus chain", 0);
    fuzz_one("-----", "minus chain", 0);
    fuzz_one("*****", "star chain", 0);
    fuzz_one("&&&&&", "ampersand chain", 0);
    fuzz_one("u32 x = 1 + 2 * 3 / 4 - 5 % 6 & 7 | 8 ^ 9;", "operator soup", 1);
    fuzz_one("u32 x = !~-*&x;", "prefix chain", 1);
}

static void test_stress_repetition(void) {
    printf("[stress: repetition]\n");
    char *buf;

    /* 1000 semicolons */
    buf = repeat_char(';', 1000);
    fuzz_one(buf, "1000 semicolons", 1);
    free(buf);

    /* 500 empty functions */
    buf = (char *)malloc(50000);
    buf[0] = '\0';
    for (int i = 0; i < 500; i++) {
        char fn[64];
        sprintf(fn, "void f%d() {} ", i);
        strcat(buf, fn);
    }
    fuzz_one(buf, "500 empty functions", 1);
    free(buf);

    /* 200 variable declarations */
    buf = (char *)malloc(20000);
    buf[0] = '\0';
    for (int i = 0; i < 200; i++) {
        char decl[64];
        sprintf(decl, "u32 var%d = %d; ", i, i);
        strcat(buf, decl);
    }
    fuzz_one(buf, "200 variable decls", 1);
    free(buf);

    /* 100 struct definitions */
    buf = (char *)malloc(20000);
    buf[0] = '\0';
    for (int i = 0; i < 100; i++) {
        char decl[128];
        sprintf(decl, "struct S%d { u32 x; u32 y; } ", i);
        strcat(buf, decl);
    }
    fuzz_one(buf, "100 struct defs", 1);
    free(buf);

    /* Very long single line */
    buf = (char *)malloc(20000);
    strcpy(buf, "u32 x = 1");
    for (int i = 0; i < 1000; i++) {
        strcat(buf, " + 1");
    }
    strcat(buf, ";");
    fuzz_one(buf, "1000-term expression", 0);
    free(buf);

    /* 100 nested blocks: { { { ... } } } (kept moderate to avoid stack overflow) */
    buf = (char *)malloc(10000);
    buf[0] = '\0';
    strcat(buf, "void f() ");
    for (int i = 0; i < 100; i++) strcat(buf, "{ ");
    for (int i = 0; i < 100; i++) strcat(buf, "} ");
    fuzz_one(buf, "100 nested blocks (stress)", 1);
    free(buf);

    /* 1000 commas */
    buf = (char *)malloc(10000);
    strcpy(buf, "void f(u32 a");
    for (int i = 0; i < 100; i++) {
        char param[32];
        sprintf(param, ", u32 p%d", i);
        strcat(buf, param);
    }
    strcat(buf, ") {}");
    fuzz_one(buf, "100-param function", 1);
    free(buf);
}

static void test_edge_token_boundaries(void) {
    printf("[edge token boundaries]\n");
    fuzz_one("u32x", "type glued to ident", 1);
    fuzz_one("ifu32", "keyword glued to type", 1);
    fuzz_one("123abc", "number glued to ident", 1);
    fuzz_one("abc123", "ident with numbers", 1);
    fuzz_one("_", "lone underscore", 1);
    fuzz_one("__", "double underscore", 1);
    fuzz_one("_123", "underscore number", 1);
    fuzz_one("struct{}", "struct no space brace", 1);
    fuzz_one("if(true){}", "if no spaces", 1);
    fuzz_one("u32\tx\t=\t5\t;", "tabs as separators", 1);
    fuzz_one("u32\rx\r=\r5\r;", "CR as separators", 1);
    fuzz_one("/**/", "empty block comment", 1);
    fuzz_one("/* unterminated", "unterminated block comment", 0);
    fuzz_one("// line comment\n", "line comment", 1);
    fuzz_one("// line comment no newline", "line comment no nl", 1);
    fuzz_one("/* /* nested */ */", "nested block comment", 1);
}

static void test_orelse_and_optional_combos(void) {
    printf("[orelse / optional combos]\n");
    fuzz_one("orelse", "bare orelse", 1);
    fuzz_one("orelse {}", "orelse block", 1);
    fuzz_one("orelse orelse orelse", "triple orelse", 1);
    fuzz_one("?u32 x = null;", "optional null", 1);
    fuzz_one("?", "lone question mark", 1);
    fuzz_one("??u32 x;", "double optional", 1);
    fuzz_one("???u32 x;", "triple optional", 1);
    fuzz_one("?*?*?*u32 x;", "optional pointer chain", 1);
}

static void test_asm_and_special(void) {
    printf("[asm / volatile / interrupt / special]\n");
    fuzz_one("asm", "bare asm", 1);
    fuzz_one("asm {", "asm open brace", 1);
    fuzz_one("asm { \"nop\" }", "asm string", 1);
    fuzz_one("volatile", "bare volatile", 1);
    fuzz_one("interrupt", "bare interrupt", 1);
    fuzz_one("interrupt void handler() {}", "interrupt func", 1);
    fuzz_one("keep", "bare keep", 1);
    fuzz_one("packed struct Foo { u8 a; u8 b; }", "packed struct", 1);
    fuzz_one("distinct", "bare distinct", 1);
    fuzz_one("typedef", "bare typedef", 1);
    fuzz_one("opaque", "bare opaque", 1);
}

static void test_combinations(void) {
    printf("[adversarial combinations]\n");
    fuzz_one("struct Foo { u32 x; } void f(Foo a) { a.x = struct { }; }",
             "struct literal as assign", 1);
    fuzz_one("u32 f() { return if; }", "return keyword", 1);
    fuzz_one("u32 f() { return struct; }", "return struct keyword", 1);
    fuzz_one("u32 f() { for (;;) { for (;;) { for (;;) { break; } } } }",
             "triple nested for with break", 1);
    fuzz_one("u32 f() { defer { defer { defer { return; } } } }",
             "nested defers with return", 1);
    fuzz_one("enum Color { red = struct, }", "enum with struct value", 1);
    fuzz_one("u32 x = as u32;", "bare as cast", 1);
    fuzz_one("u32 x = 5 as;", "trailing as", 1);
    fuzz_one("u32 x = 5 as as u32;", "double as", 1);
    fuzz_one("u32 f() { switch (1) { 1 => { } 2 => { } default => { } 3 => { } } }",
             "case after default", 1);
    fuzz_one("Pool(u32, 0) p;", "zero-size pool", 1);
    fuzz_one("Ring(u8, 0) r;", "zero-size ring", 1);
    fuzz_one("Handle(u32) h;", "handle type", 1);
    fuzz_one("Arena(1024) a;", "arena type", 1);
}

/* ======== Main ======== */

int main(void) {
    printf("=== ZER Parser Fuzz Test ===\n\n");

    /* Redirect stderr to suppress parser/checker error messages during fuzzing */
    int saved_fd = -1;
    FILE *devnull = fopen("NUL", "w");
    if (!devnull) devnull = fopen("/dev/null", "w");
    if (devnull) {
        fflush(stderr);
        saved_fd = dup(fileno(stderr));
        dup2(fileno(devnull), fileno(stderr));
        fclose(devnull);
    }

    test_empty_and_whitespace();
    test_random_ascii_garbage();
    test_incomplete_constructs();
    test_deeply_nested();
    test_long_identifiers();
    test_unterminated_strings();
    test_binary_garbage();
    test_keywords_wrong_positions();
    test_mismatched_braces();
    test_huge_numbers();
    test_semantically_wrong();
    test_mixed_random();
    test_stress_repetition();
    test_edge_token_boundaries();
    test_orelse_and_optional_combos();
    test_asm_and_special();
    test_combinations();

    /* Restore stderr */
    if (saved_fd >= 0) {
        fflush(stderr);
        dup2(saved_fd, fileno(stderr));
        close(saved_fd);
    }

    printf("\n=== Fuzz Summary ===\n");
    printf("  Tests run:      %d\n", tests_run);
    printf("  Survived:       %d\n", tests_survived);
    printf("  Crashed:        %d\n", tests_run - tests_survived);

    if (tests_survived == tests_run) {
        printf("\n  ALL %d fuzz inputs handled without crashing.\n", tests_run);
        return 0;
    } else {
        printf("\n  SOME INPUTS CAUSED CRASHES!\n");
        return 1;
    }
}
