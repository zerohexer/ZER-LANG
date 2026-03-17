CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2

zerc: main.c lexer.c lexer.h
	$(CC) $(CFLAGS) -o zerc main.c lexer.c

test_lexer: test_lexer.c lexer.c lexer.h
	$(CC) $(CFLAGS) -o test_lexer test_lexer.c lexer.c

test: zerc
	./zerc

check: test_lexer
	./test_lexer

clean:
	rm -f zerc zerc.exe test_lexer test_lexer.exe

.PHONY: test check clean
