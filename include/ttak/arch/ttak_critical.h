/**
 * @file ttak_critical.h
 * @brief Task-context and ISR-context critical sections.
 *
 *  - @c TTAK_ENTER_CRITICAL() / @c TTAK_EXIT_CRITICAL(): task context only.
 *  - @c TTAK_ENTER_CRITICAL_ISR(state) / @c TTAK_EXIT_CRITICAL_ISR(state):
 *    interrupt context only. @p state is a caller-declared
 *    @c ttak_critical_state_t that carries the interrupt mask from enter
 *    to exit; it is mandatory because FreeRTOS' FromISR API returns the
 *    mask from enter and needs it back at exit.
 *
 * Mapping: Cortex-M uses @c cpsid i / @c cpsie i (task) and PRIMASK
 * save/restore (ISR); FreeRTOS uses taskENTER_CRITICAL() and
 * taskENTER_CRITICAL_FROM_ISR(). Hosted POSIX and MSVC/x86 get no-ops —
 * preemption is the OS scheduler's job there.
 */

#ifndef TTAK_ARCH_CRITICAL_H
#define TTAK_ARCH_CRITICAL_H

#include <ttak/compat/baremetal.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Cortex-M detection (ttak_arch.h intentionally covers hosted 64-bit ISAs)
 * ============================================================================ */

#if !defined(TTAK_ARCH_CORTEX_M)
#  if defined(__ARM_ARCH_PROFILE) && (__ARM_ARCH_PROFILE == 'M')
#    define TTAK_ARCH_CORTEX_M 1
#  elif defined(__ARM_ARCH_6M__) || defined(__ARM_ARCH_7M__) \
     || defined(__ARM_ARCH_7EM__) || defined(__ARM_ARCH_8M_MAIN__) \
     || defined(__ARM_ARCH_8M_BASE__)
#    define TTAK_ARCH_CORTEX_M 1
#  endif
#endif

/* Interrupt-mask state passed from TTAK_ENTER_CRITICAL_ISR() to
 * TTAK_EXIT_CRITICAL_ISR(). */
#if TTAK_PLATFORM_FREERTOS
#  include "FreeRTOS.h"
#  include "task.h"
typedef UBaseType_t ttak_critical_state_t;
#elif defined(TTAK_ARCH_CORTEX_M)
#  include <stdint.h>
typedef uint32_t ttak_critical_state_t;
#else
typedef unsigned int ttak_critical_state_t;
#endif

/* ============================================================================
 * Task-context critical sections
 * ============================================================================ */

#if TTAK_PLATFORM_FREERTOS

#  define TTAK_ENTER_CRITICAL() taskENTER_CRITICAL()
#  define TTAK_EXIT_CRITICAL()  taskEXIT_CRITICAL()

#elif defined(TTAK_ARCH_CORTEX_M) && (defined(__GNUC__) || defined(__clang__))

#  define TTAK_ENTER_CRITICAL() __asm__ volatile("cpsid i" ::: "memory")
#  define TTAK_EXIT_CRITICAL()  __asm__ volatile("cpsie i" ::: "memory")

#else

/* Interrupts are not maskable from hosted user code. */
#  define TTAK_ENTER_CRITICAL() ((void)0)
#  define TTAK_EXIT_CRITICAL()  ((void)0)

#endif

/* ============================================================================
 * ISR-context critical sections
 * ============================================================================ */

#if TTAK_PLATFORM_FREERTOS

#  define TTAK_ENTER_CRITICAL_ISR(state) \
    do { (state) = taskENTER_CRITICAL_FROM_ISR(); } while (0)
#  define TTAK_EXIT_CRITICAL_ISR(state) \
    do { taskEXIT_CRITICAL_FROM_ISR(state); } while (0)

#elif defined(TTAK_ARCH_CORTEX_M) && (defined(__GNUC__) || defined(__clang__))

#  define TTAK_ENTER_CRITICAL_ISR(state)          \
    do {                                          \
        __asm__ volatile("mrs %0, primask\n\t"    \
                         "cpsid i"                \
                         : "=r"(state) :: "memory"); \
    } while (0)
#  define TTAK_EXIT_CRITICAL_ISR(state) \
    do { __asm__ volatile("msr primask, %0" :: "r"(state) : "memory"); } while (0)

#else

/* No-ops on hosted targets. */
#  define TTAK_ENTER_CRITICAL_ISR(state) ((void)(state))
#  define TTAK_EXIT_CRITICAL_ISR(state)  ((void)(state))

#endif

#ifdef __cplusplus
}
#endif

#endif /* TTAK_ARCH_CRITICAL_H */
