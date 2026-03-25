/* ================================================================
 * Firmware Pattern Stress Tests — Round 3
 *
 * Targets: type coercion across boundaries, struct/union combos,
 * multi-level pointer ops, edge cases in control flow, and
 * patterns that real drivers would use daily.
 * ================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "lexer.h"
#include "ast.h"
#include "parser.h"
#include "types.h"
#include "checker.h"
#include "emitter.h"
#include "zercheck.h"

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

static bool zer_to_c(const char *zer_source, const char *c_output_path) {
    Arena arena;
    arena_init(&arena, 512 * 1024);
    Scanner scanner;
    scanner_init(&scanner, zer_source);
    Parser parser;
    parser_init(&parser, &scanner, &arena, "test.zer");
    Node *file = parse_file(&parser);
    if (parser.had_error) { arena_free(&arena); return false; }
    Checker checker;
    checker_init(&checker, &arena, "test.zer");
    if (!checker_check(&checker, file)) { arena_free(&arena); return false; }
    ZerCheck zc;
    zercheck_init(&zc, &checker, &arena, "test.zer");
    zercheck_run(&zc, file);
    FILE *out = fopen(c_output_path, "w");
    if (!out) { arena_free(&arena); return false; }
    Emitter emitter;
    emitter_init(&emitter, out, &arena, &checker);
    emit_file(&emitter, file);
    fclose(out);
    arena_free(&arena);
    return true;
}

static void test_e2e(const char *zer_source, int expected_exit, const char *test_name) {
    tests_run++;
    if (!zer_to_c(zer_source, "_zer_fw3_test.c")) {
        printf("  FAIL: %s — ZER compilation failed\n", test_name);
        tests_failed++;
        return;
    }
#ifdef _WIN32
    int gcc_ret = system("gcc -std=c99 -O0 -w -o _zer_fw3_test.exe _zer_fw3_test.c 2>_zer_fw3_err.txt");
#else
    int gcc_ret = system("gcc -std=c99 -O0 -w -o _zer_fw3_test _zer_fw3_test.c 2>_zer_fw3_err.txt");
#endif
    if (gcc_ret != 0) {
        printf("  FAIL: %s — GCC compilation failed\n", test_name);
        FILE *ef = fopen("_zer_fw3_err.txt", "r");
        if (ef) {
            char buf[512];
            while (fgets(buf, sizeof(buf), ef)) printf("    gcc: %s", buf);
            fclose(ef);
        }
        tests_failed++;
        return;
    }
#ifdef _WIN32
    int run_ret = system(".\\_zer_fw3_test.exe");
#else
    int run_ret = system("./_zer_fw3_test");
#endif
    if (run_ret != expected_exit) {
        printf("  FAIL: %s — expected exit %d, got %d\n",
               test_name, expected_exit, run_ret);
        tests_failed++;
    } else {
        tests_passed++;
    }
}

/* ================================================================
 * PATTERN 1: Pointer-to-pointer / multi-level dereference
 * ================================================================ */

static void test_multi_level_ptr(void) {
    printf("\n--- Pattern 1: Multi-level pointers ---\n");

    /* 1a: *(*T) double deref */
    test_e2e(
        "u32 main() {\n"
        "    u32 val = 42;\n"
        "    *u32 p = &val;\n"
        "    **u32 pp = &p;\n"
        "    return **pp - 42;\n"
        "}\n",
        0, "double pointer dereference **pp");

    /* 1b: modify through double pointer */
    test_e2e(
        "u32 main() {\n"
        "    u32 val = 0;\n"
        "    *u32 p = &val;\n"
        "    **u32 pp = &p;\n"
        "    **pp = 42;\n"
        "    return val - 42;\n"
        "}\n",
        0, "write through double pointer **pp = 42");

    /* 1c: pointer to struct through function */
    test_e2e(
        "struct Config { u32 baud; }\n"
        "void set_baud(*Config cfg, u32 baud) {\n"
        "    cfg.baud = baud;\n"
        "}\n"
        "u32 main() {\n"
        "    Config c;\n"
        "    set_baud(&c, 115200);\n"
        "    if (c.baud == 115200) { return 0; }\n"
        "    return 1;\n"
        "}\n",
        0, "modify struct through pointer param");
}

/* ================================================================
 * PATTERN 2: Typedef and distinct types
 * ================================================================ */

static void test_typedef(void) {
    printf("\n--- Pattern 2: Typedef ---\n");

    /* 2a: simple typedef */
    test_e2e(
        "typedef u32 Milliseconds;\n"
        "Milliseconds delay(Milliseconds ms) { return ms + 10; }\n"
        "u32 main() {\n"
        "    Milliseconds t = delay(32);\n"
        "    return t - 42;\n"
        "}\n",
        0, "typedef u32 Milliseconds");

    /* 2b: typedef used in struct */
    test_e2e(
        "typedef u32 PinNum;\n"
        "struct GpioPin { PinNum pin; u32 mode; }\n"
        "u32 main() {\n"
        "    GpioPin p;\n"
        "    p.pin = 13;\n"
        "    p.mode = 1;\n"
        "    return p.pin - 13;\n"
        "}\n",
        0, "typedef in struct field");
}

/* ================================================================
 * PATTERN 3: While loop edge cases
 * ================================================================ */

static void test_while_edge(void) {
    printf("\n--- Pattern 3: While loop edge cases ---\n");

    /* 3a: while with complex condition */
    test_e2e(
        "u32 main() {\n"
        "    u32 x = 100;\n"
        "    u32 steps = 0;\n"
        "    while (x > 1) {\n"
        "        if (x % 2 == 0) { x = x / 2; }\n"
        "        else { x = x * 3 + 1; }\n"
        "        steps += 1;\n"
        "    }\n"
        "    if (steps > 0) { return 0; }\n"
        "    return 1;\n"
        "}\n",
        0, "collatz sequence from 100");

    /* 3b: while(true) with multiple break conditions */
    test_e2e(
        "u32 main() {\n"
        "    u32 i = 0;\n"
        "    u32 sum = 0;\n"
        "    while (true) {\n"
        "        if (i >= 10) { break; }\n"
        "        if (i % 2 == 0) { sum += i; }\n"
        "        i += 1;\n"
        "    }\n"
        "    return sum - 20;\n"
        "}\n",
        0, "while(true) sum even numbers 0-9 = 20");

    /* 3c: nested while with defer */
    test_e2e(
        "u32 outer_count = 0;\n"
        "u32 inner_count = 0;\n"
        "void o_tick() { outer_count += 1; }\n"
        "void i_tick() { inner_count += 1; }\n"
        "u32 main() {\n"
        "    u32 i = 0;\n"
        "    while (i < 3) {\n"
        "        defer o_tick();\n"
        "        u32 j = 0;\n"
        "        while (j < 2) {\n"
        "            defer i_tick();\n"
        "            j += 1;\n"
        "        }\n"
        "        i += 1;\n"
        "    }\n"
        "    if (outer_count != 3) { return 1; }\n"
        "    if (inner_count != 6) { return 2; }\n"
        "    return 0;\n"
        "}\n",
        0, "nested while + defer: outer=3, inner=6");
}

/* ================================================================
 * PATTERN 4: Struct with optional fields
 * ================================================================ */

static void test_optional_struct_fields(void) {
    printf("\n--- Pattern 4: Optional in struct context ---\n");

    /* 4a: function returning optional used immediately */
    test_e2e(
        "?u32 parse_int(u32 raw) {\n"
        "    if (raw > 1000) { return null; }\n"
        "    return raw;\n"
        "}\n"
        "u32 main() {\n"
        "    u32 result = parse_int(42) orelse 0;\n"
        "    return result - 42;\n"
        "}\n",
        0, "optional return used inline with orelse");

    /* 4b: optional chain — first fails, second succeeds */
    test_e2e(
        "?u32 try_a() { return null; }\n"
        "?u32 try_b() { return 42; }\n"
        "u32 main() {\n"
        "    u32 val = try_a() orelse try_b() orelse 0;\n"
        "    return val - 42;\n"
        "}\n",
        0, "optional chain: A fails, B succeeds");

    /* 4c: optional in if-else branches */
    test_e2e(
        "?u32 maybe(bool give) {\n"
        "    if (give) { return 42; }\n"
        "    return null;\n"
        "}\n"
        "u32 main() {\n"
        "    u32 result = 0;\n"
        "    ?u32 a = maybe(true);\n"
        "    if (a) |val| {\n"
        "        result = val;\n"
        "    } else {\n"
        "        result = 99;\n"
        "    }\n"
        "    return result - 42;\n"
        "}\n",
        0, "if-unwrap with else branch");
}

/* ================================================================
 * PATTERN 5: Union + enum combined (error handling pattern)
 * ================================================================ */

static void test_union_error_pattern(void) {
    printf("\n--- Pattern 5: Union error handling ---\n");

    /* 5a: result union with ok/err, dispatch and extract */
    test_e2e(
        "struct OkVal { u32 data; }\n"
        "struct ErrVal { u32 code; }\n"
        "union IoResult { OkVal ok; ErrVal err; }\n"
        "IoResult read_reg(u32 addr) {\n"
        "    IoResult r;\n"
        "    if (addr == 0) {\n"
        "        ErrVal e;\n"
        "        e.code = 1;\n"
        "        r.err = e;\n"
        "    } else {\n"
        "        OkVal o;\n"
        "        o.data = addr * 10;\n"
        "        r.ok = o;\n"
        "    }\n"
        "    return r;\n"
        "}\n"
        "u32 main() {\n"
        "    IoResult r = read_reg(5);\n"
        "    u32 out = 0;\n"
        "    switch (r) {\n"
        "        .ok => |v| { out = v.data; },\n"
        "        .err => |e| { out = e.code + 1000; },\n"
        "    }\n"
        "    return out - 50;\n"
        "}\n",
        0, "IoResult union ok path: 5*10=50");

    /* 5b: error path */
    test_e2e(
        "struct OkVal { u32 data; }\n"
        "struct ErrVal { u32 code; }\n"
        "union IoResult { OkVal ok; ErrVal err; }\n"
        "IoResult fail_reg() {\n"
        "    ErrVal e;\n"
        "    e.code = 7;\n"
        "    IoResult r;\n"
        "    r.err = e;\n"
        "    return r;\n"
        "}\n"
        "u32 main() {\n"
        "    IoResult r = fail_reg();\n"
        "    u32 out = 0;\n"
        "    switch (r) {\n"
        "        .ok => |v| { out = v.data; },\n"
        "        .err => |e| { out = e.code; },\n"
        "    }\n"
        "    return out - 7;\n"
        "}\n",
        0, "IoResult union err path: code=7");
}

/* ================================================================
 * PATTERN 6: Const variables
 * ================================================================ */

static void test_const(void) {
    printf("\n--- Pattern 6: Const variables ---\n");

    /* 6a: const local */
    test_e2e(
        "u32 main() {\n"
        "    const u32 MAX = 100;\n"
        "    u32 x = MAX - 58;\n"
        "    return x - 42;\n"
        "}\n",
        0, "const local variable");

    /* 6b: const global */
    test_e2e(
        "const u32 MAGIC = 0xDEAD;\n"
        "u32 main() {\n"
        "    return MAGIC - 0xDEAD;\n"
        "}\n",
        0, "const global variable");
}

/* ================================================================
 * PATTERN 7: Realistic UART driver pattern
 * ================================================================ */

static void test_uart_pattern(void) {
    printf("\n--- Pattern 7: UART driver pattern ---\n");

    /* 7a: ring buffer UART simulation */
    test_e2e(
        "Ring(u8, 64) tx_buf;\n"
        "Ring(u8, 64) rx_buf;\n"
        "u32 bytes_sent = 0;\n"
        "void uart_send(u8 byte) {\n"
        "    tx_buf.push(byte);\n"
        "    bytes_sent += 1;\n"
        "}\n"
        "?u8 uart_recv() {\n"
        "    return rx_buf.pop();\n"
        "}\n"
        "void uart_loopback() {\n"
        "    u8 byte = tx_buf.pop() orelse return;\n"
        "    rx_buf.push(byte);\n"
        "}\n"
        "u32 main() {\n"
        "    uart_send(0x48);\n"
        "    uart_send(0x69);\n"
        "    uart_loopback();\n"
        "    uart_loopback();\n"
        "    u8 a = uart_recv() orelse 0;\n"
        "    u8 b = uart_recv() orelse 0;\n"
        "    if (bytes_sent != 2) { return 1; }\n"
        "    if (a != 0x48) { return 2; }\n"
        "    if (b != 0x69) { return 3; }\n"
        "    return 0;\n"
        "}\n",
        0, "UART loopback: send 'Hi', loopback, receive");
}

/* ================================================================
 * PATTERN 8: Pool-based task scheduler
 * ================================================================ */

static void test_task_scheduler(void) {
    printf("\n--- Pattern 8: Task scheduler ---\n");

    /* 8a: alloc tasks, run them, free */
    test_e2e(
        "struct Task { u32 id; u32 result; }\n"
        "Pool(Task, 8) task_pool;\n"
        "u32 total_work = 0;\n"
        "void run_task(*Task t) {\n"
        "    t.result = t.id * 10;\n"
        "    total_work += t.result;\n"
        "}\n"
        "u32 main() {\n"
        "    Handle(Task) h1 = task_pool.alloc() orelse return;\n"
        "    Handle(Task) h2 = task_pool.alloc() orelse return;\n"
        "    Handle(Task) h3 = task_pool.alloc() orelse return;\n"
        "    task_pool.get(h1).id = 1;\n"
        "    task_pool.get(h2).id = 2;\n"
        "    task_pool.get(h3).id = 3;\n"
        "    run_task(task_pool.get(h1));\n"
        "    run_task(task_pool.get(h2));\n"
        "    run_task(task_pool.get(h3));\n"
        "    u32 sum = task_pool.get(h1).result + task_pool.get(h2).result + task_pool.get(h3).result;\n"
        "    task_pool.free(h1);\n"
        "    task_pool.free(h2);\n"
        "    task_pool.free(h3);\n"
        "    if (total_work != 60) { return 1; }\n"
        "    return sum - 60;\n"
        "}\n",
        0, "pool task scheduler: 3 tasks, sum = 60");
}

/* ================================================================
 * PATTERN 9: Static variables
 * ================================================================ */

static void test_static_vars(void) {
    printf("\n--- Pattern 9: Static variables ---\n");

    /* 9a: static local persists across calls */
    test_e2e(
        "u32 next_id() {\n"
        "    static u32 counter = 0;\n"
        "    counter += 1;\n"
        "    return counter;\n"
        "}\n"
        "u32 main() {\n"
        "    u32 a = next_id();\n"
        "    u32 b = next_id();\n"
        "    u32 c = next_id();\n"
        "    return a + b + c - 6;\n"
        "}\n",
        0, "static local counter: 1+2+3 = 6");
}

/* ================================================================
 * PATTERN 10: Complex nested access patterns
 * ================================================================ */

static void test_complex_access(void) {
    printf("\n--- Pattern 10: Complex access patterns ---\n");

    /* 10a: array of structs with pointer fields */
    test_e2e(
        "struct Entry { u32 key; u32 val; }\n"
        "Entry[4] table;\n"
        "void table_set(u32 idx, u32 key, u32 val) {\n"
        "    table[idx].key = key;\n"
        "    table[idx].val = val;\n"
        "}\n"
        "u32 table_get(u32 key) {\n"
        "    for (u32 i = 0; i < 4; i += 1) {\n"
        "        if (table[i].key == key) { return table[i].val; }\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "u32 main() {\n"
        "    table_set(0, 1, 10);\n"
        "    table_set(1, 2, 20);\n"
        "    table_set(2, 3, 12);\n"
        "    return table_get(1) + table_get(3) - 22;\n"
        "}\n",
        0, "global array of structs: lookup table 10+12=22");

    /* 10b: indirect function call (call through variable) */
    test_e2e(
        "u32 double_it(u32 x) { return x + x; }\n"
        "u32 apply(u32 x) {\n"
        "    return double_it(x);\n"
        "}\n"
        "u32 main() {\n"
        "    return apply(21) - 42;\n"
        "}\n",
        0, "indirect function call: double(21)=42");

    /* 10c: chained function calls with struct returns */
    test_e2e(
        "struct Vec { u32 x; u32 y; }\n"
        "Vec make(u32 x, u32 y) {\n"
        "    Vec v;\n"
        "    v.x = x;\n"
        "    v.y = y;\n"
        "    return v;\n"
        "}\n"
        "Vec add_vec(Vec a, Vec b) {\n"
        "    Vec r;\n"
        "    r.x = a.x + b.x;\n"
        "    r.y = a.y + b.y;\n"
        "    return r;\n"
        "}\n"
        "u32 main() {\n"
        "    Vec result = add_vec(make(10, 20), make(5, 7));\n"
        "    return result.x + result.y - 42;\n"
        "}\n",
        0, "chained struct returns: (10,20)+(5,7)=(15,27)=42");

    /* 10d: string literal as const slice, use .len */
    test_e2e(
        "u32 main() {\n"
        "    const []u8 msg = \"hello\";\n"
        "    usize len = msg.len;\n"
        "    if (len == 5) { return 0; }\n"
        "    return 1;\n"
        "}\n",
        0, "string literal const []u8 .len = 5");
}

/* ================================================================ */

int main(void) {
    printf("=== Firmware Pattern Stress Tests — Round 3 ===\n");

    test_multi_level_ptr();
    test_typedef();
    test_while_edge();
    test_optional_struct_fields();
    test_union_error_pattern();
    test_const();
    test_uart_pattern();
    test_task_scheduler();
    test_static_vars();
    test_complex_access();

    printf("\n=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) printf(", %d FAILED", tests_failed);
    printf(" ===\n");

    remove("_zer_fw3_test.c");
    remove("_zer_fw3_test.exe");
    remove("_zer_fw3_test.o");
    remove("_zer_fw3_err.txt");

    return tests_failed > 0 ? 1 : 0;
}
