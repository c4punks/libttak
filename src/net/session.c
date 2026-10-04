/**
 * @file session.c
 * @brief Network session manager: create, close, tick, and fault handling.
 *
 * Maintains a linked list of active sessions keyed by 64-bit ID.  A
 * periodic sanity tick checks immortal sockets and applies the configured
 * alert/restart policy.  Retiring and fault-queue processing can be
 * offloaded to an async worker thread.
 */

#include <ttak/net/session.h>

#include <ttak/net/core/port.h>
#include <ttak/mem/mem.h>
#include <ttak/async/task.h>
#include <ttak/async/sched.h>
#include <ttak/timing/timing.h>
#include <ttak/compat/pthread.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#endif

#define TTAK_NET_SANITY_INTERVAL_NS TT_SECOND(5)

static pthread_once_t net_port_once = PTHREAD_ONCE_INIT;
static ttak_net_driver_ops_t global_net_ops;
static ttak_net_os_t global_net_os;
static void net_port_bootstrap(void) {
    ttak_net_driver_detect(&global_net_ops, &global_net_os, NULL);
}

static void ttak_net_session_free(void *ptr) {
    if (!ptr) return;
    ttak_mem_free(ptr);
}

static void ttak_net_session_detach_locked(ttak_net_session_mgr_t *mgr,
                                           ttak_net_session_t *session) {
    if (!session) return;
    ttak_net_session_t **cursor = session->parent ? &session->parent->first_child : &mgr->head;
    while (*cursor) {
        if (*cursor == session) {
            *cursor = session->next_sibling;
            break;
        }
        cursor = &(*cursor)->next_sibling;
    }
    session->next_sibling = NULL;
    session->parent = NULL;
}

static void ttak_net_session_queue_retire(ttak_net_session_mgr_t *mgr,
                                          ttak_net_session_t *session,
                                          uint64_t now) {
    (void)now;
    session->next_retire = mgr->retire_head;
    mgr->retire_head = session;
}

static void ttak_net_session_flush_retire(ttak_net_session_t *list) {
    while (list) {
        ttak_net_session_t *next = list->next_retire;
        list->next_retire = NULL;
        ttak_epoch_retire(list, ttak_net_session_free);
        list = next;
    }
}

static void ttak_net_session_mark_for_retire(ttak_net_session_mgr_t *mgr,
                                             ttak_net_session_t *session,
                                             uint64_t now) {
    if (!session) return;
    ttak_net_session_t *child = session->first_child;
    while (child) {
        ttak_net_session_t *next = child->next_sibling;
        ttak_net_session_mark_for_retire(mgr, child, now);
        child = next;
    }
    session->first_child = NULL;

    if (session->endpoint) {
        ttak_net_endpoint_close(session->endpoint, session->owner, now);
    }

    ttak_net_session_detach_locked(mgr, session);
    session->state_flags |= TTAK_NET_SESSION_ZOMBIE;
    ttak_net_session_queue_retire(mgr, session, now);
}

void ttak_net_session_mgr_init(ttak_net_session_mgr_t *mgr, bool async_offload) {
    if (!mgr) return;
    pthread_once(&net_port_once, net_port_bootstrap);
    ttak_rwlock_init(&mgr->lock);
    mgr->head = NULL;
    mgr->next_id = 1;
    mgr->policy_flags = 0;
    mgr->retire_head = NULL;
    mgr->fault_head = NULL;
    mgr->async_offload = async_offload;
    mgr->net_ops = &global_net_ops;
}

void ttak_net_session_mgr_set_policy(ttak_net_session_mgr_t *mgr, uint32_t flags) {
    if (!mgr) return;
    mgr->policy_flags = flags;
}

ttak_net_session_t *ttak_net_session_mgr_create(ttak_net_session_mgr_t *mgr,
                                                ttak_shared_net_endpoint_t *endpoint,
                                                ttak_net_session_t *parent,
                                                ttak_owner_t *owner,
                                                uint64_t now) {
    if (!mgr || !endpoint || !owner) return NULL;

    ttak_net_session_t *session = ttak_mem_alloc_raw(sizeof(*session), __TTAK_UNSAFE_MEM_FOREVER__, now);
    if (!session) return NULL;
    memset(session, 0, sizeof(*session));
    session->endpoint = endpoint;
    session->parent = parent;
    session->owner = owner;
    session->generation = now;
    session->state_flags = TTAK_NET_SESSION_ACTIVE;
    session->lifetime_ns = TTAK_NET_SANITY_INTERVAL_NS;
    session->next_sanity_ns = now + TTAK_NET_SANITY_INTERVAL_NS;

    ttak_shared_result_t res = 0;
    ttak_net_endpoint_t *payload = ttak_shared_net_endpoint_access(endpoint, owner, &res);
    if (payload && res == TTAK_OWNER_SUCCESS) {
        uint64_t ttl = payload->guard.ttl_ns;
        if (ttl == UINT64_MAX) {
            session->state_flags |= TTAK_NET_SESSION_IMMORTAL;
            session->lifetime_ns = UINT64_MAX;
            session->next_sanity_ns = UINT64_MAX;
        } else if (ttl > 0) {
            session->lifetime_ns = ttl;
            session->next_sanity_ns = now + ttl;
        } else {
            session->lifetime_ns = TTAK_NET_SANITY_INTERVAL_NS;
            session->next_sanity_ns = now + TTAK_NET_SANITY_INTERVAL_NS;
        }
        ttak_shared_net_endpoint_release(endpoint);
    }

    ttak_rwlock_wrlock(&mgr->lock);
    session->id = mgr->next_id++;
    if (parent) {
        session->next_sibling = parent->first_child;
        parent->first_child = session;
    } else {
        session->next_sibling = mgr->head;
        mgr->head = session;
    }
    ttak_rwlock_unlock(&mgr->lock);
    return session;
}

static void ttak_net_session_log_alert(const ttak_net_session_t *session) {
    fprintf(stderr,
            "[TTAK_SOCK_ALERT] session=%" PRIu64 " owner=%p endpoint=%p\n",
            session ? session->id : 0,
            (void *)(session ? session->owner : NULL),
            (void *)(session ? session->endpoint : NULL));
}

static bool ttak_net_session_guard_healthy(ttak_net_session_t *session, uint64_t now) {
    if (!session || !session->endpoint) return false;
    ttak_net_guard_snapshot_t snap;
    if (ttak_net_endpoint_snapshot_guard(session->endpoint, session->owner, &snap, now) != TTAK_IO_SUCCESS) {
        return false;
    }

    struct pollfd pfd = {
        .fd = snap.fd,
        .events = POLLIN | POLLOUT | POLLERR | POLLHUP
    };
#ifdef _WIN32
    int rc = WSAPoll(&pfd, 1, 0);
#else
    int rc = poll(&pfd, 1, 0);
#endif
    ttak_net_endpoint_guard_commit(&snap, now);
    if (rc < 0) return false;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
    return true;
}

static void ttak_net_session_finish_restart(ttak_net_session_mgr_t *mgr,
                                            ttak_net_session_t *session,
                                            ttak_io_status_t status,
                                            uint64_t now) {
    ttak_net_session_t *pending = NULL;
    ttak_rwlock_wrlock(&mgr->lock);
    if (!(session->state_flags & TTAK_NET_SESSION_ZOMBIE)) {
        if (status == TTAK_IO_SUCCESS) {
            session->state_flags &= ~(TTAK_NET_SESSION_NEEDS_RESTART | TTAK_NET_SESSION_FAULTING);
            session->state_flags |= TTAK_NET_SESSION_ACTIVE;
            session->next_sanity_ns = now + TTAK_NET_SANITY_INTERVAL_NS;
        } else {
            ttak_net_session_mark_for_retire(mgr, session, now);
            pending = mgr->retire_head;
            mgr->retire_head = NULL;
        }
    }
    ttak_rwlock_unlock(&mgr->lock);
    if (pending) {
        ttak_net_session_flush_retire(pending);
    }
}

static void ttak_net_session_finish_shutdown(ttak_net_session_mgr_t *mgr,
                                             ttak_net_session_t *session,
                                             uint64_t now) {
    ttak_rwlock_wrlock(&mgr->lock);
    ttak_net_session_mark_for_retire(mgr, session, now);
    ttak_net_session_t *pending = mgr->retire_head;
    mgr->retire_head = NULL;
    ttak_rwlock_unlock(&mgr->lock);
    ttak_net_session_flush_retire(pending);
}

static void ttak_net_session_dispatch_fault(ttak_net_session_mgr_t *mgr,
                                            ttak_net_session_t *session,
                                            uint64_t now);

void ttak_net_session_mgr_close(ttak_net_session_mgr_t *mgr,
                                ttak_net_session_t *session,
                                uint64_t now) {
    if (!mgr || !session) return;
    ttak_rwlock_wrlock(&mgr->lock);
    ttak_net_session_mark_for_retire(mgr, session, now);
    ttak_net_session_t *pending = mgr->retire_head;
    mgr->retire_head = NULL;
    ttak_rwlock_unlock(&mgr->lock);
    ttak_net_session_flush_retire(pending);
}

static void ttak_net_session_collect_due(ttak_net_session_t *node,
                                         uint64_t now,
                                         ttak_net_session_t **due) {
    while (node) {
        if (!(node->state_flags & TTAK_NET_SESSION_ZOMBIE) &&
            (node->state_flags & TTAK_NET_SESSION_IMMORTAL) &&
            now >= node->next_sanity_ns) {
            node->sanity_next = *due;
            *due = node;
        }
        if (node->first_child) {
            ttak_net_session_collect_due(node->first_child, now, due);
        }
        node = node->next_sibling;
    }
}

void ttak_net_session_mgr_tick(ttak_net_session_mgr_t *mgr, uint64_t now) {
    if (!mgr) return;
    ttak_rwlock_rdlock(&mgr->lock);
    ttak_net_session_t *due = NULL;
    ttak_net_session_collect_due(mgr->head, now, &due);
    ttak_rwlock_unlock(&mgr->lock);
    if (!due) return;

    ttak_net_session_t *pending = NULL;
    ttak_rwlock_wrlock(&mgr->lock);
    while (due) {
        ttak_net_session_t *current = due;
        due = due->sanity_next;
        current->sanity_next = NULL;
        if (current->state_flags & TTAK_NET_SESSION_ZOMBIE) {
            continue;
        }
        current->next_sanity_ns = now + TTAK_NET_SANITY_INTERVAL_NS;
        current->sanity_next = pending;
        pending = current;
    }
    ttak_rwlock_unlock(&mgr->lock);
    if (!pending) return;

    while (pending) {
        ttak_net_session_t *current = pending;
        pending = pending->sanity_next;
        current->sanity_next = NULL;
        if (!ttak_net_session_guard_healthy(current, now)) {
            ttak_net_session_dispatch_fault(mgr, current, now);
        }
    }
}

void ttak_net_session_mgr_destroy(ttak_net_session_mgr_t *mgr, uint64_t now) {
    if (!mgr) return;

    ttak_rwlock_wrlock(&mgr->lock);
    ttak_net_session_t *cursor = mgr->head;
    while (cursor) {
        ttak_net_session_t *next = cursor->next_sibling;
        ttak_net_session_mark_for_retire(mgr, cursor, now);
        cursor = next;
    }
    mgr->head = NULL;
    ttak_net_session_t *pending = mgr->retire_head;
    mgr->retire_head = NULL;
    ttak_rwlock_unlock(&mgr->lock);

    ttak_net_session_flush_retire(pending);
    ttak_rwlock_destroy(&mgr->lock);
}
typedef struct ttak_net_session_task_ctx {
    ttak_net_session_mgr_t *mgr;
    ttak_net_session_t *session;
    uint64_t fault_ts;
    void (*work_fn)(struct ttak_net_session_task_ctx *);
} ttak_net_session_task_ctx_t;

static bool ttak_net_session_enqueue_task(ttak_net_session_mgr_t *mgr,
                                          void (*worker)(ttak_net_session_task_ctx_t *),
                                          ttak_net_session_t *session,
                                          uint64_t now);
static void ttak_net_session_schedule_shutdown(ttak_net_session_mgr_t *mgr,
                                               ttak_net_session_t *session,
                                               uint64_t now);
static void ttak_net_session_schedule_restart(ttak_net_session_mgr_t *mgr,
                                              ttak_net_session_t *session,
                                              uint64_t now);
static void *ttak_net_session_task_trampoline(void *arg) {
    ttak_net_session_task_ctx_t *ctx = (ttak_net_session_task_ctx_t *)arg;
    if (ctx && ctx->work_fn) {
        ctx->work_fn(ctx);
    }
    free(ctx);
    return NULL;
}
static void ttak_net_session_dispatch_fault(ttak_net_session_mgr_t *mgr,
                                            ttak_net_session_t *session,
                                            uint64_t now) {
    if (mgr->policy_flags & TTAK_SOCK_ALERT) {
        ttak_net_session_log_alert(session);
    }
    if (mgr->policy_flags & TTAK_SOCK_RESTART) {
        session->state_flags |= (TTAK_NET_SESSION_NEEDS_RESTART | TTAK_NET_SESSION_FAULTING);
        ttak_net_session_schedule_restart(mgr, session, now);
    } else {
        ttak_net_session_schedule_shutdown(mgr, session, now);
    }
}

static void ttak_net_session_shutdown_work(ttak_net_session_task_ctx_t *ctx) {
    if (!ctx) return;
    ttak_net_session_finish_shutdown(ctx->mgr, ctx->session, ttak_get_tick_count());
}

static void ttak_net_session_restart_work(ttak_net_session_task_ctx_t *ctx) {
    if (!ctx) return;
    ttak_io_status_t status = ttak_net_endpoint_force_restart(ctx->session->endpoint,
                                                              ctx->session->owner,
                                                              ttak_get_tick_count());
    ttak_net_session_finish_restart(ctx->mgr, ctx->session, status, ttak_get_tick_count());
}

static bool ttak_net_session_enqueue_task(ttak_net_session_mgr_t *mgr,
                                          void (*worker)(ttak_net_session_task_ctx_t *),
                                          ttak_net_session_t *session,
                                          uint64_t now) {
    if (!mgr->async_offload || !worker) return false;
    ttak_net_session_task_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) return false;
    ctx->mgr = mgr;
    ctx->session = session;
    ctx->fault_ts = now;
    ctx->work_fn = worker;
    ttak_task_t *task = ttak_task_create(ttak_net_session_task_trampoline, ctx, NULL, now);
    if (!task) {
        free(ctx);
        return false;
    }
    ttak_task_set_domain(task, TTAK_TASK_DOMAIN_NET);
    ttak_task_set_urgency(task, 85);
    ttak_async_schedule(task, now, 0);
    return true;
}

static void ttak_net_session_schedule_shutdown(ttak_net_session_mgr_t *mgr,
                                               ttak_net_session_t *session,
                                               uint64_t now) {
    if (!ttak_net_session_enqueue_task(mgr, ttak_net_session_shutdown_work, session, now)) {
        ttak_net_session_finish_shutdown(mgr, session, now);
    }
}

static void ttak_net_session_schedule_restart(ttak_net_session_mgr_t *mgr,
                                              ttak_net_session_t *session,
                                              uint64_t now) {
    if (!ttak_net_session_enqueue_task(mgr, ttak_net_session_restart_work, session, now)) {
        ttak_io_status_t status = ttak_net_endpoint_force_restart(session->endpoint, session->owner, now);
        ttak_net_session_finish_restart(mgr, session, status, now);
    }
}
