#ifndef ZER_LEXER_H
#define ZER_LEXER_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* ---- Token Types ---- */
typedef enum {
    /* === Types === */
    TOK_U8, TOK_U16, TOK_U32, TOK_U64,
    TOK_I8, TOK_I16, TOK_I32, TOK_I64,
    TOK_USIZE,
    TOK_F32, TOK_F64,
    TOK_BOOL,
    TOK_VOID,
    TOK_OPAQUE,

    /* === Declaration keywords === */
    TOK_STRUCT, TOK_PACKED, TOK_SHARED, TOK_ENUM, TOK_UNION,
    TOK_CONST, TOK_TYPEDEF, TOK_DISTINCT,

    /* === Control flow === */
    TOK_IF, TOK_ELSE, TOK_FOR, TOK_WHILE,
    TOK_SWITCH, TOK_BREAK, TOK_CONTINUE, TOK_RETURN, TOK_GOTO, TOK_SPAWN,
    TOK_DEFAULT,

    /* === Error handling === */
    TOK_ORELSE, TOK_NULL,

    /* === Boolean literals === */
    TOK_TRUE, TOK_FALSE,

    /* === Memory builtins === */
    TOK_POOL, TOK_RING, TOK_ARENA, TOK_HANDLE, TOK_SLAB, TOK_BARRIER,

    /* === Special keywords === */
    TOK_DEFER, TOK_IMPORT, TOK_CINCLUDE, TOK_VOLATILE, TOK_INTERRUPT,
    TOK_ASM, TOK_STATIC, TOK_KEEP, TOK_AS, TOK_MMIO, TOK_COMPTIME, TOK_THREADLOCAL, TOK_ASYNC,
    TOK_STATIC_ASSERT,
    TOK_CONTAINER,
    TOK_DO,
    TOK_SEMAPHORE,

    /* === Literals === */
    TOK_IDENT,          /* user identifier */
    TOK_NUMBER_INT,     /* integer literal: 42, 0xFF, 0b1010 */
    TOK_NUMBER_FLOAT,   /* float literal: 3.14 */
    TOK_STRING,         /* "hello" */
    TOK_CHAR,           /* 'a', '\n', '\x0A' */

    /* === Single-character tokens === */
    TOK_LPAREN,         /* ( */
    TOK_RPAREN,         /* ) */
    TOK_LBRACE,         /* { */
    TOK_RBRACE,         /* } */
    TOK_LBRACKET,       /* [ */
    TOK_RBRACKET,       /* ] */
    TOK_SEMICOLON,      /* ; */
    TOK_COLON,          /* : */
    TOK_COMMA,          /* , */
    TOK_TILDE,          /* ~ */
    TOK_AT,             /* @ (intrinsic prefix) */
    TOK_QUESTION,       /* ? (optional type) */

    /* === Operators (1 or 2 characters) === */
    TOK_DOT,            /* . */
    TOK_DOTDOT,         /* .. (range/slice) */
    TOK_PLUS,           /* + */
    TOK_MINUS,          /* - */
    TOK_STAR,           /* * */
    TOK_SLASH,          /* / */
    TOK_PERCENT,        /* % */
    TOK_AMP,            /* & */
    TOK_PIPE,           /* | */
    TOK_CARET,          /* ^ */
    TOK_BANG,           /* ! */
    TOK_EQ,             /* = */
    TOK_LT,             /* < */
    TOK_GT,             /* > */

    /* === Two-character operators === */
    TOK_EQEQ,           /* == */
    TOK_BANGEQ,          /* != */
    TOK_LTEQ,            /* <= */
    TOK_GTEQ,            /* >= */
    TOK_LSHIFT,          /* << */
    TOK_RSHIFT,          /* >> */
    TOK_AMPAMP,          /* && */
    TOK_PIPEPIPE,        /* || */

    /* === Compound assignment === */
    TOK_PLUSEQ,          /* += */
    TOK_MINUSEQ,         /* -= */
    TOK_STAREQ,          /* *= */
    TOK_SLASHEQ,         /* /= */
    TOK_PERCENTEQ,       /* %= */
    TOK_AMPEQ,           /* &= */
    TOK_PIPEEQ,          /* |= */
    TOK_CARETEQ,         /* ^= */
    TOK_LSHIFTEQ,        /* <<= */
    TOK_RSHIFTEQ,        /* >>= */

    /* === Arrow === */
    TOK_ARROW,           /* => (switch arm) */

    /* === Special === */
    TOK_ERROR,           /* lexer error */
    TOK_EOF,             /* end of input */
} TokenType;

/* ---- Token ---- */
typedef struct {
    TokenType type;
    const char *start;   /* points into source string — no copy */
    size_t length;
    int line;
} Token;

/* ---- Scanner state ---- */
typedef struct {
    const char *source;  /* entire source string */
    size_t pos;          /* current position */
    size_t len;          /* total length */
    int line;            /* current line number */
} Scanner;

/* ---- API ---- */
void scanner_init(Scanner *s, const char *source);
Token next_token(Scanner *s);
const char *token_type_name(TokenType type);

#endif /* ZER_LEXER_H */
