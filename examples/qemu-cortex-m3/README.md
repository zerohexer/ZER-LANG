# ZER on ARM Cortex-M3 (QEMU)

Real freestanding firmware written in ZER. No OS. No libc. No runtime.

## What This Proves

- ZER compiles to valid ARM Thumb-2 machine code via `arm-none-eabi-gcc`
- Bounds-checked arrays work on bare metal
- Optional types (`?u32 + orelse`) work freestanding
- Exhaustive enum switch works on ARM
- Total firmware size: **1225 bytes**

## Build & Run

```bash
# Prerequisites: arm-none-eabi-gcc, qemu-system-arm
make          # build firmware.elf
make qemu     # run on QEMU (Ctrl+A then X to exit)
make test     # run with 3-second timeout (auto-exits)
```

## Pipeline

```
hello.zer → zerc → hello.c → arm-none-eabi-gcc → firmware.elf → QEMU
```

## Output

```
========================================
  ZER-LANG on ARM Cortex-M3 (QEMU)
  Memory-safe C. No runtime. No OS.
========================================

Array sum (10+20+30+40): 100
100 / 4 = 25
100 / 0 = 0 (orelse 0 — division by zero caught)
State.idle = 1, State.running = 2, State.done = 3

All demos passed. ZER runs on ARM.
```

## Files

| File | Purpose |
|------|---------|
| `hello.zer` | ZER firmware source |
| `startup.c` | Cortex-M3 vector table + Reset_Handler |
| `link.ld` | Linker script (256K flash, 64K SRAM) |
| `Makefile` | Build automation |
