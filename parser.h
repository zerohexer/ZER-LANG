#ifndef ZER_PARSER_H
#define ZER_PARSER_H

#include "lexer.h"
#include "ast.h"

/* ---- Parser state ---- */
typedef struct {
    Scanner *scanner;
    Token current;          /* current token (lookahead) */
    Token previous;         /* previously consumed token */
    Arena *arena;           /* arena for AST allocation */
    bool had_error;
    bool panic_mode;        /* suppress cascading errors */
    bool oom;               /* arena allocation failed — stop parsing */
    const char *file_name;  /* for error messages */
    const char *source;     /* source text for error display (NULL = skip source line) */
    int depth;              /* nesting depth for recursion limit */
} Parser;

/* ---- API ---- */
void parser_init(Parser *p, Scanner *scanner, Arena *arena, const char *file_name);
Node *parse_file(Parser *p);   /* parse entire source file → NODE_FILE */

#endif /* ZER_PARSER_H */
