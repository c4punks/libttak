/**
 * @file io.c
 * @brief I/O guard lifecycle, buffer staging, and poll/async dispatch.
 *
 * Implements TTL expiry checks for ttak_io_guard_t, the staging buffer
 * acquire/release cycle backed by ttak_detachable arenas, and the poll
 * loop that drives async read/write completions.
 */

#include <ttak/io/io.h>

#include <ttak/async/sched.h>
#include <ttak/async/task.h>
#include <ttak/mem/mem.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <winsock2.h>
#define ttak_io_close_handle _close
#else
#include <poll.h>
#include <unistd.h>
#define ttak_io_close_handle close
#endif

typedef struct ttak_io_poll_task_ctx {
    const ttak_io_guard_t *guard;
    short events;
    int timeout_ms;
    ttak_io_poll_cb cb;
    void *user;
} ttak_io_poll_task_ctx_t;

static int ttak_io_perform_poll(int fd, short events, int timeout_ms, short *revents) {
    if (fd < 0) {
        if (revents) *revents = POLLERR;
        errno = EBADF;
        return -1;
    }

    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    pfd.events = events;

#ifdef _WIN32
    int rc = WSAPoll(&pfd, 1, timeout_ms);
#else
    int rc = poll(&pfd, 1, timeout_ms);
#endif

    if (revents) {
        *revents = (rc > 0) ? pfd.revents : 0;
    }
    return rc;
}

static void ttak_io_format_resource_tag(ttak_io_guard_t *guard) {
    if (!guard) return;
    snprintf(guard->resource_tag, sizeof(guard->resource_tag), "iofd-%d", guard->fd);
}

ttak_io_status_t ttak_io_guard_init(ttak_io_guard_t *guard,
                                    int fd,
                                    ttak_owner_t *owner,
                                    uint64_t ttl_ns,
                                    uint64_t now) {
    if (!guard || fd < 0 || ttl_ns == 0) {
        return TTAK_IO_ERR_INVALID_ARGUMENT;
    }

    memset(guard, 0, sizeof(*guard));
    guard->fd = fd;
    guard->owner = owner;
    guard->ttl_ns = ttl_ns;
    guard->expires_at = (UINT64_MAX - now < ttl_ns) ? UINT64_MAX : (now + ttl_ns);
    guard->last_used = now;
    guard->closed = false;
    ttak_io_format_resource_tag(guard);

    if (owner) {
        ttak_owner_register_resource(owner, guard->resource_tag, guard);
    }

    return TTAK_IO_SUCCESS;
}

ttak_io_status_t ttak_io_guard_refresh(ttak_io_guard_t *guard, uint64_t now) {
    if (!guard || guard->fd < 0) {
        return TTAK_IO_ERR_INVALID_ARGUMENT;
    }
    if (guard->closed) {
        return TTAK_IO_ERR_EXPIRED_GUARD;
    }
    guard->last_used = now;
    guard->expires_at = (UINT64_MAX - now < guard->ttl_ns) ? UINT64_MAX : (now + guard->ttl_ns);
    return TTAK_IO_SUCCESS;
}

ttak_io_status_t ttak_io_guard_close(ttak_io_guard_t *guard, uint64_t now) {
    if (!guard) return TTAK_IO_ERR_INVALID_ARGUMENT;
    if (guard->fd >= 0) {
        ttak_io_close_handle(guard->fd);
    }
    guard->fd = -1;
    guard->closed = true;
    guard->last_used = now;
    guard->expires_at = now;
    return TTAK_IO_SUCCESS;
}

bool ttak_io_guard_valid(const ttak_io_guard_t *guard, uint64_t now) {
    if (!guard) return false;
    if (guard->closed) return false;
    if (guard->fd < 0) return false;
    if (guard->expires_at != 0 && now > guard->expires_at) return false;
    return true;
}

static ttak_detachable_context_t *ttak_io_select_arena(void) {
    return ttak_detachable_context_default();
}

ttak_io_status_t ttak_io_buffer_acquire(ttak_io_buffer_t *buffer,
                                        void *user_ptr,
                                        size_t len,
                                        ttak_io_buffer_mode_t mode,
                                        uint64_t now) {
    if (!buffer || (len > 0 && !user_ptr)) {
        return TTAK_IO_ERR_INVALID_ARGUMENT;
    }
    memset(buffer, 0, sizeof(*buffer));
    buffer->user_ptr = user_ptr;
    buffer->len = len;
    buffer->mode = mode;
    buffer->arena = ttak_io_select_arena();

    if (len == 0) {
        buffer->staging.data = NULL;
        buffer->staging.size = 0;
        return TTAK_IO_SUCCESS;
    }

    buffer->staging = ttak_detachable_mem_alloc(buffer->arena, len, now);
    if (!buffer->staging.data) {
        return TTAK_IO_ERR_SYS_FAILURE;
    }

    if (mode == TTAK_IO_BUFFER_WRITE) {
        return ttak_io_buffer_sync_in(buffer, now);
    }
    return TTAK_IO_SUCCESS;
}

void *ttak_io_buffer_data(ttak_io_buffer_t *buffer) {
    if (!buffer) return NULL;
    return buffer->staging.data;
}

ttak_io_status_t ttak_io_buffer_sync_in(ttak_io_buffer_t *buffer, uint64_t now) {
    if (!buffer || buffer->mode != TTAK_IO_BUFFER_WRITE) {
        return TTAK_IO_ERR_INVALID_ARGUMENT;
    }
    if (buffer->len == 0) return TTAK_IO_SUCCESS;

    void *safe = ttak_mem_access(buffer->user_ptr, now);
    if (!safe && buffer->user_ptr) {
        return TTAK_IO_ERR_INVALID_ARGUMENT;
    }
    memcpy(buffer->staging.data, safe ? safe : buffer->user_ptr, buffer->len);
    return TTAK_IO_SUCCESS;
}

ttak_io_status_t ttak_io_buffer_sync_out(ttak_io_buffer_t *buffer, size_t bytes, uint64_t now) {
    if (!buffer || buffer->mode != TTAK_IO_BUFFER_READ) {
        return TTAK_IO_ERR_INVALID_ARGUMENT;
    }
    if (bytes > buffer->len) {
        return TTAK_IO_ERR_RANGE;
    }
    if (bytes == 0) return TTAK_IO_SUCCESS;

    void *safe = ttak_mem_access(buffer->user_ptr, now);
    if (!safe && buffer->user_ptr) {
        return TTAK_IO_ERR_INVALID_ARGUMENT;
    }
    memcpy(safe ? safe : buffer->user_ptr, buffer->staging.data, bytes);
    return TTAK_IO_SUCCESS;
}

void ttak_io_buffer_release(ttak_io_buffer_t *buffer) {
    if (!buffer) return;
    if (buffer->arena && buffer->staging.data) {
        ttak_detachable_mem_free(buffer->arena, &buffer->staging);
    }
    buffer->staging.data = NULL;
    buffer->staging.size = 0;
    buffer->user_ptr = NULL;
    buffer->len = 0;
}

static void *ttak_io_poll_worker(void *arg) {
    ttak_io_poll_task_ctx_t *ctx = (ttak_io_poll_task_ctx_t *)arg;
    short revents = 0;
    ttak_io_status_t status = TTAK_IO_SUCCESS;
    int fd = -1;
    uint64_t now = ttak_get_tick_count();

    if (ctx->guard && ttak_io_guard_valid(ctx->guard, now)) {
        fd = ctx->guard->fd;
        int rc = ttak_io_perform_poll(fd, ctx->events, ctx->timeout_ms, &revents);
        if (rc < 0) {
            status = TTAK_IO_ERR_SYS_FAILURE;
            revents = POLLERR;
        } else if (rc == 0) {
            status = TTAK_IO_ERR_NEEDS_RETRY;
        }
    } else {
        status = TTAK_IO_ERR_EXPIRED_GUARD;
        revents = POLLHUP;
    }

    if (ctx->cb) {
        ctx->cb(fd, (status == TTAK_IO_SUCCESS) ? revents : (short)(revents | POLLERR), ctx->user);
    }

    ttak_mem_free(ctx);
    return NULL;
}

ttak_io_status_t ttak_io_poll_wait(const ttak_io_guard_t *guard,
                                   short events,
                                   int timeout_ms,
                                   ttak_io_poll_cb callback,
                                   void *user,
                                   short *out_revents,
                                   bool schedule_async,
                                   uint64_t now) {
    if (!guard) return TTAK_IO_ERR_INVALID_ARGUMENT;
    if (!ttak_io_guard_valid(guard, now)) {
        return TTAK_IO_ERR_EXPIRED_GUARD;
    }

    if (!schedule_async) {
        short revents = 0;
        int rc = ttak_io_perform_poll(guard->fd, events, timeout_ms, &revents);
        if (out_revents) {
            *out_revents = revents;
        }
        if (rc < 0) {
            return TTAK_IO_ERR_SYS_FAILURE;
        }
        if (rc == 0) {
            return TTAK_IO_ERR_NEEDS_RETRY;
        }
        if (callback) {
            callback(guard->fd, revents, user);
        }
        return TTAK_IO_SUCCESS;
    }

    ttak_io_poll_task_ctx_t *ctx = ttak_mem_alloc_raw(sizeof(*ctx), __TTAK_UNSAFE_MEM_FOREVER__, now);
    if (!ctx) {
        return TTAK_IO_ERR_SYS_FAILURE;
    }
    ctx->guard = guard;
    ctx->events = events;
    ctx->timeout_ms = timeout_ms;
    ctx->cb = callback;
    ctx->user = user;

    ttak_task_t *task = ttak_task_create(ttak_io_poll_worker, ctx, NULL, now);
    if (!task) {
        ttak_mem_free(ctx);
        return TTAK_IO_ERR_SYS_FAILURE;
    }
    ttak_task_set_domain(task, TTAK_TASK_DOMAIN_IO);
    ttak_async_schedule(task, now, 0);
    return TTAK_IO_SUCCESS;
}
