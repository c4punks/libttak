/**
 * @file async.c
 * @brief Asynchronous I/O: schedules read/write tasks on the worker pool.
 *
 * Wraps ttak_io_sync_read / ttak_io_sync_write as async tasks submitted
 * to the scheduler.  The completion callback is invoked on the worker
 * thread when the operation finishes or the timeout expires.
 */

#include <ttak/io/async.h>

#include <ttak/io/sync.h>
#include <ttak/async/sched.h>
#include <ttak/mem/mem.h>
#include <ttak/timing/timing.h>

#ifdef _WIN32
#include <winsock2.h>
#elif TTAK_NET_LWIP
#include "../../internal/ttak/net_lwip.h"
#else
#include <poll.h>
#endif

typedef struct ttak_io_async_ctx {
    ttak_io_guard_t *guard;
    void *buffer;
    size_t len;
    int timeout_ms;
    ttak_io_async_result_cb cb;
    void *user;
    bool is_write;
} ttak_io_async_ctx_t;

static void ttak_io_async_finish(ttak_io_async_ctx_t *ctx, ttak_io_status_t status, size_t bytes) {
    if (ctx->cb) {
        ctx->cb(status, bytes, ctx->user);
    }
    ttak_mem_free(ctx);
}

static void ttak_io_async_read_ready(int fd, short revents, void *user) {
    (void)fd;
    ttak_io_async_ctx_t *ctx = (ttak_io_async_ctx_t *)user;
    size_t bytes = 0;
    ttak_io_status_t status;
    uint64_t now = ttak_get_tick_count();

    if (!(revents & POLLIN)) {
        status = TTAK_IO_ERR_NEEDS_RETRY;
    } else {
        status = ttak_io_sync_read(ctx->guard, ctx->buffer, ctx->len, &bytes, now);
    }

    ttak_io_async_finish(ctx, status, bytes);
}

static void ttak_io_async_write_ready(int fd, short revents, void *user) {
    (void)fd;
    ttak_io_async_ctx_t *ctx = (ttak_io_async_ctx_t *)user;
    size_t bytes = 0;
    ttak_io_status_t status;
    uint64_t now = ttak_get_tick_count();

    if (!(revents & POLLOUT)) {
        status = TTAK_IO_ERR_NEEDS_RETRY;
    } else {
        status = ttak_io_sync_write(ctx->guard, ctx->buffer, ctx->len, &bytes, now);
    }

    ttak_io_async_finish(ctx, status, bytes);
}

ttak_io_status_t ttak_io_async_read(ttak_io_guard_t *guard,
                                    void *dst,
                                    size_t len,
                                    int timeout_ms,
                                    ttak_io_async_result_cb cb,
                                    void *user,
                                    uint64_t now) {
    if (!guard || (!dst && len > 0)) {
        return TTAK_IO_ERR_INVALID_ARGUMENT;
    }

    ttak_io_async_ctx_t *ctx = ttak_mem_alloc_raw(sizeof(*ctx), __TTAK_UNSAFE_MEM_FOREVER__, now);
    if (!ctx) return TTAK_IO_ERR_SYS_FAILURE;
    ctx->guard = guard;
    ctx->buffer = dst;
    ctx->len = len;
    ctx->timeout_ms = timeout_ms;
    ctx->cb = cb;
    ctx->user = user;
    ctx->is_write = false;

    ttak_io_status_t status = ttak_io_poll_wait(guard,
                                                POLLIN,
                                                timeout_ms,
                                                ttak_io_async_read_ready,
                                                ctx,
                                                NULL,
                                                true,
                                                now);
    if (status != TTAK_IO_SUCCESS) {
        ttak_mem_free(ctx);
    }
    return status;
}

ttak_io_status_t ttak_io_async_write(ttak_io_guard_t *guard,
                                     const void *src,
                                     size_t len,
                                     int timeout_ms,
                                     ttak_io_async_result_cb cb,
                                     void *user,
                                     uint64_t now) {
    if (!guard || (!src && len > 0)) {
        return TTAK_IO_ERR_INVALID_ARGUMENT;
    }

    ttak_io_async_ctx_t *ctx = ttak_mem_alloc_raw(sizeof(*ctx), __TTAK_UNSAFE_MEM_FOREVER__, now);
    if (!ctx) return TTAK_IO_ERR_SYS_FAILURE;
    ctx->guard = guard;
    ctx->buffer = (void *)src;
    ctx->len = len;
    ctx->timeout_ms = timeout_ms;
    ctx->cb = cb;
    ctx->user = user;
    ctx->is_write = true;

    ttak_io_status_t status = ttak_io_poll_wait(guard,
                                                POLLOUT,
                                                timeout_ms,
                                                ttak_io_async_write_ready,
                                                ctx,
                                                NULL,
                                                true,
                                                now);
    if (status != TTAK_IO_SUCCESS) {
        ttak_mem_free(ctx);
    }
    return status;
}
