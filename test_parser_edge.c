#include <stdio.h>
#include <string.h>
#include "lexer.h"
#include "ast.h"
#include "parser.h"

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

static Node *parse_source(const char *source, Arena *arena, bool *had_error) {
    Scanner scanner;
    scanner_init(&scanner, source);
    Parser parser;
    parser_init(&parser, &scanner, arena, "test");
    Node *file = parse_file(&parser);
    *had_error = parser.had_error;
    return file;
}

static void expect_ok(const char *source, const char *test_name) {
    Arena arena;
    arena_init(&arena, 64 * 1024);
    tests_run++;
    bool err;
    Node *file = parse_source(source, &arena, &err);
    if (!err && file && file->kind == NODE_FILE) {
        tests_passed++;
    } else {
        printf("  FAIL: %s\n", test_name);
        tests_failed++;
    }
    arena_free(&arena);
}

static void expect_fail(const char *source, const char *test_name) {
    Arena arena;
    arena_init(&arena, 64 * 1024);
    tests_run++;
    bool err;
    parse_source(source, &arena, &err);
    if (err) {
        tests_passed++;
    } else {
        printf("  FAIL (expected error): %s\n", test_name);
        tests_failed++;
    }
    arena_free(&arena);
}

/* ================================================================
 * EDGE CASE: Named type variable declarations inside functions
 * Task t; / Task t = expr; / MyType val;
 * ================================================================ */
static void test_named_type_var_decl(void) {
    printf("[named type var decl in function body]\n");

    /* Named type followed by identifier = var decl */
    expect_ok(
        "void main() { Task t; }",
        "Task t;");
    expect_ok(
        "void main() { Task t = foo(); }",
        "Task t = foo();");
    expect_ok(
        "void main() { UartError err; }",
        "UartError err;");
    expect_ok(
        "void main() { SensorData data = read(); }",
        "SensorData data = read();");

    /* Named type shouldn't break expression statements */
    expect_ok(
        "void main() { foo(); }",
        "foo() is not confused with var decl");
    expect_ok(
        "void main() { foo.bar(); }",
        "foo.bar() is not confused with var decl");
    expect_ok(
        "void main() { foo = 5; }",
        "foo = 5 is not confused with var decl");
    expect_ok(
        "void main() { foo[0] = 5; }",
        "foo[0] = 5 is not confused with var decl");
}

/* ================================================================
 * EDGE CASE: Pointer type var decl vs dereference assignment
 * *u32 ptr; vs *ptr = 5;
 * ================================================================ */
static void test_pointer_type_vs_deref(void) {
    printf("[pointer type var decl vs dereference]\n");

    /* Pointer type var decl */
    expect_ok(
        "void main() { *u32 ptr; }",
        "*u32 ptr;");
    expect_ok(
        "void main() { *Task t = get_task(); }",
        "*Task t = get_task();");
    expect_ok(
        "void main() { *opaque raw; }",
        "*opaque raw;");
    expect_ok(
        "void main() { ?*Task maybe = find(); }",
        "?*Task maybe = find();");

    /* Dereference assignment — NOT a var decl */
    expect_ok(
        "void main() { *ptr = 5; }",
        "*ptr = 5;");
    expect_ok(
        "void main() { *reg = 0xFF; }",
        "*reg = 0xFF;");

    /* Chained: tasks.get(h).priority = 5; */
    expect_ok(
        "void main() { tasks.get(h).priority = 5; }",
        "tasks.get(h).priority = 5;");
}

/* ================================================================
 * EDGE CASE: All type forms as var decls inside functions
 * ================================================================ */
static void test_all_type_var_decls(void) {
    printf("[all type forms as var decls]\n");

    expect_ok("void f() { u8 a; }", "u8");
    expect_ok("void f() { u16 a; }", "u16");
    expect_ok("void f() { u32 a; }", "u32");
    expect_ok("void f() { u64 a; }", "u64");
    expect_ok("void f() { i8 a; }", "i8");
    expect_ok("void f() { i16 a; }", "i16");
    expect_ok("void f() { i32 a; }", "i32");
    expect_ok("void f() { i64 a; }", "i64");
    expect_ok("void f() { usize a; }", "usize");
    expect_ok("void f() { f32 a; }", "f32");
    expect_ok("void f() { f64 a; }", "f64");
    expect_ok("void f() { bool a; }", "bool");
    expect_ok("void f() { *u32 a; }", "*u32");
    expect_ok("void f() { *Task a; }", "*Task (named pointer)");
    expect_ok("void f() { ?u32 a; }", "?u32");
    expect_ok("void f() { ?*u32 a; }", "?*u32");
    expect_ok("void f() { ?*Task a; }", "?*Task");
    expect_ok("void f() { []u8 a; }", "[]u8");
    expect_ok("void f() { []Task a; }", "[]Task");
    expect_ok("void f() { u8[256] a; }", "u8[256]");
    expect_ok("void f() { Task[8] a; }", "Task[8] (named array)");
    expect_ok("void f() { const u32 a = 5; }", "const u32");
    expect_ok("void f() { const []u8 a = \"hi\"; }", "const []u8");
    expect_ok("void f() { static u32 a; }", "static u32");
    expect_ok("void f() { Pool(Task, 8) a; }", "Pool(Task, 8)");
    expect_ok("void f() { Ring(u8, 256) a; }", "Ring(u8, 256)");
    expect_ok("void f() { Handle(Task) a; }", "Handle(Task)");
    expect_ok("void f() { Arena a; }", "Arena");
}

/* ================================================================
 * EDGE CASE: Complex expressions
 * ================================================================ */
static void test_complex_expressions(void) {
    printf("[complex expressions]\n");

    /* Chained method calls */
    expect_ok(
        "void f() { a.b.c.d(); }",
        "chained field access + call");

    /* Nested function calls */
    expect_ok(
        "void f() { foo(bar(baz())); }",
        "nested calls");

    /* Complex assignment targets */
    expect_ok(
        "void f() { arr[i] = 5; }",
        "array index assignment");
    expect_ok(
        "void f() { obj.field = 5; }",
        "field assignment");
    expect_ok(
        "void f() { arr[i].field = 5; }",
        "index then field assignment");

    /* Compound assignment */
    expect_ok(
        "void f() { x += 1; }",
        "+= assignment");
    expect_ok(
        "void f() { x -= 1; }",
        "-= assignment");
    expect_ok(
        "void f() { x *= 2; }",
        "*= assignment");
    expect_ok(
        "void f() { x <<= 3; }",
        "<<= assignment");
    expect_ok(
        "void f() { x >>= 1; }",
        ">>= assignment");
    expect_ok(
        "void f() { x &= 0xFF; }",
        "&= assignment");
    expect_ok(
        "void f() { x |= (1 << 5); }",
        "|= assignment");

    /* Bit manipulation */
    expect_ok(
        "void f() { u32 v = a & b | c ^ d; }",
        "bitwise operators");
    expect_ok(
        "void f() { u32 v = ~a & 0xFF; }",
        "bitwise not + and");

    /* Comparison chains */
    expect_ok(
        "void f() { bool b = x >= 10 && y != 0 || z < 5; }",
        "complex boolean");

    /* Nested parens */
    expect_ok(
        "void f() { u32 v = ((a + b) * (c - d)) / e; }",
        "deeply nested parens");

    /* Orelse chains */
    expect_ok(
        "void f() { u32 v = a() orelse b() orelse 0; }",
        "chained orelse");

    /* Address of field */
    expect_ok(
        "void f() { *u32 p = &obj.field; }",
        "address of field");
}

/* ================================================================
 * EDGE CASE: Volatile variable declarations
 * ================================================================ */
static void test_volatile(void) {
    printf("[volatile declarations]\n");

    expect_ok(
        "volatile *u32 reg;",
        "volatile global pointer");
    expect_ok(
        "void f() { volatile u32 x; }",
        "volatile local");
}

/* ================================================================
 * EDGE CASE: Function pointer types
 * void (*callback)(i32 event, *opaque ctx);
 * ================================================================ */
static void test_function_pointers(void) {
    printf("[function pointers]\n");

    /* global function pointer variable */
    expect_ok(
        "u32 (*callback)(u32, u32);",
        "global function pointer declaration");

    /* function pointer as parameter */
    expect_ok(
        "u32 apply(u32 (*op)(u32, u32), u32 x, u32 y) {\n"
        "    return op(x, y);\n"
        "}\n",
        "function pointer parameter");

    /* local function pointer variable */
    expect_ok(
        "u32 add(u32 a, u32 b) { return a + b; }\n"
        "void f() {\n"
        "    u32 (*fn)(u32, u32) = add;\n"
        "    fn(1, 2);\n"
        "}\n",
        "local function pointer variable");

    /* struct with function pointer field */
    expect_ok(
        "struct Ops {\n"
        "    u32 (*compute)(u32, u32);\n"
        "    void (*notify)(u32);\n"
        "}\n",
        "struct with function pointer fields");

    /* void function pointer */
    expect_ok(
        "void (*handler)(u32 event);\n"
        "void register_cb(void (*cb)(u32 event)) {\n"
        "    handler = cb;\n"
        "}\n",
        "void function pointer + callback registration");
}

/* ================================================================
 * EDGE CASE: Switch edge cases
 * ================================================================ */
static void test_switch_edge_cases(void) {
    printf("[switch edge cases]\n");

    /* Multi-value arms */
    expect_ok(
        "void f() {\n"
        "    switch (x) {\n"
        "        0, 1, 2 => idle(),\n"
        "        3, 4 => active(),\n"
        "        default => error(),\n"
        "    }\n"
        "}\n",
        "multi-value arms");

    /* Block bodies */
    expect_ok(
        "void f() {\n"
        "    switch (x) {\n"
        "        .idle => { start(); log(); },\n"
        "        .done => { cleanup(); },\n"
        "    }\n"
        "}\n",
        "block arm bodies");

    /* Bool switch */
    expect_ok(
        "void f() {\n"
        "    switch (ready) {\n"
        "        true => proceed(),\n"
        "        false => wait(),\n"
        "    }\n"
        "}\n",
        "bool switch");
}

/* ================================================================
 * EDGE CASE: Defer edge cases
 * ================================================================ */
static void test_defer_edge_cases(void) {
    printf("[defer edge cases]\n");

    /* Defer method call */
    expect_ok(
        "void f() { defer mutex.unlock(); }",
        "defer method call");

    /* Defer with block containing multiple stmts */
    expect_ok(
        "void f() {\n"
        "    defer {\n"
        "        cs_high();\n"
        "        mutex_unlock();\n"
        "    }\n"
        "}\n",
        "defer block multiple stmts");
}

/* ================================================================
 * EDGE CASE: Orelse with block
 * ================================================================ */
static void test_orelse_edge_cases(void) {
    printf("[orelse edge cases]\n");

    expect_ok(
        "void f() {\n"
        "    queue.push_checked(cmd) orelse {\n"
        "        log_error();\n"
        "        report();\n"
        "    };\n"
        "}\n",
        "orelse block");

    expect_ok(
        "void f() { u32 v = read() orelse continue; }",
        "orelse continue");
}

/* ================================================================
 * EDGE CASE: Nested control flow
 * ================================================================ */
static void test_nested_control_flow(void) {
    printf("[nested control flow]\n");

    expect_ok(
        "void f() {\n"
        "    if (a) {\n"
        "        if (b) {\n"
        "            if (c) { x = 1; }\n"
        "        }\n"
        "    }\n"
        "}\n",
        "triple nested if");

    expect_ok(
        "void f() {\n"
        "    for (u32 i = 0; i < 10; i += 1) {\n"
        "        while (running) {\n"
        "            if (done) { break; }\n"
        "        }\n"
        "    }\n"
        "}\n",
        "for + while + if + break");

    expect_ok(
        "void f() {\n"
        "    if (a) |val| {\n"
        "        if (b) |val2| {\n"
        "            process(val, val2);\n"
        "        }\n"
        "    }\n"
        "}\n",
        "nested if-unwrap captures");

    /* else if chain */
    expect_ok(
        "void f() {\n"
        "    if (a) { x = 1; }\n"
        "    else if (b) { x = 2; }\n"
        "    else if (c) { x = 3; }\n"
        "    else { x = 4; }\n"
        "}\n",
        "else if chain");
}

/* ================================================================
 * EDGE CASE: Realistic complete programs from spec
 * ================================================================ */
static void test_full_programs(void) {
    printf("[full programs from spec]\n");

    /* Struct + function + Pool + Handle */
    expect_ok(
        "struct Task {\n"
        "    u32 pid;\n"
        "    u32 priority;\n"
        "}\n"
        "\n"
        "Pool(Task, 8) tasks;\n"
        "\n"
        "void main() {\n"
        "    Handle(Task) h = tasks.alloc() orelse return;\n"
        "    tasks.get(h).priority = 5;\n"
        "    tasks.get(h).pid = 42;\n"
        "    tasks.free(h);\n"
        "}\n",
        "struct + pool + handle");

    /* Enum + switch */
    expect_ok(
        "enum TaskState {\n"
        "    idle,\n"
        "    running,\n"
        "    blocked,\n"
        "    done,\n"
        "}\n"
        "\n"
        "void process(u32 state) {\n"
        "    switch (state) {\n"
        "        .idle => start(),\n"
        "        .running => wait(),\n"
        "        .blocked => retry(),\n"
        "        .done => cleanup(),\n"
        "    }\n"
        "}\n",
        "enum + exhaustive switch");

    /* Union + switch + captures */
    expect_ok(
        "union Message {\n"
        "    u32 sensor;\n"
        "    u32 command;\n"
        "}\n"
        "\n"
        "void handle(u32 msg) {\n"
        "    switch (msg) {\n"
        "        .sensor => |data| { log(data); },\n"
        "        .command => |*cmd| { execute(cmd); },\n"
        "    }\n"
        "}\n",
        "union + switch + captures");

    /* Import + interrupt + ring */
    expect_ok(
        "import gpio;\n"
        "\n"
        "Ring(u8, 256) rx_buf;\n"
        "\n"
        "interrupt USART1 as \"USART1_IRQHandler\" {\n"
        "    u8 byte = 0xFF;\n"
        "    rx_buf.push(byte);\n"
        "}\n",
        "import + interrupt + ring");

    /* Typedef + distinct */
    expect_ok(
        "typedef u32 Milliseconds;\n"
        "distinct typedef u32 Celsius;\n"
        "distinct typedef u32 Fahrenheit;\n"
        "\n"
        "void main() {\n"
        "    Milliseconds ms = 1000;\n"
        "    Celsius c = 25;\n"
        "}\n",
        "typedef + distinct typedef");

    /* Packed struct */
    expect_ok(
        "packed struct SensorPacket {\n"
        "    u8 id;\n"
        "    u16 temperature;\n"
        "    u16 pressure;\n"
        "    u8 checksum;\n"
        "}\n",
        "packed struct");

    /* Defer pattern */
    expect_ok(
        "void transfer() {\n"
        "    mutex_lock();\n"
        "    defer mutex_unlock();\n"
        "    cs_low();\n"
        "    defer cs_high();\n"
        "    if (error()) { return; }\n"
        "    do_work();\n"
        "}\n",
        "defer cleanup pattern");

    /* Complex UART read */
    expect_ok(
        "?u32 uart_read([]u8 buf) {\n"
        "    u32 count = 0;\n"
        "    while (count < buf.len) {\n"
        "        if (rx_buf.pop()) |byte| {\n"
        "            buf[count] = byte;\n"
        "            count += 1;\n"
        "        } else {\n"
        "            break;\n"
        "        }\n"
        "    }\n"
        "    if (count == 0) { return null; }\n"
        "    return count;\n"
        "}\n",
        "UART read with optional unwrap");

    /* Orelse chain */
    expect_ok(
        "void main() {\n"
        "    u32 val = a() orelse b() orelse return;\n"
        "}\n",
        "orelse chain with return");

    /* Intrinsics */
    expect_ok(
        "void main() {\n"
        "    u16 small = @truncate(u32, big);\n"
        "    u32 addr = @ptrtoint(ptr);\n"
        "    @barrier();\n"
        "}\n",
        "intrinsics");

    /* Asm */
    expect_ok(
        "void disable_irq() {\n"
        "    asm(\"cpsid i\");\n"
        "}\n",
        "asm");

    /* Static function */
    expect_ok(
        "static void configure_pins() {\n"
        "    u32 x = 0;\n"
        "}\n",
        "static function");

    /* Keep parameter */
    expect_ok(
        "void register_cb(keep *opaque ctx) {\n"
        "}\n",
        "keep parameter");
}

/* ================================================================ */

int main(void) {
    printf("=== ZER Parser Edge Case Tests ===\n\n");

    test_named_type_var_decl();
    test_pointer_type_vs_deref();
    test_all_type_var_decls();
    test_complex_expressions();
    test_volatile();
    test_function_pointers();
    test_switch_edge_cases();
    test_defer_edge_cases();
    test_orelse_edge_cases();
    test_nested_control_flow();
    test_full_programs();

    printf("\n=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(", %d FAILED", tests_failed);
    }
    printf(" ===\n");

    return tests_failed > 0 ? 1 : 0;
}
