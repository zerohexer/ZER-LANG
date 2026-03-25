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

test_firmware: test_firmware_patterns.c $(LIB_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test_firmware2: test_firmware_patterns2.c $(LIB_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test_firmware3: test_firmware_patterns3.c $(LIB_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test_production: test_production.c $(LIB_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test_fuzz: test_fuzz.c $(LIB_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

# ---- Run all tests ----
check: zerc test_lexer test_parser test_parser_edge test_checker test_checker_full test_extra test_gaps test_emit test_zercheck test_firmware test_firmware2 test_firmware3 test_production test_fuzz
	./test_lexer
	./test_parser
	./test_parser_edge
	./test_checker
	./test_checker_full
	./test_extra
	./test_gaps
	./test_emit
	./test_zercheck
	./test_firmware
	./test_firmware2
	./test_firmware3
	./test_production
	./test_fuzz
	@echo "=== Module import tests ==="
	@cd test_modules && ./run_tests.sh

# ---- LSP server ----
zer-lsp: zer_lsp.c $(LIB_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

# ---- Release build (clean binaries in release/) ----
release: zerc zer-lsp
	mkdir -p release
	cp -f zerc release/ 2>/dev/null; cp -f zerc.exe release/ 2>/dev/null; \
	cp -f zer-lsp release/ 2>/dev/null; cp -f zer-lsp.exe release/ 2>/dev/null; \
	echo "Release binaries in release/" && echo "Add to PATH: $(CURDIR)/release"

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
	      test_firmware test_firmware.exe \
	      test_firmware2 test_firmware2.exe \
	      test_firmware3 test_firmware3.exe \
	      test_production test_production.exe \
	      test_fuzz test_fuzz.exe \
	      demo_lexer demo_lexer.exe \
	      _zer_test_out.c _zer_test_out.exe _zer_test_out.o _zer_gcc_err.txt
	rm -rf release

# ---- Docker build + test (avoids AV false positives) ----
docker-check:
	docker build -t zer-lang-dev .
	docker run --rm zer-lang-dev make check

docker-build:
	docker build -t zer-lang-dev .
	docker run --rm zer-lang-dev make zerc

docker-shell:
	docker build -t zer-lang-dev .
	docker run --rm -it zer-lang-dev bash

# ---- Docker release: build Linux binaries (extracted to release/) ----
docker-release:
	docker build -t zer-lang-dev .
	@mkdir -p release
	docker rm -f zer-release 2>/dev/null; true
	docker run --name zer-release zer-lang-dev make zerc zer-lsp
	docker cp zer-release:/zer/zerc release/zerc
	docker cp zer-release:/zer/zer-lsp release/zer-lsp
	docker rm zer-release
	@echo "Linux binaries in release/ (zerc, zer-lsp)"

# ---- Docker release Windows: cross-compile with mingw inside Docker ----
docker-release-win:
	docker build -t zer-lang-dev .
	@mkdir -p release
	docker rm -f zer-release-win 2>/dev/null; true
	docker run --name zer-release-win zer-lang-dev sh -c \
		"apt-get update -qq && apt-get install -y -qq gcc-mingw-w64-x86-64-posix >/dev/null 2>&1 && \
		 x86_64-w64-mingw32-gcc -std=c99 -O2 -I. -o zerc.exe lexer.c parser.c ast.c types.c checker.c emitter.c zercheck.c zerc_main.c && \
		 x86_64-w64-mingw32-gcc -std=c99 -O2 -I. -o zer-lsp.exe zer_lsp.c lexer.c parser.c ast.c types.c checker.c emitter.c zercheck.c"
	docker cp zer-release-win:/zer/zerc.exe release/zerc.exe
	docker cp zer-release-win:/zer/zer-lsp.exe release/zer-lsp.exe
	docker rm zer-release-win
	@echo "Windows binaries in release/ (zerc.exe, zer-lsp.exe)"

# ---- Docker release all: both Linux + Windows ----
docker-release-all: docker-release docker-release-win
	@echo "All binaries in release/"

.PHONY: check clean release docker-check docker-build docker-shell docker-release docker-release-win docker-release-all
