#!/bin/bash
# ZER-LANG Release Packaging Script
# Creates platform-specific zip files for distribution.
#
# Usage: ./scripts/release.sh <version>
# Example: ./scripts/release.sh 0.2
#
# Output: release/
#   zer-v0.2-windows-x86_64.zip   (zerc.exe + portable MinGW-w64)
#   zer-v0.2-linux-x86_64.zip     (zerc binary, expects system gcc)
#   zer-v0.2-macos-x86_64.zip     (zerc binary, expects system cc/gcc)

set -e

VERSION="${1:?Usage: release.sh <version>}"
RELEASE_DIR="release/v${VERSION}"
rm -rf "$RELEASE_DIR"
mkdir -p "$RELEASE_DIR"

echo "=== Building ZER v${VERSION} ==="

# ---- Linux build (via Docker) ----
echo "--- Linux x86_64 ---"
LINUX_DIR="${RELEASE_DIR}/zer-v${VERSION}-linux-x86_64"
mkdir -p "$LINUX_DIR/lib"

docker build --no-cache -t zer-build . 2>/dev/null
docker run --rm zer-build bash -c 'make zerc && cat zerc' > "${LINUX_DIR}/zerc"
chmod +x "${LINUX_DIR}/zerc"

# Copy stdlib and examples
cp lib/*.zer "${LINUX_DIR}/lib/" 2>/dev/null || true
cp lib/*.h "${LINUX_DIR}/lib/" 2>/dev/null || true
cp -r examples "${LINUX_DIR}/examples" 2>/dev/null || true

# Create README for the release
cat > "${LINUX_DIR}/README.txt" << EOF
ZER v${VERSION} — Memory-Safe C
================================

Requires: GCC (apt install gcc / yum install gcc)

Usage:
  ./zerc input.zer -o output.c       # emit C
  ./zerc input.zer --run             # emit C + compile + run
  ./zerc input.zer --lib -o lib.c    # emit without runtime

stdlib:
  lib/str.zer   — string/byte operations (freestanding)
  lib/fmt.zer   — formatted output (needs libc)
  lib/io.zer    — file I/O (needs libc)

More info: https://github.com/zerohexer/ZER-LANG
License: MPL-2.0 with Runtime Exception
EOF

(cd "$RELEASE_DIR" && zip -r "zer-v${VERSION}-linux-x86_64.zip" "zer-v${VERSION}-linux-x86_64/")
echo "Created: ${RELEASE_DIR}/zer-v${VERSION}-linux-x86_64.zip"

# ---- Windows build ----
echo "--- Windows x86_64 ---"
WIN_DIR="${RELEASE_DIR}/zer-v${VERSION}-windows-x86_64"
mkdir -p "$WIN_DIR/lib"

# Build zerc.exe (native or cross-compile)
if command -v x86_64-w64-mingw32-gcc &>/dev/null; then
    # Cross-compile from Linux
    x86_64-w64-mingw32-gcc -std=c99 -O2 -o "${WIN_DIR}/zerc.exe" \
        lexer.c parser.c ast.c types.c checker.c emitter.c zercheck.c zerc_main.c
elif command -v gcc &>/dev/null && [[ "$OSTYPE" == "msys"* || "$OSTYPE" == "mingw"* || "$OSTYPE" == "cygwin"* ]]; then
    # Native Windows build
    gcc -std=c99 -O2 -o "${WIN_DIR}/zerc.exe" \
        lexer.c parser.c ast.c types.c checker.c emitter.c zercheck.c zerc_main.c
else
    echo "WARNING: Cannot build Windows binary (no mingw cross-compiler)"
    echo "         Build manually: make zerc → copy zerc.exe to ${WIN_DIR}/"
fi

cp lib/*.zer "${WIN_DIR}/lib/" 2>/dev/null || true
cp lib/*.h "${WIN_DIR}/lib/" 2>/dev/null || true
cp -r examples "${WIN_DIR}/examples" 2>/dev/null || true

cat > "${WIN_DIR}/README.txt" << EOF
ZER v${VERSION} — Memory-Safe C
================================

Quick start:
  zerc.exe input.zer --run

If you have GCC in PATH, --run works immediately.

To bundle GCC (so others don't need to install it):
  1. Download MinGW-w64 portable from:
     https://github.com/niXman/mingw-builds-binaries/releases
  2. Extract into this folder as: gcc/
     So you have: zerc.exe + gcc/bin/gcc.exe
  3. zerc will find the bundled GCC automatically.

Usage:
  zerc.exe input.zer -o output.c       # emit C
  zerc.exe input.zer --run             # emit C + compile + run
  zerc.exe input.zer --lib -o lib.c    # emit without runtime

More info: https://github.com/zerohexer/ZER-LANG
License: MPL-2.0 with Runtime Exception
EOF

(cd "$RELEASE_DIR" && zip -r "zer-v${VERSION}-windows-x86_64.zip" "zer-v${VERSION}-windows-x86_64/")
echo "Created: ${RELEASE_DIR}/zer-v${VERSION}-windows-x86_64.zip"

# ---- macOS note ----
echo "--- macOS ---"
MACOS_DIR="${RELEASE_DIR}/zer-v${VERSION}-macos-x86_64"
mkdir -p "$MACOS_DIR/lib"

cat > "${MACOS_DIR}/README.txt" << EOF
ZER v${VERSION} — Memory-Safe C
================================

macOS: Build from source.

  1. Install Xcode Command Line Tools: xcode-select --install
  2. Clone: git clone https://github.com/zerohexer/ZER-LANG
  3. Build: make zerc
  4. Run: ./zerc input.zer --run

macOS 'gcc' is actually Apple Clang — it compiles ZER's emitted C fine.

More info: https://github.com/zerohexer/ZER-LANG
License: MPL-2.0 with Runtime Exception
EOF

cp lib/*.zer "${MACOS_DIR}/lib/" 2>/dev/null || true
cp lib/*.h "${MACOS_DIR}/lib/" 2>/dev/null || true

(cd "$RELEASE_DIR" && zip -r "zer-v${VERSION}-macos-x86_64.zip" "zer-v${VERSION}-macos-x86_64/")
echo "Created: ${RELEASE_DIR}/zer-v${VERSION}-macos-x86_64.zip"

echo ""
echo "=== Release v${VERSION} packages ==="
ls -lh "${RELEASE_DIR}"/*.zip
echo ""
echo "To publish: gh release create v${VERSION} ${RELEASE_DIR}/*.zip --title 'ZER v${VERSION}'"
