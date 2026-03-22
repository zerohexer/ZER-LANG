ZERC = ../../zerc.exe
CC = arm-none-eabi-gcc
CFLAGS = -mcpu=cortex-m3 -mthumb -nostdlib -ffreestanding -O2 -fwrapv
LDFLAGS = -T link.ld -Wl,--gc-sections

all: firmware.elf

# Step 1: ZER → C (no --lib: preamble provides types/runtime)
hello.c: hello.zer
	$(ZERC) hello.zer

# Step 2: C → ARM ELF
firmware.elf: hello.c startup.c link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o firmware.elf startup.c hello.c

# Step 3: Run on QEMU (Ctrl+A then X to exit)
qemu: firmware.elf
	qemu-system-arm -machine lm3s6965evb -nographic -kernel firmware.elf

# Quick run with timeout (auto-exits after 3 seconds)
test: firmware.elf
	timeout 3 qemu-system-arm -machine lm3s6965evb -nographic -kernel firmware.elf || true

clean:
	rm -f hello.c firmware.elf

.PHONY: all qemu test clean
