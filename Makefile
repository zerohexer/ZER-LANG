CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2

zerc: main.c lexer.c lexer.h
	$(CC) $(CFLAGS) -o zerc main.c lexer.c

test: zerc
	./zerc

clean:
	rm -f zerc zerc.exe

.PHONY: test clean
