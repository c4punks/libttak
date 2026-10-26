/**
 * @file syscalls.c
 * @brief newlib retargeting for the bare-metal test harness.
 *
 * stdout/stderr go to board_putc(); exit() reports through board_exit().
 * The remaining syscalls come from libnosys (--specs=nosys.specs).
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include "board.h"

#ifndef TTAK_BM_STACK_RESERVE
#define TTAK_BM_STACK_RESERVE (16u * 1024u)
#endif

extern uint8_t _end[], _estack[];

void *_sbrk(ptrdiff_t incr) {
    static uintptr_t brk;
    if (!brk) brk = (uintptr_t)_end;
    uintptr_t limit = (uintptr_t)_estack - TTAK_BM_STACK_RESERVE;
    if (incr > 0 && (uintptr_t)incr > limit - brk) {
        errno = ENOMEM;
        return (void *)-1;
    }
    uintptr_t prev = brk;
    brk += (uintptr_t)incr;
    return (void *)prev;
}

int _write(int fd, const char *buf, int len) {
    (void)fd;
    for (int i = 0; i < len; ++i) {
        if (buf[i] == '\n') board_putc('\r');
        board_putc(buf[i]);
    }
    return len;
}

int _fstat(int fd, struct stat *st) {
    (void)fd;
    st->st_mode = S_IFCHR;
    return 0;
}

int _isatty(int fd) {
    (void)fd;
    return 1;
}

void _exit(int code) {
    board_exit(code);
}

/* abort() -> raise(SIGABRT) -> _kill(); report it like an exit code. */
int _kill(int pid, int sig) {
    (void)pid;
    board_exit(128 + sig);
}

int _getpid(void) {
    return 1;
}

/* Unbuffered stdout keeps output ordered with stderr and survives faults. */
__attribute__((constructor)) static void ttak_bm_stdio_init(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}
