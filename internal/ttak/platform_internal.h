#ifndef TTAK_INTERNAL_PLATFORM_INTERNAL_H
#define TTAK_INTERNAL_PLATFORM_INTERNAL_H

/**
 * @file platform_internal.h
 * @brief Helpers shared by src/platform/baremetal.c and freertos.c.
 */

#include <stdint.h>

/** Hardware clock: the DWT cycle counter where available. */
uint64_t ttak_bm_hw_now_ns(void);

/** Busy-wait on the hardware clock; usable before an RTOS scheduler runs. */
void ttak_bm_spin_ns(uint64_t ns);

#endif /* TTAK_INTERNAL_PLATFORM_INTERNAL_H */
