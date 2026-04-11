FROM gcc:13

WORKDIR /zer

# Copy source files
COPY *.c *.h Makefile ./
COPY test_modules/ test_modules/
COPY lib/ lib/
COPY tools/ tools/
COPY tests/ tests/
COPY rust_tests/ rust_tests/
COPY zig_tests/ zig_tests/

# Fix CRLF line endings from Windows git checkout
RUN find . -name "*.sh" -exec sed -i 's/\r$//' {} + && \
    find . -name "*.zer" -exec sed -i 's/\r$//' {} +

# Build and test
CMD ["make", "check"]
