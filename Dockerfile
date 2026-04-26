FROM gcc:13

# QEMU + ARM cross-toolchain for rust_tests/qemu/ (MMIO tests that need
# mapped hardware addresses — can't run on hosted Linux).
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        qemu-system-arm \
        gcc-arm-none-eabi \
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

# Fix CRLF line endings from Windows git checkout
RUN find . -name "*.sh" -exec sed -i 's/\r$//' {} + && \
    find . -name "*.zer" -exec sed -i 's/\r$//' {} +

# Build and test
CMD ["make", "check"]
