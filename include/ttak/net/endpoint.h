#ifndef TTAK_NET_ENDPOINT_H
#define TTAK_NET_ENDPOINT_H

#include <stdint.h>
#include <stdbool.h>

#include <ttak/shared/shared.h>
#include <ttak/io/io.h>
#include <ttak/net/lattice.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Transport families supported by the endpoint wrapper.
 */
typedef enum ttak_net_endpoint_type {
    TTAK_NET_ENDPOINT_IPV4 = 0,
    TTAK_NET_ENDPOINT_IPV6 = 1,
    TTAK_NET_ENDPOINT_UNIX = 2
} ttak_net_endpoint_type_t;

typedef enum ttak_net_endpoint_role {
    TTAK_NET_ROLE_CLIENT = (1u << 0),
    TTAK_NET_ROLE_SERVER = (1u << 1),
    TTAK_NET_ROLE_CUSTOM = (1u << 2),
    TTAK_NET_ROLE_LATTICE_ACCEL = (1u << 3)
} ttak_net_endpoint_role_t;

struct ttak_net_endpoint;
typedef ttak_io_status_t (*ttak_net_restart_op)(struct ttak_net_endpoint *ep, uint64_t now);

/**
 * @brief Shared socket wrapper guarded by ttak_io_guard_t and owner policies.
 */
typedef struct ttak_net_endpoint {
    int fd;
    int domain;
    int socktype;
    int protocol;
    ttak_net_endpoint_type_t type;
    uint64_t generation_id;
    ttak_io_guard_t guard;
    uint32_t role_flags;
    ttak_net_restart_op restart;
    int listen_backlog;
    void *restart_ctx;
    ttak_net_lattice_t *lattice; /* Optional acceleration lattice */
    struct {
        uint8_t storage[128];
        uint8_t len;
    } addr;
} ttak_net_endpoint_t;

TTAK_SHARED_DEFINE_WRAPPER(net_endpoint, ttak_net_endpoint_t)

typedef struct ttak_net_guard_snapshot {
    int fd;
    uint64_t guard_generation;
    uint64_t ttl_ns;
    ttak_shared_net_endpoint_t *endpoint;
    ttak_owner_t *owner;
} ttak_net_guard_snapshot_t;

/**
 * @brief Role configuration passed to ttak_net_endpoint_set_role().
 *
 * @param role_flags Combination of ::ttak_net_endpoint_role flags.
 * @param listen_backlog Desired backlog for server sockets (falls back to SOMAXCONN).
 * @param restart_ctx Optional user context forwarded to custom restart hooks.
 * @param restart_cb Optional custom restart handler.
 */
typedef struct ttak_net_endpoint_attr {
    uint32_t role_flags;
    int listen_backlog;
    void *restart_ctx;
    ttak_net_restart_op restart_cb;
} ttak_net_endpoint_attr_t;

/**
 * @brief Constructs a shared endpoint bound to @p owner.
 */
ttak_shared_net_endpoint_t *ttak_net_endpoint_create(ttak_owner_t *owner, uint64_t now);

/**
 * @brief Closes and destroys the shared endpoint.
 */
void ttak_net_endpoint_destroy(ttak_shared_net_endpoint_t *endpoint,
                               ttak_owner_t *owner,
                               uint64_t now);

/**
 * @brief Binds an existing descriptor into the endpoint wrapper.
 */
ttak_io_status_t ttak_net_endpoint_bind_fd(ttak_shared_net_endpoint_t *endpoint,
                                           ttak_owner_t *owner,
                                           int fd,
                                           int domain,
                                           int socktype,
                                           int protocol,
                                           const void *addr_bytes,
                                           uint8_t addr_len,
                                           ttak_net_endpoint_type_t type,
                                           uint64_t ttl_ns,
                                           uint64_t now);

/**
 * @brief Configures the semantic role for restart handling.
 *
 * @param endpoint Shared endpoint instance.
 * @param role_flags Bitmask composed of ::ttak_net_endpoint_role_t constants.
 * @param restart_cb Optional custom restart hook (pass NULL to use defaults).
 */
void ttak_net_endpoint_set_role(ttak_shared_net_endpoint_t *endpoint,
                                const ttak_net_endpoint_attr_t *attr,
                                ttak_owner_t *owner,
                                uint64_t now);

/**
 * @brief Closes the descriptor tracked by the endpoint.
 */
ttak_io_status_t ttak_net_endpoint_close(ttak_shared_net_endpoint_t *endpoint,
                                         ttak_owner_t *owner,
                                         uint64_t now);

/**
 * @brief Captures guard metadata so callers can use the fd safely after
 *        releasing shared access.
 */
ttak_io_status_t ttak_net_endpoint_snapshot_guard(ttak_shared_net_endpoint_t *endpoint,
                                                  ttak_owner_t *owner,
                                                  ttak_net_guard_snapshot_t *snapshot,
                                                  uint64_t now);

/**
 * @brief Reapplies TTL refresh once the caller finishes using the snapshot.
 */
ttak_io_status_t ttak_net_endpoint_guard_commit(const ttak_net_guard_snapshot_t *snapshot,
                                                uint64_t now);

/**
 * @brief Attempts to recreate the socket using stored domain/protocol info.
 */
ttak_io_status_t ttak_net_endpoint_force_restart(ttak_shared_net_endpoint_t *endpoint,
                                                 ttak_owner_t *owner,
                                                 uint64_t now);

/**
 * @brief Sets non-blocking I/O mode on the endpoint descriptor.
 */
ttak_io_status_t ttak_net_endpoint_set_nonblocking(ttak_shared_net_endpoint_t *endpoint,
                                                   ttak_owner_t *owner,
                                                   bool nonblocking,
                                                   uint64_t now);

/**
 * @brief Configures TCP_NODELAY (Nagle algorithm disabled/enabled).
 */
ttak_io_status_t ttak_net_endpoint_set_nodelay(ttak_shared_net_endpoint_t *endpoint,
                                               ttak_owner_t *owner,
                                               bool nodelay,
                                               uint64_t now);

/**
 * @brief Configures SO_REUSEADDR and SO_REUSEPORT (if available).
 */
ttak_io_status_t ttak_net_endpoint_set_reuse(ttak_shared_net_endpoint_t *endpoint,
                                             ttak_owner_t *owner,
                                             bool reuse_addr,
                                             bool reuse_port,
                                             uint64_t now);

#ifdef __cplusplus
}
#endif

#endif /* TTAK_NET_ENDPOINT_H */
