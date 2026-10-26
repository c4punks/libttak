/**
 * @file board.h
 * @brief Board hooks for the bare-metal unit-test harness.
 *
 * Each board_*.c implements console output and how to stop. board_exit()
 * (startup_cortex_m.c) prints "@@TTAK_EXIT=<code>@@" before calling
 * board_halt() so host scripts can read the result from a serial port or
 * emulator log.
 */

#ifndef TTAK_BAREMETAL_BOARD_H
#define TTAK_BAREMETAL_BOARD_H

void board_init(void);
void board_putc(char c);
void board_halt(int code) __attribute__((noreturn));
void board_exit(int code) __attribute__((noreturn));

#endif /* TTAK_BAREMETAL_BOARD_H */
