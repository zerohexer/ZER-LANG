/* Linker script for LM3S6965 (QEMU) — Cortex-M3.
 * Flash: 256K at 0x00000000, SRAM: 64K at 0x20000000.
 * Copied from examples/qemu-cortex-m3/link.ld — kept identical so
 * test builds use the same memory map as the firmware examples. */

MEMORY {
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 256K
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}

_estack = ORIGIN(SRAM) + LENGTH(SRAM);

SECTIONS {
    .vectors : {
        KEEP(*(.vectors))
    } > FLASH

    .text : {
        *(.text*)
        *(.rodata*)
    } > FLASH

    _sidata = LOADADDR(.data);

    .data : {
        _sdata = .;
        *(.data*)
        _edata = .;
    } > SRAM AT > FLASH

    .bss : {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        _ebss = .;
    } > SRAM
}
