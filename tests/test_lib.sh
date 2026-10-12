#!/bin/bash
# BUG-1593..1596: the standard library in lib/ must BUILD AND RUN as shipped.
# It had silently rotted: fmt.zer called functions only str.zer defined, io.zer
# had undeclared C functions, its C externs were emitted with a module prefix
# (`io__fopen`, never linkable), `*opaque` crossed the C boundary as a 16-byte
# struct, and a param-view return was a false leak. Nothing compiled lib/, so
# none of that was visible. This runs a program importing every module.
set -u
ZERC="$(cd "$(dirname "$0")/.." && pwd)/zerc"
LIB="$(cd "$(dirname "$0")/../lib" && pwd)"
D="$(mktemp -d)"
trap 'rm -rf "$D"' EXIT
cp "$LIB"/*.zer "$LIB"/*.h "$D"/
cat > "$D/libtest.zer" <<'ZER'
import fmt;
import io;
import str;
u8[64] rb;
u8[24] nb;
u32 main() {
    print("lib ");
    print_u32(42);
    print_nl();
    usize n = fmt_i64(nb, -1234);
    if (!bytes_equal(nb[0..n], "-1234")) { return 1; }
    if (!bytes_equal(bytes_trim("  ab "), "ab")) { return 2; }
    if (!io_write_file("libtest.out", "zer io ok")) { return 3; }
    if (io_read_file_buf("libtest.out", rb)) |s| {
        if (!bytes_equal(s, "zer io ok")) { return 4; }
    } else { return 5; }
    eprintln("lib stderr ok");
    return 0;
}
ZER
cd "$D"
fail=0
for m in fmt io str; do
    if ! "$ZERC" "$m.zer" -o "$m.c" >"$m.log" 2>&1; then
        echo "  FAIL: lib/$m.zer does not compile:"; grep -m3 -E "error|zercheck" "$m.log"; fail=1
    fi
done
if ! "$ZERC" libtest.zer -o libtest.c >zl.log 2>&1; then
    echo "  FAIL: a program importing lib/ does not compile:"; grep -m5 -E "error|zercheck" zl.log; fail=1
elif ! gcc -std=c99 -fwrapv -w -I. libtest.c -o libtest >gl.log 2>&1; then
    echo "  FAIL: GCC refuses the emitted library code:"; grep -m5 error gl.log; fail=1
else
    out=$(./libtest 2>/dev/null); rc=$?
    if [ $rc -ne 0 ] || [ "$out" != "lib 42" ]; then
        echo "  FAIL: library self-test exit=$rc stdout='$out'"; fail=1
    fi
fi
if [ $fail -eq 0 ]; then echo "OK — lib/ builds and runs (fmt, io, str)."; exit 0; fi
exit 1
