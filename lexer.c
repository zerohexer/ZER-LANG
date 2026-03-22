#include "lexer.h"
#include <string.h>
#include <stdio.h>

/* ---- Character classification ---- */

static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '_';
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_hex_digit(char c) {
    return is_digit(c) ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static bool is_alnum(char c) {
    return is_alpha(c) || is_digit(c);
}

/* ---- Scanner init ---- */

void scanner_init(Scanner *s, const char *source) {
    s->source = source;
    s->pos = 0;
    s->len = strlen(source);
    s->line = 1;
}

/* ---- Helpers ---- */

/* peek current character without advancing */
static char peek(Scanner *s) {
    if (s->pos >= s->len) return '\0';
    return s->source[s->pos];
}

/* peek one character ahead */
static char peek_next(Scanner *s) {
    if (s->pos + 1 >= s->len) return '\0';
    return s->source[s->pos + 1];
}

/* advance and return current character */
static char advance(Scanner *s) {
    if (s->pos >= s->len) return '\0';
    return s->source[s->pos++];
}

/* consume next char if it matches expected */
static bool match(Scanner *s, char expected) {
    if (s->pos >= s->len) return false;
    if (s->source[s->pos] != expected) return false;
    s->pos++;
    return true;
}

/* create a token from start position to current position */
static Token make_token(Scanner *s, TokenType type, size_t start) {
    Token t;
    t.type = type;
    t.start = &s->source[start];
    t.length = s->pos - start;
    t.line = s->line;
    return t;
}

/* create an error token with a message */
static Token error_token(Scanner *s, const char *message) {
    Token t;
    t.type = TOK_ERROR;
    t.start = message;
    t.length = strlen(message);
    t.line = s->line;
    return t;
}

/* ---- Skip whitespace and comments ---- */

static void skip_whitespace(Scanner *s) {
    for (;;) {
        char c = peek(s);
        switch (c) {
            case ' ':
            case '\t':
            case '\r':
                advance(s);
                break;
            case '\n':
                s->line++;
                advance(s);
                break;
            case '/':
                if (peek_next(s) == '/') {
                    /* line comment — skip until newline */
                    while (peek(s) != '\n' && peek(s) != '\0') advance(s);
                } else if (peek_next(s) == '*') {
                    /* block comment — skip until */
                    advance(s); /* skip / */
                    advance(s); /* skip * */
                    while (!(peek(s) == '*' && peek_next(s) == '/')) {
                        if (peek(s) == '\0') return; /* unterminated — caught later */
                        if (peek(s) == '\n') s->line++;
                        advance(s);
                    }
                    advance(s); /* skip * */
                    advance(s); /* skip / */
                } else {
                    return; /* just a slash — not whitespace */
                }
                break;
            default:
                return;
        }
    }
}

/* ---- Keyword trie ---- */
/* check if scanned word matches a keyword.
   word starts at source[start], length is word_len. */

static TokenType check_keyword(const char *word, size_t word_len) {
    /* trie: switch on first letter, narrow down, compare rest */
    switch (word[0]) {
    case 'A':
        if (word_len == 5 && memcmp(word+1, "rena", 4) == 0) return TOK_ARENA;
        break;
    case 'H':
        if (word_len == 6 && memcmp(word+1, "andle", 5) == 0) return TOK_HANDLE;
        break;
    case 'P':
        if (word_len == 4 && memcmp(word+1, "ool", 3) == 0) return TOK_POOL;
        break;
    case 'R':
        if (word_len == 4 && memcmp(word+1, "ing", 3) == 0) return TOK_RING;
        break;
    case 'a':
        if (word_len == 2 && word[1] == 's') return TOK_AS;
        if (word_len == 3 && memcmp(word+1, "sm", 2) == 0) return TOK_ASM;
        break;
    case 'b':
        if (word_len == 4 && memcmp(word+1, "ool", 3) == 0) return TOK_BOOL;
        if (word_len == 5 && memcmp(word+1, "reak", 4) == 0) return TOK_BREAK;
        break;
    case 'c':
        if (word_len == 5 && memcmp(word+1, "onst", 4) == 0) return TOK_CONST;
        if (word_len == 8 && memcmp(word+1, "ontinue", 7) == 0) return TOK_CONTINUE;
        if (word_len == 8 && memcmp(word+1, "include", 7) == 0) return TOK_CINCLUDE;
        break;
    case 'd':
        if (word_len == 5 && memcmp(word+1, "efer", 4) == 0) return TOK_DEFER;
        if (word_len == 7 && memcmp(word+1, "efault", 6) == 0) return TOK_DEFAULT;
        if (word_len == 8 && memcmp(word+1, "istinct", 7) == 0) return TOK_DISTINCT;
        break;
    case 'e':
        if (word_len == 4 && memcmp(word+1, "lse", 3) == 0) return TOK_ELSE;
        if (word_len == 4 && memcmp(word+1, "num", 3) == 0) return TOK_ENUM;
        break;
    case 'f':
        if (word_len >= 2) {
            switch (word[1]) {
            case '3':
                if (word_len == 3 && word[2] == '2') return TOK_F32;
                break;
            case '6':
                if (word_len == 3 && word[2] == '4') return TOK_F64;
                break;
            case 'a':
                if (word_len == 5 && memcmp(word+2, "lse", 3) == 0) return TOK_FALSE;
                break;
            case 'o':
                if (word_len == 3 && word[2] == 'r') return TOK_FOR;
                break;
            }
        }
        break;
    case 'i':
        if (word_len >= 2) {
            switch (word[1]) {
            case '8':
                if (word_len == 2) return TOK_I8;
                break;
            case '1':
                if (word_len == 3 && word[2] == '6') return TOK_I16;
                break;
            case '3':
                if (word_len == 3 && word[2] == '2') return TOK_I32;
                break;
            case '6':
                if (word_len == 3 && word[2] == '4') return TOK_I64;
                break;
            case 'f':
                if (word_len == 2) return TOK_IF;
                break;
            case 'm':
                if (word_len == 6 && memcmp(word+2, "port", 4) == 0) return TOK_IMPORT;
                break;
            case 'n':
                if (word_len == 9 && memcmp(word+2, "terrupt", 7) == 0) return TOK_INTERRUPT;
                break;
            }
        }
        break;
    case 'k':
        if (word_len == 4 && memcmp(word+1, "eep", 3) == 0) return TOK_KEEP;
        break;
    case 'n':
        if (word_len == 4 && memcmp(word+1, "ull", 3) == 0) return TOK_NULL;
        break;
    case 'o':
        if (word_len == 6 && memcmp(word+1, "relse", 5) == 0) return TOK_ORELSE;
        if (word_len == 6 && memcmp(word+1, "paque", 5) == 0) return TOK_OPAQUE;
        break;
    case 'p':
        if (word_len == 6 && memcmp(word+1, "acked", 5) == 0) return TOK_PACKED;
        break;
    case 'r':
        if (word_len == 6 && memcmp(word+1, "eturn", 5) == 0) return TOK_RETURN;
        break;
    case 's':
        if (word_len == 6 && memcmp(word+1, "truct", 5) == 0) return TOK_STRUCT;
        if (word_len == 6 && memcmp(word+1, "witch", 5) == 0) return TOK_SWITCH;
        if (word_len == 6 && memcmp(word+1, "tatic", 5) == 0) return TOK_STATIC;
        break;
    case 't':
        if (word_len >= 2) {
            switch (word[1]) {
            case 'r':
                if (word_len == 4 && memcmp(word+2, "ue", 2) == 0) return TOK_TRUE;
                break;
            case 'y':
                if (word_len == 7 && memcmp(word+2, "pedef", 5) == 0) return TOK_TYPEDEF;
                break;
            }
        }
        break;
    case 'u':
        if (word_len >= 2) {
            switch (word[1]) {
            case '8':
                if (word_len == 2) return TOK_U8;
                break;
            case '1':
                if (word_len == 3 && word[2] == '6') return TOK_U16;
                break;
            case '3':
                if (word_len == 3 && word[2] == '2') return TOK_U32;
                break;
            case '6':
                if (word_len == 3 && word[2] == '4') return TOK_U64;
                break;
            case 'n':
                if (word_len == 5 && memcmp(word+2, "ion", 3) == 0) return TOK_UNION;
                break;
            case 's':
                if (word_len == 5 && memcmp(word+2, "ize", 3) == 0) return TOK_USIZE;
                break;
            }
        }
        break;
    case 'v':
        if (word_len == 4 && memcmp(word+1, "oid", 3) == 0) return TOK_VOID;
        if (word_len == 8 && memcmp(word+1, "olatile", 7) == 0) return TOK_VOLATILE;
        break;
    case 'w':
        if (word_len == 5 && memcmp(word+1, "hile", 4) == 0) return TOK_WHILE;
        break;
    }
    return TOK_IDENT; /* not a keyword — user identifier */
}

/* ---- Scan a word (identifier or keyword) ---- */

static Token scan_word(Scanner *s) {
    size_t start = s->pos - 1; /* already consumed first char */
    while (is_alnum(peek(s))) advance(s);
    size_t word_len = s->pos - start;
    TokenType type = check_keyword(&s->source[start], word_len);
    return make_token(s, type, start);
}

/* ---- Scan a number (integer or float) ---- */

static Token scan_number(Scanner *s) {
    size_t start = s->pos - 1; /* already consumed first digit */
    TokenType type = TOK_NUMBER_INT;

    /* hex: 0x... */
    if (s->source[start] == '0' && peek(s) == 'x') {
        advance(s); /* skip 'x' */
        if (!is_hex_digit(peek(s))) {
            return error_token(s, "expected hex digit after '0x'");
        }
        while (is_hex_digit(peek(s)) || peek(s) == '_') advance(s);
        return make_token(s, TOK_NUMBER_INT, start);
    }

    /* binary: 0b... */
    if (s->source[start] == '0' && peek(s) == 'b') {
        advance(s); /* skip 'b' */
        if (peek(s) != '0' && peek(s) != '1') {
            return error_token(s, "expected binary digit after '0b'");
        }
        while (peek(s) == '0' || peek(s) == '1' || peek(s) == '_') advance(s);
        return make_token(s, TOK_NUMBER_INT, start);
    }

    /* decimal digits with underscore separators */
    while (is_digit(peek(s)) || peek(s) == '_') advance(s);

    /* fractional part: 3.14 */
    if (peek(s) == '.' && is_digit(peek_next(s))) {
        type = TOK_NUMBER_FLOAT;
        advance(s); /* skip '.' */
        while (is_digit(peek(s)) || peek(s) == '_') advance(s);
    }

    return make_token(s, type, start);
}

/* ---- Scan a string literal ---- */

static bool is_valid_escape(char c) {
    return c == 'n' || c == 't' || c == 'r' || c == '\\' ||
           c == '"' || c == '0' || c == 'x';
}

static Token scan_string(Scanner *s) {
    size_t start = s->pos - 1; /* already consumed opening " */
    while (peek(s) != '"' && peek(s) != '\0') {
        if (peek(s) == '\n') s->line++;
        if (peek(s) == '\\') {
            advance(s); /* skip backslash */
            char esc = peek(s);
            if (esc == '\0') return error_token(s, "unterminated string");
            if (!is_valid_escape(esc)) {
                return error_token(s, "invalid escape sequence in string");
            }
            if (esc == 'x') {
                advance(s); /* skip 'x' */
                if (!is_hex_digit(peek(s)) || !is_hex_digit(peek_next(s))) {
                    return error_token(s, "expected two hex digits after '\\x'");
                }
                advance(s); /* skip first hex digit */
            }
        }
        advance(s);
    }
    if (peek(s) == '\0') {
        return error_token(s, "unterminated string");
    }
    advance(s); /* consume closing " */
    return make_token(s, TOK_STRING, start);
}

/* ---- Scan a character literal ---- */

static Token scan_char(Scanner *s) {
    size_t start = s->pos - 1; /* already consumed opening ' */
    if (peek(s) == '\\') {
        advance(s); /* skip backslash */
        char esc = peek(s);
        if (esc == '\0') return error_token(s, "unterminated character literal");
        if (!is_valid_escape(esc) && esc != '\'') {
            return error_token(s, "invalid escape sequence in character literal");
        }
        if (esc == 'x') {
            advance(s); /* skip 'x' */
            if (!is_hex_digit(peek(s)) || !is_hex_digit(peek_next(s))) {
                return error_token(s, "expected two hex digits after '\\x'");
            }
            advance(s); /* skip first hex digit */
        }
        advance(s); /* skip escape char or second hex digit */
    } else if (peek(s) == '\'' || peek(s) == '\0') {
        return error_token(s, "empty character literal");
    } else {
        advance(s); /* skip the character */
    }
    if (peek(s) != '\'') {
        return error_token(s, "unterminated character literal");
    }
    advance(s); /* consume closing ' */
    return make_token(s, TOK_CHAR, start);
}

/* ---- Main scanner function ---- */

Token next_token(Scanner *s) {
    skip_whitespace(s);

    if (s->pos >= s->len) {
        return make_token(s, TOK_EOF, s->pos);
    }

    size_t start = s->pos;
    char c = advance(s);

    /* word: keyword or identifier */
    if (is_alpha(c)) return scan_word(s);

    /* number: integer or float */
    if (is_digit(c)) return scan_number(s);

    /* string literal */
    if (c == '"') return scan_string(s);

    /* character literal */
    if (c == '\'') return scan_char(s);

    /* operators and punctuation */
    switch (c) {
    /* single character — unambiguous */
    case '(': return make_token(s, TOK_LPAREN, start);
    case ')': return make_token(s, TOK_RPAREN, start);
    case '{': return make_token(s, TOK_LBRACE, start);
    case '}': return make_token(s, TOK_RBRACE, start);
    case '[': return make_token(s, TOK_LBRACKET, start);
    case ']': return make_token(s, TOK_RBRACKET, start);
    case ';': return make_token(s, TOK_SEMICOLON, start);
    case ',': return make_token(s, TOK_COMMA, start);
    case '~': return make_token(s, TOK_TILDE, start);
    case '@': return make_token(s, TOK_AT, start);
    case '?': return make_token(s, TOK_QUESTION, start);

    /* dot or dot-dot (range) */
    case '.':
        if (match(s, '.')) return make_token(s, TOK_DOTDOT, start);
        return make_token(s, TOK_DOT, start);

    /* plus or plus-eq */
    case '+':
        if (match(s, '=')) return make_token(s, TOK_PLUSEQ, start);
        return make_token(s, TOK_PLUS, start);

    /* minus or minus-eq */
    case '-':
        if (match(s, '=')) return make_token(s, TOK_MINUSEQ, start);
        return make_token(s, TOK_MINUS, start);

    /* star or star-eq */
    case '*':
        if (match(s, '=')) return make_token(s, TOK_STAREQ, start);
        return make_token(s, TOK_STAR, start);

    /* slash or slash-eq (comments already handled in skip_whitespace) */
    case '/':
        if (match(s, '=')) return make_token(s, TOK_SLASHEQ, start);
        return make_token(s, TOK_SLASH, start);

    /* percent or percent-eq */
    case '%':
        if (match(s, '=')) return make_token(s, TOK_PERCENTEQ, start);
        return make_token(s, TOK_PERCENT, start);

    /* ampersand: & or &= or && */
    case '&':
        if (match(s, '&')) return make_token(s, TOK_AMPAMP, start);
        if (match(s, '=')) return make_token(s, TOK_AMPEQ, start);
        return make_token(s, TOK_AMP, start);

    /* pipe: | or |= or || */
    case '|':
        if (match(s, '|')) return make_token(s, TOK_PIPEPIPE, start);
        if (match(s, '=')) return make_token(s, TOK_PIPEEQ, start);
        return make_token(s, TOK_PIPE, start);

    /* caret: ^ or ^= */
    case '^':
        if (match(s, '=')) return make_token(s, TOK_CARETEQ, start);
        return make_token(s, TOK_CARET, start);

    /* bang: ! or != */
    case '!':
        if (match(s, '=')) return make_token(s, TOK_BANGEQ, start);
        return make_token(s, TOK_BANG, start);

    /* equals: = or == or => (arrow) */
    case '=':
        if (match(s, '=')) return make_token(s, TOK_EQEQ, start);
        if (match(s, '>')) return make_token(s, TOK_ARROW, start);
        return make_token(s, TOK_EQ, start);

    /* less: < or <= or << or <<= */
    case '<':
        if (match(s, '<')) {
            if (match(s, '=')) return make_token(s, TOK_LSHIFTEQ, start);
            return make_token(s, TOK_LSHIFT, start);
        }
        if (match(s, '=')) return make_token(s, TOK_LTEQ, start);
        return make_token(s, TOK_LT, start);

    /* greater: > or >= or >> or >>= */
    case '>':
        if (match(s, '>')) {
            if (match(s, '=')) return make_token(s, TOK_RSHIFTEQ, start);
            return make_token(s, TOK_RSHIFT, start);
        }
        if (match(s, '=')) return make_token(s, TOK_GTEQ, start);
        return make_token(s, TOK_GT, start);
    }

    /* unknown character */
    return error_token(s, "unexpected character");
}

/* ---- Token type name (for debugging) ---- */

const char *token_type_name(TokenType type) {
    switch (type) {
    case TOK_U8: return "u8";
    case TOK_U16: return "u16";
    case TOK_U32: return "u32";
    case TOK_U64: return "u64";
    case TOK_I8: return "i8";
    case TOK_I16: return "i16";
    case TOK_I32: return "i32";
    case TOK_I64: return "i64";
    case TOK_USIZE: return "usize";
    case TOK_F32: return "f32";
    case TOK_F64: return "f64";
    case TOK_BOOL: return "bool";
    case TOK_VOID: return "void";
    case TOK_OPAQUE: return "opaque";
    case TOK_STRUCT: return "struct";
    case TOK_PACKED: return "packed";
    case TOK_ENUM: return "enum";
    case TOK_UNION: return "union";
    case TOK_CONST: return "const";
    case TOK_TYPEDEF: return "typedef";
    case TOK_DISTINCT: return "distinct";
    case TOK_IF: return "if";
    case TOK_ELSE: return "else";
    case TOK_FOR: return "for";
    case TOK_WHILE: return "while";
    case TOK_SWITCH: return "switch";
    case TOK_BREAK: return "break";
    case TOK_CONTINUE: return "continue";
    case TOK_RETURN: return "return";
    case TOK_DEFAULT: return "default";
    case TOK_ORELSE: return "orelse";
    case TOK_NULL: return "null";
    case TOK_TRUE: return "true";
    case TOK_FALSE: return "false";
    case TOK_POOL: return "Pool";
    case TOK_RING: return "Ring";
    case TOK_ARENA: return "Arena";
    case TOK_HANDLE: return "Handle";
    case TOK_DEFER: return "defer";
    case TOK_IMPORT: return "import";
    case TOK_CINCLUDE: return "cinclude";
    case TOK_VOLATILE: return "volatile";
    case TOK_INTERRUPT: return "interrupt";
    case TOK_ASM: return "asm";
    case TOK_STATIC: return "static";
    case TOK_KEEP: return "keep";
    case TOK_AS: return "as";
    case TOK_IDENT: return "IDENT";
    case TOK_NUMBER_INT: return "INT";
    case TOK_NUMBER_FLOAT: return "FLOAT";
    case TOK_STRING: return "STRING";
    case TOK_CHAR: return "CHAR";
    case TOK_LPAREN: return "(";
    case TOK_RPAREN: return ")";
    case TOK_LBRACE: return "{";
    case TOK_RBRACE: return "}";
    case TOK_LBRACKET: return "[";
    case TOK_RBRACKET: return "]";
    case TOK_SEMICOLON: return ";";
    case TOK_COMMA: return ",";
    case TOK_TILDE: return "~";
    case TOK_AT: return "@";
    case TOK_QUESTION: return "?";
    case TOK_DOT: return ".";
    case TOK_DOTDOT: return "..";
    case TOK_PLUS: return "+";
    case TOK_MINUS: return "-";
    case TOK_STAR: return "*";
    case TOK_SLASH: return "/";
    case TOK_PERCENT: return "%";
    case TOK_AMP: return "&";
    case TOK_PIPE: return "|";
    case TOK_CARET: return "^";
    case TOK_BANG: return "!";
    case TOK_EQ: return "=";
    case TOK_LT: return "<";
    case TOK_GT: return ">";
    case TOK_EQEQ: return "==";
    case TOK_BANGEQ: return "!=";
    case TOK_LTEQ: return "<=";
    case TOK_GTEQ: return ">=";
    case TOK_LSHIFT: return "<<";
    case TOK_RSHIFT: return ">>";
    case TOK_AMPAMP: return "&&";
    case TOK_PIPEPIPE: return "||";
    case TOK_PLUSEQ: return "+=";
    case TOK_MINUSEQ: return "-=";
    case TOK_STAREQ: return "*=";
    case TOK_SLASHEQ: return "/=";
    case TOK_PERCENTEQ: return "%=";
    case TOK_AMPEQ: return "&=";
    case TOK_PIPEEQ: return "|=";
    case TOK_CARETEQ: return "^=";
    case TOK_LSHIFTEQ: return "<<=";
    case TOK_RSHIFTEQ: return ">>=";
    case TOK_ARROW: return "=>";
    case TOK_ERROR: return "ERROR";
    case TOK_EOF: return "EOF";
    }
    return "UNKNOWN";
}
