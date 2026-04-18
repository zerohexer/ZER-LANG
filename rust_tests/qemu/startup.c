/* Minimal Cortex-M3 startup for QEMU lm3s6965evb + ARM semihosting exit.
 *
 * Based on examples/qemu-cortex-m3/startup.c but adds:
 *   - Semihosting `exit` call after main() returns so QEMU terminates
 *     with the test's exit code instead of hanging in a halt loop.
 *
 * Semihosting op SYS_EXIT_EXTENDED (0x20) with parameter block
 *   [0] = 0x20026 (ADP_Stopped_ApplicationExit)
 *   [1] = exit_code
 * makes qemu-system-arm exit with WIFEXITED=1 and WEXITSTATUS=exit_code
 * when invoked with -semihosting-config enable=on,target=native.
 */

#include <stdint.h>

extern int main(void);

/* Freestanding stubs — GCC generates calls to these for struct ops */
void *memcpy(void *dst, const void *src, unsigned int n) {
    unsigned char *d = dst; const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}
void *memset(void *dst, int c, unsigned int n) {
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}
int memcmp(const void *a, const void *b, unsigned int n) {
    const unsigned char *p = a, *q = b;
    while (n--) { if (*p != *q) return *p - *q; p++; q++; }
    return 0;
}

/* Linker-provided symbols */
extern unsigned int _estack;
extern unsigned int _sdata, _edata, _sidata;
extern unsigned int _sbss, _ebss;

/* ARM semihosting: SYS_EXIT_EXTENDED (0x20) with reason + exit code.
 * QEMU interprets this and terminates the VM with the given exit code. */
static void semihost_exit(int code) {
    /* Parameter block for SYS_EXIT_EXTENDED:
     *   [0] = reason (ADP_Stopped_ApplicationExit = 0x20026)
     *   [1] = exit code
     */
    volatile unsigned int block[2];
    block[0] = 0x20026;
    block[1] = (unsigned int)code;
    register unsigned int r0 __asm__("r0") = 0x20;          /* SYS_EXIT_EXTENDED */
    register unsigned int r1 __asm__("r1") = (unsigned int)(unsigned long)block;
    __asm__ volatile ("bkpt 0xAB" : : "r"(r0), "r"(r1) : "memory");
    /* If semihosting disabled, BKPT traps — qemu aborts */
    while (1) { }
}

void Reset_Handler(void) {
    /* Copy .data from flash to SRAM */
    unsigned int *src = &_sidata;
    unsigned int *dst = &_sdata;
    while (dst < &_edata) *dst++ = *src++;

    /* Zero .bss */
    dst = &_sbss;
    while (dst < &_ebss) *dst++ = 0;

    /* Call ZER main and terminate via semihosting */
    int ret = main();
    semihost_exit(ret);
}

void Default_Handler(void) {
    /* Exit with failure code if an exception fires */
    semihost_exit(139);
}

/* Minimal vector table */
__attribute__((section(".vectors")))
void (*const vectors[])(void) = {
    (void (*)(void))(&_estack),  /* Initial stack pointer */
    Reset_Handler,                /* Reset */
    Default_Handler,              /* NMI */
    Default_Handler,              /* HardFault */
};
