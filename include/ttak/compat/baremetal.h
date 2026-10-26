/**
 * @file baremetal.h
 * @brief Minimal POSIX surface for OS-less (bare-metal) targets.
 *
 * Build with @c -DTTAK_PLATFORM_BAREMETAL=1 (the Makefile does this for
 * @c PLATFORM=baremetal) to target microcontrollers such as Cortex-M parts
 * running newlib without an RTOS. newlib declares nothing for threads and
 * clocks unless the POSIX feature macros are set, so this header supplies
 * the handful of calls libttak relies on:
 *
 *  - clock_gettime() / nanosleep() / usleep() backed by ttak_platform_now_ns()
 *  - sched_yield() as a no-op
 *
 * Threading primitives are covered by the single-threaded stubs in
 * <ttak/compat/pthread.h>. Adding -DTTAK_PLATFORM_FREERTOS=1 (Makefile:
 * PLATFORM=freertos) swaps them, and the clock/sleep/yield calls, for the
 * FreeRTOS implementations in src/platform/freertos.c.
 *
 * All definitions in src/platform/baremetal.c are weak, so applications
 * that already provide clock_gettime() (e.g. from their own syscalls) or a
 * better time source keep theirs. The usual override is:
 *
 * @code
 * uint32_t ttak_platform_cpu_hz(void) { return SystemCoreClock; }
 * @endcode
 *
 * The default ttak_platform_now_ns() uses the DWT cycle counter on
 * ARMv7-M / ARMv8-M Mainline. Cores without one (Cortex-M0/M0+/M23) or
 * emulators that do not model DWT must override ttak_platform_now_ns().
 */

#ifndef TTAK_COMPAT_BAREMETAL_H
#define TTAK_COMPAT_BAREMETAL_H

#ifndef TTAK_PLATFORM_BAREMETAL
#define TTAK_PLATFORM_BAREMETAL 0
#endif

/* FreeRTOS builds are bare-metal builds with a real threading backend. */
#ifndef TTAK_PLATFORM_FREERTOS
#define TTAK_PLATFORM_FREERTOS 0
#endif
#if TTAK_PLATFORM_FREERTOS && !TTAK_PLATFORM_BAREMETAL
#error "TTAK_PLATFORM_FREERTOS=1 also requires TTAK_PLATFORM_BAREMETAL=1"
#endif

/* The socket and I/O modules on lwIP's BSD socket API. Its sockets need
 * lwIP's tcpip thread, so this needs FreeRTOS. */
#ifndef TTAK_NET_LWIP
#define TTAK_NET_LWIP 0
#endif
#if TTAK_NET_LWIP && !TTAK_PLATFORM_FREERTOS
#error "TTAK_NET_LWIP=1 requires TTAK_PLATFORM_FREERTOS=1"
#endif

#if TTAK_PLATFORM_BAREMETAL

#include <stdint.h>
#include <time.h>
#include <sys/types.h> /* clockid_t, useconds_t */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME ((clockid_t)1)
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC ((clockid_t)4)
#endif

int clock_gettime(clockid_t clock_id, struct timespec *tp);
int nanosleep(const struct timespec *rqtp, struct timespec *rmtp);
int usleep(useconds_t usec);
int sched_yield(void);

/**
 * @brief Monotonic nanosecond clock used by every libttak timing path.
 *
 * Weak; override to use SysTick, a hardware timer or an RTC.
 */
uint64_t ttak_platform_now_ns(void);

/**
 * @brief Core clock frequency used to scale the DWT cycle counter.
 *
 * Weak; the default returns 16 MHz (STM32 HSI after reset).
 */
uint32_t ttak_platform_cpu_hz(void);

#ifdef __cplusplus
}
#endif

#endif /* TTAK_PLATFORM_BAREMETAL */

#endif /* TTAK_COMPAT_BAREMETAL_H */
