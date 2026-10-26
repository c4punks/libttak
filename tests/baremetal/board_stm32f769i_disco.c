/**
 * @file board_stm32f769i_disco.c
 * @brief STM32F769I-DISCO: USART1 (PA9/PA10) -> ST-LINK virtual COM port.
 *
 * Runs from the reset clock (16 MHz HSI), which also matches libttak's
 * default ttak_platform_cpu_hz(), so the DWT-based default clock is used
 * unmodified. Console: 115200 8N1.
 */

#include <stdint.h>
#include "board.h"

#define RCC_BASE      0x40023800u
#define RCC_AHB1ENR   (*(volatile uint32_t *)(RCC_BASE + 0x30u))
#define RCC_APB2ENR   (*(volatile uint32_t *)(RCC_BASE + 0x44u))

#define GPIOA_BASE    0x40020000u
#define GPIOA_MODER   (*(volatile uint32_t *)(GPIOA_BASE + 0x00u))
#define GPIOA_OSPEEDR (*(volatile uint32_t *)(GPIOA_BASE + 0x08u))
#define GPIOA_AFRH    (*(volatile uint32_t *)(GPIOA_BASE + 0x24u))

#define USART1_BASE   0x40011000u
#define USART1_CR1    (*(volatile uint32_t *)(USART1_BASE + 0x00u))
#define USART1_BRR    (*(volatile uint32_t *)(USART1_BASE + 0x0Cu))
#define USART1_ISR    (*(volatile uint32_t *)(USART1_BASE + 0x1Cu))
#define USART1_TDR    (*(volatile uint32_t *)(USART1_BASE + 0x28u))

#define USART_ISR_TC  (1u << 6)
#define USART_ISR_TXE (1u << 7)

#define BOARD_PCLK2_HZ 16000000u
#define BOARD_BAUD     115200u

void board_init(void) {
    RCC_AHB1ENR |= (1u << 0);   /* GPIOAEN */
    RCC_APB2ENR |= (1u << 4);   /* USART1EN */
    (void)RCC_APB2ENR;          /* read back: clock enable takes effect */

    /* PA9 = USART1_TX, PA10 = USART1_RX, alternate function 7. */
    GPIOA_MODER = (GPIOA_MODER & ~((3u << 18) | (3u << 20))) | (2u << 18) | (2u << 20);
    GPIOA_OSPEEDR |= (3u << 18) | (3u << 20);
    GPIOA_AFRH = (GPIOA_AFRH & ~((0xFu << 4) | (0xFu << 8))) | (7u << 4) | (7u << 8);

    USART1_CR1 = 0;
    USART1_BRR = (BOARD_PCLK2_HZ + BOARD_BAUD / 2) / BOARD_BAUD;
    USART1_CR1 = (1u << 3) | (1u << 2) | (1u << 0); /* TE | RE | UE */
}

void board_putc(char c) {
    while (!(USART1_ISR & USART_ISR_TXE)) {
    }
    USART1_TDR = (uint8_t)c;
}

void board_halt(int code) {
    (void)code;
    while (!(USART1_ISR & USART_ISR_TC)) {
    }
    for (;;) {
        __asm__ volatile("wfi");
    }
}
