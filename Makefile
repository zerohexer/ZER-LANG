CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2 -I.

# Core source files for the compiler
CORE_SRCS = lexer.c parser.c ast.c types.c checker.c emitter.c zercheck.c ir.c ir_lower.c zerc_main.c
CORE_OBJS = $(CORE_SRCS:.c=.o)

# Library sources (everything except zerc_main)
LIB_SRCS = lexer.c parser.c ast.c types.c checker.c emitter.c zercheck.c ir.c ir_lower.c
LIB_OBJS = $(LIB_SRCS:.c=.o)

# ---- Compiler binary ----
zerc: $(CORE_OBJS)
	$(CC) $(CFLAGS) -o zerc $^

# ---- Code index (for LLM context — query with grep, don't load fully) ----
tags: $(CORE_SRCS) $(LIB_SRCS) *.h
	ctags --fields=+nS --extras=+q -R $(CORE_SRCS) *.h

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

test_semantic_fuzz: tests/test_semantic_fuzz.c
	$(CC) $(CFLAGS) -o $@ $^

# ---- Run all tests ----
check: zerc test_lexer test_parser test_parser_edge test_checker test_checker_full test_extra test_gaps test_emit test_zercheck test_firmware test_firmware2 test_firmware3 test_production test_fuzz test_semantic_fuzz
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
	./test_semantic_fuzz
	@echo "=== Module import tests ==="
	@cd test_modules && ./run_tests.sh
	@echo "=== ZER integration tests ==="
	@bash tests/test_zer.sh
	@echo "=== Rust-equivalent safety tests ==="
	@bash rust_tests/run_tests.sh ./zerc
	@echo "=== Zig-translated tests ==="
	@bash zig_tests/run_tests.sh ./zerc
	@echo "=== Conversion tool tests ==="
	@bash tests/test_convert.sh

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
	      test_semantic_fuzz test_semantic_fuzz.exe \
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

docker-test-convert:
	docker build -t zer-lang-dev .
	docker run --rm zer-lang-dev bash tests/test_convert.sh

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

# ---- Docker release Windows: cross-compile with mingw (pre-installed in image) ----
docker-release-win:
	docker build -t zer-lang-win -f Dockerfile.win .
	@mkdir -p release
	docker rm -f zer-release-win 2>/dev/null; true
	docker run --name zer-release-win zer-lang-win
	docker cp zer-release-win:/zer/zerc.exe release/zerc.exe
	docker cp zer-release-win:/zer/zer-lsp.exe release/zer-lsp.exe
	docker rm zer-release-win
	@echo "Windows binaries in release/ (zerc.exe, zer-lsp.exe)"

# ---- Docker release all: both Linux + Windows ----
docker-release-all: docker-release docker-release-win
	@echo "All binaries in release/"

# ---- Docker install: build Windows binaries in Docker, copy to MinGW bin (on PATH) ----
docker-install: docker-release-win
	cp release/zerc.exe /c/msys64/mingw64/bin/zerc.exe
	mv -f /c/msys64/mingw64/bin/zer-lsp.exe /c/msys64/mingw64/bin/zer-lsp.old.exe 2>/dev/null; true
	cp release/zer-lsp.exe /c/msys64/mingw64/bin/zer-lsp.exe
	rm -f /c/msys64/mingw64/bin/zer-lsp.old.exe 2>/dev/null; true
	@echo "Installed zerc.exe + zer-lsp.exe to C:\\msys64\\mingw64\\bin\\"
	@echo "Restart VS Code to pick up the new LSP"

# ---- Install: build natively, copy to MinGW bin (on PATH) — may trigger AV ----
install: zerc
	cp zerc.exe /c/msys64/mingw64/bin/zerc.exe 2>/dev/null || cp zerc /c/msys64/mingw64/bin/zerc 2>/dev/null
	@echo "Installed zerc to C:\\msys64\\mingw64\\bin\\"

# ---- Docker VSIX: build VS Code extension with bundled compiler + GCC ----
docker-vsix:
	docker build -t zer-lang-vsix -f Dockerfile.vsix .
	@mkdir -p release
	docker rm -f zer-vsix-out 2>/dev/null; true
	docker run --name zer-vsix-out zer-lang-vsix true
	docker cp zer-vsix-out:/out/zer-lang.vsix release/zer-lang.vsix
	docker rm zer-vsix-out
	@echo "VS Code extension: release/zer-lang.vsix"
	@echo "Install: code --install-extension release/zer-lang.vsix"

.PHONY: check clean release install docker-check docker-build docker-test-convert docker-shell docker-release docker-release-win docker-release-all docker-install docker-vsix
