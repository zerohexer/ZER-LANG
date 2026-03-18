/* ================================================================
 * Firmware Pattern Stress Tests — Round 2
 *
 * Targets lowest-confidence areas after round 1 fixes.
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
    zercheck_run(&zc, file); /* don't fail on zercheck warnings */
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
    if (!zer_to_c(zer_source, "_zer_fw2_test.c")) {
        printf("  FAIL: %s — ZER compilation failed\n", test_name);
        tests_failed++;
        return;
    }
    int gcc_ret = system("gcc -std=c99 -O0 -w -o _zer_fw2_test.exe _zer_fw2_test.c 2>_zer_fw2_err.txt");
    if (gcc_ret != 0) {
        printf("  FAIL: %s — GCC compilation failed\n", test_name);
        FILE *ef = fopen("_zer_fw2_err.txt", "r");
        if (ef) {
            char buf[512];
            while (fgets(buf, sizeof(buf), ef)) printf("    gcc: %s", buf);
            fclose(ef);
        }
        tests_failed++;
        return;
    }
    int run_ret = system(".\\_zer_fw2_test.exe");
    if (run_ret != expected_exit) {
        printf("  FAIL: %s — expected exit %d, got %d\n",
               test_name, expected_exit, run_ret);
        tests_failed++;
    } else {
        tests_passed++;
    }
}

/* ================================================================
 * PATTERN 1: Optional pointer ?*T — null sentinel codegen
 * ================================================================ */

static void test_optional_pointer(void) {
    printf("\n--- Pattern 1: Optional pointer ?*T ---\n");

    /* 1a: ?*T returning non-null */
    test_e2e(
        "struct Node { u32 val; }\n"
        "Node global_node;\n"
        "?*Node find(bool exists) {\n"
        "    if (exists) { return &global_node; }\n"
        "    return null;\n"
        "}\n"
        "u32 main() {\n"
        "    global_node.val = 42;\n"
        "    *Node n = find(true) orelse return;\n"
        "    return n.val - 42;\n"
        "}\n",
        0, "?*T return non-null, orelse unwrap");

    /* 1b: ?*T returning null */
    test_e2e(
        "struct Item { u32 x; }\n"
        "?*Item nothing() { return null; }\n"
        "u32 main() {\n"
        "    *Item p = nothing() orelse return;\n"
        "    return p.x;\n"
        "}\n",
        0, "?*T return null, orelse return exits");

    /* 1c: if-unwrap on ?*T */
    test_e2e(
        "struct Data { u32 val; }\n"
        "Data g;\n"
        "?*Data maybe() { g.val = 55; return &g; }\n"
        "u32 main() {\n"
        "    ?*Data opt = maybe();\n"
        "    if (opt) |ptr| {\n"
        "        return ptr.val - 55;\n"
        "    }\n"
        "    return 1;\n"
        "}\n",
        0, "if-unwrap on ?*T captures pointer");

    /* 1d: ?*T null check in if-unwrap */
    test_e2e(
        "struct X { u32 v; }\n"
        "?*X get_null() { return null; }\n"
        "u32 main() {\n"
        "    ?*X opt = get_null();\n"
        "    if (opt) |ptr| {\n"
        "        return 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n",
        0, "?*T null skips if-unwrap body");

    /* 1e: ?*T passed through multiple functions */
    test_e2e(
        "struct Buf { u32 len; }\n"
        "Buf g_buf;\n"
        "?*Buf get_buf() { g_buf.len = 100; return &g_buf; }\n"
        "u32 read_len(?*Buf b) {\n"
        "    *Buf actual = b orelse return;\n"
        "    return actual.len;\n"
        "}\n"
        "u32 main() {\n"
        "    ?*Buf b = get_buf();\n"
        "    return read_len(b) - 100;\n"
        "}\n",
        0, "?*T passed between functions");
}

/* ================================================================
 * PATTERN 2: Integer coercion and literal compatibility
 * ================================================================ */

static void test_integer_coercion(void) {
    printf("\n--- Pattern 2: Integer coercion ---\n");

    /* 2a: u8 widened to u32 in expression */
    test_e2e(
        "u32 main() {\n"
        "    u8 small = 200;\n"
        "    u32 big = small;\n"
        "    return big - 200;\n"
        "}\n",
        0, "u8 widened to u32");

    /* 2b: literal 0 as bool */
    test_e2e(
        "u32 main() {\n"
        "    bool b = false;\n"
        "    if (b) { return 1; }\n"
        "    return 0;\n"
        "}\n",
        0, "false is falsy");

    /* 2c: integer literal fits in u8 */
    test_e2e(
        "u32 main() {\n"
        "    u8 x = 255;\n"
        "    u32 result = @truncate(u32, x);\n"
        "    return result - 255;\n"
        "}\n",
        0, "literal 255 fits in u8");

    /* 2d: negative literal in i32 */
    test_e2e(
        "u32 main() {\n"
        "    i32 x = -10;\n"
        "    i32 y = 52;\n"
        "    i32 sum = x + y;\n"
        "    return @bitcast(u32, sum) - 42;\n"
        "}\n",
        0, "negative literal + positive = 42");

    /* 2e: compound assignment u32 += 1 */
    test_e2e(
        "u32 main() {\n"
        "    u32 x = 41;\n"
        "    x += 1;\n"
        "    return x - 42;\n"
        "}\n",
        0, "compound assignment u32 += 1");
}

/* ================================================================
 * PATTERN 3: Nested loops with break/continue
 * ================================================================ */

static void test_nested_loops(void) {
    printf("\n--- Pattern 3: Nested loops ---\n");

    /* 3a: break from inner loop only */
    test_e2e(
        "u32 main() {\n"
        "    u32 count = 0;\n"
        "    for (u32 i = 0; i < 3; i += 1) {\n"
        "        for (u32 j = 0; j < 10; j += 1) {\n"
        "            if (j == 2) { break; }\n"
        "            count += 1;\n"
        "        }\n"
        "    }\n"
        "    return count - 6;\n"
        "}\n",
        0, "break inner loop: 3 outer * 2 inner = 6");

    /* 3b: continue in inner loop */
    test_e2e(
        "u32 main() {\n"
        "    u32 count = 0;\n"
        "    for (u32 i = 0; i < 3; i += 1) {\n"
        "        for (u32 j = 0; j < 4; j += 1) {\n"
        "            if (j == 1) { continue; }\n"
        "            count += 1;\n"
        "        }\n"
        "    }\n"
        "    return count - 9;\n"
        "}\n",
        0, "continue inner: 3 outer * 3 (skip j=1) = 9");

    /* 3c: while inside for */
    test_e2e(
        "u32 main() {\n"
        "    u32 total = 0;\n"
        "    for (u32 i = 0; i < 3; i += 1) {\n"
        "        u32 n = 0;\n"
        "        while (n < i + 1) {\n"
        "            total += 1;\n"
        "            n += 1;\n"
        "        }\n"
        "    }\n"
        "    return total - 6;\n"
        "}\n",
        0, "while inside for: 1+2+3 = 6");

    /* 3d: defer in nested loop — inner only */
    test_e2e(
        "u32 d_count = 0;\n"
        "void d_inc() { d_count += 1; }\n"
        "u32 main() {\n"
        "    for (u32 i = 0; i < 2; i += 1) {\n"
        "        for (u32 j = 0; j < 3; j += 1) {\n"
        "            defer d_inc();\n"
        "        }\n"
        "    }\n"
        "    return d_count - 6;\n"
        "}\n",
        0, "defer in inner loop: 2*3 = 6 defers");

    /* 3e: break inner with defer in outer */
    test_e2e(
        "u32 outer_d = 0;\n"
        "void o_inc() { outer_d += 1; }\n"
        "u32 main() {\n"
        "    for (u32 i = 0; i < 3; i += 1) {\n"
        "        defer o_inc();\n"
        "        for (u32 j = 0; j < 10; j += 1) {\n"
        "            if (j == 1) { break; }\n"
        "        }\n"
        "    }\n"
        "    return outer_d - 3;\n"
        "}\n",
        0, "inner break doesn't fire outer defer early");
}

/* ================================================================
 * PATTERN 4: Array and bounds checking
 * ================================================================ */

static void test_arrays(void) {
    printf("\n--- Pattern 4: Arrays and bounds ---\n");

    /* 4a: array init + access */
    test_e2e(
        "u32 main() {\n"
        "    u32[4] arr;\n"
        "    arr[0] = 10;\n"
        "    arr[1] = 20;\n"
        "    arr[2] = 30;\n"
        "    arr[3] = 40;\n"
        "    return arr[0] + arr[3] - 50;\n"
        "}\n",
        0, "array init and access");

    /* 4b: array in loop */
    test_e2e(
        "u32 main() {\n"
        "    u32[5] arr;\n"
        "    for (u32 i = 0; i < 5; i += 1) {\n"
        "        arr[i] = i * 10;\n"
        "    }\n"
        "    return arr[4] - 40;\n"
        "}\n",
        0, "array filled in loop");

    /* 4c: array passed to function via pointer */
    test_e2e(
        "u32 sum_three(*u32 data) {\n"
        "    return data[0] + data[1] + data[2];\n"
        "}\n"
        "u32 main() {\n"
        "    u32[3] arr;\n"
        "    arr[0] = 10;\n"
        "    arr[1] = 20;\n"
        "    arr[2] = 12;\n"
        "    return sum_three(&arr[0]) - 42;\n"
        "}\n",
        0, "array passed via &arr[0]");

    /* 4d: array of structs */
    test_e2e(
        "struct Pair { u32 a; u32 b; }\n"
        "u32 main() {\n"
        "    Pair[3] pairs;\n"
        "    pairs[0].a = 1;\n"
        "    pairs[0].b = 2;\n"
        "    pairs[1].a = 10;\n"
        "    pairs[1].b = 20;\n"
        "    pairs[2].a = 100;\n"
        "    pairs[2].b = 200;\n"
        "    u32 sum = pairs[0].a + pairs[1].b + pairs[2].a;\n"
        "    return sum - 121;\n"
        "}\n",
        0, "array of structs: field access");
}

/* ================================================================
 * PATTERN 5: Mutable pointer capture |*val|
 * ================================================================ */

static void test_mutable_capture(void) {
    printf("\n--- Pattern 5: Mutable capture |*val| ---\n");

    /* 5a: modify optional value through |*val| */
    test_e2e(
        "?u32 get_val() { return 10; }\n"
        "u32 main() {\n"
        "    ?u32 x = get_val();\n"
        "    if (x) |*v| {\n"
        "        *v = 42;\n"
        "    }\n"
        "    u32 result = x orelse 0;\n"
        "    return result - 42;\n"
        "}\n",
        0, "|*val| modifies original");

    /* 5b: |*val| on null optional */
    test_e2e(
        "?u32 nothing() { return null; }\n"
        "u32 main() {\n"
        "    ?u32 x = nothing();\n"
        "    if (x) |*v| {\n"
        "        *v = 99;\n"
        "    }\n"
        "    u32 result = x orelse 7;\n"
        "    return result - 7;\n"
        "}\n",
        0, "|*val| on null skips body");
}

/* ================================================================
 * PATTERN 6: Complex function call patterns
 * ================================================================ */

static void test_complex_calls(void) {
    printf("\n--- Pattern 6: Complex call patterns ---\n");

    /* 6a: function returning struct used immediately */
    test_e2e(
        "struct Pair { u32 a; u32 b; }\n"
        "Pair make(u32 x, u32 y) {\n"
        "    Pair p;\n"
        "    p.a = x;\n"
        "    p.b = y;\n"
        "    return p;\n"
        "}\n"
        "u32 sum_pair(Pair p) { return p.a + p.b; }\n"
        "u32 main() {\n"
        "    return sum_pair(make(17, 25)) - 42;\n"
        "}\n",
        0, "struct return passed directly to function");

    /* 6b: recursive function */
    test_e2e(
        "u32 fib(u32 n) {\n"
        "    if (n <= 1) { return n; }\n"
        "    return fib(n - 1) + fib(n - 2);\n"
        "}\n"
        "u32 main() {\n"
        "    return fib(10) - 55;\n"
        "}\n",
        0, "recursive fibonacci(10) = 55");

    /* 6c: mutual function calls */
    test_e2e(
        "u32 is_even(u32 n);\n"
        "u32 is_odd(u32 n) {\n"
        "    if (n == 0) { return 0; }\n"
        "    return is_even(n - 1);\n"
        "}\n"
        "u32 is_even(u32 n) {\n"
        "    if (n == 0) { return 1; }\n"
        "    return is_odd(n - 1);\n"
        "}\n"
        "u32 main() {\n"
        "    return is_even(10) - 1;\n"
        "}\n",
        0, "mutual recursion: is_even(10) = 1");

    /* 6d: function with many parameters */
    test_e2e(
        "u32 add5(u32 a, u32 b, u32 c, u32 d, u32 e) {\n"
        "    return a + b + c + d + e;\n"
        "}\n"
        "u32 main() {\n"
        "    return add5(1, 2, 3, 4, 32) - 42;\n"
        "}\n",
        0, "5-parameter function");
}

/* ================================================================
 * PATTERN 7: Global state + multiple functions
 * ================================================================ */

static void test_global_state(void) {
    printf("\n--- Pattern 7: Global state ---\n");

    /* 7a: global counter modified by multiple functions */
    test_e2e(
        "u32 counter = 0;\n"
        "void add(u32 n) { counter += n; }\n"
        "void sub(u32 n) { counter -= n; }\n"
        "u32 main() {\n"
        "    add(50);\n"
        "    sub(8);\n"
        "    return counter - 42;\n"
        "}\n",
        0, "global counter add/sub");

    /* 7b: global struct modified by function */
    test_e2e(
        "struct State { u32 count; bool active; }\n"
        "State g_state;\n"
        "void activate() {\n"
        "    g_state.active = true;\n"
        "    g_state.count = 1;\n"
        "}\n"
        "void tick() {\n"
        "    if (g_state.active) {\n"
        "        g_state.count += 1;\n"
        "    }\n"
        "}\n"
        "u32 main() {\n"
        "    activate();\n"
        "    tick();\n"
        "    tick();\n"
        "    tick();\n"
        "    return g_state.count - 4;\n"
        "}\n",
        0, "global struct modified by functions");

    /* 7c: global array */
    test_e2e(
        "u32[10] buffer;\n"
        "u32 buf_idx = 0;\n"
        "void buf_write(u32 val) {\n"
        "    buffer[buf_idx] = val;\n"
        "    buf_idx += 1;\n"
        "}\n"
        "u32 main() {\n"
        "    buf_write(10);\n"
        "    buf_write(20);\n"
        "    buf_write(12);\n"
        "    return buffer[0] + buffer[1] + buffer[2] - 42;\n"
        "}\n",
        0, "global array with write function");
}

/* ================================================================
 * PATTERN 8: Intrinsics in realistic contexts
 * ================================================================ */

static void test_intrinsics(void) {
    printf("\n--- Pattern 8: Intrinsics ---\n");

    /* 8a: @size of struct */
    test_e2e(
        "struct Header { u32 magic; u32 version; u32 length; }\n"
        "u32 main() {\n"
        "    usize s = @size(Header);\n"
        "    if (s >= 12) { return 0; }\n"
        "    return 1;\n"
        "}\n",
        0, "@size(Header) >= 12");

    /* 8b: @truncate in loop */
    test_e2e(
        "u32 main() {\n"
        "    u32 sum = 0;\n"
        "    for (u32 i = 0; i < 5; i += 1) {\n"
        "        u8 byte = @truncate(u8, i);\n"
        "        sum += @truncate(u32, byte);\n"
        "    }\n"
        "    return sum - 10;\n"
        "}\n",
        0, "@truncate in loop: 0+1+2+3+4 = 10");

    /* 8c: @ptrcast for type punning */
    test_e2e(
        "u32 main() {\n"
        "    u32 val = 0x42;\n"
        "    *u8 byte_ptr = @ptrcast(*u8, &val);\n"
        "    u32 first_byte = @truncate(u32, *byte_ptr);\n"
        "    return first_byte - 0x42;\n"
        "}\n",
        0, "@ptrcast u32* to u8* reads first byte");

    /* 8d: @bitcast roundtrip */
    test_e2e(
        "u32 main() {\n"
        "    u32 orig = 42;\n"
        "    i32 as_signed = @bitcast(i32, orig);\n"
        "    u32 back = @bitcast(u32, as_signed);\n"
        "    return back - 42;\n"
        "}\n",
        0, "@bitcast u32→i32→u32 roundtrip");
}

/* ================================================================
 * PATTERN 9: Enum with explicit values and comparison
 * ================================================================ */

static void test_enum_advanced(void) {
    printf("\n--- Pattern 9: Advanced enum ---\n");

    /* 9a: enum comparison with == */
    test_e2e(
        "enum Dir { north, south, east, west }\n"
        "u32 main() {\n"
        "    Dir d = Dir.east;\n"
        "    if (d == Dir.east) { return 0; }\n"
        "    return 1;\n"
        "}\n",
        0, "enum equality comparison");

    /* 9b: enum != comparison */
    test_e2e(
        "enum Color { red, green, blue }\n"
        "u32 main() {\n"
        "    Color c = Color.red;\n"
        "    if (c != Color.blue) { return 0; }\n"
        "    return 1;\n"
        "}\n",
        0, "enum inequality comparison");

    /* 9c: enum passed to function and returned */
    test_e2e(
        "enum Status { ok, err }\n"
        "Status check(u32 val) {\n"
        "    if (val > 100) { return Status.err; }\n"
        "    return Status.ok;\n"
        "}\n"
        "u32 main() {\n"
        "    Status s = check(50);\n"
        "    if (s == Status.ok) { return 0; }\n"
        "    return 1;\n"
        "}\n",
        0, "enum returned from function");

    /* 9d: enum in struct field */
    test_e2e(
        "enum Level { low, mid, high }\n"
        "struct Config { Level priority; u32 value; }\n"
        "u32 main() {\n"
        "    Config c;\n"
        "    c.priority = Level.high;\n"
        "    c.value = 42;\n"
        "    if (c.priority == Level.high) { return c.value - 42; }\n"
        "    return 1;\n"
        "}\n",
        0, "enum in struct field");
}

/* ================================================================
 * PATTERN 10: Compound expressions and operator precedence
 * ================================================================ */

static void test_expressions(void) {
    printf("\n--- Pattern 10: Complex expressions ---\n");

    /* 10a: chained comparisons with && */
    test_e2e(
        "u32 in_range(u32 x, u32 lo, u32 hi) {\n"
        "    if (x >= lo && x <= hi) { return 1; }\n"
        "    return 0;\n"
        "}\n"
        "u32 main() {\n"
        "    return in_range(5, 1, 10) - 1;\n"
        "}\n",
        0, "chained comparison with &&");

    /* 10b: nested ternary-like pattern using if */
    test_e2e(
        "u32 clamp(u32 x, u32 lo, u32 hi) {\n"
        "    if (x < lo) { return lo; }\n"
        "    if (x > hi) { return hi; }\n"
        "    return x;\n"
        "}\n"
        "u32 main() {\n"
        "    u32 a = clamp(5, 0, 100);\n"
        "    u32 b = clamp(200, 0, 100);\n"
        "    return a + b - 105;\n"
        "}\n",
        0, "clamp function: 5 + 100 = 105");

    /* 10c: bitwise operations — ZER requires explicit bool (no implicit int→bool) */
    test_e2e(
        "u32 main() {\n"
        "    u32 flags = 0;\n"
        "    flags = flags | 0x01;\n"
        "    flags = flags | 0x04;\n"
        "    flags = flags | 0x10;\n"
        "    if ((flags & 0x01) != 0) {\n"
        "        if ((flags & 0x04) != 0) {\n"
        "            if ((flags & 0x10) != 0) {\n"
        "                return flags - 0x15;\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "    return 1;\n"
        "}\n",
        0, "bitwise OR + AND flag checks (explicit != 0)");

    /* 10d: shift operations */
    test_e2e(
        "u32 main() {\n"
        "    u32 x = 1;\n"
        "    x = x << 5;\n"
        "    u32 y = x >> 3;\n"
        "    return y - 4;\n"
        "}\n",
        0, "shift: (1<<5)>>3 = 4");

    /* 10e: modulo in loop */
    test_e2e(
        "u32 main() {\n"
        "    u32 sum = 0;\n"
        "    for (u32 i = 0; i < 20; i += 1) {\n"
        "        if (i % 3 == 0) { sum += 1; }\n"
        "    }\n"
        "    return sum - 7;\n"
        "}\n",
        0, "modulo in loop: count multiples of 3 in [0,20)");
}

/* ================================================================ */

int main(void) {
    printf("=== Firmware Pattern Stress Tests — Round 2 ===\n");

    test_optional_pointer();
    test_integer_coercion();
    test_nested_loops();
    test_arrays();
    test_mutable_capture();
    test_complex_calls();
    test_global_state();
    test_intrinsics();
    test_enum_advanced();
    test_expressions();

    printf("\n=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) printf(", %d FAILED", tests_failed);
    printf(" ===\n");

    remove("_zer_fw2_test.c");
    remove("_zer_fw2_test.exe");
    remove("_zer_fw2_test.o");
    remove("_zer_fw2_err.txt");

    return tests_failed > 0 ? 1 : 0;
}
