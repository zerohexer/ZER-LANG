CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2 -I.

# Core source files for the compiler
# zercheck_ir.c is the CFG-based replacement for zercheck.c (per docs/cfg_migration_plan.md).
# During migration both coexist: zercheck.c is primary, zercheck_ir.c grows to feature
# parity, verified via dual-run in Phase E, then zercheck.c deleted at Phase F (v0.5.0).
CORE_SRCS = lexer.c parser.c ast.c types.c checker.c emitter.c zercheck.c zercheck_ir.c ir.c ir_lower.c zerc_main.c src/safety/handle_state.c src/safety/range_checks.c src/safety/type_kind.c src/safety/coerce_rules.c src/safety/context_bans.c src/safety/escape_rules.c src/safety/provenance_rules.c src/safety/mmio_rules.c src/safety/optional_rules.c src/safety/move_rules.c src/safety/atomic_rules.c src/safety/container_rules.c src/safety/misc_rules.c src/safety/isr_rules.c src/safety/arith_rules.c src/safety/variant_rules.c src/safety/stack_rules.c src/safety/comptime_rules.c src/safety/cast_rules.c src/safety/concurrency_rules.c src/safety/asm_categories.c src/safety/asm_register_lookup.c src/safety/asm_register_tables_x86_64.c src/safety/asm_register_tables_x86_64_avx512f.c src/safety/asm_register_tables_aarch64.c src/safety/asm_register_tables_riscv64.c src/safety/asm_instruction_table_x86_64.c src/safety/asm_instruction_table_aarch64.c src/safety/asm_instruction_table_riscv64.c
CORE_OBJS = $(CORE_SRCS:.c=.o)

# Library sources (everything except zerc_main)
# src/safety/*.c files are VST-verified predicates also linked into zerc.
# See docs/formal_verification_plan.md Level 3 — same .c verified by
# `make check-vst` and built into zerc. Any divergence breaks CI.
LIB_SRCS = lexer.c parser.c ast.c types.c checker.c emitter.c zercheck.c zercheck_ir.c ir.c ir_lower.c src/safety/handle_state.c src/safety/range_checks.c src/safety/type_kind.c src/safety/coerce_rules.c src/safety/context_bans.c src/safety/escape_rules.c src/safety/provenance_rules.c src/safety/mmio_rules.c src/safety/optional_rules.c src/safety/move_rules.c src/safety/atomic_rules.c src/safety/container_rules.c src/safety/misc_rules.c src/safety/isr_rules.c src/safety/arith_rules.c src/safety/variant_rules.c src/safety/stack_rules.c src/safety/comptime_rules.c src/safety/cast_rules.c src/safety/concurrency_rules.c src/safety/asm_categories.c src/safety/asm_register_lookup.c src/safety/asm_register_tables_x86_64.c src/safety/asm_register_tables_x86_64_avx512f.c src/safety/asm_register_tables_aarch64.c src/safety/asm_register_tables_riscv64.c src/safety/asm_instruction_table_x86_64.c src/safety/asm_instruction_table_aarch64.c src/safety/asm_instruction_table_riscv64.c
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

# Phase F2 (2026-05-03): test_zercheck.c is no longer in `make check`.
# It tests zercheck.c (AST analyzer) directly — narrow unit-test
# patterns that don't reflect production. Production safety analysis
# went to zercheck_ir in Phase F1 and is verified by:
#   - 538 ZER integration tests (tests/zer/ + tests/zer_fail/)
#   - 200 fuzz tests
#   - 139 conversion tests
#   - 28 module tests
#   - 5 cross-arch tests
# These cover all production-relevant safety patterns.
#
# The build target is kept (for manual `make test_zercheck` if
# someone wants to test zercheck.c specifically), but it's not
# wired into `make check`.
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

test_ir_validate: test_ir_validate.c $(LIB_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

# ---- Run all tests ----
check: zerc test_lexer test_parser test_parser_edge test_checker test_checker_full test_extra test_gaps test_emit test_firmware test_firmware2 test_firmware3 test_production test_fuzz test_semantic_fuzz test_ir_validate
	./test_lexer
	./test_parser
	./test_parser_edge
	./test_checker
	./test_checker_full
	./test_extra
	./test_gaps
	./test_emit
	./test_firmware
	./test_firmware2
	./test_firmware3
	./test_production
	./test_fuzz
	./test_semantic_fuzz
	./test_ir_validate
	@echo "=== Module import tests ==="
	@cd test_modules && ./run_tests.sh
	@echo "=== ZER integration tests ==="
	@bash tests/test_zer.sh
	@echo "=== Rust-equivalent safety tests ==="
	@bash rust_tests/run_tests.sh ./zerc
	@echo "=== Rust MMIO tests (QEMU Cortex-M3) ==="
	@bash rust_tests/qemu/run_tests.sh ./zerc
	@echo "=== Zig-translated tests ==="
	@bash zig_tests/run_tests.sh ./zerc
	@echo "=== Conversion tool tests ==="
	@bash tests/test_convert.sh
	@echo "=== Cross-arch end-to-end (F2/F5/F6-minimum) ==="
	@bash tests/test_cross_arch.sh
	@echo "=== Walker audit (IR vs AST emitter parity) ==="
	@bash tools/walker_audit.sh
	@echo "=== Walker default-clause audit (Stage 2 Part B discipline) ==="
	@bash tools/walker_default_audit.sh
	@echo "=== Fixed-buffer audit (Stage 3 — Rule #7 enforcement) ==="
	@bash tools/audit_fixed_buffers.sh
	@echo "=== Emit audit (dead-stub fingerprints) ==="
	@bash tools/emit_audit.sh ./zerc

# Stage 3 standalone target — run just the fixed-buffer linter.
check-fixed-buffers:
	@bash tools/audit_fixed_buffers.sh

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

# ---- D-Alpha-7.5 Session F2: build-time-gen for asm tables ----
# Regenerates per-arch register tables by probing GCC. Output is
# vendored as src/safety/asm_register_tables_<arch>.c and committed
# to git. Rerun when ISA extends (rare, ~once per 2-5 years per arch).
#
# Runs inside Docker so cross-compilers (aarch64-linux-gnu-gcc,
# riscv64-linux-gnu-gcc) need only be available in the container.
gen-asm-tables:
	docker build -t zer-lang-dev .
	docker run --rm -v $$(pwd):/src zer-lang-dev bash -c '\
	    cp -r /src/scripts /tmp/scripts && \
	    cp -r /src/src /tmp/src && \
	    cd /tmp && \
	    bash scripts/gen_register_tables.sh x86_64 && \
	    cp /tmp/src/safety/asm_register_tables_x86_64.c src/safety/asm_register_tables_x86_64_avx512f.c src/safety/asm_register_tables_aarch64.c src/safety/asm_register_tables_riscv64.c src/safety/asm_instruction_table_x86_64.c src/safety/asm_instruction_table_aarch64.c src/safety/asm_instruction_table_riscv64.c /src/src/safety/'
	@echo ""
	@echo "Tables regenerated. Review the diff and commit:"
	@echo "  git diff src/safety/asm_register_tables_*.c"

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

# ---- Formal-verification proofs (Coq + Iris) ----
# Requires Docker image `zer-proofs` (build once: `make check-proofs-image`).
# The proofs are the "tests" — compilation IS the correctness check.
# A broken proof means a compiler invariant was violated.

# ---- Full correctness-oracle pipeline ----
# Runs ALL verification layers: tests, Level 1 (Iris/Coq), Level 3 (VST on
# extracted predicates), and safety-coverage audit. Use this in CI.
#
# Individual targets:
#   check                    -> tests only (~5 min)
#   check-proofs             -> Iris/Coq proofs, ~330 theorems, 0 admits (~10 min)
#   check-vst                -> VST on extracted predicates (~3 min)
#   check-safety-coverage    -> safety_list.md coverage audit (<1 min)
check-all: check check-proofs check-vst check-safety-coverage
	@echo "=== ALL VERIFICATION LAYERS GREEN ==="

check-proofs-image:
	docker build -t zer-proofs -f proofs/operational/Dockerfile proofs/operational

check-proofs:
	@cd proofs/operational && rm -f CoqMakefile CoqMakefile.conf
	cd proofs/operational && MSYS_NO_PATHCONV=1 docker run --rm \
	    -v "$$(pwd -W 2>/dev/null || pwd):/work" -w /work zer-proofs \
	    bash -c 'eval $$(opam env) && make'
	@echo "=== All proofs compile green ==="
	@echo "Verify zero admits across Iris subsets:"
	@# Exclude pre-Iris files (adequacy.v, handle_safety.v, syntax.v,
	@# semantics.v, typing.v in lambda_zer_handle) — preserved as insurance.
	@# Iris files always start with iris_*. Per-subset foundation files
	@# (syntax.v, semantics.v) are included when in non-handle subsets.
	@FILES=""; \
	 for f in proofs/operational/lambda_zer_handle/iris_*.v \
	          proofs/operational/lambda_zer_move/*.v \
	          proofs/operational/lambda_zer_opaque/*.v \
	          proofs/operational/lambda_zer_escape/*.v \
	          proofs/operational/lambda_zer_mmio/*.v \
	          proofs/operational/lambda_zer_typing/*.v; do \
	    FILES="$$FILES $$f"; \
	 done; \
	 if grep -l 'Admitted\|admit\.' $$FILES 2>/dev/null | grep -q .; then \
	    echo "FAIL: admits found in:"; \
	    grep -l 'Admitted\|admit\.' $$FILES; \
	    exit 1; \
	 else \
	    file_count=$$(echo $$FILES | wc -w); \
	    echo "OK: zero admits across $$file_count Iris/subset .v files"; \
	 fi

# ---- Level 3 VST: verify C source against Coq specs ----
# proofs/vst/ with coq-vst + CompCert clightgen.

check-vst-image:
	docker build -t zer-vst -f proofs/vst/Dockerfile proofs/vst

check-vst:
	# Mount the WHOLE repo so VST sees both src/safety/ (the real
	# predicates linked into zerc) and proofs/vst/ (the VST specs).
	# Extracted predicates are clightgen'd from the SAME .c file that
	# `make zerc` compiles — single source of truth. If someone edits
	# src/safety/*.c in a way that breaks the Coq spec, this target
	# fails and blocks the PR. That's the correctness-oracle loop.
	MSYS_NO_PATHCONV=1 docker run --rm \
	    -v "$$(pwd -W 2>/dev/null || pwd):/repo" -w /repo zer-vst \
	    bash -c 'eval $$(opam env) && \
	        clightgen -normalize src/safety/handle_state.c src/safety/range_checks.c src/safety/type_kind.c src/safety/coerce_rules.c src/safety/context_bans.c src/safety/escape_rules.c src/safety/provenance_rules.c src/safety/mmio_rules.c src/safety/optional_rules.c src/safety/move_rules.c src/safety/atomic_rules.c src/safety/container_rules.c src/safety/misc_rules.c src/safety/isr_rules.c src/safety/arith_rules.c src/safety/variant_rules.c src/safety/stack_rules.c src/safety/comptime_rules.c src/safety/cast_rules.c src/safety/concurrency_rules.c src/safety/asm_categories.c src/safety/asm_register_lookup.c src/safety/asm_register_tables_x86_64.c src/safety/asm_register_tables_x86_64_avx512f.c src/safety/asm_register_tables_aarch64.c src/safety/asm_register_tables_riscv64.c src/safety/asm_instruction_table_x86_64.c src/safety/asm_instruction_table_aarch64.c src/safety/asm_instruction_table_riscv64.c && \
	        cd proofs/vst && \
	        clightgen -normalize simple_check.c zer_checks.c zer_checks2.c && \
	        coqc -Q . zer_vst simple_check.v && \
	        coqc -Q . zer_vst verif_simple_check.v && \
	        coqc -Q . zer_vst zer_checks.v && \
	        coqc -Q . zer_vst verif_zer_checks.v && \
	        coqc -Q . zer_vst zer_checks2.v && \
	        coqc -Q . zer_vst verif_zer_checks2.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety ../../src/safety/handle_state.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety verif_handle_state.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety ../../src/safety/range_checks.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety verif_range_checks.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety ../../src/safety/type_kind.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety verif_type_kind.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety ../../src/safety/coerce_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety verif_coerce_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety ../../src/safety/context_bans.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety verif_context_bans.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety ../../src/safety/escape_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety verif_escape_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety ../../src/safety/provenance_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety verif_provenance_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety ../../src/safety/mmio_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety verif_mmio_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety ../../src/safety/optional_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety verif_optional_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety ../../src/safety/move_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety verif_move_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety ../../src/safety/atomic_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety verif_atomic_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety ../../src/safety/container_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety verif_container_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety ../../src/safety/misc_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety verif_misc_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety ../../src/safety/isr_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety verif_isr_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety ../../src/safety/arith_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety verif_arith_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety ../../src/safety/variant_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety verif_variant_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety ../../src/safety/stack_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety verif_stack_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety ../../src/safety/comptime_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety verif_comptime_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety ../../src/safety/cast_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety verif_cast_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety ../../src/safety/concurrency_rules.v && \
	        coqc -Q . zer_vst -Q ../../src/safety zer_safety verif_concurrency_rules.v'
	@echo "=== VST proofs compile green ==="
	@if grep -l 'Admitted\|admit\.' proofs/vst/verif_*.v 2>/dev/null | grep -q .; then \
	    echo "FAIL: admits found in VST proofs"; exit 1; \
	else \
	    count=$$(ls proofs/vst/verif_*.v 2>/dev/null | wc -l); \
	    echo "OK: zero admits across $$count VST verification files"; \
	fi

# ---- Safety-coverage audit: verify every compiler check has a curation entry ----
# Regenerates docs/safety_coverage_raw.md and warns if any unique predicate
# is missing from docs/safety_list.md (the curated coverage matrix).

check-safety-coverage:
	bash tools/safety_coverage.sh > docs/safety_coverage_raw.md.new
	@if ! diff -q docs/safety_coverage_raw.md docs/safety_coverage_raw.md.new > /dev/null; then \
	    echo "WARNING: safety_coverage_raw.md out of date. Review diff:"; \
	    diff docs/safety_coverage_raw.md docs/safety_coverage_raw.md.new || true; \
	    mv docs/safety_coverage_raw.md.new docs/safety_coverage_raw.md; \
	else \
	    rm docs/safety_coverage_raw.md.new; \
	    echo "OK: safety_coverage_raw.md up to date"; \
	fi

.PHONY: check check-all clean release install docker-check docker-build docker-test-convert docker-shell docker-release docker-release-win docker-release-all docker-install docker-vsix check-proofs-image check-proofs check-safety-coverage check-vst-image check-vst
