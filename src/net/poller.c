/**
 * @file poller.c
 * @brief Scalable I/O multiplexer implementation supporting epoll and poll fallback.
 */

#include <ttak/net/poller.h>
#include <ttak/mem/mem.h>

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if defined(_WIN32)
#include <winsock2.h>
#define TTAK_NET_USE_EPOLL 0
#else
#include <poll.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/epoll.h>
#define TTAK_NET_USE_EPOLL 1
#else
#define TTAK_NET_USE_EPOLL 0
#endif
#endif

ttak_io_status_t ttak_net_poller_init(ttak_net_poller_t *poller, size_t max_endpoints, uint64_t now) {
    if (!poller) return TTAK_IO_ERR_INVALID_ARGUMENT;
    if (max_endpoints == 0) max_endpoints = 64;

    memset(poller, 0, sizeof(*poller));
    ttak_mutex_init(&poller->lock);
    poller->capacity = max_endpoints;
    poller->count = 0;
    poller->epoll_fd = -1;

#if defined(TTAK_NET_USE_EPOLL) && (TTAK_NET_USE_EPOLL == 1)
    poller->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
#endif

    poller->entries = (ttak_net_poller_entry_t *)ttak_mem_alloc_raw(
        poller->capacity * sizeof(ttak_net_poller_entry_t),
        __TTAK_UNSAFE_MEM_FOREVER__,
        now
    );
    if (!poller->entries) {
#if defined(TTAK_NET_USE_EPOLL) && (TTAK_NET_USE_EPOLL == 1)
        if (poller->epoll_fd >= 0) {
            close(poller->epoll_fd);
            poller->epoll_fd = -1;
        }
#endif
        return TTAK_IO_ERR_SYS_FAILURE;
    }
    memset(poller->entries, 0, poller->capacity * sizeof(ttak_net_poller_entry_t));
    return TTAK_IO_SUCCESS;
}

void ttak_net_poller_destroy(ttak_net_poller_t *poller, uint64_t now) {
    if (!poller) return;
    (void)now;
    ttak_mutex_lock(&poller->lock);
#if defined(TTAK_NET_USE_EPOLL) && (TTAK_NET_USE_EPOLL == 1)
    if (poller->epoll_fd >= 0) {
        close(poller->epoll_fd);
        poller->epoll_fd = -1;
    }
#endif
    if (poller->entries) {
        ttak_mem_free(poller->entries);
        poller->entries = NULL;
    }
    poller->capacity = 0;
    poller->count = 0;
    ttak_mutex_unlock(&poller->lock);
    ttak_mutex_destroy(&poller->lock);
}

ttak_io_status_t ttak_net_poller_add(ttak_net_poller_t *poller,
                                     ttak_shared_net_endpoint_t *endpoint,
                                     uint32_t events,
                                     void *user_data,
                                     ttak_owner_t *owner,
                                     uint64_t now) {
    if (!poller || !endpoint || !owner) return TTAK_IO_ERR_INVALID_ARGUMENT;

    ttak_net_guard_snapshot_t snap;
    ttak_io_status_t st = ttak_net_endpoint_snapshot_guard(endpoint, owner, &snap, now);
    if (st != TTAK_IO_SUCCESS || snap.fd < 0) {
        return st != TTAK_IO_SUCCESS ? st : TTAK_IO_ERR_INVALID_ARGUMENT;
    }

    ttak_mutex_lock(&poller->lock);

    /* Check if already tracked */
    ssize_t slot = -1;
    ssize_t free_slot = -1;
    for (size_t i = 0; i < poller->capacity; ++i) {
        if (poller->entries[i].active && poller->entries[i].endpoint == endpoint) {
            slot = (ssize_t)i;
            break;
        }
        if (!poller->entries[i].active && free_slot < 0) {
            free_slot = (ssize_t)i;
        }
    }

    if (slot >= 0) {
        /* Update existing entry */
        poller->entries[slot].events = events;
        poller->entries[slot].user_data = user_data;
        poller->entries[slot].fd = snap.fd;
#if defined(TTAK_NET_USE_EPOLL) && (TTAK_NET_USE_EPOLL == 1)
        if (poller->epoll_fd >= 0) {
            struct epoll_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.data.u64 = (uint64_t)slot;
            if (events & TTAK_NET_EVENT_READ)  ev.events |= EPOLLIN;
            if (events & TTAK_NET_EVENT_WRITE) ev.events |= EPOLLOUT;
            ev.events |= EPOLLERR | EPOLLHUP;
            epoll_ctl(poller->epoll_fd, EPOLL_CTL_MOD, snap.fd, &ev);
        }
#endif
        ttak_mutex_unlock(&poller->lock);
        return TTAK_IO_SUCCESS;
    }

    if (free_slot < 0) {
        /* Expand capacity */
        size_t new_cap = poller->capacity * 2;
        ttak_net_poller_entry_t *new_entries = (ttak_net_poller_entry_t *)ttak_mem_alloc_raw(
            new_cap * sizeof(ttak_net_poller_entry_t),
            __TTAK_UNSAFE_MEM_FOREVER__,
            now
        );
        if (!new_entries) {
            ttak_mutex_unlock(&poller->lock);
            return TTAK_IO_ERR_SYS_FAILURE;
        }
        memset(new_entries, 0, new_cap * sizeof(ttak_net_poller_entry_t));
        memcpy(new_entries, poller->entries, poller->capacity * sizeof(ttak_net_poller_entry_t));
        ttak_mem_free(poller->entries);
        free_slot = (ssize_t)poller->capacity;
        poller->entries = new_entries;
        poller->capacity = new_cap;
    }

    poller->entries[free_slot].endpoint = endpoint;
    poller->entries[free_slot].fd = snap.fd;
    poller->entries[free_slot].events = events;
    poller->entries[free_slot].user_data = user_data;
    poller->entries[free_slot].active = true;
    poller->count++;

#if defined(TTAK_NET_USE_EPOLL) && (TTAK_NET_USE_EPOLL == 1)
    if (poller->epoll_fd >= 0) {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.data.u64 = (uint64_t)free_slot;
        if (events & TTAK_NET_EVENT_READ)  ev.events |= EPOLLIN;
        if (events & TTAK_NET_EVENT_WRITE) ev.events |= EPOLLOUT;
        ev.events |= EPOLLERR | EPOLLHUP;
        if (epoll_ctl(poller->epoll_fd, EPOLL_CTL_ADD, snap.fd, &ev) < 0) {
            /* If epoll fails on e.g. non-epollable descriptors, we still retain poll array */
        }
    }
#endif

    ttak_mutex_unlock(&poller->lock);
    return TTAK_IO_SUCCESS;
}

ttak_io_status_t ttak_net_poller_del(ttak_net_poller_t *poller,
                                     ttak_shared_net_endpoint_t *endpoint,
                                     ttak_owner_t *owner,
                                     uint64_t now) {
    if (!poller || !endpoint) return TTAK_IO_ERR_INVALID_ARGUMENT;
    (void)owner;
    (void)now;

    ttak_mutex_lock(&poller->lock);
    for (size_t i = 0; i < poller->capacity; ++i) {
        if (poller->entries[i].active && poller->entries[i].endpoint == endpoint) {
#if defined(TTAK_NET_USE_EPOLL) && (TTAK_NET_USE_EPOLL == 1)
            if (poller->epoll_fd >= 0 && poller->entries[i].fd >= 0) {
                epoll_ctl(poller->epoll_fd, EPOLL_CTL_DEL, poller->entries[i].fd, NULL);
            }
#endif
            memset(&poller->entries[i], 0, sizeof(ttak_net_poller_entry_t));
            if (poller->count > 0) poller->count--;
            ttak_mutex_unlock(&poller->lock);
            return TTAK_IO_SUCCESS;
        }
    }
    ttak_mutex_unlock(&poller->lock);
    return TTAK_IO_ERR_RANGE;
}

int ttak_net_poller_wait(ttak_net_poller_t *poller,
                         ttak_net_poller_event_t *out_events,
                         size_t max_events,
                         int timeout_ms,
                         uint64_t now) {
    if (!poller || !out_events || max_events == 0) return -1;
    (void)now;

    ttak_mutex_lock(&poller->lock);

#if defined(TTAK_NET_USE_EPOLL) && (TTAK_NET_USE_EPOLL == 1)
    if (poller->epoll_fd >= 0) {
        int ep_fd = poller->epoll_fd;
        size_t ev_cap = max_events > 64 ? 64 : max_events;
        struct epoll_event evs[64];
        ttak_mutex_unlock(&poller->lock);

        int rc = epoll_wait(ep_fd, evs, (int)ev_cap, timeout_ms);
        if (rc <= 0) {
            return rc;
        }

        ttak_mutex_lock(&poller->lock);
        int out_count = 0;
        for (int i = 0; i < rc && (size_t)out_count < max_events; ++i) {
            size_t idx = (size_t)evs[i].data.u64;
            if (idx < poller->capacity && poller->entries[idx].active) {
                uint32_t triggered = 0;
                if (evs[i].events & EPOLLIN)  triggered |= TTAK_NET_EVENT_READ;
                if (evs[i].events & EPOLLOUT) triggered |= TTAK_NET_EVENT_WRITE;
                if (evs[i].events & EPOLLERR) triggered |= TTAK_NET_EVENT_ERROR;
                if (evs[i].events & EPOLLHUP) triggered |= TTAK_NET_EVENT_HUP;

                out_events[out_count].endpoint = poller->entries[idx].endpoint;
                out_events[out_count].fd = poller->entries[idx].fd;
                out_events[out_count].events = triggered;
                out_events[out_count].user_data = poller->entries[idx].user_data;
                out_count++;
            }
        }
        ttak_mutex_unlock(&poller->lock);
        return out_count;
    }
#endif

    /* Portable poll fallback */
    size_t active_count = 0;
    for (size_t i = 0; i < poller->capacity; ++i) {
        if (poller->entries[i].active && poller->entries[i].fd >= 0) {
            active_count++;
        }
    }

    if (active_count == 0) {
        ttak_mutex_unlock(&poller->lock);
        if (timeout_ms > 0) {
#if defined(_WIN32)
            Sleep((DWORD)timeout_ms);
#else
            usleep((useconds_t)timeout_ms * 1000);
#endif
        }
        return 0;
    }

#if defined(_WIN32)
    WSAPOLLFD *pfds = (WSAPOLLFD *)malloc(active_count * sizeof(WSAPOLLFD));
#else
    struct pollfd *pfds = (struct pollfd *)malloc(active_count * sizeof(struct pollfd));
#endif
    size_t *entry_indices = (size_t *)malloc(active_count * sizeof(size_t));
    if (!pfds || !entry_indices) {
        free(pfds);
        free(entry_indices);
        ttak_mutex_unlock(&poller->lock);
        return -1;
    }

    size_t pfd_idx = 0;
    for (size_t i = 0; i < poller->capacity; ++i) {
        if (poller->entries[i].active && poller->entries[i].fd >= 0) {
            pfds[pfd_idx].fd = poller->entries[i].fd;
            pfds[pfd_idx].events = 0;
            if (poller->entries[i].events & TTAK_NET_EVENT_READ)  pfds[pfd_idx].events |= POLLIN;
            if (poller->entries[i].events & TTAK_NET_EVENT_WRITE) pfds[pfd_idx].events |= POLLOUT;
            pfds[pfd_idx].revents = 0;
            entry_indices[pfd_idx] = i;
            pfd_idx++;
        }
    }
    ttak_mutex_unlock(&poller->lock);

#if defined(_WIN32)
    int rc = WSAPoll(pfds, (ULONG)active_count, timeout_ms);
#else
    int rc = poll(pfds, (nfds_t)active_count, timeout_ms);
#endif

    int out_count = 0;
    if (rc > 0) {
        ttak_mutex_lock(&poller->lock);
        for (size_t i = 0; i < active_count && (size_t)out_count < max_events; ++i) {
            if (pfds[i].revents != 0) {
                size_t e_idx = entry_indices[i];
                if (e_idx < poller->capacity && poller->entries[e_idx].active) {
                    uint32_t triggered = 0;
                    if (pfds[i].revents & POLLIN)  triggered |= TTAK_NET_EVENT_READ;
                    if (pfds[i].revents & POLLOUT) triggered |= TTAK_NET_EVENT_WRITE;
                    if (pfds[i].revents & POLLERR) triggered |= TTAK_NET_EVENT_ERROR;
                    if (pfds[i].revents & POLLHUP) triggered |= TTAK_NET_EVENT_HUP;

                    out_events[out_count].endpoint = poller->entries[e_idx].endpoint;
                    out_events[out_count].fd = poller->entries[e_idx].fd;
                    out_events[out_count].events = triggered;
                    out_events[out_count].user_data = poller->entries[e_idx].user_data;
                    out_count++;
                }
            }
        }
        ttak_mutex_unlock(&poller->lock);
    }

    free(pfds);
    free(entry_indices);
    return (rc < 0) ? -1 : out_count;
}
