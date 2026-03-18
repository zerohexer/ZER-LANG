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
