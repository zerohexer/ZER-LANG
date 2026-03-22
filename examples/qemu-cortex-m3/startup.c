/* Minimal Cortex-M3 startup for QEMU lm3s6965evb */
/* Sets up stack pointer and vector table, calls main() */

extern void main(void);

/* Linker-provided symbols */
extern unsigned int _estack;
extern unsigned int _sdata, _edata, _sidata;
extern unsigned int _sbss, _ebss;

void Reset_Handler(void) {
    /* Copy .data from flash to SRAM */
    unsigned int *src = &_sidata;
    unsigned int *dst = &_sdata;
    while (dst < &_edata) *dst++ = *src++;

    /* Zero .bss */
    dst = &_sbss;
    while (dst < &_ebss) *dst++ = 0;

    /* Call ZER main */
    main();

    /* Halt if main returns */
    while (1) {}
}

void Default_Handler(void) {
    while (1) {}
}

/* Minimal vector table */
__attribute__((section(".vectors")))
void (*const vectors[])(void) = {
    (void (*)(void))(&_estack),  /* Initial stack pointer */
    Reset_Handler,                /* Reset */
    Default_Handler,              /* NMI */
    Default_Handler,              /* HardFault */
};
