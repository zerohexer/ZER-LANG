#include <stdio.h>
#include <string.h>
#include "lexer.h"
#include "ast.h"
#include "parser.h"

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

static Node *parse_source(const char *source, Arena *arena) {
    Scanner scanner;
    scanner_init(&scanner, source);
    Parser parser;
    parser_init(&parser, &scanner, arena, "test");
    Node *file = parse_file(&parser);
    if (parser.had_error) return NULL;
    return file;
}

static void expect_parse_ok(const char *source, const char *test_name) {
    Arena arena;
    arena_init(&arena, 64 * 1024);
    tests_run++;
    Node *file = parse_source(source, &arena);
    if (file && file->kind == NODE_FILE) {
        tests_passed++;
    } else {
        printf("  FAIL: %s — parse error\n", test_name);
        tests_failed++;
    }
    arena_free(&arena);
}

static void expect_node_kind(const char *source, NodeKind expected, int decl_index,
                             const char *test_name) {
    Arena arena;
    arena_init(&arena, 64 * 1024);
    tests_run++;
    Node *file = parse_source(source, &arena);
    if (!file || file->kind != NODE_FILE) {
        printf("  FAIL: %s — parse error\n", test_name);
        tests_failed++;
        arena_free(&arena);
        return;
    }
    if (decl_index >= file->file.decl_count) {
        printf("  FAIL: %s — not enough declarations (got %d, need %d)\n",
               test_name, file->file.decl_count, decl_index + 1);
        tests_failed++;
        arena_free(&arena);
        return;
    }
    Node *decl = file->file.decls[decl_index];
    if (decl->kind != expected) {
        printf("  FAIL: %s — expected %s, got %s\n",
               test_name, node_kind_name(expected), node_kind_name(decl->kind));
        tests_failed++;
    } else {
        tests_passed++;
    }
    arena_free(&arena);
}

/* ---- Test groups ---- */

static void test_variable_declarations(void) {
    printf("[variable declarations]\n");
    expect_parse_ok("u32 x = 5;", "simple var");
    expect_parse_ok("i64 big = 100000;", "i64 var");
    expect_parse_ok("bool flag = true;", "bool var");
    expect_parse_ok("f32 pi = 3.14;", "float var");
    expect_parse_ok("const u32 MAX = 255;", "const var");
    expect_parse_ok("u8[256] buf;", "array var");
    expect_parse_ok("[]u8 data;", "slice var");
    expect_parse_ok("*u32 ptr;", "pointer var");
    expect_parse_ok("?*u32 maybe;", "optional pointer var");
    expect_parse_ok("?u32 result;", "optional value var");
    expect_parse_ok("Pool(Task, 8) tasks;", "pool var");
    expect_parse_ok("Ring(u8, 256) rx_buf;", "ring var");
    expect_parse_ok("Handle(Task) h;", "handle var");
    expect_parse_ok("Arena scratch;", "arena var");
    expect_node_kind("u32 x = 5;", NODE_GLOBAL_VAR, 0, "var is GLOBAL_VAR");
}

static void test_function_declarations(void) {
    printf("[function declarations]\n");
    expect_parse_ok(
        "u32 add(u32 a, u32 b) {\n"
        "    return a + b;\n"
        "}\n",
        "simple function");
    expect_parse_ok(
        "void process(*Task t) {\n"
        "    t.priority = 5;\n"
        "}\n",
        "pointer param function");
    expect_parse_ok(
        "?u32 uart_read([]u8 buf) {\n"
        "    if (true) { return 0; }\n"
        "    return null;\n"
        "}\n",
        "optional return function");
    expect_parse_ok(
        "void register_cb(keep *opaque ctx) {\n"
        "}\n",
        "keep param function");
    expect_node_kind("u32 add(u32 a, u32 b) { return 0; }", NODE_FUNC_DECL, 0,
                     "func is FUNC_DECL");
}

static void test_struct_declarations(void) {
    printf("[struct declarations]\n");
    expect_parse_ok(
        "struct Task {\n"
        "    u32 pid;\n"
        "    u32 priority;\n"
        "}\n",
        "simple struct");
    expect_parse_ok(
        "packed struct SensorPacket {\n"
        "    u8 id;\n"
        "    u16 temperature;\n"
        "    u8 checksum;\n"
        "}\n",
        "packed struct");
    expect_node_kind("struct Foo { u32 x; }", NODE_STRUCT_DECL, 0, "struct is STRUCT_DECL");
}

static void test_enum_declarations(void) {
    printf("[enum declarations]\n");
    expect_parse_ok(
        "enum TaskState {\n"
        "    idle,\n"
        "    running,\n"
        "    blocked,\n"
        "    done,\n"
        "}\n",
        "simple enum");
    expect_node_kind("enum Color { red, green, blue }", NODE_ENUM_DECL, 0,
                     "enum is ENUM_DECL");
}

static void test_union_declarations(void) {
    printf("[union declarations]\n");
    expect_parse_ok(
        "union Message {\n"
        "    u32 sensor;\n"
        "    u32 command;\n"
        "}\n",
        "simple union");
    expect_node_kind("union Msg { u32 a; u32 b; }", NODE_UNION_DECL, 0,
                     "union is UNION_DECL");
}

static void test_typedef(void) {
    printf("[typedef]\n");
    expect_parse_ok("typedef u32 Milliseconds;", "simple typedef");
    expect_parse_ok("distinct typedef u32 Celsius;", "distinct typedef");
    expect_node_kind("typedef u32 Ms;", NODE_TYPEDEF, 0, "typedef is TYPEDEF");
}

static void test_import(void) {
    printf("[import]\n");
    expect_parse_ok("import uart;", "simple import");
    expect_node_kind("import gpio;", NODE_IMPORT, 0, "import is IMPORT");
}

static void test_interrupt(void) {
    printf("[interrupt]\n");
    expect_parse_ok("interrupt SysTick { }", "simple interrupt");
    expect_parse_ok("interrupt UART_1 as \"USART1_IRQHandler\" { }", "interrupt with as");
    expect_node_kind("interrupt Foo { }", NODE_INTERRUPT, 0, "interrupt is INTERRUPT");
}

static void test_expressions(void) {
    printf("[expressions]\n");
    expect_parse_ok("u32 x = 1 + 2;", "addition");
    expect_parse_ok("u32 x = 1 + 2 * 3;", "precedence");
    expect_parse_ok("u32 x = (1 + 2) * 3;", "parenthesized");
    expect_parse_ok("bool b = x > 5 && y < 10;", "logical and comparison");
    expect_parse_ok("u32 x = a | b & c;", "bitwise");
    expect_parse_ok("u32 x = a << 3;", "shift");
    expect_parse_ok("u32 x = ~a;", "bitwise not");
    expect_parse_ok("u32 x = -5;", "unary minus");
    expect_parse_ok("bool b = !flag;", "logical not");
    expect_parse_ok("u32 x = *ptr;", "dereference");
    expect_parse_ok("*u32 p = &val;", "address of");
}

static void test_function_calls(void) {
    printf("[function calls]\n");
    expect_parse_ok(
        "void main() {\n"
        "    foo();\n"
        "    bar(1, 2);\n"
        "    baz(a, b, c);\n"
        "}\n",
        "basic calls");
    expect_parse_ok(
        "void main() {\n"
        "    t.run();\n"
        "    tasks.alloc();\n"
        "    tasks.get(h).priority = 5;\n"
        "}\n",
        "method-style calls");
}

static void test_control_flow(void) {
    printf("[control flow]\n");
    expect_parse_ok(
        "void main() {\n"
        "    if (x > 5) { y = 1; }\n"
        "}\n",
        "simple if");
    expect_parse_ok(
        "void main() {\n"
        "    if (x > 5) { y = 1; } else { y = 2; }\n"
        "}\n",
        "if-else");
    expect_parse_ok(
        "void main() {\n"
        "    if (maybe) |val| { process(val); }\n"
        "}\n",
        "if-unwrap with capture");
    expect_parse_ok(
        "void main() {\n"
        "    if (maybe) |*val| { *val = 5; }\n"
        "}\n",
        "if-unwrap with pointer capture");
    expect_parse_ok(
        "void main() {\n"
        "    for (u32 i = 0; i < 10; i += 1) { process(i); }\n"
        "}\n",
        "for loop");
    expect_parse_ok(
        "void main() {\n"
        "    while (running) { poll(); }\n"
        "}\n",
        "while loop");
}

static void test_switch(void) {
    printf("[switch]\n");
    expect_parse_ok(
        "void main() {\n"
        "    switch (state) {\n"
        "        .idle => start(),\n"
        "        .running => stop(),\n"
        "    }\n"
        "}\n",
        "enum switch");
    expect_parse_ok(
        "void main() {\n"
        "    switch (x) {\n"
        "        0 => foo(),\n"
        "        1 => bar(),\n"
        "        default => baz(),\n"
        "    }\n"
        "}\n",
        "integer switch with default");
    expect_parse_ok(
        "void main() {\n"
        "    switch (msg) {\n"
        "        .sensor => |data| { process(data); },\n"
        "        .command => |*cmd| { execute(cmd); },\n"
        "    }\n"
        "}\n",
        "union switch with captures");
}

static void test_orelse(void) {
    printf("[orelse]\n");
    expect_parse_ok(
        "void main() {\n"
        "    u32 val = read() orelse 0;\n"
        "}\n",
        "orelse value");
    expect_parse_ok(
        "void main() {\n"
        "    u32 val = read() orelse return;\n"
        "}\n",
        "orelse return");
    expect_parse_ok(
        "void main() {\n"
        "    u32 val = read() orelse break;\n"
        "}\n",
        "orelse break");
}

static void test_defer(void) {
    printf("[defer]\n");
    expect_parse_ok(
        "void main() {\n"
        "    defer cleanup();\n"
        "}\n",
        "defer expression");
    expect_parse_ok(
        "void main() {\n"
        "    defer { a(); b(); }\n"
        "}\n",
        "defer block");
}

static void test_asm(void) {
    printf("[asm]\n");
    expect_parse_ok(
        "naked void reset() {\n"
        "    unsafe asm(\"nop\");\n"
        "}\n"
        "i32 main() { return 0; }\n",
        "unsafe asm statement");
}

static void test_intrinsics(void) {
    printf("[intrinsics]\n");
    expect_parse_ok(
        "void main() {\n"
        "    u32 s = @size(u32);\n"
        "}\n",
        "@size intrinsic");
    expect_parse_ok(
        "void main() {\n"
        "    @barrier();\n"
        "}\n",
        "@barrier intrinsic");
}

static void test_indexing_and_slicing(void) {
    printf("[indexing and slicing]\n");
    expect_parse_ok(
        "void main() {\n"
        "    u32 x = arr[0];\n"
        "    u32 y = arr[i];\n"
        "}\n",
        "array indexing");
    expect_parse_ok(
        "void main() {\n"
        "    []u8 sub = buf[0..5];\n"
        "}\n",
        "slice with range");
    expect_parse_ok(
        "void main() {\n"
        "    []u8 rest = buf[2..];\n"
        "}\n",
        "slice from start");
}

static void test_realistic_code(void) {
    printf("[realistic code]\n");
    expect_parse_ok(
        "import gpio;\n"
        "\n"
        "Ring(u8, 256) rx_buf;\n"
        "\n"
        "void uart_init(u32 baud) {\n"
        "    u32 brr = 16000000 / baud;\n"
        "}\n"
        "\n"
        "interrupt USART1 {\n"
        "    u8 byte = @truncate(u32, 0xFF);\n"
        "    rx_buf.push(byte);\n"
        "}\n"
        "\n"
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
        "UART driver fragment");
}

static void test_ast_print(void) {
    printf("[ast print]\n");
    Arena arena;
    arena_init(&arena, 64 * 1024);
    const char *source =
        "u32 add(u32 a, u32 b) {\n"
        "    return a + b;\n"
        "}\n";
    Node *file = parse_source(source, &arena);
    tests_run++;
    if (file) {
        printf("  --- AST dump ---\n");
        ast_print(file, 1);
        printf("  --- end dump ---\n");
        tests_passed++;
    } else {
        printf("  FAIL: ast_print — parse error\n");
        tests_failed++;
    }
    arena_free(&arena);
}

/* ================================================================ */

int main(void) {
    printf("=== ZER Parser Unit Tests ===\n\n");

    test_variable_declarations();
    test_function_declarations();
    test_struct_declarations();
    test_enum_declarations();
    test_union_declarations();
    test_typedef();
    test_import();
    test_interrupt();
    test_expressions();
    test_function_calls();
    test_control_flow();
    test_switch();
    test_orelse();
    test_defer();
    test_asm();
    test_intrinsics();
    test_indexing_and_slicing();
    test_realistic_code();
    test_ast_print();

    printf("\n=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(", %d FAILED", tests_failed);
    }
    printf(" ===\n");

    return tests_failed > 0 ? 1 : 0;
}
