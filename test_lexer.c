#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "lexer.h"

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

/* expect exactly one token of a given type, then EOF */
static void expect_single(const char *input, TokenType expected, const char *test_name) {
    Scanner s;
    scanner_init(&s, input);
    Token t = next_token(&s);
    tests_run++;
    if (t.type != expected) {
        printf("  FAIL: %s — expected %s, got %s ('%.*s')\n",
               test_name, token_type_name(expected), token_type_name(t.type),
               (int)t.length, t.start);
        tests_failed++;
    } else {
        tests_passed++;
    }
}

/* expect a sequence of token types, terminated by TOK_EOF */
static void expect_sequence(const char *input, TokenType *expected, int count, const char *test_name) {
    Scanner s;
    scanner_init(&s, input);
    tests_run++;
    for (int i = 0; i < count; i++) {
        Token t = next_token(&s);
        if (t.type != expected[i]) {
            printf("  FAIL: %s — token %d: expected %s, got %s ('%.*s')\n",
                   test_name, i, token_type_name(expected[i]), token_type_name(t.type),
                   (int)t.length, t.start);
            tests_failed++;
            return;
        }
    }
    Token eof = next_token(&s);
    if (eof.type != TOK_EOF) {
        printf("  FAIL: %s — expected EOF after %d tokens, got %s\n",
               test_name, count, token_type_name(eof.type));
        tests_failed++;
        return;
    }
    tests_passed++;
}

/* expect a token with specific lexeme text */
static void expect_token_text(const char *input, TokenType expected_type,
                              const char *expected_text, const char *test_name) {
    Scanner s;
    scanner_init(&s, input);
    Token t = next_token(&s);
    tests_run++;
    if (t.type != expected_type) {
        printf("  FAIL: %s — expected type %s, got %s\n",
               test_name, token_type_name(expected_type), token_type_name(t.type));
        tests_failed++;
        return;
    }
    size_t expected_len = strlen(expected_text);
    if (t.length != expected_len || memcmp(t.start, expected_text, expected_len) != 0) {
        printf("  FAIL: %s — expected text '%s', got '%.*s'\n",
               test_name, expected_text, (int)t.length, t.start);
        tests_failed++;
        return;
    }
    tests_passed++;
}

/* expect token on a specific line number */
static void expect_line(const char *input, int skip, int expected_line, const char *test_name) {
    Scanner s;
    scanner_init(&s, input);
    Token t;
    for (int i = 0; i <= skip; i++) {
        t = next_token(&s);
    }
    tests_run++;
    if (t.line != expected_line) {
        printf("  FAIL: %s — expected line %d, got %d\n",
               test_name, expected_line, t.line);
        tests_failed++;
    } else {
        tests_passed++;
    }
}

/* ================================================================ */

static void test_all_keywords(void) {
    printf("[keywords]\n");
    expect_single("u8", TOK_U8, "u8");
    expect_single("u16", TOK_U16, "u16");
    expect_single("u32", TOK_U32, "u32");
    expect_single("u64", TOK_U64, "u64");
    expect_single("i8", TOK_I8, "i8");
    expect_single("i16", TOK_I16, "i16");
    expect_single("i32", TOK_I32, "i32");
    expect_single("i64", TOK_I64, "i64");
    expect_single("usize", TOK_USIZE, "usize");
    expect_single("f32", TOK_F32, "f32");
    expect_single("f64", TOK_F64, "f64");
    expect_single("bool", TOK_BOOL, "bool");
    expect_single("void", TOK_VOID, "void");
    expect_single("opaque", TOK_OPAQUE, "opaque");
    expect_single("struct", TOK_STRUCT, "struct");
    expect_single("packed", TOK_PACKED, "packed");
    expect_single("enum", TOK_ENUM, "enum");
    expect_single("union", TOK_UNION, "union");
    expect_single("const", TOK_CONST, "const");
    expect_single("typedef", TOK_TYPEDEF, "typedef");
    expect_single("distinct", TOK_DISTINCT, "distinct");
    expect_single("if", TOK_IF, "if");
    expect_single("else", TOK_ELSE, "else");
    expect_single("for", TOK_FOR, "for");
    expect_single("while", TOK_WHILE, "while");
    expect_single("switch", TOK_SWITCH, "switch");
    expect_single("break", TOK_BREAK, "break");
    expect_single("continue", TOK_CONTINUE, "continue");
    expect_single("return", TOK_RETURN, "return");
    expect_single("default", TOK_DEFAULT, "default");
    expect_single("orelse", TOK_ORELSE, "orelse");
    expect_single("null", TOK_NULL, "null");
    expect_single("true", TOK_TRUE, "true");
    expect_single("false", TOK_FALSE, "false");
    expect_single("Pool", TOK_POOL, "Pool");
    expect_single("Ring", TOK_RING, "Ring");
    expect_single("Arena", TOK_ARENA, "Arena");
    expect_single("Handle", TOK_HANDLE, "Handle");
    expect_single("defer", TOK_DEFER, "defer");
    expect_single("import", TOK_IMPORT, "import");
    expect_single("volatile", TOK_VOLATILE, "volatile");
    expect_single("interrupt", TOK_INTERRUPT, "interrupt");
    expect_single("asm", TOK_ASM, "asm");
    /* `unsafe` keyword removed 2026-04-25 — `unsafe asm` renamed to bare `asm` */
    expect_single("static", TOK_STATIC, "static");
    expect_single("keep", TOK_KEEP, "keep");
    expect_single("as", TOK_AS, "as");
}

static void test_keyword_prefixes_are_identifiers(void) {
    printf("[keyword prefixes as identifiers]\n");
    /* words that start like keywords but aren't */
    expect_single("u8x", TOK_IDENT, "u8x is ident");
    expect_single("u32_val", TOK_IDENT, "u32_val is ident");
    expect_single("i8bit", TOK_IDENT, "i8bit is ident");
    expect_single("if_cond", TOK_IDENT, "if_cond is ident");
    expect_single("forks", TOK_IDENT, "forks is ident");
    expect_single("returning", TOK_IDENT, "returning is ident");
    expect_single("constant", TOK_IDENT, "constant is ident");
    expect_single("structure", TOK_IDENT, "structure is ident");
    expect_single("Pooling", TOK_IDENT, "Pooling is ident");
    expect_single("Handlebar", TOK_IDENT, "Handlebar is ident");
    expect_single("trueish", TOK_IDENT, "trueish is ident");
    expect_single("nullable", TOK_IDENT, "nullable is ident");
    expect_single("_private", TOK_IDENT, "_private is ident");
    expect_single("__double", TOK_IDENT, "__double is ident");
    expect_single("_", TOK_IDENT, "underscore is ident");
}

static void test_number_literals(void) {
    printf("[number literals]\n");
    expect_single("0", TOK_NUMBER_INT, "zero");
    expect_single("42", TOK_NUMBER_INT, "decimal int");
    expect_single("999999", TOK_NUMBER_INT, "large decimal");
    expect_single("0xFF", TOK_NUMBER_INT, "hex uppercase");
    expect_single("0xDEAD", TOK_NUMBER_INT, "hex DEAD");
    expect_single("0xab", TOK_NUMBER_INT, "hex lowercase");
    expect_single("0x0", TOK_NUMBER_INT, "hex zero");
    expect_single("0b0", TOK_NUMBER_INT, "binary zero");
    expect_single("0b1010", TOK_NUMBER_INT, "binary 1010");
    expect_single("0b11111111", TOK_NUMBER_INT, "binary 8 bits");
    expect_single("3.14", TOK_NUMBER_FLOAT, "float 3.14");
    expect_single("0.5", TOK_NUMBER_FLOAT, "float 0.5");
    expect_single("100.0", TOK_NUMBER_FLOAT, "float 100.0");

    /* underscore separators */
    expect_single("1_000_000", TOK_NUMBER_INT, "decimal underscores");
    expect_single("0xFF_FF", TOK_NUMBER_INT, "hex underscores");
    expect_single("0b1010_0101", TOK_NUMBER_INT, "binary underscores");
    expect_single("3.141_592", TOK_NUMBER_FLOAT, "float underscores");
    expect_token_text("1_000", TOK_NUMBER_INT, "1_000", "underscore lexeme");

    /* lexeme correctness */
    expect_token_text("0xFF", TOK_NUMBER_INT, "0xFF", "hex lexeme");
    expect_token_text("0b1010", TOK_NUMBER_INT, "0b1010", "binary lexeme");
    expect_token_text("3.14", TOK_NUMBER_FLOAT, "3.14", "float lexeme");

    /* empty hex/binary errors */
    expect_single("0x", TOK_ERROR, "empty hex is error");
    expect_single("0b", TOK_ERROR, "empty binary is error");
    expect_single("0xG", TOK_ERROR, "hex with only invalid digit is error");
    expect_single("0b2", TOK_ERROR, "binary with only invalid digit is error");
}

static void test_number_followed_by_dot(void) {
    printf("[number followed by dot]\n");
    /* 5.method() — 5 is int, then dot, not float */
    TokenType expected[] = { TOK_NUMBER_INT, TOK_DOT, TOK_IDENT };
    expect_sequence("5.method", expected, 3, "int.method not float");
}

static void test_string_literals(void) {
    printf("[string literals]\n");
    expect_single("\"hello\"", TOK_STRING, "simple string");
    expect_single("\"\"", TOK_STRING, "empty string");
    expect_single("\"hello world\"", TOK_STRING, "string with space");
    expect_single("\"escaped \\\"quote\\\"\"", TOK_STRING, "escaped quotes");
    expect_single("\"tab\\there\"", TOK_STRING, "escaped tab");
    expect_single("\"newline\\nhere\"", TOK_STRING, "escaped newline");
    expect_single("\"backslash\\\\end\"", TOK_STRING, "escaped backslash");

    /* lexeme includes quotes */
    expect_token_text("\"hello\"", TOK_STRING, "\"hello\"", "string includes quotes");
}

static void test_string_escapes(void) {
    printf("[string escape validation]\n");
    /* valid escapes */
    expect_single("\"\\n\"", TOK_STRING, "escape n");
    expect_single("\"\\t\"", TOK_STRING, "escape t");
    expect_single("\"\\r\"", TOK_STRING, "escape r");
    expect_single("\"\\\\\"", TOK_STRING, "escape backslash");
    expect_single("\"\\\"\"", TOK_STRING, "escape quote");
    expect_single("\"\\0\"", TOK_STRING, "escape null");
    expect_single("\"\\x0A\"", TOK_STRING, "escape hex");
    expect_single("\"\\xFF\"", TOK_STRING, "escape hex FF");

    /* invalid escapes */
    expect_single("\"\\q\"", TOK_ERROR, "invalid escape q");
    expect_single("\"\\a\"", TOK_ERROR, "invalid escape a");
    expect_single("\"\\1\"", TOK_ERROR, "invalid escape digit");

    /* bad hex escapes */
    expect_single("\"\\xGG\"", TOK_ERROR, "hex escape bad digits");
    expect_single("\"\\x0\"", TOK_ERROR, "hex escape only one digit");
}

static void test_char_literals(void) {
    printf("[character literals]\n");
    expect_single("'a'", TOK_CHAR, "simple char");
    expect_single("'Z'", TOK_CHAR, "uppercase char");
    expect_single("'0'", TOK_CHAR, "digit char");
    expect_single("' '", TOK_CHAR, "space char");

    /* escape sequences */
    expect_single("'\\n'", TOK_CHAR, "char escape n");
    expect_single("'\\t'", TOK_CHAR, "char escape t");
    expect_single("'\\r'", TOK_CHAR, "char escape r");
    expect_single("'\\0'", TOK_CHAR, "char escape null");
    expect_single("'\\\\'" , TOK_CHAR, "char escape backslash");
    expect_single("'\\''", TOK_CHAR, "char escape single quote");
    expect_single("'\\x0A'", TOK_CHAR, "char hex escape");
    expect_single("'\\xFF'", TOK_CHAR, "char hex FF");

    /* lexeme correctness */
    expect_token_text("'a'", TOK_CHAR, "'a'", "char lexeme");
    expect_token_text("'\\n'", TOK_CHAR, "'\\n'", "char escape lexeme");

    /* errors */
    expect_single("''", TOK_ERROR, "empty char literal");
    expect_single("'", TOK_ERROR, "unterminated char");
    expect_single("'ab'", TOK_ERROR, "multi-char literal");
    expect_single("'\\q'", TOK_ERROR, "char invalid escape");
    expect_single("'\\xGG'", TOK_ERROR, "char bad hex escape");
}

static void test_unterminated_string(void) {
    printf("[unterminated string]\n");
    expect_single("\"oops", TOK_ERROR, "unterminated string is error");
}

static void test_all_single_char_tokens(void) {
    printf("[single char tokens]\n");
    expect_single("(", TOK_LPAREN, "lparen");
    expect_single(")", TOK_RPAREN, "rparen");
    expect_single("{", TOK_LBRACE, "lbrace");
    expect_single("}", TOK_RBRACE, "rbrace");
    expect_single("[", TOK_LBRACKET, "lbracket");
    expect_single("]", TOK_RBRACKET, "rbracket");
    expect_single(";", TOK_SEMICOLON, "semicolon");
    expect_single(",", TOK_COMMA, "comma");
    expect_single("~", TOK_TILDE, "tilde");
    expect_single("@", TOK_AT, "at");
    expect_single("?", TOK_QUESTION, "question");
    expect_single(".", TOK_DOT, "dot");
    expect_single("+", TOK_PLUS, "plus");
    expect_single("-", TOK_MINUS, "minus");
    expect_single("*", TOK_STAR, "star");
    expect_single("/", TOK_SLASH, "slash");
    expect_single("%", TOK_PERCENT, "percent");
    expect_single("&", TOK_AMP, "amp");
    expect_single("|", TOK_PIPE, "pipe");
    expect_single("^", TOK_CARET, "caret");
    expect_single("!", TOK_BANG, "bang");
    expect_single("=", TOK_EQ, "eq");
    expect_single("<", TOK_LT, "lt");
    expect_single(">", TOK_GT, "gt");
}

static void test_two_char_operators(void) {
    printf("[two char operators]\n");
    expect_single("..", TOK_DOTDOT, "dotdot");
    expect_single("==", TOK_EQEQ, "eqeq");
    expect_single("!=", TOK_BANGEQ, "bangeq");
    expect_single("<=", TOK_LTEQ, "lteq");
    expect_single(">=", TOK_GTEQ, "gteq");
    expect_single("<<", TOK_LSHIFT, "lshift");
    expect_single(">>", TOK_RSHIFT, "rshift");
    expect_single("&&", TOK_AMPAMP, "ampamp");
    expect_single("||", TOK_PIPEPIPE, "pipepipe");
    expect_single("=>", TOK_ARROW, "arrow");
}

static void test_compound_assignment(void) {
    printf("[compound assignment]\n");
    expect_single("+=", TOK_PLUSEQ, "pluseq");
    expect_single("-=", TOK_MINUSEQ, "minuseq");
    expect_single("*=", TOK_STAREQ, "stareq");
    expect_single("/=", TOK_SLASHEQ, "slasheq");
    expect_single("%=", TOK_PERCENTEQ, "percenteq");
    expect_single("&=", TOK_AMPEQ, "ampeq");
    expect_single("|=", TOK_PIPEEQ, "pipeeq");
    expect_single("^=", TOK_CARETEQ, "careteq");
    expect_single("<<=", TOK_LSHIFTEQ, "lshifteq");
    expect_single(">>=", TOK_RSHIFTEQ, "rshifteq");
}

static void test_comments(void) {
    printf("[comments]\n");
    /* line comment — entire line skipped */
    expect_single("// this is a comment", TOK_EOF, "line comment only");

    /* line comment before token */
    expect_single("// comment\n42", TOK_NUMBER_INT, "line comment then token");

    /* block comment */
    expect_single("/* block */", TOK_EOF, "block comment only");

    /* block comment before token */
    expect_single("/* comment */ 42", TOK_NUMBER_INT, "block comment then token");

    /* nested-looking block comment (not nested — stops at first close) */
    TokenType expected[] = { TOK_NUMBER_INT };
    expect_sequence("/* outer /* inner */ 42", expected, 1, "block comment stops at first close");

    /* multiline block comment */
    expect_single("/* line1\nline2\nline3 */", TOK_EOF, "multiline block comment");
}

static void test_whitespace(void) {
    printf("[whitespace handling]\n");
    /* tabs, spaces, carriage returns all skipped */
    expect_single("  \t\t  42", TOK_NUMBER_INT, "spaces and tabs");
    expect_single("\r\n42", TOK_NUMBER_INT, "crlf before token");
    expect_single("   ", TOK_EOF, "only whitespace");
    expect_single("", TOK_EOF, "empty input");
    expect_single("\n\n\n", TOK_EOF, "only newlines");
}

static void test_line_tracking(void) {
    printf("[line tracking]\n");
    expect_line("x", 0, 1, "first token on line 1");
    expect_line("\nx", 0, 2, "token after newline on line 2");
    expect_line("\n\n\nx", 0, 4, "token after 3 newlines on line 4");
    expect_line("a\nb\nc", 2, 3, "third token on line 3");
    /* block comment with newlines advances line count */
    expect_line("/* \n\n */ x", 0, 3, "token after multiline block comment");
    /* line comment newline advances line */
    expect_line("// comment\nx", 0, 2, "token after line comment");
}

static void test_adjacent_operators(void) {
    printf("[adjacent operators]\n");
    /* make sure << doesn't eat = when it's not <<= */
    TokenType shift_eq[] = { TOK_LSHIFT, TOK_EQ };
    expect_sequence("<< =", shift_eq, 2, "lshift space eq");

    /* >>= is a single token */
    TokenType rshifteq[] = { TOK_RSHIFTEQ };
    expect_sequence(">>=", rshifteq, 1, "rshifteq no space");

    /* multiple operators no spaces */
    TokenType ops[] = { TOK_PLUS, TOK_MINUS, TOK_STAR };
    expect_sequence("+-*", ops, 3, "plus minus star no space");
}

static void test_complex_expressions(void) {
    printf("[complex expressions]\n");

    /* optional type: ?*Task */
    TokenType opt[] = { TOK_QUESTION, TOK_STAR, TOK_IDENT };
    expect_sequence("?*Task", opt, 3, "optional pointer type");

    /* range slice: data[0..5] */
    TokenType slice[] = { TOK_IDENT, TOK_LBRACKET, TOK_NUMBER_INT,
                          TOK_DOTDOT, TOK_NUMBER_INT, TOK_RBRACKET };
    expect_sequence("data[0..5]", slice, 6, "range slice");

    /* switch arm: => */
    TokenType sw[] = { TOK_NUMBER_INT, TOK_ARROW, TOK_IDENT,
                       TOK_LPAREN, TOK_RPAREN };
    expect_sequence("1 => foo()", sw, 5, "switch arm");

    /* memory builtin: Pool(Task, 8) */
    TokenType pool[] = { TOK_POOL, TOK_LPAREN, TOK_IDENT, TOK_COMMA,
                         TOK_NUMBER_INT, TOK_RPAREN };
    expect_sequence("Pool(Task, 8)", pool, 6, "pool builtin");

    /* chained field access: a.b.c */
    TokenType chain[] = { TOK_IDENT, TOK_DOT, TOK_IDENT, TOK_DOT, TOK_IDENT };
    expect_sequence("a.b.c", chain, 5, "chained field access");

    /* bitwise shift with assignment: x <<= 3 */
    TokenType bsa[] = { TOK_IDENT, TOK_LSHIFTEQ, TOK_NUMBER_INT };
    expect_sequence("x <<= 3", bsa, 3, "shift assign");

    /* defer with block: defer { cleanup(); } */
    TokenType def[] = { TOK_DEFER, TOK_LBRACE, TOK_IDENT, TOK_LPAREN,
                        TOK_RPAREN, TOK_SEMICOLON, TOK_RBRACE };
    expect_sequence("defer { cleanup(); }", def, 7, "defer block");

    /* inline asm */
    TokenType ia[] = { TOK_ASM, TOK_LBRACE, TOK_STRING, TOK_RBRACE };
    expect_sequence("asm { \"nop\" }", ia, 4, "inline asm");
}

static void test_edge_cases(void) {
    printf("[edge cases]\n");

    /* single zero */
    expect_token_text("0", TOK_NUMBER_INT, "0", "lone zero");

    /* 0x with valid hex followed by non-hex — stops scanning */
    expect_token_text("0xFG", TOK_NUMBER_INT, "0xF", "hex stops at non-hex char");

    /* 0b with valid binary followed by non-binary — stops scanning */
    expect_token_text("0b102", TOK_NUMBER_INT, "0b10", "binary stops at non-binary");

    /* underscores in hex/binary edge cases */
    expect_token_text("0xFF_00", TOK_NUMBER_INT, "0xFF_00", "hex underscore lexeme");
    expect_token_text("0b10_10", TOK_NUMBER_INT, "0b10_10", "binary underscore lexeme");

    /* dot-dot vs dot */
    TokenType dd[] = { TOK_DOT, TOK_DOT, TOK_DOT };
    expect_sequence(". . .", dd, 3, "three separate dots");

    TokenType dd2[] = { TOK_DOTDOT, TOK_DOT };
    expect_sequence("...", dd2, 2, "dotdot then dot");

    /* unknown character */
    expect_single("#", TOK_ERROR, "hash is error");
    expect_single("`", TOK_ERROR, "backtick is error");

    /* identifier starting with underscore followed by digit */
    expect_single("_0", TOK_IDENT, "underscore digit ident");
    expect_single("_123abc", TOK_IDENT, "underscore digits letters");
}

static void test_realistic_code(void) {
    printf("[realistic code snippets]\n");

    /* struct definition */
    TokenType st[] = { TOK_STRUCT, TOK_IDENT, TOK_LBRACE,
                       TOK_U32, TOK_IDENT, TOK_SEMICOLON,
                       TOK_BOOL, TOK_IDENT, TOK_SEMICOLON,
                       TOK_RBRACE };
    expect_sequence("struct Point { u32 x; bool active; }", st, 10, "struct def");

    /* volatile register access */
    TokenType vol[] = { TOK_VOLATILE, TOK_STAR, TOK_IDENT, TOK_EQ,
                        TOK_NUMBER_INT, TOK_SEMICOLON };
    expect_sequence("volatile *REG = 0xFF;", vol, 6, "volatile register");

    /* for loop */
    TokenType fl[] = { TOK_FOR, TOK_LPAREN, TOK_IDENT, TOK_RPAREN,
                       TOK_PIPE, TOK_IDENT, TOK_PIPE, TOK_LBRACE, TOK_RBRACE };
    expect_sequence("for (items) |item| {}", fl, 9, "for loop capture");

    /* enum with arrow */
    TokenType en[] = { TOK_ENUM, TOK_LBRACE, TOK_IDENT, TOK_ARROW,
                       TOK_NUMBER_INT, TOK_COMMA, TOK_RBRACE };
    expect_sequence("enum { A => 1, }", en, 7, "enum with arrow");

    /* import */
    TokenType imp[] = { TOK_IMPORT, TOK_STRING, TOK_SEMICOLON };
    expect_sequence("import \"std.io\";", imp, 3, "import statement");

    /* orelse chain */
    TokenType orel[] = { TOK_IDENT, TOK_LPAREN, TOK_RPAREN, TOK_ORELSE,
                         TOK_IDENT, TOK_LPAREN, TOK_RPAREN, TOK_ORELSE,
                         TOK_RETURN, TOK_SEMICOLON };
    expect_sequence("a() orelse b() orelse return;", orel, 10, "orelse chain");

    /* interrupt with as rename */
    TokenType isr[] = { TOK_INTERRUPT, TOK_IDENT, TOK_AS, TOK_STRING, TOK_LBRACE, TOK_RBRACE };
    expect_sequence("interrupt UART_1 as \"USART1_IRQHandler\" {}", isr, 6, "interrupt as rename");
}

/* ================================================================ */

int main(void) {
    printf("=== ZER Lexer Unit Tests ===\n\n");

    test_all_keywords();
    test_keyword_prefixes_are_identifiers();
    test_number_literals();
    test_number_followed_by_dot();
    test_string_literals();
    test_string_escapes();
    test_char_literals();
    test_unterminated_string();
    test_all_single_char_tokens();
    test_two_char_operators();
    test_compound_assignment();
    test_comments();
    test_whitespace();
    test_line_tracking();
    test_adjacent_operators();
    test_complex_expressions();
    test_edge_cases();
    test_realistic_code();

    printf("\n=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(", %d FAILED", tests_failed);
    }
    printf(" ===\n");

    return tests_failed > 0 ? 1 : 0;
}
