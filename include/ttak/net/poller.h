/**
 * @file poller.h
 * @brief Scalable I/O multiplexer (epoll / poll / Windows WSAPoll) for network endpoints.
 */

#ifndef TTAK_NET_POLLER_H
#define TTAK_NET_POLLER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <ttak/net/endpoint.h>
#include <ttak/io/io.h>
#include <ttak/sync/sync.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ttak_net_event {
    TTAK_NET_EVENT_READ  = (1u << 0),
    TTAK_NET_EVENT_WRITE = (1u << 1),
    TTAK_NET_EVENT_ERROR = (1u << 2),
    TTAK_NET_EVENT_HUP   = (1u << 3)
} ttak_net_event_t;

typedef struct ttak_net_poller_event {
    ttak_shared_net_endpoint_t *endpoint;
    int fd;
    uint32_t events; /* Bitwise OR of ttak_net_event_t */
    void *user_data;
} ttak_net_poller_event_t;

typedef struct ttak_net_poller_entry {
    ttak_shared_net_endpoint_t *endpoint;
    int fd;
    uint32_t events;
    void *user_data;
    bool active;
} ttak_net_poller_entry_t;

typedef struct ttak_net_poller {
    ttak_mutex_t lock;
    int epoll_fd; /* >= 0 when epoll is used on Linux, -1 on poll fallback/baremetal */
    size_t capacity;
    size_t count;
    ttak_net_poller_entry_t *entries;
} ttak_net_poller_t;

/**
 * @brief Initializes a net poller instance.
 *
 * @param poller Poller struct to initialize.
 * @param max_endpoints Capacity hint for tracked endpoints.
 * @param now Current timestamp in nanoseconds.
 * @return ttak_io_status_t TTAK_IO_SUCCESS or error.
 */
ttak_io_status_t ttak_net_poller_init(ttak_net_poller_t *poller, size_t max_endpoints, uint64_t now);

/**
 * @brief Destroys the poller and frees tracked tables.
 */
void ttak_net_poller_destroy(ttak_net_poller_t *poller, uint64_t now);

/**
 * @brief Adds or updates an endpoint in the poller.
 *
 * @param poller The poller instance.
 * @param endpoint Shared endpoint to monitor.
 * @param events Bitmask of events to watch (TTAK_NET_EVENT_READ, etc.).
 * @param user_data Opaque pointer returned upon event trigger.
 * @param owner Calling owner.
 * @param now Current timestamp in nanoseconds.
 * @return ttak_io_status_t Status code.
 */
ttak_io_status_t ttak_net_poller_add(ttak_net_poller_t *poller,
                                     ttak_shared_net_endpoint_t *endpoint,
                                     uint32_t events,
                                     void *user_data,
                                     ttak_owner_t *owner,
                                     uint64_t now);

/**
 * @brief Removes an endpoint from the poller.
 */
ttak_io_status_t ttak_net_poller_del(ttak_net_poller_t *poller,
                                     ttak_shared_net_endpoint_t *endpoint,
                                     ttak_owner_t *owner,
                                     uint64_t now);

/**
 * @brief Waits for ready events up to timeout_ms.
 *
 * @param poller The poller instance.
 * @param out_events Array to store ready events.
 * @param max_events Capacity of out_events.
 * @param timeout_ms Timeout in milliseconds (-1 for infinite, 0 for non-blocking poll).
 * @param now Current timestamp in nanoseconds.
 * @return Number of ready events (>= 0), or -1 on error.
 */
int ttak_net_poller_wait(ttak_net_poller_t *poller,
                         ttak_net_poller_event_t *out_events,
                         size_t max_events,
                         int timeout_ms,
                         uint64_t now);

#ifdef __cplusplus
}
#endif

#endif /* TTAK_NET_POLLER_H */
