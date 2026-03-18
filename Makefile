CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2

zerc: main.c lexer.c lexer.h
	$(CC) $(CFLAGS) -o zerc main.c lexer.c

test_lexer: test_lexer.c lexer.c lexer.h
	$(CC) $(CFLAGS) -o test_lexer test_lexer.c lexer.c

test_parser: test_parser.c parser.c ast.c lexer.c parser.h ast.h lexer.h
	$(CC) $(CFLAGS) -o test_parser test_parser.c parser.c ast.c lexer.c

test_parser_edge: test_parser_edge.c parser.c ast.c lexer.c parser.h ast.h lexer.h
	$(CC) $(CFLAGS) -o test_parser_edge test_parser_edge.c parser.c ast.c lexer.c

test: zerc
	./zerc

check: test_lexer test_parser test_parser_edge
	./test_lexer
	./test_parser
	./test_parser_edge

clean:
	rm -f zerc zerc.exe test_lexer test_lexer.exe test_parser test_parser.exe test_parser_edge test_parser_edge.exe *.o

.PHONY: test check clean
