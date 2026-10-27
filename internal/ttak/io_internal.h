/**
 * @file io_internal.h
 * @brief Shared internals of the io modules.
 */

#ifndef TTAK_INTERNAL_IO_H
#define TTAK_INTERNAL_IO_H

#include <ttak/mem/detachable.h>

/**
 * @brief Detachable context for I/O staging buffers and zero-copy regions.
 *
 * These buffers have a single owner that frees them explicitly, so they are
 * released at once rather than retired to an epoch. The default context
 * retires them, and nothing reclaims them unless the application drives
 * the epoch GC itself.
 */
ttak_detachable_context_t *ttak_io_arena(void);

#endif /* TTAK_INTERNAL_IO_H */
