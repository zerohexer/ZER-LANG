FROM gcc:13

# QEMU + ARM cross-toolchain for rust_tests/qemu/ (MMIO tests that need
# mapped hardware addresses — can't run on hosted Linux).
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        qemu-system-arm \
        gcc-arm-none-eabi \
        gcc-aarch64-linux-gnu \
        gcc-riscv64-linux-gnu \
        libc6-dev-arm64-cross \
        libc6-dev-riscv64-cross \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /zer

# Copy source files
COPY *.c *.h Makefile ./
COPY src/ src/
COPY test_modules/ test_modules/
COPY lib/ lib/
COPY tools/ tools/
COPY tests/ tests/
COPY rust_tests/ rust_tests/
COPY zig_tests/ zig_tests/
COPY scripts/ scripts/
# The doc audits `make check` runs (audit_limitations, audit_docs_budget,
# audit_reference_examples) READ these. A script wired into `make check` whose
# input is not copied here passes natively and fails only in this image, and
# every gate after it then never runs (BUG-1662).
COPY CLAUDE.md ./
COPY docs/ docs/

# Fix CRLF line endings from Windows git checkout (.md too: the doc budgets
# count bytes, and reference.md's examples are compiled)
RUN find . -name "*.sh" -exec sed -i 's/\r$//' {} + && \
    find . -name "*.zer" -exec sed -i 's/\r$//' {} + && \
    find . -name "*.c" -exec sed -i 's/\r$//' {} + && \
    find . -name "*.h" -exec sed -i 's/\r$//' {} + && \
    find . -name "*.md" -exec sed -i 's/\r$//' {} +

# Build and test
CMD ["make", "check"]
