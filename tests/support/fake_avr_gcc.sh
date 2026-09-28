#!/bin/sh
# BUG-1375 test support: answers zerc's target probe (`gcc -dM -E -`) the way
# avr-gcc does — 16-bit size_t, __AVR__ defined — so an AVR-only checker rule
# can be tested on a host with no AVR toolchain. Anything else it is asked to do
# fails, so it can only be used for checker-verdict (negative) tests.
case "$*" in
  *-dM*) printf '#define __SIZEOF_SIZE_T__ 2\n#define __AVR__ 1\n#define __AVR_ARCH__ 5\n'; exit 0 ;;
esac
exit 1
