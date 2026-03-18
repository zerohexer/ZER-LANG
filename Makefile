CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2 -I.

# Core source files for the compiler
CORE_SRCS = lexer.c parser.c ast.c types.c checker.c emitter.c zercheck.c zerc_main.c
CORE_OBJS = $(CORE_SRCS:.c=.o)

# Library sources (everything except zerc_main)
LIB_SRCS = lexer.c parser.c ast.c types.c checker.c emitter.c zercheck.c
LIB_OBJS = $(LIB_SRCS:.c=.o)

# ---- Compiler binary ----
zerc: $(CORE_OBJS)
	$(CC) $(CFLAGS) -o zerc $^

# ---- Test binaries ----
test_lexer: test_lexer.c lexer.c
	$(CC) $(CFLAGS) -o $@ $^

test_parser: test_parser.c parser.c ast.c lexer.c
	$(CC) $(CFLAGS) -o $@ $^

test_parser_edge: test_parser_edge.c parser.c ast.c lexer.c
	$(CC) $(CFLAGS) -o $@ $^

test_checker: test_checker.c $(LIB_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test_checker_full: test_checker_full.c $(LIB_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test_extra: test_extra.c $(LIB_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test_gaps: test_gaps.c $(LIB_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test_emit: test_emit.c $(LIB_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test_zercheck: test_zercheck.c $(LIB_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

# ---- Run all tests ----
check: test_lexer test_parser test_parser_edge test_checker test_checker_full test_extra test_gaps test_emit test_zercheck
	./test_lexer
	./test_parser
	./test_parser_edge
	./test_checker
	./test_checker_full
	./test_extra
	./test_gaps
	./test_emit
	./test_zercheck

# ---- LSP server ----
zer-lsp: zer_lsp.c $(LIB_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

# ---- Old demo (lexer only) ----
demo_lexer: main.c lexer.c
	$(CC) $(CFLAGS) -o $@ $^

# ---- Clean ----
clean:
	rm -f zerc zerc.exe zer-lsp zer-lsp.exe *.o \
	      test_lexer test_lexer.exe \
	      test_parser test_parser.exe \
	      test_parser_edge test_parser_edge.exe \
	      test_checker test_checker.exe \
	      test_checker_full test_checker_full.exe \
	      test_extra test_extra.exe \
	      test_gaps test_gaps.exe \
	      test_emit test_emit.exe \
	      test_zercheck test_zercheck.exe \
	      demo_lexer demo_lexer.exe \
	      _zer_test_out.c _zer_test_out.exe _zer_test_out.o _zer_gcc_err.txt

.PHONY: check clean
