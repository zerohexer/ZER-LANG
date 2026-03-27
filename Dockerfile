FROM gcc:13

WORKDIR /zer

# Copy source files
COPY *.c *.h Makefile ./
COPY test_modules/ test_modules/
COPY lib/ lib/
COPY tools/ tools/
COPY tests/ tests/

# Build and test
CMD ["make", "check"]
