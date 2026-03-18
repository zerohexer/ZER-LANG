#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "lexer.h"
#include "ast.h"
#include "parser.h"
#include "types.h"
#include "checker.h"
#include "emitter.h"

/* ================================================================
 * ZER C Emitter Tests
 *
 * Pipeline: ZER source → parse → check → emit C → compile with GCC → run
 * ================================================================ */

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

/* compile ZER source to C, write to file */
static bool zer_to_c(const char *zer_source, const char *c_output_path) {
    Arena arena;
    arena_init(&arena, 256 * 1024);

    Scanner scanner;
    scanner_init(&scanner, zer_source);
    Parser parser;
    parser_init(&parser, &scanner, &arena, "test.zer");
    Node *file = parse_file(&parser);
    if (parser.had_error) { arena_free(&arena); return false; }

    Checker checker;
    checker_init(&checker, &arena, "test.zer");
    if (!checker_check(&checker, file)) { arena_free(&arena); return false; }

    FILE *out = fopen(c_output_path, "w");
    if (!out) { arena_free(&arena); return false; }

    Emitter emitter;
    emitter_init(&emitter, out, &arena, &checker);
    emit_file(&emitter, file);

    fclose(out);
    arena_free(&arena);
    return true;
}

/* full pipeline: ZER → C → GCC → run → check exit code */
static void test_compile_and_run(const char *zer_source, int expected_exit,
                                  const char *test_name) {
    tests_run++;

    /* step 1: ZER → C */
    if (!zer_to_c(zer_source, "_zer_test_out.c")) {
        printf("  FAIL: %s — ZER compilation failed\n", test_name);
        tests_failed++;
        return;
    }

    /* step 2: C → binary with GCC */
    int gcc_ret = system("gcc -std=c99 -o _zer_test_out.exe _zer_test_out.c 2>_zer_gcc_err.txt");
    if (gcc_ret != 0) {
        printf("  FAIL: %s — GCC compilation failed\n", test_name);
        /* print gcc errors */
        FILE *errf = fopen("_zer_gcc_err.txt", "r");
        if (errf) {
            char buf[512];
            while (fgets(buf, sizeof(buf), errf)) printf("    gcc: %s", buf);
            fclose(errf);
        }
        tests_failed++;
        return;
    }

    /* step 3: run binary */
    int run_ret = system(".\\_zer_test_out.exe");
    /* system() returns wait status, extract exit code */
    int exit_code = run_ret;
#ifdef _WIN32
    /* on Windows, system() returns the exit code directly */
#else
    exit_code = WEXITSTATUS(run_ret);
#endif

    if (exit_code != expected_exit) {
        printf("  FAIL: %s — expected exit %d, got %d\n",
               test_name, expected_exit, exit_code);
        tests_failed++;
    } else {
        tests_passed++;
    }
}

/* just check that ZER → C succeeds and GCC accepts it */
static void test_compile_only(const char *zer_source, const char *test_name) {
    tests_run++;

    if (!zer_to_c(zer_source, "_zer_test_out.c")) {
        printf("  FAIL: %s — ZER compilation failed\n", test_name);
        tests_failed++;
        return;
    }

    int gcc_ret = system("gcc -std=c99 -c -o _zer_test_out.o _zer_test_out.c 2>_zer_gcc_err.txt");
    if (gcc_ret != 0) {
        printf("  FAIL: %s — GCC compilation failed\n", test_name);
        FILE *errf = fopen("_zer_gcc_err.txt", "r");
        if (errf) {
            char buf[512];
            while (fgets(buf, sizeof(buf), errf)) printf("    gcc: %s", buf);
            fclose(errf);
        }
        tests_failed++;
    } else {
        tests_passed++;
    }
}

/* ================================================================ */

static void test_milestone_zero(void) {
    printf("[MILESTONE ZERO: u32 x = 5; return x;]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32 x = 5;\n"
        "    return x;\n"
        "}\n",
        5,
        "milestone zero — return 5");
}

static void test_basic_arithmetic(void) {
    printf("[basic arithmetic]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32 a = 3;\n"
        "    u32 b = 4;\n"
        "    u32 c = a + b;\n"
        "    return c;\n"
        "}\n",
        7,
        "3 + 4 = 7");

    test_compile_and_run(
        "u32 main() {\n"
        "    u32 x = 10;\n"
        "    u32 y = x - 3;\n"
        "    return y;\n"
        "}\n",
        7,
        "10 - 3 = 7");

    test_compile_and_run(
        "u32 main() {\n"
        "    u32 x = 6;\n"
        "    u32 y = x * 7;\n"
        "    return y;\n"
        "}\n",
        42,
        "6 * 7 = 42");
}

static void test_if_else(void) {
    printf("[if/else]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32 x = 10;\n"
        "    if (x > 5) { return 1; }\n"
        "    return 0;\n"
        "}\n",
        1,
        "if (10 > 5) returns 1");

    test_compile_and_run(
        "u32 main() {\n"
        "    u32 x = 3;\n"
        "    if (x > 5) { return 1; } else { return 2; }\n"
        "}\n",
        2,
        "if (3 > 5) else returns 2");
}

static void test_for_loop(void) {
    printf("[for loop]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32 sum = 0;\n"
        "    for (u32 i = 0; i < 10; i += 1) {\n"
        "        sum += 1;\n"
        "    }\n"
        "    return sum;\n"
        "}\n",
        10,
        "loop 10 times, sum = 10");
}

static void test_while_loop(void) {
    printf("[while loop]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32 x = 0;\n"
        "    while (x < 5) {\n"
        "        x += 1;\n"
        "    }\n"
        "    return x;\n"
        "}\n",
        5,
        "while x < 5, returns 5");
}

static void test_function_call(void) {
    printf("[function call]\n");
    test_compile_and_run(
        "u32 add(u32 a, u32 b) {\n"
        "    return a + b;\n"
        "}\n"
        "u32 main() {\n"
        "    return add(3, 4);\n"
        "}\n",
        7,
        "add(3, 4) = 7");
}

static void test_struct(void) {
    printf("[struct]\n");
    test_compile_only(
        "struct Point {\n"
        "    u32 x;\n"
        "    u32 y;\n"
        "}\n"
        "u32 main() {\n"
        "    Point p;\n"
        "    p.x = 5;\n"
        "    p.y = 10;\n"
        "    return p.x;\n"
        "}\n",
        "struct declaration and field access");
}

static void test_negative_numbers(void) {
    printf("[negative numbers]\n");
    test_compile_and_run(
        "i32 main() {\n"
        "    i32 x = -5;\n"
        "    i32 y = x + 10;\n"
        "    return y;\n"
        "}\n",
        5,
        "-5 + 10 = 5");
}

static void test_bool_ops(void) {
    printf("[boolean operations]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    bool a = true;\n"
        "    bool b = false;\n"
        "    if (a && !b) { return 1; }\n"
        "    return 0;\n"
        "}\n",
        1,
        "true && !false = true");
}

static void test_switch(void) {
    printf("[switch]\n");
    test_compile_and_run(
        "void noop() { }\n"
        "u32 main() {\n"
        "    u32 x = 2;\n"
        "    u32 result = 0;\n"
        "    switch (x) {\n"
        "        1 => result = 10,\n"
        "        2 => result = 20,\n"
        "        default => result = 99,\n"
        "    }\n"
        "    return result;\n"
        "}\n",
        20,
        "switch on 2 returns 20");
}

static void test_bitwise(void) {
    printf("[bitwise operators]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32 a = 0xFF;\n"
        "    u32 b = a & 0x0F;\n"
        "    return b;\n"
        "}\n",
        15,
        "0xFF & 0x0F = 15");

    test_compile_and_run(
        "u32 main() {\n"
        "    u32 a = 1;\n"
        "    u32 b = a << 3;\n"
        "    return b;\n"
        "}\n",
        8,
        "1 << 3 = 8");
}

static void test_compound_assignment(void) {
    printf("[compound assignment]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32 x = 10;\n"
        "    x += 5;\n"
        "    x -= 3;\n"
        "    x *= 2;\n"
        "    return x;\n"
        "}\n",
        24,
        "(10+5-3)*2 = 24");
}

static void test_nested_if(void) {
    printf("[nested if]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32 a = 10;\n"
        "    u32 b = 20;\n"
        "    if (a > 5) {\n"
        "        if (b > 15) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "    return 0;\n"
        "}\n",
        1,
        "nested if both true returns 1");
}

static void test_multiple_functions(void) {
    printf("[multiple functions]\n");
    test_compile_and_run(
        "u32 square(u32 x) {\n"
        "    return x * x;\n"
        "}\n"
        "u32 add(u32 a, u32 b) {\n"
        "    return a + b;\n"
        "}\n"
        "u32 main() {\n"
        "    return add(square(3), square(4));\n"
        "}\n",
        25,
        "3^2 + 4^2 = 25");
}

static void test_pointer(void) {
    printf("[pointers]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32 x = 42;\n"
        "    *u32 p = &x;\n"
        "    return *p;\n"
        "}\n",
        42,
        "*(&x) = 42");

    test_compile_and_run(
        "void set(*u32 p, u32 val) {\n"
        "    *p = val;\n"
        "}\n"
        "u32 main() {\n"
        "    u32 x = 0;\n"
        "    set(&x, 99);\n"
        "    return x;\n"
        "}\n",
        99,
        "set via pointer = 99");
}

static void test_array(void) {
    printf("[arrays]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32[5] arr;\n"
        "    arr[0] = 10;\n"
        "    arr[1] = 20;\n"
        "    arr[2] = 30;\n"
        "    return arr[0] + arr[1] + arr[2];\n"
        "}\n",
        60,
        "arr[0]+arr[1]+arr[2] = 60");
}

static void test_struct_field(void) {
    printf("[struct field access]\n");
    test_compile_and_run(
        "struct Point {\n"
        "    u32 x;\n"
        "    u32 y;\n"
        "}\n"
        "u32 main() {\n"
        "    Point p;\n"
        "    p.x = 10;\n"
        "    p.y = 20;\n"
        "    return p.x + p.y;\n"
        "}\n",
        30,
        "p.x + p.y = 30");
}

static void test_enum(void) {
    printf("[enum]\n");
    test_compile_only(
        "enum Color {\n"
        "    red,\n"
        "    green,\n"
        "    blue,\n"
        "}\n",
        "enum compiles to C");
}

static void test_typedef_emit(void) {
    printf("[typedef]\n");
    test_compile_and_run(
        "typedef u32 Score;\n"
        "Score add_score(Score a, Score b) {\n"
        "    return a + b;\n"
        "}\n"
        "u32 main() {\n"
        "    Score s = add_score(10, 20);\n"
        "    return s;\n"
        "}\n",
        30,
        "typedef Score = u32, add_score = 30");
}

static void test_break_continue(void) {
    printf("[break/continue]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32 sum = 0;\n"
        "    for (u32 i = 0; i < 20; i += 1) {\n"
        "        if (i >= 10) { break; }\n"
        "        if (i == 5) { continue; }\n"
        "        sum += 1;\n"
        "    }\n"
        "    return sum;\n"
        "}\n",
        9,
        "loop 0-19, break at 10, skip 5 = 9 iterations");
}

static void test_global_var(void) {
    printf("[global variables]\n");
    test_compile_and_run(
        "u32 counter = 0;\n"
        "void increment() {\n"
        "    counter += 1;\n"
        "}\n"
        "u32 main() {\n"
        "    increment();\n"
        "    increment();\n"
        "    increment();\n"
        "    return counter;\n"
        "}\n",
        3,
        "global counter incremented 3 times");
}

static void test_recursion(void) {
    printf("[recursion]\n");
    test_compile_and_run(
        "u32 factorial(u32 n) {\n"
        "    if (n <= 1) { return 1; }\n"
        "    return n * factorial(n - 1);\n"
        "}\n"
        "u32 main() {\n"
        "    return factorial(5);\n"
        "}\n",
        120,
        "factorial(5) = 120");
}

static void test_intrinsic_size(void) {
    printf("[intrinsic @size]\n");
    test_compile_and_run(
        "usize main() {\n"
        "    usize s = @size(u32);\n"
        "    return s;\n"
        "}\n",
        4,
        "@size(u32) = 4");
}

static void test_optional_type(void) {
    printf("[optional type ?T]\n");
    test_compile_and_run(
        "?u32 maybe_five() {\n"
        "    return 5;\n"
        "}\n"
        "?u32 nothing() {\n"
        "    return null;\n"
        "}\n"
        "u32 main() {\n"
        "    u32 a = maybe_five() orelse 0;\n"
        "    u32 b = nothing() orelse 99;\n"
        "    if (a == 5) { return b; }\n"
        "    return 0;\n"
        "}\n",
        99,
        "?u32 function returns value and null correctly");
}

static void test_orelse_value(void) {
    printf("[orelse with value]\n");
    test_compile_and_run(
        "?u32 maybe() { return 42; }\n"
        "?u32 nothing() { return null; }\n"
        "u32 main() {\n"
        "    u32 a = maybe() orelse 0;\n"
        "    u32 b = nothing() orelse 99;\n"
        "    return a + b;\n"
        "}\n",
        141,
        "42 orelse 0 + null orelse 99 = 141");
}

static void test_orelse_return(void) {
    printf("[orelse return]\n");
    test_compile_and_run(
        "?u32 nothing() { return null; }\n"
        "u32 try_get() {\n"
        "    u32 val = nothing() orelse return;\n"
        "    return val;\n"
        "}\n"
        "u32 main() {\n"
        "    try_get();\n"
        "    return 1;\n"
        "}\n",
        1,
        "orelse return exits function");
}

static void test_if_unwrap(void) {
    printf("[if-unwrap with capture]\n");
    test_compile_and_run(
        "?u32 maybe() { return 42; }\n"
        "?u32 nothing() { return null; }\n"
        "u32 main() {\n"
        "    ?u32 a = maybe();\n"
        "    u32 result = 0;\n"
        "    if (a) |val| {\n"
        "        result = val;\n"
        "    }\n"
        "    return result;\n"
        "}\n",
        42,
        "if (maybe) |val| captures 42");

    test_compile_and_run(
        "?u32 nothing() { return null; }\n"
        "u32 main() {\n"
        "    ?u32 b = nothing();\n"
        "    u32 result = 99;\n"
        "    if (b) |val| {\n"
        "        result = val;\n"
        "    }\n"
        "    return result;\n"
        "}\n",
        99,
        "if (null) |val| body skipped");
}

/* ================================================================ */

int main(void) {
    printf("=== ZER C Emitter Tests (End-to-End) ===\n\n");

    test_milestone_zero();
    test_basic_arithmetic();
    test_if_else();
    test_for_loop();
    test_while_loop();
    test_function_call();
    test_struct();
    test_negative_numbers();
    test_bool_ops();
    test_switch();
    test_bitwise();
    test_compound_assignment();
    test_nested_if();
    test_multiple_functions();
    test_pointer();
    test_array();
    test_struct_field();
    test_enum();
    test_typedef_emit();
    test_break_continue();
    test_global_var();
    test_recursion();
    test_intrinsic_size();
    test_optional_type();
    test_orelse_value();
    test_orelse_return();
    test_if_unwrap();

    /* cleanup temp files */
    remove("_zer_test_out.c");
    remove("_zer_test_out.exe");
    remove("_zer_test_out.o");
    remove("_zer_gcc_err.txt");

    printf("\n=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(", %d FAILED", tests_failed);
    }
    printf(" ===\n");

    return tests_failed > 0 ? 1 : 0;
}
