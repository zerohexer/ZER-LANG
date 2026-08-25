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
    /* BUG-878: >0 while parsing a 2C funcptr RETURN type. `parse_type` is
     * greedy about a trailing `[N]`, so `?*(u32,u32) -> u32 [3] ops` built
     * `fn(u32,u32) -> u32[3]` — a funcptr returning an array — instead of an
     * array of funcptrs. A ZER function cannot return an array at ANY depth of
     * the return type, so a `[` reached while this is set can only belong to
     * the DECLARATION, and is left for the declaration's own array-suffix
     * branch to consume. */
    int no_array_suffix;
} Parser;

/* ---- API ---- */
void parser_init(Parser *p, Scanner *scanner, Arena *arena, const char *file_name);
Node *parse_file(Parser *p);   /* parse entire source file → NODE_FILE */

#endif /* ZER_PARSER_H */
