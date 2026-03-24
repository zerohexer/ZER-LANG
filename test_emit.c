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
    int gcc_ret = system("gcc -std=c99 -fwrapv -o _zer_test_out.exe _zer_test_out.c 2>_zer_gcc_err.txt");
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

    int gcc_ret = system("gcc -std=c99 -fwrapv -c -o _zer_test_out.o _zer_test_out.c 2>_zer_gcc_err.txt");
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

    printf("[defer]\n");
    test_compile_and_run(
        "u32 counter = 0;\n"
        "void increment() { counter += 1; }\n"
        "void do_work() {\n"
        "    defer increment();\n"
        "    defer increment();\n"
        "    defer increment();\n"
        "}\n"
        "u32 main() {\n"
        "    do_work();\n"
        "    return counter;\n"
        "}\n",
        3,
        "3 defers executed = counter 3");

    printf("[Pool alloc/get/free]\n");
    test_compile_and_run(
        "struct Task { u32 pid; u32 priority; }\n"
        "Pool(Task, 8) tasks;\n"
        "u32 main() {\n"
        "    Handle(Task) h = tasks.alloc() orelse return;\n"
        "    tasks.get(h).pid = 42;\n"
        "    u32 pid = tasks.get(h).pid;\n"
        "    tasks.free(h);\n"
        "    return pid;\n"
        "}\n",
        42,
        "pool alloc → set pid → get pid → free = 42");

    printf("[Ring push/pop]\n");
    test_compile_and_run(
        "Ring(u8, 256) buf;\n"
        "u32 main() {\n"
        "    buf.push(42);\n"
        "    buf.push(99);\n"
        "    u8 first = buf.pop() orelse 0;\n"
        "    return first;\n"
        "}\n",
        42,
        "ring push 42,99 → pop = 42");

    printf("[optional pointer ?*T]\n");
    test_compile_and_run(
        "struct Task { u32 pid; }\n"
        "Task global_task;\n"
        "?*Task find(bool exists) {\n"
        "    global_task.pid = 77;\n"
        "    if (exists) { return &global_task; }\n"
        "    return null;\n"
        "}\n"
        "u32 main() {\n"
        "    *Task t = find(true) orelse return;\n"
        "    return t.pid;\n"
        "}\n",
        77,
        "?*Task find → orelse return → t.pid = 77");

    test_compile_and_run(
        "struct Task { u32 pid; }\n"
        "?*Task nothing() { return null; }\n"
        "u32 main() {\n"
        "    *Task t = nothing() orelse return;\n"
        "    return t.pid;\n"
        "}\n",
        0,
        "?*Task null → orelse return exits with 0");

    printf("[intrinsic @truncate]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32 big = 0x1234;\n"
        "    u8 small = @truncate(u8, big);\n"
        "    return small;\n"
        "}\n",
        0x34,
        "@truncate(u8, 0x1234) = 0x34");

    printf("[intrinsic @barrier]\n");
    test_compile_only(
        "void f() {\n"
        "    @barrier();\n"
        "    @barrier_store();\n"
        "    @barrier_load();\n"
        "}\n",
        "barrier intrinsics compile");

    printf("[static local variables]\n");
    test_compile_and_run(
        "u32 counter() {\n"
        "    static u32 count = 0;\n"
        "    count += 1;\n"
        "    return count;\n"
        "}\n"
        "u32 main() {\n"
        "    counter();\n"
        "    counter();\n"
        "    return counter();\n"
        "}\n",
        3,
        "static local retains value across calls");

    printf("[division and modulo]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32 a = 17;\n"
        "    u32 b = a / 5;\n"
        "    u32 c = a % 5;\n"
        "    return b + c;\n"
        "}\n",
        5,
        "17/5=3, 17%%5=2, 3+2=5");

    printf("[nested struct]\n");
    test_compile_and_run(
        "struct Inner { u32 val; }\n"
        "struct Outer { Inner inner; u32 extra; }\n"
        "u32 main() {\n"
        "    Outer o;\n"
        "    o.inner.val = 10;\n"
        "    o.extra = 20;\n"
        "    return o.inner.val + o.extra;\n"
        "}\n",
        30,
        "nested struct field access = 30");

    printf("[string literal]\n");
    test_compile_only(
        "void f() {\n"
        "    const []u8 msg = \"hello\";\n"
        "}\n",
        "string literal as const []u8 compiles");

    printf("[packed struct]\n");
    test_compile_only(
        "packed struct Packet {\n"
        "    u8 id;\n"
        "    u16 value;\n"
        "    u8 checksum;\n"
        "}\n",
        "packed struct emits __attribute__((packed))");

    printf("[Ring(u32)]\n");
    test_compile_and_run(
        "Ring(u32, 16) buf;\n"
        "u32 main() {\n"
        "    buf.push(100);\n"
        "    buf.push(200);\n"
        "    u32 first = buf.pop() orelse 0;\n"
        "    return first;\n"
        "}\n",
        100,
        "Ring(u32) push/pop = 100");

    printf("[@saturate]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32 big = 1000;\n"
        "    u8 small = @saturate(u8, big);\n"
        "    return small;\n"
        "}\n",
        255,
        "@saturate(u8, 1000) = 255");

    printf("[@config]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32 val = @config(\"MAX\", 42);\n"
        "    return val;\n"
        "}\n",
        42,
        "@config default value = 42");

    printf("[orelse break/continue]\n");
    test_compile_and_run(
        "?u32 nothing() { return null; }\n"
        "?u32 something() { return 42; }\n"
        "u32 main() {\n"
        "    u32 result = 0;\n"
        "    for (u32 i = 0; i < 5; i += 1) {\n"
        "        u32 val = nothing() orelse continue;\n"
        "        result = val;\n"
        "    }\n"
        "    return result;\n"
        "}\n",
        0,
        "orelse continue skips all iterations");

    test_compile_and_run(
        "?u32 nothing() { return null; }\n"
        "u32 main() {\n"
        "    u32 result = 99;\n"
        "    while (true) {\n"
        "        u32 val = nothing() orelse break;\n"
        "        result = val;\n"
        "    }\n"
        "    return result;\n"
        "}\n",
        99,
        "orelse break exits loop");

    printf("[volatile]\n");
    test_compile_only(
        "void f() {\n"
        "    volatile u32 x = 0;\n"
        "    x = 5;\n"
        "    u32 y = x;\n"
        "}\n",
        "volatile variable compiles");

    printf("[bit extraction]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32 reg = 0xABCD;\n"
        "    u32 nibble = reg[7..4];\n"
        "    return nibble;\n"
        "}\n",
        0xC,
        "0xABCD[7..4] = 0xC");

    test_compile_and_run(
        "u32 main() {\n"
        "    u32 val = 0xFF;\n"
        "    u32 bits = val[3..0];\n"
        "    return bits;\n"
        "}\n",
        0xF,
        "0xFF[3..0] = 0xF");

    printf("[tagged union]\n");
    test_compile_only(
        "struct SensorData { u32 temperature; }\n"
        "struct Command { u32 code; }\n"
        "union Message {\n"
        "    SensorData sensor;\n"
        "    Command command;\n"
        "}\n",
        "tagged union declaration compiles");

    printf("[@bitcast]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    i32 neg = -1;\n"
        "    u32 bits = @bitcast(u32, neg);\n"
        "    if (bits > 0) { return 1; }\n"
        "    return 0;\n"
        "}\n",
        1,
        "@bitcast(u32, -1) gives large positive");

    printf("[orelse block]\n");
    test_compile_and_run(
        "u32 log_count = 0;\n"
        "void log_error() { log_count += 1; }\n"
        "?u32 nothing() { return null; }\n"
        "u32 main() {\n"
        "    nothing() orelse { log_error(); };\n"
        "    return log_count;\n"
        "}\n",
        1,
        "orelse block executes on null");

    printf("[array of structs]\n");
    test_compile_and_run(
        "struct Task { u32 pid; u32 priority; }\n"
        "u32 main() {\n"
        "    Task[4] tasks;\n"
        "    tasks[0].pid = 10;\n"
        "    tasks[1].pid = 20;\n"
        "    return tasks[0].pid + tasks[1].pid;\n"
        "}\n",
        30,
        "array of structs field access = 30");

    printf("[function returning struct]\n");
    test_compile_and_run(
        "struct Point { u32 x; u32 y; }\n"
        "Point create(u32 x, u32 y) {\n"
        "    Point p;\n"
        "    p.x = x;\n"
        "    p.y = y;\n"
        "    return p;\n"
        "}\n"
        "u32 main() {\n"
        "    Point p = create(3, 4);\n"
        "    return p.x + p.y;\n"
        "}\n",
        7,
        "function returning struct = 7");

    printf("[@ptrcast]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32 x = 42;\n"
        "    *u32 p = &x;\n"
        "    *u8 raw = @ptrcast(*u8, p);\n"
        "    *u32 back = @ptrcast(*u32, raw);\n"
        "    return *back;\n"
        "}\n",
        42,
        "@ptrcast round-trip = 42");

    printf("[u64 arithmetic]\n");
    test_compile_and_run(
        "u64 main() {\n"
        "    u64 a = 1000000;\n"
        "    u64 b = 1000000;\n"
        "    u64 c = a * b;\n"
        "    if (c > 999999) { return 1; }\n"
        "    return 0;\n"
        "}\n",
        1,
        "u64 large multiplication");

    printf("[else if chain]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32 x = 3;\n"
        "    u32 result = 0;\n"
        "    if (x == 1) { result = 10; }\n"
        "    else if (x == 2) { result = 20; }\n"
        "    else if (x == 3) { result = 30; }\n"
        "    else { result = 99; }\n"
        "    return result;\n"
        "}\n",
        30,
        "else if chain x=3 → 30");

    printf("[while true + break]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32 count = 0;\n"
        "    while (true) {\n"
        "        count += 1;\n"
        "        if (count >= 7) { break; }\n"
        "    }\n"
        "    return count;\n"
        "}\n",
        7,
        "while(true) break at 7");

    printf("[mutable pointer capture |*val|]\n");
    test_compile_and_run(
        "?u32 get_val() { return 10; }\n"
        "u32 main() {\n"
        "    ?u32 x = get_val();\n"
        "    if (x) |*v| {\n"
        "        *v = 99;\n"
        "    }\n"
        "    u32 result = x orelse 0;\n"
        "    return result;\n"
        "}\n",
        99,
        "|*val| modifies original optional value");

    printf("[multi-value switch arms]\n");
    test_compile_and_run(
        "void noop() { }\n"
        "u32 main() {\n"
        "    u32 x = 3;\n"
        "    u32 result = 0;\n"
        "    switch (x) {\n"
        "        0, 1, 2 => result = 10,\n"
        "        3, 4 => result = 20,\n"
        "        default => result = 99,\n"
        "    }\n"
        "    return result;\n"
        "}\n",
        20,
        "3,4 => 20 multi-value arm");

    printf("[@offset]\n");
    test_compile_and_run(
        "struct Data { u32 a; u32 b; u32 c; }\n"
        "usize main() {\n"
        "    usize off = @offset(Data, b);\n"
        "    return off;\n"
        "}\n",
        4,
        "@offset(Data, b) = 4 bytes");

    printf("[orelse continue in for]\n");
    test_compile_and_run(
        "?u32 maybe(u32 i) {\n"
        "    if (i == 2) { return null; }\n"
        "    return i;\n"
        "}\n"
        "u32 main() {\n"
        "    u32 sum = 0;\n"
        "    for (u32 i = 0; i < 5; i += 1) {\n"
        "        u32 val = maybe(i) orelse continue;\n"
        "        sum += val;\n"
        "    }\n"
        "    return sum;\n"
        "}\n",
        8,
        "orelse continue in for: 0+1+skip+3+4 = 8");

    printf("[union switch runtime routing]\n");
    test_compile_and_run(
        "struct SensorData { u32 temp; }\n"
        "struct Command { u32 code; }\n"
        "union Message {\n"
        "    SensorData sensor;\n"
        "    Command command;\n"
        "}\n"
        "u32 main() {\n"
        "    SensorData sd;\n"
        "    sd.temp = 42;\n"
        "    Message msg;\n"
        "    msg.sensor = sd;\n"
        "    u32 result = 0;\n"
        "    switch (msg) {\n"
        "        .sensor => |data| { result = data.temp; },\n"
        "        .command => |cmd| { result = cmd.code; },\n"
        "    }\n"
        "    return result;\n"
        "}\n",
        42,
        "union construct + switch routes to sensor = 42");

    printf("[defer inside for loop]\n");
    test_compile_and_run(
        "u32 counter = 0;\n"
        "void tick() { counter += 1; }\n"
        "u32 main() {\n"
        "    for (u32 i = 0; i < 3; i += 1) {\n"
        "        defer tick();\n"
        "    }\n"
        "    return counter;\n"
        "}\n",
        3,
        "defer in loop fires 3 times");

    printf("[pool generation counter]\n");
    test_compile_and_run(
        "struct Task { u32 pid; }\n"
        "Pool(Task, 4) pool;\n"
        "u32 main() {\n"
        "    Handle(Task) h = pool.alloc() orelse return;\n"
        "    pool.get(h).pid = 42;\n"
        "    u32 pid_before = pool.get(h).pid;\n"
        "    pool.free(h);\n"
        "    Handle(Task) h2 = pool.alloc() orelse return;\n"
        "    pool.get(h2).pid = 99;\n"
        "    u32 pid_after = pool.get(h2).pid;\n"
        "    pool.free(h2);\n"
        "    return pid_before + pid_after;\n"
        "}\n",
        141,
        "pool alloc/free/realloc: 42 + 99 = 141");

    printf("[bounds check — valid access]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32[4] arr;\n"
        "    arr[0] = 10;\n"
        "    arr[3] = 40;\n"
        "    return arr[0] + arr[3];\n"
        "}\n",
        50,
        "valid array access passes bounds check = 50");

    printf("[bounds check — out of bounds traps]\n");
    /* out-of-bounds access should abort (non-zero exit) */
    {
        tests_run++;
        Arena a; arena_init(&a, 128*1024);
        if (zer_to_c(
            "u32 main() {\n"
            "    u32[4] arr;\n"
            "    arr[10] = 99;\n"
            "    return 0;\n"
            "}\n", "_zer_test_out.c")) {
            int gcc = system("gcc -std=c99 -o _zer_test_out.exe _zer_test_out.c 2>_zer_gcc_err.txt");
            if (gcc == 0) {
                int run = system(".\\_zer_test_out.exe");
                if (run != 0) {
                    tests_passed++; /* non-zero exit = trap fired */
                } else {
                    printf("  FAIL: bounds check — out of bounds should trap\n");
                    tests_failed++;
                }
            } else {
                printf("  FAIL: bounds check — GCC compilation failed\n");
                tests_failed++;
            }
        } else {
            printf("  FAIL: bounds check — ZER compilation failed\n");
            tests_failed++;
        }
        arena_free(&a);
    }

    printf("[combo: defer + orelse continue in for]\n");
    test_compile_and_run(
        "u32 cleanup_count = 0;\n"
        "void cleanup() { cleanup_count += 1; }\n"
        "?u32 maybe(u32 i) {\n"
        "    if (i == 1) { return null; }\n"
        "    return i;\n"
        "}\n"
        "u32 main() {\n"
        "    u32 sum = 0;\n"
        "    for (u32 i = 0; i < 4; i += 1) {\n"
        "        defer cleanup();\n"
        "        u32 val = maybe(i) orelse continue;\n"
        "        sum += val;\n"
        "    }\n"
        "    return sum + cleanup_count;\n"
        "}\n",
        9,
        "defer+orelse+for: sum=0+2+3=5, cleanup=4, total=9");

    printf("[combo: nested orelse 3 levels]\n");
    test_compile_and_run(
        "?u32 fail() { return null; }\n"
        "?u32 also_fail() { return null; }\n"
        "?u32 succeed() { return 77; }\n"
        "u32 main() {\n"
        "    u32 x = fail() orelse also_fail() orelse succeed() orelse 0;\n"
        "    return x;\n"
        "}\n",
        77,
        "3-level orelse chain: fail→fail→succeed=77");

    printf("[combo: bounds check before function call]\n");
    test_compile_and_run(
        "u32 double_it(u32 x) { return x * 2; }\n"
        "u32 main() {\n"
        "    u32[4] arr;\n"
        "    arr[0] = 5;\n"
        "    arr[1] = 10;\n"
        "    u32 result = double_it(arr[0]) + double_it(arr[1]);\n"
        "    return result;\n"
        "}\n",
        30,
        "bounds check + function call: double(5)+double(10)=30");

    printf("[func ptr: local variable]\n");
    test_compile_and_run(
        "u32 add(u32 a, u32 b) { return a + b; }\n"
        "u32 main() {\n"
        "    u32 (*fn)(u32, u32) = add;\n"
        "    return fn(3, 4);\n"
        "}\n",
        7,
        "local function pointer variable: add(3,4)=7");

    printf("[func ptr: reassign]\n");
    test_compile_and_run(
        "u32 add(u32 a, u32 b) { return a + b; }\n"
        "u32 mul(u32 a, u32 b) { return a * b; }\n"
        "u32 main() {\n"
        "    u32 (*fn)(u32, u32) = add;\n"
        "    fn = mul;\n"
        "    return fn(3, 4);\n"
        "}\n",
        12,
        "reassign function pointer: mul(3,4)=12");

    printf("[func ptr: parameter]\n");
    test_compile_and_run(
        "u32 add(u32 a, u32 b) { return a + b; }\n"
        "u32 apply(u32 (*op)(u32, u32), u32 x, u32 y) {\n"
        "    return op(x, y);\n"
        "}\n"
        "u32 main() {\n"
        "    return apply(add, 10, 20);\n"
        "}\n",
        30,
        "function pointer as parameter: apply(add,10,20)=30");

    printf("[func ptr: struct field (vtable)]\n");
    test_compile_and_run(
        "u32 add(u32 a, u32 b) { return a + b; }\n"
        "u32 mul(u32 a, u32 b) { return a * b; }\n"
        "struct Ops {\n"
        "    u32 (*compute)(u32, u32);\n"
        "}\n"
        "u32 main() {\n"
        "    Ops ops;\n"
        "    ops.compute = add;\n"
        "    u32 r1 = ops.compute(5, 6);\n"
        "    ops.compute = mul;\n"
        "    u32 r2 = ops.compute(5, 6);\n"
        "    return r1 + r2;\n"
        "}\n",
        41,
        "struct function pointer field: add(5,6)+mul(5,6)=41");

    printf("[func ptr: global variable]\n");
    test_compile_and_run(
        "u32 add(u32 a, u32 b) { return a + b; }\n"
        "u32 (*g_fn)(u32, u32);\n"
        "u32 main() {\n"
        "    g_fn = add;\n"
        "    return g_fn(20, 5);\n"
        "}\n",
        25,
        "global function pointer: g_fn=add, g_fn(20,5)=25");

    printf("[func ptr: callback registration]\n");
    test_compile_and_run(
        "void (*saved_cb)(u32 val);\n"
        "u32 result = 0;\n"
        "void register_cb(void (*cb)(u32 val)) {\n"
        "    saved_cb = cb;\n"
        "}\n"
        "void my_handler(u32 val) {\n"
        "    result = val + 10;\n"
        "}\n"
        "u32 main() {\n"
        "    register_cb(my_handler);\n"
        "    saved_cb(5);\n"
        "    return result;\n"
        "}\n",
        15,
        "callback registration: handler(5) sets result=15");

    printf("[combo: @inttoptr + @ptrtoint roundtrip]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32 x = 42;\n"
        "    usize addr = @ptrtoint(&x);\n"
        "    *u32 p = @inttoptr(*u32, addr);\n"
        "    return *p;\n"
        "}\n",
        42,
        "@ptrtoint→@inttoptr roundtrip = 42");

    /* ---- BUG-025 regression: function pointer declarations ---- */
    printf("\n--- BUG-025 regression: function pointers ---\n");

    printf("[func ptr: as struct field accessed via instance]\n");
    test_compile_and_run(
        "u32 double_it(u32 x) { return x + x; }\n"
        "struct Ops { u32 (*transform)(u32); }\n"
        "u32 main() {\n"
        "    Ops ops;\n"
        "    ops.transform = double_it;\n"
        "    return ops.transform(21);\n"
        "}\n",
        42,
        "struct field func ptr: ops.transform(21)=42");

    printf("[func ptr: passed to another function and called]\n");
    test_compile_and_run(
        "u32 apply_twice(u32 (*f)(u32), u32 x) {\n"
        "    return f(f(x));\n"
        "}\n"
        "u32 inc(u32 x) { return x + 1; }\n"
        "u32 main() { return apply_twice(inc, 8); }\n",
        10,
        "func ptr param: apply_twice(inc, 8)=10");

    /* ---- Arena E2E tests ---- */
    printf("\n--- Arena E2E ---\n");

    /* Arena.over + alloc + orelse return */
    test_compile_and_run(
        "struct Task { u32 id; u32 prio; }\n"
        "u8[4096] scratch_mem;\n"
        "Arena scratch;\n"
        "u32 main() {\n"
        "    scratch = Arena.over(scratch_mem);\n"
        "    *Task t = scratch.alloc(Task) orelse return;\n"
        "    t.id = 42;\n"
        "    t.prio = 7;\n"
        "    return t.id + t.prio;\n"
        "}\n",
        49,
        "arena.alloc(Task): 42+7=49");

    /* Arena.over + alloc_slice + orelse return */
    test_compile_and_run(
        "struct Elem { u32 v; }\n"
        "u8[4096] buf;\n"
        "Arena a;\n"
        "u32 main() {\n"
        "    a = Arena.over(buf);\n"
        "    []Elem items = a.alloc_slice(Elem, 4) orelse return;\n"
        "    items[0].v = 10;\n"
        "    items[1].v = 20;\n"
        "    items[2].v = 30;\n"
        "    items[3].v = 40;\n"
        "    return items[0].v + items[3].v;\n"
        "}\n",
        50,
        "arena.alloc_slice(Elem, 4): 10+40=50");

    /* Arena reset — allocate, reset, re-allocate */
    test_compile_and_run(
        "struct Node { u32 val; }\n"
        "u8[4096] mem;\n"
        "Arena ar;\n"
        "u32 main() {\n"
        "    ar = Arena.over(mem);\n"
        "    *Node n1 = ar.alloc(Node) orelse return;\n"
        "    n1.val = 99;\n"
        "    ar.unsafe_reset();\n"
        "    *Node n2 = ar.alloc(Node) orelse return;\n"
        "    return n2.val;\n"
        "}\n",
        0,
        "arena reset + re-alloc: zeroed after reset");

    /* Arena exhaustion — alloc on tiny buffer returns null */
    test_compile_and_run(
        "struct Big { u32 a; u32 b; u32 c; u32 d; }\n"
        "u8[8] tiny;\n"
        "Arena small;\n"
        "u32 main() {\n"
        "    small = Arena.over(tiny);\n"
        "    ?*Big b = small.alloc(Big);\n"
        "    if (b) |_p| { return 1; }\n"
        "    return 42;\n"
        "}\n",
        42,
        "arena exhaustion: alloc fails on tiny buffer");

    /* Multiple allocs from same arena */
    test_compile_and_run(
        "struct Pair { u32 x; u32 y; }\n"
        "u8[4096] pool_mem;\n"
        "Arena pa;\n"
        "u32 main() {\n"
        "    pa = Arena.over(pool_mem);\n"
        "    *Pair a = pa.alloc(Pair) orelse return;\n"
        "    *Pair b = pa.alloc(Pair) orelse return;\n"
        "    a.x = 10;\n"
        "    a.y = 20;\n"
        "    b.x = 30;\n"
        "    b.y = 40;\n"
        "    return a.x + b.y;\n"
        "}\n",
        50,
        "arena multiple allocs: a.x+b.y=50");

    /* ---- ring.push_checked E2E ---- */
    printf("\n--- ring.push_checked ---\n");

    /* push_checked succeeds when not full */
    test_compile_and_run(
        "Ring(u8, 4) q;\n"
        "u32 main() {\n"
        "    q.push_checked(10) orelse return;\n"
        "    q.push_checked(20) orelse return;\n"
        "    u8 a = q.pop() orelse return;\n"
        "    return a;\n"
        "}\n",
        10,
        "ring.push_checked: success returns 10");

    /* push_checked fails when full */
    test_compile_and_run(
        "Ring(u8, 2) tiny;\n"
        "u32 main() {\n"
        "    tiny.push_checked(1) orelse return;\n"
        "    tiny.push_checked(2) orelse return;\n"
        "    ?void r = tiny.push_checked(3);\n"
        "    if (r) |_v| { return 1; }\n"
        "    return 42;\n"
        "}\n",
        42,
        "ring.push_checked: full ring returns null");

    /* ---- @container E2E ---- */
    printf("\n--- @container ---\n");

    test_compile_and_run(
        "struct Node { u32 x; u32 y; }\n"
        "u32 main() {\n"
        "    Node n;\n"
        "    n.x = 10;\n"
        "    n.y = 55;\n"
        "    *u32 yptr = &n.y;\n"
        "    *Node recovered = @container(*Node, yptr, y);\n"
        "    return recovered.x + recovered.y;\n"
        "}\n",
        65,
        "@container: recover Node from &n.y → 10+55=65");

    /* ---- Arena alignment ---- */
    printf("\n--- Arena alignment ---\n");

    /* Alloc small struct then u32 struct — u32 must be aligned */
    test_compile_and_run(
        "struct Byte { u8 b; }\n"
        "struct Word { u32 val; }\n"
        "u8[4096] amem;\n"
        "Arena aa;\n"
        "u32 main() {\n"
        "    aa = Arena.over(amem);\n"
        "    *Byte b = aa.alloc(Byte) orelse return;\n"
        "    b.b = 0xFF;\n"
        "    *Word w = aa.alloc(Word) orelse return;\n"
        "    w.val = 42;\n"
        "    return w.val;\n"
        "}\n",
        42,
        "arena alignment: Byte then Word — u32 aligned after u8");

    /* ---- Signed overflow wrapping ---- */
    printf("\n--- Signed overflow wrapping ---\n");

    test_compile_and_run(
        "u32 main() {\n"
        "    i8 x = 127;\n"
        "    x = x + 1;\n"
        "    return @truncate(u32, @bitcast(u8, x));\n"
        "}\n",
        128,
        "i8 overflow: 127+1 wraps to -128, bitcast to u8 = 128");

    /* ---- BUG-028: type_name double buffer ---- */
    /* The bug: type_name() used a single static buffer, so
     * printf("%s vs %s", type_name(a), type_name(b)) showed "u32 vs u32"
     * even for u32 vs i32. Fix: two alternating buffers.
     * Test: verify type_name returns distinct strings for distinct types. */
    printf("\n--- BUG-028: type_name double buffer ---\n");
    {
        const char *n1 = type_name(ty_u32);
        const char *n2 = type_name(ty_i32);
        tests_run++;
        if (n1 != n2 && strcmp(n1, "u32") == 0 && strcmp(n2, "i32") == 0) {
            tests_passed++;
        } else {
            printf("  FAIL: type_name dual buffer — got \"%s\" and \"%s\"\n", n1, n2);
            tests_failed++;
        }
    }

    /* ---- BUG-029: ?void bare return ---- */
    printf("\n--- BUG-029: ?void bare return ---\n");

    printf("[?void: bare return emits { 1 } not { 0, 1 }]\n");
    test_compile_and_run(
        "?void try_something(bool ok) {\n"
        "    if (!ok) { return null; }\n"
        "    return;\n"
        "}\n"
        "u32 main() {\n"
        "    if (try_something(true)) |_| { return 42; }\n"
        "    return 0;\n"
        "}\n",
        42,
        "?void function: bare return = success, return null = failure");

    printf("[?void: return null]\n");
    test_compile_and_run(
        "?void try_something(bool ok) {\n"
        "    if (!ok) { return null; }\n"
        "    return;\n"
        "}\n"
        "u32 main() {\n"
        "    if (try_something(false)) |_| { return 1; }\n"
        "    return 42;\n"
        "}\n",
        42,
        "?void function: false → null → else path = 42");

    /* ---- BUG-030: ?bool typedef ---- */
    printf("\n--- BUG-030: ?bool typedef ---\n");

    printf("[?bool: optional bool variable]\n");
    test_compile_and_run(
        "?bool check(bool v) {\n"
        "    if (v) { return true; }\n"
        "    return null;\n"
        "}\n"
        "u32 main() {\n"
        "    ?bool r = check(true);\n"
        "    if (r) |val| {\n"
        "        if (val) { return 42; }\n"
        "    }\n"
        "    return 0;\n"
        "}\n",
        42,
        "?bool function: return true → unwrap → 42");

    /* ---- BUG-031: @saturate signed ---- */
    printf("\n--- BUG-031: @saturate signed ---\n");

    printf("[@saturate(i8): clamp positive overflow]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    i32 big = 200;\n"
        "    i8 s = @saturate(i8, big);\n"
        "    u32 r = @truncate(u32, s);\n"
        "    return r;\n"
        "}\n",
        127,
        "@saturate(i8, 200) = 127");

    printf("[@saturate(u8): clamp unsigned]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32 big = 300;\n"
        "    u8 s = @saturate(u8, big);\n"
        "    u32 r = @truncate(u32, s);\n"
        "    return r;\n"
        "}\n",
        255,
        "@saturate(u8, 300) = 255");

    /* ---- BUG-032: optional var init with ident wrapping ---- */
    printf("\n--- BUG-032: optional var init NODE_IDENT wrapping ---\n");

    printf("[?u32 x = plain_u32_var]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32 val = 42;\n"
        "    ?u32 opt = val;\n"
        "    u32 result = opt orelse 0;\n"
        "    return result;\n"
        "}\n",
        42,
        "?u32 opt = val: wraps u32 into optional, unwrap = 42");

    printf("[?u32 x = optional_var (already ?T)]\n");
    test_compile_and_run(
        "?u32 get_val() { return 42; }\n"
        "u32 main() {\n"
        "    ?u32 a = get_val();\n"
        "    ?u32 b = a;\n"
        "    return b orelse 0;\n"
        "}\n",
        42,
        "?u32 b = a: already optional, assign directly");

    /* ---- BUG-033: float precision ---- */
    printf("\n--- BUG-033: float precision ---\n");

    printf("[float: precision preserved]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    f64 pi = 3.141592653589793;\n"
        "    f64 check = pi * 1000000.0;\n"
        "    u32 result = @truncate(u32, check);\n"
        "    return result - 3141550;\n"
        "}\n",
        42,
        "f64 pi preserved to enough precision: 3141592 - 3141550 = 42");

    /* ---- BUG-034: emit_type TYPE_FUNC_PTR complete ---- */
    printf("\n--- BUG-034: emit_type TYPE_FUNC_PTR ---\n");

    printf("[func ptr: as parameter type in another func ptr]\n");
    test_compile_only(
        "u32 apply(u32 (*f)(u32), u32 x) { return f(x); }\n"
        "u32 inc(u32 x) { return x + 1; }\n"
        "u32 main() { return apply(inc, 41); }\n",
        "func ptr as param compiles correctly");

    /* ================================================================
     * Bit extraction edge cases
     * ================================================================ */
    printf("\n--- Bit extraction edge cases ---\n");

    printf("[bit extract: single bit [0..0]]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32 x = 0xFF;\n"
        "    u32 b = x[0..0];\n"
        "    return b;\n"
        "}\n",
        1,
        "0xFF[0..0] = 1 (single bit)");

    printf("[bit extract: sub-range [7..0] of u32]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32 x = 0xDEADBEEF;\n"
        "    u32 lo = x[7..0];\n"
        "    return lo;\n"
        "}\n",
        239,
        "0xDEADBEEF[7..0] = 0xEF = 239");

    printf("[bit extract: mid-range [15..8]]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32 x = 0x00FF00;\n"
        "    u32 mid = x[15..8];\n"
        "    return mid;\n"
        "}\n",
        255,
        "0x00FF00[15..8] = 0xFF = 255");

    /* ================================================================
     * Defer stress tests
     * ================================================================ */
    printf("\n--- Defer stress tests ---\n");

    printf("[defer: multiple defers reverse order]\n");
    test_compile_and_run(
        "u32 g = 0;\n"
        "void set1() { g = 1; }\n"
        "void set2() { g = 2; }\n"
        "void set3() { g = 3; }\n"
        "void work() {\n"
        "    defer set1();\n"
        "    defer set2();\n"
        "    defer set3();\n"
        "}\n"
        "u32 main() {\n"
        "    work();\n"
        "    return g;\n"
        "}\n",
        1,
        "3 defers in reverse: set3, set2, set1 — g ends at 1");

    printf("[defer: in nested loop with break]\n");
    test_compile_and_run(
        "u32 counter = 0;\n"
        "void tick() { counter += 1; }\n"
        "u32 main() {\n"
        "    for (u32 i = 0; i < 3; i += 1) {\n"
        "        for (u32 j = 0; j < 5; j += 1) {\n"
        "            defer tick();\n"
        "            if (j == 2) { break; }\n"
        "        }\n"
        "    }\n"
        "    return counter;\n"
        "}\n",
        9,
        "defer in inner loop with break at j==2: 3 iters * 3 ticks = 9");

    printf("[defer: inside switch arm]\n");
    test_compile_and_run(
        "u32 g = 0;\n"
        "void bump() { g += 10; }\n"
        "void noop() { }\n"
        "u32 route(u32 x) {\n"
        "    u32 result = 0;\n"
        "    switch (x) {\n"
        "        1 => {\n"
        "            defer bump();\n"
        "            result = 1;\n"
        "        },\n"
        "        2 => {\n"
        "            defer bump();\n"
        "            result = 2;\n"
        "        },\n"
        "        default => result = 99,\n"
        "    }\n"
        "    return result + g;\n"
        "}\n"
        "u32 main() {\n"
        "    return route(1);\n"
        "}\n",
        11,
        "defer in switch arm 1: result=1 + g=10 = 11");

    /* ================================================================
     * Empty-ish struct / edge types
     * ================================================================ */
    printf("\n--- Struct and type edge cases ---\n");

    printf("[tiny struct via arena]\n");
    test_compile_and_run(
        "struct Tiny { u8 x; }\n"
        "u8[4096] mem;\n"
        "Arena ar;\n"
        "u32 main() {\n"
        "    ar = Arena.over(mem);\n"
        "    *Tiny t = ar.alloc(Tiny) orelse return;\n"
        "    t.x = 42;\n"
        "    u32 r = @truncate(u32, t.x);\n"
        "    return r;\n"
        "}\n",
        42,
        "single-field struct Tiny via arena: x = 42");

    printf("[void function with no return]\n");
    test_compile_and_run(
        "u32 g = 0;\n"
        "void side_effect() { g = 42; }\n"
        "u32 main() {\n"
        "    side_effect();\n"
        "    return g;\n"
        "}\n",
        42,
        "void fn sets global, main returns it = 42");

    /* ================================================================
     * Buffer / array edge cases
     * ================================================================ */
    printf("\n--- Buffer / array edge cases ---\n");

    printf("[array element zero init]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u8[1] buf;\n"
        "    buf[0] = 0;\n"
        "    u32 r = @truncate(u32, buf[0]);\n"
        "    return r;\n"
        "}\n",
        0,
        "u8[1] buf, set to 0, read back = 0");

    printf("[string literal passed to function]\n");
    test_compile_only(
        "void process([]u8 msg) { }\n"
        "void start() {\n"
        "    const []u8 s = \"hello\";\n"
        "    process(s);\n"
        "}\n",
        "function taking []u8 param, passing const string literal");

    /* ================================================================
     * Switch on larger enum
     * ================================================================ */
    printf("\n--- Switch on larger enum ---\n");

    printf("[enum 5 variants, switch all arms]\n");
    test_compile_and_run(
        "enum Dir { north, south, east, west, center, }\n"
        "void noop() { }\n"
        "u32 main() {\n"
        "    Dir d = Dir.west;\n"
        "    u32 result = 0;\n"
        "    switch (d) {\n"
        "        Dir.north => result = 1,\n"
        "        Dir.south => result = 2,\n"
        "        Dir.east => result = 3,\n"
        "        Dir.west => result = 4,\n"
        "        Dir.center => result = 5,\n"
        "    }\n"
        "    return result;\n"
        "}\n",
        4,
        "enum Dir with 5 variants, switch to west = 4");

    printf("[integer switch with default]\n");
    test_compile_and_run(
        "void noop() { }\n"
        "u32 main() {\n"
        "    u32 x = 42;\n"
        "    u32 result = 0;\n"
        "    switch (x) {\n"
        "        0 => result = 1,\n"
        "        1 => result = 2,\n"
        "        default => result = 99,\n"
        "    }\n"
        "    return result;\n"
        "}\n",
        99,
        "integer switch, no arm matches, default = 99");

    /* ================================================================
     * Nested type combinations
     * ================================================================ */
    printf("\n--- Nested type combinations ---\n");

    printf("[struct with ?*T field, null path]\n");
    test_compile_and_run(
        "struct Node { u32 val; ?*Node next; }\n"
        "Node a;\n"
        "u32 main() {\n"
        "    a.val = 10;\n"
        "    a.next = null;\n"
        "    if (a.next) |n| {\n"
        "        return n.val;\n"
        "    }\n"
        "    return a.val;\n"
        "}\n",
        10,
        "struct with ?*Node next=null, fallthrough returns 10");

    printf("[function returning ?*T from arena, caller unwraps]\n");
    test_compile_and_run(
        "struct Item { u32 id; }\n"
        "u8[4096] mem;\n"
        "Arena ar;\n"
        "?*Item make_item(u32 id) {\n"
        "    *Item it = ar.alloc(Item) orelse return;\n"
        "    it.id = id;\n"
        "    return it;\n"
        "}\n"
        "u32 main() {\n"
        "    ar = Arena.over(mem);\n"
        "    *Item p = make_item(77) orelse return;\n"
        "    return p.id;\n"
        "}\n",
        77,
        "fn returning ?*Item from arena, caller unwraps = 77");

    /* enum switch inside if-unwrap */
    test_compile_and_run(
        "enum Status { pending, running, done, failed }\n"
        "?Status next_status(u32 step) {\n"
        "    if (step == 0) { return Status.pending; }\n"
        "    if (step == 1) { return Status.running; }\n"
        "    if (step == 2) { return Status.done; }\n"
        "    return null;\n"
        "}\n"
        "u32 main() {\n"
        "    u32 result = 0;\n"
        "    ?Status s = next_status(2);\n"
        "    if (s) |st| {\n"
        "        switch (st) {\n"
        "            .pending => { result = 1; }\n"
        "            .running => { result = 2; }\n"
        "            .done => { result = 42; }\n"
        "            .failed => { result = 99; }\n"
        "        }\n"
        "    }\n"
        "    return result;\n"
        "}\n",
        42,
        "enum switch inside if-unwrap: ?Status done => 42");

    /* cinclude — C header inclusion */
    test_compile_and_run(
        "cinclude \"string.h\";\n"
        "u8[64] buf;\n"
        "u32 main() {\n"
        "    buf[0] = 72;\n"
        "    buf[1] = 105;\n"
        "    buf[2] = 0;\n"
        "    return buf[0];\n"
        "}\n",
        72,
        "cinclude: includes C header, uses C types, returns 72");

    /* BUG-043: ?void assign null — was emitting { 0, 0 } for 1-field struct */
    test_compile_and_run(
        "?void status;\n"
        "u32 main() {\n"
        "    status = null;\n"
        "    if (status) |_v| { return 1; }\n"
        "    return 0;\n"
        "}\n",
        0,
        "?void assign null: no excess initializer, has_value=0");

    /* BUG-044: slice auto-zero — was emitting = 0 for struct types */
    test_compile_and_run(
        "[]u8 global_slice;\n"
        "u32 main() {\n"
        "    []u8 local_slice;\n"
        "    u32 g = @truncate(u32, global_slice.len);\n"
        "    u32 l = @truncate(u32, local_slice.len);\n"
        "    return g + l;\n"
        "}\n",
        0,
        "slice auto-zero: []u8 global+local init to {0}, len=0");

    /* BUG-045: non-u8/u32 array slicing — was emitting void* ptr */
    test_compile_and_run(
        "u32[8] arr;\n"
        "u32 main() {\n"
        "    arr[0] = 10;\n"
        "    arr[1] = 20;\n"
        "    arr[2] = 30;\n"
        "    []u32 sl = arr[0..3];\n"
        "    return sl[0] + sl[1] + sl[2];\n"
        "}\n",
        60,
        "u32 array slicing: arr[0..3] → []u32, sum = 60");

    /* BUG-046: @trap() was rejected as unknown intrinsic */
    test_compile_and_run(
        "u32 main() {\n"
        "    bool should_trap = false;\n"
        "    if (should_trap) { @trap(); }\n"
        "    return 42;\n"
        "}\n",
        42,
        "@trap() compiles and is conditional: skipped = 42");

    /* BUG-047: ?void var-decl null init — was emitting = 0 for struct */
    test_compile_and_run(
        "?void x = null;\n"
        "u32 main() {\n"
        "    ?void y = null;\n"
        "    if (x) |_v| { return 1; }\n"
        "    if (y) |_v| { return 2; }\n"
        "    return 0;\n"
        "}\n",
        0,
        "?void var-decl null: global+local init to {0}");

    /* BUG-048: ?T orelse return as expression — guard was missing */
    test_compile_and_run(
        "?u32 get_val() { return null; }\n"
        "u32 main() {\n"
        "    get_val() orelse return;\n"
        "    return 1;\n"
        "}\n",
        0,
        "?u32 orelse return as expr: null fires return, exit 0");

    /* BUG-049: slice-of-slice missing .ptr */
    test_compile_and_run(
        "u32 do_sub([]u8 sl) {\n"
        "    []u8 sub = sl[1..4];\n"
        "    return @truncate(u32, sub[0]) + @truncate(u32, sub[2]);\n"
        "}\n"
        "u8[5] g_buf;\n"
        "u32 main() {\n"
        "    g_buf[0] = 10; g_buf[1] = 20; g_buf[2] = 30;\n"
        "    g_buf[3] = 40; g_buf[4] = 50;\n"
        "    []u8 sl = g_buf[0..5];\n"
        "    return do_sub(sl);\n"
        "}\n",
        60,
        "slice-of-slice: sl[1..4], sub[0]=20 + sub[2]=40 = 60");

    /* BUG-054: array→slice coercion at call site */
    test_compile_and_run(
        "u32 sum([]u8 data) {\n"
        "    return @truncate(u32, data[0]) + @truncate(u32, data[1]);\n"
        "}\n"
        "u8[4] buf;\n"
        "u32 main() {\n"
        "    buf[0] = 30; buf[1] = 12;\n"
        "    return sum(buf);\n"
        "}\n",
        42,
        "array→slice coercion at call: u8[4] → []u8 param = 42");

    /* BUG-054: array→slice coercion at var-decl */
    test_compile_and_run(
        "u8[3] arr;\n"
        "u32 main() {\n"
        "    arr[0] = 5; arr[1] = 10; arr[2] = 15;\n"
        "    []u8 sl = arr;\n"
        "    return @truncate(u32, sl[0]) + @truncate(u32, sl[1]) + @truncate(u32, sl[2]);\n"
        "}\n",
        30,
        "array→slice coercion at var-decl: u8[3] → []u8 = 30");

    /* BUG-068: enum explicit values */
    test_compile_and_run(
        "enum Prio { low = 1, med = 5, high = 10 }\n"
        "u32 main() {\n"
        "    Prio p = Prio.high;\n"
        "    u32 result = 0;\n"
        "    switch (p) {\n"
        "        .low => { result = 1; }\n"
        "        .med => { result = 5; }\n"
        "        .high => { result = 10; }\n"
        "    }\n"
        "    return result;\n"
        "}\n",
        10,
        "enum explicit values: low=1, med=5, high=10 → switch high = 10");

    /* negative enum values */
    test_compile_and_run(
        "enum Code { err = -1, ok = 0, warn = 1 }\n"
        "u32 main() {\n"
        "    Code c = Code.err;\n"
        "    u32 result = 0;\n"
        "    switch (c) {\n"
        "        .err => { result = 99; }\n"
        "        .ok => { result = 0; }\n"
        "        .warn => { result = 1; }\n"
        "    }\n"
        "    return result;\n"
        "}\n",
        99,
        "enum negative value: err=-1 → switch .err = 99");

    /* enum with gaps: auto-increment after explicit */
    test_compile_and_run(
        "enum Code { ok = 0, warn = 100, err, fatal }\n"
        "u32 main() {\n"
        "    u32 result = 0;\n"
        "    Code c = Code.fatal;\n"
        "    switch (c) {\n"
        "        .ok => { result = 0; }\n"
        "        .warn => { result = 100; }\n"
        "        .err => { result = 101; }\n"
        "        .fatal => { result = 102; }\n"
        "    }\n"
        "    return result;\n"
        "}\n",
        102,
        "enum with gaps: ok=0, warn=100, err=101, fatal=102");

    /* []Struct slice across functions — was anonymous struct mismatch */
    test_compile_and_run(
        "struct Pt { u32 x; u32 y; }\n"
        "u32 sum_pts([]Pt pts) {\n"
        "    u32 total = 0;\n"
        "    for (u32 i = 0; i < @truncate(u32, pts.len); i += 1) {\n"
        "        total += pts[i].x + pts[i].y;\n"
        "    }\n"
        "    return total;\n"
        "}\n"
        "u8[4096] mem;\n"
        "Arena ar;\n"
        "u32 main() {\n"
        "    ar = Arena.over(mem);\n"
        "    []Pt pts = ar.alloc_slice(Pt, 3) orelse return;\n"
        "    pts[0].x = 1; pts[0].y = 2;\n"
        "    pts[1].x = 3; pts[1].y = 4;\n"
        "    pts[2].x = 5; pts[2].y = 6;\n"
        "    return sum_pts(pts);\n"
        "}\n",
        21,
        "[]Struct across functions: sum_pts([]Pt) = 1+2+3+4+5+6 = 21");

    /* BUG-055: @cast wrap/unwrap distinct typedef */
    test_compile_and_run(
        "distinct typedef u32 Celsius;\n"
        "u32 main() {\n"
        "    Celsius c = @cast(Celsius, 100);\n"
        "    u32 raw = @cast(u32, c);\n"
        "    return raw;\n"
        "}\n",
        100,
        "@cast wrap u32→Celsius then unwrap Celsius→u32 = 100");

    /* ?FuncPtr — optional function pointer with null sentinel */
    test_compile_and_run(
        "u32 double_it(u32 x) { return x * 2; }\n"
        "u32 apply(?u32 (*fn)(u32), u32 val) {\n"
        "    u32 (*f)(u32) = fn orelse return;\n"
        "    return f(val);\n"
        "}\n"
        "u32 main() {\n"
        "    u32 a = apply(double_it, 10);\n"
        "    u32 b = apply(null, 10);\n"
        "    return a + b;\n"
        "}\n",
        20,
        "?FuncPtr: apply(double_it,10)=20, apply(null,10)=0, sum=20");

    /* function pointer typedef */
    test_compile_and_run(
        "typedef u32 (*BinOp)(u32, u32);\n"
        "u32 add(u32 a, u32 b) { return a + b; }\n"
        "u32 mul(u32 a, u32 b) { return a * b; }\n"
        "u32 run(BinOp op, u32 x, u32 y) {\n"
        "    return op(x, y);\n"
        "}\n"
        "u32 main() {\n"
        "    return run(add, 20, 22);\n"
        "}\n",
        42,
        "typedef BinOp: add(20,22) via typedef'd func ptr = 42");

    /* ?FuncPtr in struct field */
    test_compile_and_run(
        "struct Device {\n"
        "    u32 id;\n"
        "    ?void (*on_event)(u32);\n"
        "}\n"
        "u32 g_event = 0;\n"
        "void handler(u32 e) { g_event = e; }\n"
        "u32 main() {\n"
        "    Device d;\n"
        "    d.id = 1;\n"
        "    d.on_event = handler;\n"
        "    if (d.on_event) |cb| { cb(42); }\n"
        "    return g_event;\n"
        "}\n",
        42,
        "?FuncPtr in struct: optional callback fires, g_event=42");

    /* []Struct slice passed between functions */
    test_compile_and_run(
        "struct Item { u32 val; }\n"
        "u32 sum_items([]Item items) {\n"
        "    u32 total = 0;\n"
        "    for (u32 i = 0; i < @truncate(u32, items.len); i += 1) {\n"
        "        total += items[i].val;\n"
        "    }\n"
        "    return total;\n"
        "}\n"
        "u8[4096] mem;\n"
        "Arena ar;\n"
        "u32 main() {\n"
        "    ar = Arena.over(mem);\n"
        "    []Item items = ar.alloc_slice(Item, 3) orelse return;\n"
        "    items[0].val = 10;\n"
        "    items[1].val = 20;\n"
        "    items[2].val = 30;\n"
        "    return sum_items(items);\n"
        "}\n",
        60,
        "[]Struct across functions: alloc→fill→pass→sum = 60");

    /* BUG-073: typedef function pointer used as parameter type */
    test_compile_and_run(
        "typedef u32 (*MathOp)(u32, u32);\n"
        "u32 add(u32 a, u32 b) { return a + b; }\n"
        "u32 run(MathOp op, u32 x, u32 y) {\n"
        "    return op(x, y);\n"
        "}\n"
        "u32 main() {\n"
        "    return run(add, 20, 22);\n"
        "}\n",
        42,
        "typedef func ptr as param: MathOp run(add, 20,22) = 42");

    /* distinct typedef function pointer — checker unwraps distinct for call */
    test_compile_and_run(
        "distinct typedef u32 (*SafeOp)(u32, u32);\n"
        "u32 add(u32 a, u32 b) { return a + b; }\n"
        "u32 main() {\n"
        "    SafeOp op = @cast(SafeOp, add);\n"
        "    return op(20, 22);\n"
        "}\n",
        42,
        "distinct typedef func ptr: SafeOp @cast + call = 42");

    /* Global ?FuncPtr init null */
    test_compile_and_run(
        "?u32 (*g_transform)(u32) = null;\n"
        "u32 double_it(u32 x) { return x * 2; }\n"
        "u32 main() {\n"
        "    u32 (*fn)(u32) = g_transform orelse return;\n"
        "    return fn(5);\n"
        "}\n",
        0,
        "global ?FuncPtr = null, orelse return fires, exit 0");

    /* Global ?FuncPtr init with value */
    test_compile_and_run(
        "u32 triple(u32 x) { return x * 3; }\n"
        "?u32 (*g_op)(u32) = triple;\n"
        "u32 main() {\n"
        "    u32 (*fn)(u32) = g_op orelse return;\n"
        "    return fn(14);\n"
        "}\n",
        42,
        "global ?FuncPtr = triple, unwrap and call, 14*3 = 42");

    /* ---- BUG-078/079: Inline bounds checks ---- */
    printf("\n[BUG-078: bounds check in if condition]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32[4] arr;\n"
        "    arr[0] = 10; arr[1] = 20; arr[2] = 30; arr[3] = 40;\n"
        "    u32 i = 2;\n"
        "    if (arr[i] == 30) { return 1; }\n"
        "    return 0;\n"
        "}\n",
        1,
        "bounds check in if condition — valid access i=2");

    printf("[BUG-079: short-circuit && respects bounds]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32[4] arr;\n"
        "    arr[0] = 10;\n"
        "    u32 i = 10;\n"
        "    bool result = (i < 4) && (arr[i] == 42);\n"
        "    if (result) { return 1; }\n"
        "    return 0;\n"
        "}\n",
        0,
        "short-circuit && — i=10, left false, no trap on right");

    printf("[BUG-078: bounds check in while condition]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32[4] arr;\n"
        "    arr[0] = 1; arr[1] = 2; arr[2] = 3; arr[3] = 99;\n"
        "    u32 i = 0;\n"
        "    while (arr[i] < 50) {\n"
        "        i += 1;\n"
        "    }\n"
        "    return i;\n"
        "}\n",
        3,
        "bounds check in while — stops at arr[3]=99, i=3");

    printf("[BUG-078: bounds check in for condition]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u32[4] arr;\n"
        "    arr[0] = 5; arr[1] = 10; arr[2] = 15; arr[3] = 20;\n"
        "    u32 sum = 0;\n"
        "    for (u32 i = 0; i < 4; i += 1) {\n"
        "        sum += arr[i];\n"
        "    }\n"
        "    return sum;\n"
        "}\n",
        50,
        "bounds check in for loop body — sum 5+10+15+20=50");

    /* ---- BUG-099: hex escape in char literals ---- */
    printf("\n[BUG-099: \\x hex escape]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u8 a = '\\x0A';\n"
        "    u8 b = '\\xFF';\n"
        "    u8 c = '\\x00';\n"
        "    return @truncate(u32, a) + @truncate(u32, c);\n"
        "}\n",
        10,
        "\\x0A = 10, \\x00 = 0, sum = 10");

    /* ---- BUG-101: bare return in ?*T function ---- */
    printf("[BUG-101: bare return in ?*T → NULL]\n");
    test_compile_and_run(
        "struct Task { u32 id; }\n"
        "?*Task get_none() { return; }\n"
        "u32 main() {\n"
        "    ?*Task t = get_none();\n"
        "    if (t) |task| { return 1; }\n"
        "    return 0;\n"
        "}\n",
        0,
        "bare return in ?*T = none, if-unwrap skipped");

    /* ---- BUG-102: defer in if-unwrap scope ---- */
    printf("[BUG-102: defer fires at if-unwrap scope exit]\n");
    test_compile_and_run(
        "u32 counter = 0;\n"
        "void inc() { counter += 1; }\n"
        "?u32 maybe() { return 42; }\n"
        "?u32 nothing() { return null; }\n"
        "u32 main() {\n"
        "    if (maybe()) |val| {\n"
        "        defer inc();\n"
        "        counter += 10;\n"
        "    }\n"
        "    u32 after_if = counter;\n"
        "    if (nothing()) |val| {\n"
        "        defer inc();\n"
        "        counter += 100;\n"
        "    }\n"
        "    return after_if;\n"
        "}\n",
        11,
        "defer fires inside if-unwrap block, counter=11 before after_if");

    /* ---- BUG-104/105: TYPE_DISTINCT unwrap in optional/slice emission ---- */
    printf("\n[BUG-104: ?DistinctStruct uses named typedef]\n");
    test_compile_and_run(
        "struct Point { u32 x; u32 y; }\n"
        "distinct typedef Point Vec2;\n"
        "?Vec2 maybe_vec() { return null; }\n"
        "u32 main() {\n"
        "    ?Vec2 v = maybe_vec();\n"
        "    if (v) |val| { return 1; }\n"
        "    return 0;\n"
        "}\n",
        0,
        "?DistinctStruct: null → if-unwrap skipped → 0");

    printf("[BUG-104: ?Distinct(u32) orelse]\n");
    test_compile_and_run(
        "distinct typedef u32 Score;\n"
        "?Score get_score() { return @cast(Score, 42); }\n"
        "u32 main() {\n"
        "    Score s = get_score() orelse return;\n"
        "    return @cast(u32, s);\n"
        "}\n",
        42,
        "?Distinct orelse return: unwrap Score → 42");

    printf("[BUG-105: []DistinctType slice]\n");
    test_compile_and_run(
        "distinct typedef u32 Meters;\n"
        "u32 main() {\n"
        "    Meters[4] arr;\n"
        "    arr[0] = @cast(Meters, 10);\n"
        "    arr[1] = @cast(Meters, 20);\n"
        "    []Meters s = arr[0..2];\n"
        "    return @cast(u32, s[0]);\n"
        "}\n",
        10,
        "[]Distinct: slice of distinct u32 → 10");

    printf("[BUG-110: ?[]DistinctType]\n");
    test_compile_and_run(
        "distinct typedef u32 Score;\n"
        "Score[2] g_scores;\n"
        "?[]Score get(bool v) {\n"
        "    if (v) {\n"
        "        g_scores[0] = @cast(Score, 77);\n"
        "        g_scores[1] = @cast(Score, 88);\n"
        "        return g_scores[0..2];\n"
        "    }\n"
        "    return null;\n"
        "}\n"
        "u32 main() {\n"
        "    []Score s = get(true) orelse return;\n"
        "    return @cast(u32, s[0]);\n"
        "}\n",
        77,
        "?[]Distinct: optional slice of distinct → 77");

    /* BUG-085: non-u8/u32 slice expression */
    printf("\n[BUG-085: u16 slice expression]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u16[4] arr;\n"
        "    arr[0] = 10; arr[1] = 20; arr[2] = 30; arr[3] = 40;\n"
        "    []u16 s = arr[0..4];\n"
        "    return @truncate(u32, s[2]);\n"
        "}\n",
        30,
        "u16 slice expression with named typedef → 30");

    printf("[BUG-085: i32 slice expression]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    i32[3] arr;\n"
        "    arr[0] = 10; arr[1] = 20; arr[2] = 30;\n"
        "    []i32 s = arr[0..3];\n"
        "    return @bitcast(u32, s[1]);\n"
        "}\n",
        20,
        "i32 slice expression with named typedef → 20");

    /* BUG-132: side-effect index as lvalue */
    printf("\n[BUG-132: arr[func()] = val lvalue]\n");
    test_compile_and_run(
        "u32 counter = 0;\n"
        "u32 next() { counter += 1; return counter; }\n"
        "u32 main() {\n"
        "    u32[10] arr;\n"
        "    arr[next()] = 42;\n"
        "    return arr[1];\n"
        "}\n",
        42,
        "side-effect index lvalue: arr[func()] = 42, counter=1");

    /* BUG-121: constant folding for array sizes */
    printf("\n[BUG-121: array size expressions]\n");
    test_compile_and_run(
        "u32 main() {\n"
        "    u8[4 * 256] buf;\n"
        "    buf[1023] = 99;\n"
        "    return @truncate(u32, buf[1023]);\n"
        "}\n",
        99,
        "array size 4*256 = 1024, buf[1023] = 99");

    test_compile_and_run(
        "u32 main() {\n"
        "    u32[512 + 512] arr;\n"
        "    arr[1023] = 42;\n"
        "    return arr[1023];\n"
        "}\n",
        42,
        "array size 512+512 = 1024, arr[1023] = 42");

    /* BUG-119: bounds check single-eval for side-effecting index */
    printf("[BUG-119: single-eval bounds check]\n");
    test_compile_and_run(
        "u32 counter = 0;\n"
        "u32 next_idx() { counter += 1; return counter; }\n"
        "u32 main() {\n"
        "    u32[10] arr;\n"
        "    arr[0] = 0; arr[1] = 11; arr[2] = 22;\n"
        "    counter = 0;\n"
        "    u32 val = arr[next_idx()];\n"
        "    return counter;\n"
        "}\n",
        1,
        "func-call index evaluated once, counter=1");

    /* BUG-089: distinct func ptr + array-to-slice coercion */
    printf("[BUG-089: distinct func ptr array coerce]\n");
    test_compile_and_run(
        "u32 sum3([]u32 d) {\n"
        "    return d[0] + d[1] + d[2];\n"
        "}\n"
        "distinct typedef u32 (*Summer)([]u32);\n"
        "u32 main() {\n"
        "    Summer fn = @cast(Summer, sum3);\n"
        "    u32[3] buf;\n"
        "    buf[0] = 10; buf[1] = 20; buf[2] = 30;\n"
        "    return fn(buf);\n"
        "}\n",
        60,
        "distinct func ptr + array-to-slice: sum [10,20,30] = 60");

    /* BUG-111/112: distinct struct field access + auto-zero */
    printf("[BUG-111: distinct struct field access + pointer deref]\n");
    test_compile_and_run(
        "struct Task { u32 id; }\n"
        "distinct typedef Task Job;\n"
        "Job global_job;\n"
        "?*Job find_job() { global_job.id = 42; return &global_job; }\n"
        "u32 main() {\n"
        "    if (find_job()) |j| { return j.id; }\n"
        "    return 0;\n"
        "}\n",
        42,
        "distinct struct: field access + ptr deref + global auto-zero → 42");

    /* BUG-113: []bool named typedef */
    printf("[BUG-113: []bool slice]\n");
    test_compile_and_run(
        "u32 count_true([]bool f) {\n"
        "    u32 c = 0;\n"
        "    for (usize i = 0; i < f.len; i += 1) {\n"
        "        if (f[i]) { c += 1; }\n"
        "    }\n"
        "    return c;\n"
        "}\n"
        "u32 main() {\n"
        "    bool[4] a;\n"
        "    a[0] = true; a[1] = false; a[2] = true; a[3] = true;\n"
        "    return count_true(a[0..4]);\n"
        "}\n",
        3,
        "[]bool param + slice expression → count 3 trues");

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
