#include <stdio.h>
#include <string.h>
#include "lexer.h"
#include "ast.h"
#include "parser.h"
#include "types.h"
#include "checker.h"

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

static bool run_checker(const char *source, Arena *arena) {
    Scanner scanner;
    scanner_init(&scanner, source);
    Parser parser;
    parser_init(&parser, &scanner, arena, "test");
    Node *file = parse_file(&parser);
    if (parser.had_error) return false;

    Checker checker;
    checker_init(&checker, arena, "test");
    return checker_check(&checker, file);
}

/* expect type checking to pass */
static void expect_ok(const char *source, const char *test_name) {
    Arena arena;
    arena_init(&arena, 128 * 1024);
    tests_run++;
    if (run_checker(source, &arena)) {
        tests_passed++;
    } else {
        printf("  FAIL: %s — expected OK, got errors\n", test_name);
        tests_failed++;
    }
    arena_free(&arena);
}

/* expect type checking to produce errors */
static void expect_error(const char *source, const char *test_name) {
    Arena arena;
    arena_init(&arena, 128 * 1024);
    tests_run++;
    if (!run_checker(source, &arena)) {
        tests_passed++;
    } else {
        printf("  FAIL: %s — expected error, got OK\n", test_name);
        tests_failed++;
    }
    arena_free(&arena);
}

/* ================================================================ */

static void test_primitive_declarations(void) {
    printf("[primitive declarations]\n");
    expect_ok("u32 x = 5;", "u32 = int");
    expect_ok("i64 y = 100;", "i64 = int");
    expect_ok("bool flag = true;", "bool = true");
    expect_ok("f32 pi = 3.14;", "f32 = float");
    expect_ok("u8 byte = 0;", "u8 = int");
}

static void test_integer_coercion(void) {
    printf("[integer coercion]\n");

    /* widening — should pass */
    expect_ok(
        "void f() { u8 a = 5; u32 b = a; }",
        "u8 → u32 implicit widening");
    expect_ok(
        "void f() { u16 a = 5; u32 b = a; }",
        "u16 → u32 implicit widening");
    expect_ok(
        "void f() { u8 a = 5; i16 b = a; }",
        "u8 → i16 unsigned to larger signed");

    /* narrowing — should error */
    expect_error(
        "void f() { u32 a = 5; u16 b = a; }",
        "u32 → u16 narrowing rejected");
    expect_error(
        "void f() { i32 a = 5; i16 b = a; }",
        "i32 → i16 narrowing rejected");

    /* signed/unsigned mismatch — should error */
    expect_error(
        "void f() { i32 a = 5; u32 b = a; }",
        "i32 → u32 sign mismatch rejected");
}

static void test_float_coercion(void) {
    printf("[float coercion]\n");
    expect_ok(
        "void f() { f32 a = 1.0; f64 b = a; }",
        "f32 → f64 widening");
    expect_error(
        "void f() { f64 a = 1.0; f32 b = a; }",
        "f64 → f32 narrowing rejected");
}

static void test_arithmetic_type_checking(void) {
    printf("[arithmetic type checking]\n");
    expect_ok(
        "void f() { u32 a = 1; u32 b = 2; u32 c = a + b; }",
        "u32 + u32 = u32");
    expect_ok(
        "void f() { u32 a = 1; u32 b = a * 2; }",
        "u32 * u32");
    expect_error(
        "void f() { bool a = true; u32 b = a + 1; }",
        "bool + int rejected");
}

static void test_comparison(void) {
    printf("[comparison]\n");
    expect_ok(
        "void f() { u32 a = 1; bool b = a > 5; }",
        "u32 > u32 = bool");
    expect_ok(
        "void f() { u32 a = 1; u32 b = 2; bool c = a == b; }",
        "u32 == u32 = bool");
}

static void test_logical_operators(void) {
    printf("[logical operators]\n");
    expect_ok(
        "void f() { bool a = true; bool b = false; bool c = a && b; }",
        "bool && bool");
    expect_error(
        "void f() { u32 a = 1; bool b = a && true; }",
        "u32 && bool rejected");
}

static void test_struct_type_checking(void) {
    printf("[struct type checking]\n");
    expect_ok(
        "struct Task { u32 pid; u32 priority; }\n"
        "void f() { Task t; t.pid = 42; }",
        "struct field access");
    expect_ok(
        "struct Task { u32 pid; }\n"
        "void f(Task task) { *Task t = &task; t.pid = 5; }",
        "pointer auto-deref field access");
}

static void test_optional_types(void) {
    printf("[optional types]\n");
    expect_ok(
        "void f() { ?u32 maybe = null; }",
        "?u32 = null");
    expect_ok(
        "void f() { ?u32 maybe = 5; }",
        "?u32 = u32 (implicit wrap)");
}

static void test_if_unwrap(void) {
    printf("[if-unwrap]\n");
    expect_ok(
        "void f() {\n"
        "    ?u32 maybe = 5;\n"
        "    if (maybe) |val| { u32 x = val; }\n"
        "}\n",
        "if-unwrap with capture");
    expect_error(
        "void f() {\n"
        "    u32 x = 5;\n"
        "    if (x) |val| { }\n"
        "}\n",
        "if-unwrap on non-optional rejected");
}

static void test_orelse(void) {
    printf("[orelse]\n");
    expect_ok(
        "void f() { ?u32 x = 5; u32 y = x orelse 0; }",
        "orelse with default value");
    expect_ok(
        "void f() { ?u32 x = 5; u32 y = x orelse return; }",
        "orelse return");
    expect_error(
        "void f() { u32 x = 5; u32 y = x orelse 0; }",
        "orelse on non-optional rejected");
}

static void test_function_calls(void) {
    printf("[function calls]\n");
    expect_ok(
        "u32 add(u32 a, u32 b) { return a + b; }\n"
        "void f() { u32 x = add(1, 2); }",
        "simple function call");
    expect_error(
        "u32 add(u32 a, u32 b) { return a + b; }\n"
        "void f() { u32 x = add(1); }",
        "wrong arg count rejected");
}

static void test_return_type(void) {
    printf("[return type]\n");
    expect_ok(
        "u32 f() { return 5; }",
        "return u32 from u32 function");
    expect_ok(
        "?u32 f() { return null; }",
        "return null from ?u32 function");
    expect_ok(
        "void f() { return; }",
        "bare return from void function");
    expect_error(
        "u32 f() { return true; }",
        "return bool from u32 function rejected");
}

static void test_break_continue_outside_loop(void) {
    printf("[break/continue outside loop]\n");
    expect_error(
        "void f() { break; }",
        "break outside loop rejected");
    expect_error(
        "void f() { continue; }",
        "continue outside loop rejected");
    expect_ok(
        "void f() { while (true) { break; } }",
        "break inside loop OK");
    expect_ok(
        "void f() { for (u32 i = 0; i < 10; i += 1) { continue; } }",
        "continue inside for OK");
}

static void test_pool_handle(void) {
    printf("[pool/handle builtins]\n");
    expect_ok(
        "struct Task { u32 pid; }\n"
        "Pool(Task, 8) tasks;\n"
        "void f() {\n"
        "    Handle(Task) h = tasks.alloc() orelse return;\n"
        "    tasks.get(h).pid = 42;\n"
        "    tasks.free(h);\n"
        "}\n",
        "pool alloc/get/free");
}

static void test_ring_buffer(void) {
    printf("[ring buffer builtins]\n");
    expect_ok(
        "Ring(u8, 256) rx_buf;\n"
        "void f() {\n"
        "    rx_buf.push(0xFF);\n"
        "    if (rx_buf.pop()) |byte| { u8 b = byte; }\n"
        "}\n",
        "ring push/pop");
}

static void test_array_slice(void) {
    printf("[array and slice]\n");
    expect_ok(
        "void f() {\n"
        "    u8[256] buf;\n"
        "    u8 x = buf[0];\n"
        "}\n",
        "array indexing");
    expect_ok(
        "void f() {\n"
        "    u8[256] buf;\n"
        "    []u8 s = buf[0..5];\n"
        "}\n",
        "array slicing to slice");
}

static void test_enum_declaration(void) {
    printf("[enum]\n");
    expect_ok(
        "enum Color { red, green, blue }\n",
        "enum declaration");
}

static void test_typedef(void) {
    printf("[typedef]\n");
    expect_ok(
        "typedef u32 Milliseconds;\n"
        "void f() { Milliseconds ms = 1000; }",
        "typedef alias");
}

static void test_forward_reference(void) {
    printf("[forward reference]\n");
    expect_ok(
        "void a() { b(); }\n"
        "void b() { }\n",
        "call function defined later");
}

static void test_realistic_program(void) {
    printf("[realistic program]\n");
    expect_ok(
        "struct Task {\n"
        "    u32 pid;\n"
        "    u32 priority;\n"
        "}\n"
        "\n"
        "Pool(Task, 8) tasks;\n"
        "Ring(u8, 256) rx_buf;\n"
        "\n"
        "u32 add(u32 a, u32 b) {\n"
        "    return a + b;\n"
        "}\n"
        "\n"
        "?u32 read_byte() {\n"
        "    if (rx_buf.pop()) |byte| {\n"
        "        return byte;\n"
        "    }\n"
        "    return null;\n"
        "}\n"
        "\n"
        "void main() {\n"
        "    Handle(Task) h = tasks.alloc() orelse return;\n"
        "    tasks.get(h).pid = 1;\n"
        "    tasks.get(h).priority = add(1, 2);\n"
        "\n"
        "    u32 val = read_byte() orelse 0;\n"
        "\n"
        "    for (u32 i = 0; i < 10; i += 1) {\n"
        "        rx_buf.push(0);\n"
        "    }\n"
        "\n"
        "    tasks.free(h);\n"
        "}\n",
        "complete program with struct, pool, ring, functions, optionals");
}

/* ---- New tests for TODOs ---- */

static void test_const_assignment(void) {
    printf("[const assignment]\n");
    expect_error(
        "void f() { const u32 x = 5; x = 10; }",
        "assign to const rejected");
    expect_ok(
        "void f() { u32 x = 5; x = 10; }",
        "assign to mutable OK");
}

static void test_non_storable(void) {
    printf("[non-storable get() result]\n");
    expect_error(
        "struct Task { u32 pid; }\n"
        "Pool(Task, 8) tasks;\n"
        "void f() {\n"
        "    Handle(Task) h = tasks.alloc() orelse return;\n"
        "    *Task ptr = tasks.get(h);\n"
        "}\n",
        "store get() result rejected");
    expect_ok(
        "struct Task { u32 pid; }\n"
        "Pool(Task, 8) tasks;\n"
        "void f() {\n"
        "    Handle(Task) h = tasks.alloc() orelse return;\n"
        "    tasks.get(h).pid = 42;\n"
        "}\n",
        "inline get() field access OK");
}

static void test_distinct_typedef(void) {
    printf("[distinct typedef]\n");
    expect_ok(
        "distinct typedef u32 Celsius;\n"
        "void f() { Celsius c = 25; }",
        "distinct typedef declaration");
    expect_error(
        "distinct typedef u32 Celsius;\n"
        "distinct typedef u32 Fahrenheit;\n"
        "void f() { Celsius c = 25; Fahrenheit f = c; }",
        "distinct types not interchangeable");
}

static void test_exhaustive_switch(void) {
    printf("[exhaustive switch]\n");
    expect_error(
        "void foo() { }\n"
        "void f() {\n"
        "    switch (42) {\n"
        "        0 => foo(),\n"
        "    }\n"
        "}\n",
        "integer switch without default rejected");
    expect_ok(
        "void foo() { }\n"
        "void bar() { }\n"
        "void f() {\n"
        "    switch (42) {\n"
        "        0 => foo(),\n"
        "        default => bar(),\n"
        "    }\n"
        "}\n",
        "integer switch with default OK");
}

static void test_ufcs(void) {
    printf("[UFCS]\n");
    expect_ok(
        "struct Task { u32 pid; }\n"
        "void run(*Task t) { }\n"
        "void f() {\n"
        "    Task task;\n"
        "    task.run();\n"
        "}\n",
        "UFCS: task.run() → run(&task)");
}

/* ---- Audit tests: safety gaps and edge cases ---- */

static void test_union_field_access(void) {
    printf("[union field access]\n");
    expect_error(
        "union Msg { u32 sensor; u32 command; }\n"
        "void f(Msg m) { u32 x = m.sensor; }",
        "direct union field access rejected");
}

static void test_scope_escape(void) {
    printf("[scope escape]\n");
    expect_error(
        "*u32 f() { u32 local = 5; return &local; }",
        "return &local rejected");
    expect_ok(
        "*u32 f() { static u32 global = 5; return &global; }",
        "return &static OK");
}

static void test_store_through(void) {
    printf("[store-through validation]\n");
    expect_error(
        "static *u32 global_ptr;\n"
        "void f() { u32 local = 5; global_ptr = &local; }",
        "store &local in static rejected");
}

static void test_keep_validation(void) {
    printf("[keep parameter validation]\n");
    expect_error(
        "void register_cb(keep *u32 ctx) { }\n"
        "void f() { u32 local = 5; register_cb(&local); }",
        "pass &local to keep param rejected");
    expect_ok(
        "void register_cb(keep *u32 ctx) { }\n"
        "static u32 global_data = 5;\n"
        "void f() { register_cb(&global_data); }",
        "pass &static to keep param OK");
}

static void test_float_integer_mixing(void) {
    printf("[float/integer mixing]\n");
    expect_error(
        "void f() { u32 a = 1; f32 b = 1.0; u32 c = a + b; }",
        "u32 + f32 rejected");
}

static void test_compound_assignment_types(void) {
    printf("[compound assignment type check]\n");
    expect_ok(
        "void f() { u32 x = 5; x += 1; }",
        "u32 += u32 OK");
    expect_ok(
        "void f() { u32 x = 5; x <<= 2; }",
        "u32 <<= u32 OK");
}

static void test_void_return_in_nonvoid(void) {
    printf("[void return in non-void function]\n");
    expect_error(
        "u32 f() { return; }",
        "bare return in u32 function rejected");
    expect_ok(
        "?u32 f() { return; }",
        "bare return in ?u32 function OK (success for ?void)");
}

static void test_nested_optionals(void) {
    printf("[nested optionals]\n");
    expect_ok(
        "void f() {\n"
        "    ?u32 inner = 5;\n"
        "    if (inner) |val| { u32 x = val; }\n"
        "}\n",
        "simple optional unwrap");
}

static void test_const_slice_coercion(void) {
    printf("[const slice coercion]\n");
    expect_ok(
        "void read_only(const []u8 data) { }\n"
        "void f() {\n"
        "    u8[256] buf;\n"
        "    read_only(buf);\n"
        "}\n",
        "mutable array → const slice");
}

static void test_signed_unsigned_rejection(void) {
    printf("[signed/unsigned rejection]\n");
    expect_error(
        "void f() { i32 a = 5; u32 b = a; }",
        "i32 → u32 implicit rejected");
}

/* ================================================================ */

int main(void) {
    printf("=== ZER Type Checker Tests ===\n\n");

    test_primitive_declarations();
    test_integer_coercion();
    test_float_coercion();
    test_arithmetic_type_checking();
    test_comparison();
    test_logical_operators();
    test_struct_type_checking();
    test_optional_types();
    test_if_unwrap();
    test_orelse();
    test_function_calls();
    test_return_type();
    test_break_continue_outside_loop();
    test_pool_handle();
    test_ring_buffer();
    test_array_slice();
    test_enum_declaration();
    test_typedef();
    test_forward_reference();
    test_realistic_program();
    test_const_assignment();
    test_non_storable();
    test_distinct_typedef();
    test_exhaustive_switch();
    test_ufcs();
    test_union_field_access();
    test_scope_escape();
    test_store_through();
    test_keep_validation();
    test_float_integer_mixing();
    test_compound_assignment_types();
    test_void_return_in_nonvoid();
    test_nested_optionals();
    test_const_slice_coercion();
    test_signed_unsigned_rejection();

    printf("\n=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(", %d FAILED", tests_failed);
    }
    printf(" ===\n");

    return tests_failed > 0 ? 1 : 0;
}
