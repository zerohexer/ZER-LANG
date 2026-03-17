#include <stdio.h>
#include <stdlib.h>
#include "lexer.h"

/* read entire file into a string */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char *buf = (char *)malloc(size + 1);
    if (!buf) {
        fprintf(stderr, "error: out of memory\n");
        fclose(f);
        exit(1);
    }
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    const char *source;
    char *file_buf = NULL;

    if (argc == 2) {
        /* read from file */
        file_buf = read_file(argv[1]);
        source = file_buf;
    } else {
        /* default test input */
        source =
            "u32 x = 5;\n"
            "?*Task maybe = find_task();\n"
            "if (maybe) |val| {\n"
            "    val.priority = 42;\n"
            "}\n"
            "Pool(Task, 8) tasks;\n"
            "Handle(Task) h = tasks.alloc() orelse return;\n"
            "tasks.get(h).name = \"hello\";\n"
            "tasks.free(h);\n"
            "// this is a comment\n"
            "u32 result = a + b * c;\n"
            "if (x >= 10 && y != 0) {\n"
            "    arr[i] = data[0..5];\n"
            "}\n";
    }

    printf("=== ZER Lexer Test ===\n\n");
    printf("Input:\n%s\n", source);
    printf("Tokens:\n");

    Scanner scanner;
    scanner_init(&scanner, source);

    int count = 0;
    for (;;) {
        Token t = next_token(&scanner);

        /* print: line number, token type, lexeme */
        printf("  %3d  %-12s  '%.*s'\n",
               t.line,
               token_type_name(t.type),
               (int)t.length,
               t.start);

        count++;

        if (t.type == TOK_EOF) break;
        if (t.type == TOK_ERROR) {
            fprintf(stderr, "LEXER ERROR at line %d: %.*s\n",
                    t.line, (int)t.length, t.start);
            break;
        }
    }

    printf("\nTotal: %d tokens\n", count);

    if (file_buf) free(file_buf);
    return 0;
}
