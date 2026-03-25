FROM gcc:13

WORKDIR /zer

# Copy source files
COPY *.c *.h Makefile ./
COPY test_modules/ test_modules/

# Build and test
CMD ["make", "check"]
