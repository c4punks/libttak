/**
 * @file sync.c
 * @brief Blocking read/write wrappers that honour the TTL guard.
 *
 * Translates POSIX read()/write() (or Windows _read()/_write()) return
 * values and errno codes into typed ttak_io_status_t results, retrying
 * on EINTR and failing fast when the guard has expired.
 */

#include <ttak/io/sync.h>

#include <ttak/mem/mem.h>

#include <errno.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
typedef int ttak_io_ssize_t;
#define ttak_io_safe_read  _read
#define ttak_io_safe_write _write
#elif TTAK_NET_LWIP
#include "../../internal/ttak/net_lwip.h"
typedef ssize_t ttak_io_ssize_t;
#define ttak_io_safe_read  read
#define ttak_io_safe_write write
#else
#include <unistd.h>
typedef ssize_t ttak_io_ssize_t;
#define ttak_io_safe_read  read
#define ttak_io_safe_write write
#endif

static ttak_io_status_t ttak_io_prepare_guard(ttak_io_guard_t *guard, uint64_t now) {
    if (!guard) return TTAK_IO_ERR_INVALID_ARGUMENT;
    if (!ttak_io_guard_valid(guard, now)) {
        ttak_io_guard_close(guard, now);
        return TTAK_IO_ERR_EXPIRED_GUARD;
    }
    return TTAK_IO_SUCCESS;
}

ttak_io_status_t ttak_io_sync_read(ttak_io_guard_t *guard,
                                   void *dst,
                                   size_t len,
                                   size_t *bytes_read,
                                   uint64_t now) {
    if (!dst && len > 0) return TTAK_IO_ERR_INVALID_ARGUMENT;

    ttak_io_status_t status = ttak_io_prepare_guard(guard, now);
    if (status != TTAK_IO_SUCCESS) return status;

    if (len == 0) {
        if (bytes_read) *bytes_read = 0;
        return TTAK_IO_SUCCESS;
    }

    ttak_io_buffer_t buffer;
    status = ttak_io_buffer_acquire(&buffer, dst, len, TTAK_IO_BUFFER_READ, now);
    if (status != TTAK_IO_SUCCESS) return status;

    ttak_io_ssize_t total = 0;
    unsigned char *cursor = (unsigned char *)ttak_io_buffer_data(&buffer);

    while ((size_t)total < len) {
        ttak_io_ssize_t rc = ttak_io_safe_read(guard->fd, cursor + total, (unsigned int)(len - (size_t)total));
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            status = TTAK_IO_ERR_SYS_FAILURE;
            break;
        }
        if (rc == 0) {
            break;
        }
        total += rc;
        if ((size_t)total == len) break;
    }

    if (status == TTAK_IO_SUCCESS) {
        status = ttak_io_buffer_sync_out(&buffer, (size_t)total, now);
        if (status == TTAK_IO_SUCCESS) {
            ttak_io_guard_refresh(guard, now);
        }
    }

    if (bytes_read) {
        *bytes_read = (status == TTAK_IO_SUCCESS) ? (size_t)total : 0;
    }

    ttak_io_buffer_release(&buffer);
    return status;
}

ttak_io_status_t ttak_io_sync_write(ttak_io_guard_t *guard,
                                    const void *src,
                                    size_t len,
                                    size_t *bytes_written,
                                    uint64_t now) {
    if (!src && len > 0) return TTAK_IO_ERR_INVALID_ARGUMENT;

    ttak_io_status_t status = ttak_io_prepare_guard(guard, now);
    if (status != TTAK_IO_SUCCESS) return status;

    if (len == 0) {
        if (bytes_written) *bytes_written = 0;
        return TTAK_IO_SUCCESS;
    }

    ttak_io_buffer_t buffer;
    status = ttak_io_buffer_acquire(&buffer, (void *)src, len, TTAK_IO_BUFFER_WRITE, now);
    if (status != TTAK_IO_SUCCESS) return status;

    status = ttak_io_buffer_sync_in(&buffer, now);
    if (status != TTAK_IO_SUCCESS) {
        ttak_io_buffer_release(&buffer);
        return status;
    }

    ttak_io_ssize_t total = 0;
    unsigned char *cursor = (unsigned char *)ttak_io_buffer_data(&buffer);

    while ((size_t)total < len) {
        ttak_io_ssize_t rc = ttak_io_safe_write(guard->fd, cursor + total, (unsigned int)(len - (size_t)total));
        if (rc < 0) {
            if (errno == EINTR) continue;
            status = TTAK_IO_ERR_SYS_FAILURE;
            break;
        }
        total += rc;
    }

    if (status == TTAK_IO_SUCCESS) {
        ttak_io_guard_refresh(guard, now);
    }

    if (bytes_written) {
        *bytes_written = (status == TTAK_IO_SUCCESS) ? (size_t)total : 0;
    }

    ttak_io_buffer_release(&buffer);
    return status;
}
