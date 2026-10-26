/**
 * @file startup_cortex_m.c
 * @brief Minimal ARMv7-M startup: vector table, .data/.bss init, FPU enable.
 */

#include <stdint.h>
#include <string.h>
#include "board.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern void __libc_init_array(void);
extern int main(void);
extern void SysTick_Handler(void);

void Reset_Handler(void);
void Default_Handler(void);
void HardFault_Handler(void);

void NMI_Handler(void)        __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler(void)  __attribute__((weak, alias("HardFault_Handler")));
void BusFault_Handler(void)   __attribute__((weak, alias("HardFault_Handler")));
void UsageFault_Handler(void) __attribute__((weak, alias("HardFault_Handler")));
void SVC_Handler(void)        __attribute__((weak, alias("Default_Handler")));
void DebugMon_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler(void)     __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void)    __attribute__((weak, alias("Default_Handler")));

__attribute__((section(".isr_vector"), used))
void (*const g_vectors[16])(void) = {
    (void (*)(void))&_estack,
    Reset_Handler,
    NMI_Handler,
    HardFault_Handler,
    MemManage_Handler,
    BusFault_Handler,
    UsageFault_Handler,
    0, 0, 0, 0,
    SVC_Handler,
    DebugMon_Handler,
    0,
    PendSV_Handler,
    SysTick_Handler,
};

#define SCB_CPACR (*(volatile uint32_t *)0xE000ED88u)
#define SCB_SHCSR (*(volatile uint32_t *)0xE000ED24u)
#define SCB_CFSR  (*(volatile uint32_t *)0xE000ED28u)
#define SCB_HFSR  (*(volatile uint32_t *)0xE000ED2Cu)
#define SCB_MMFAR (*(volatile uint32_t *)0xE000ED34u)
#define SCB_BFAR  (*(volatile uint32_t *)0xE000ED38u)

/* Linked with -nostartfiles: __libc_init_array still calls these. */
void _init(void) {}
void _fini(void) {}

static void reset_c(void) __attribute__((used, noreturn));
static void reset_c(void) {
    uint32_t *src = &_sidata;
    for (uint32_t *dst = &_sdata; dst < &_edata;) *dst++ = *src++;
    for (uint32_t *dst = &_sbss; dst < &_ebss;) *dst++ = 0;

    /* Route MemManage/BusFault/UsageFault to their own handlers. */
    SCB_SHCSR |= (1u << 16) | (1u << 17) | (1u << 18);

    board_init();
    __libc_init_array();
    board_exit(main());
}

/* Enable CP10/CP11 before any compiler-generated VFP instruction runs. */
__attribute__((naked)) void Reset_Handler(void) {
    __asm__ volatile(
        "ldr r0, =0xE000ED88\n\t"
        "ldr r1, [r0]\n\t"
        "orr r1, r1, #(0xF << 20)\n\t"
        "str r1, [r0]\n\t"
        "dsb\n\t"
        "isb\n\t"
        "b reset_c\n\t");
}

static void put_str(const char *s) {
    while (*s) board_putc(*s++);
}

static void put_hex(uint32_t v) {
    static const char digits[] = "0123456789abcdef";
    put_str("0x");
    for (int i = 28; i >= 0; i -= 4) board_putc(digits[(v >> i) & 0xF]);
}

/* Fault output bypasses stdio: the heap or newlib state may be corrupt. */
void HardFault_Handler(void) {
    uint32_t *frame;
    __asm__ volatile(
        "tst lr, #4\n\t"
        "ite eq\n\t"
        "mrseq %0, msp\n\t"
        "mrsne %0, psp\n\t" : "=r"(frame));
    put_str("\r\n[FAULT] CFSR=");  put_hex(SCB_CFSR);
    put_str(" HFSR=");             put_hex(SCB_HFSR);
    put_str(" MMFAR=");            put_hex(SCB_MMFAR);
    put_str(" BFAR=");             put_hex(SCB_BFAR);
    put_str(" PC=");               put_hex(frame[6]);
    put_str(" LR=");               put_hex(frame[5]);
    put_str("\r\n");
    board_exit(99);
}

void board_exit(int code) {
    static const char digits[] = "0123456789";
    char buf[12];
    int n = 0;
    unsigned v = (code < 0) ? (unsigned)-code : (unsigned)code;
    do { buf[n++] = digits[v % 10]; v /= 10; } while (v && n < (int)sizeof(buf));
    put_str("\r\n@@TTAK_EXIT=");
    if (code < 0) board_putc('-');
    while (n) board_putc(buf[--n]);
    put_str("@@\r\n");
    board_halt(code);
}

void Default_Handler(void) {
    put_str("\r\n[FAULT] unexpected exception\r\n");
    board_exit(98);
}
