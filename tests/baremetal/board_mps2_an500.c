/**
 * @file board_mps2_an500.c
 * @brief QEMU mps2-an500 (Cortex-M7): semihosting console and exit code.
 *
 * QEMU does not model the DWT cycle counter, so this board overrides the
 * weak ttak_platform_now_ns() with a SysTick-driven clock. Run with
 * -semihosting-config enable=on,target=native.
 */

#include <stdint.h>
#include "board.h"

#define SYST_CSR (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR (*(volatile uint32_t *)0xE000E018u)

#define BOARD_CPU_HZ   25000000u
#define TICK_RELOAD    (BOARD_CPU_HZ / 1000u - 1u)

#define SEMIHOST_SYS_WRITEC        0x03
#define SEMIHOST_SYS_EXIT_EXTENDED 0x20
#define ADP_STOPPED_APPLICATION_EXIT 0x20026u

static volatile uint64_t g_ms;

static uint32_t semihost(uint32_t op, const void *arg) {
    register uint32_t r0 __asm__("r0") = op;
    register const void *r1 __asm__("r1") = arg;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

void SysTick_Handler(void) {
    g_ms++;
}

uint32_t ttak_platform_cpu_hz(void) {
    return BOARD_CPU_HZ;
}

uint64_t ttak_platform_now_ns(void) {
    uint64_t ms;
    uint32_t val;
    do {
        ms = g_ms;
        val = SYST_CVR;
    } while (ms != g_ms);
    return ms * 1000000ull + ((uint64_t)(TICK_RELOAD - val) * 1000000ull) / (TICK_RELOAD + 1u);
}

void board_init(void) {
    SYST_RVR = TICK_RELOAD;
    SYST_CVR = 0;
    SYST_CSR = 7u; /* CLKSOURCE=cpu | TICKINT | ENABLE */
}

void board_putc(char c) {
    semihost(SEMIHOST_SYS_WRITEC, &c);
}

void board_halt(int code) {
    uint32_t block[2] = { ADP_STOPPED_APPLICATION_EXIT, (uint32_t)code };
    semihost(SEMIHOST_SYS_EXIT_EXTENDED, block);
    for (;;) {
    }
}
