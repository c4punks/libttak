/**
 * @file endpoint.c
 * @brief Shared network endpoint lifecycle: create, bind, role, restart.
 *
 * Wraps a raw socket fd inside a reference-counted shared handle with TTL
 * guard and optional restart hooks for client/server roles.  All operations
 * require a valid ttak_owner_t to enforce epoch-safe access.
 */

#include <ttak/net/endpoint.h>

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ttak/compat/pthread.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#endif
#include <ttak/net/core/port.h>
#include <ttak/net/core/port.h>

static ttak_io_status_t ttak_net_endpoint_restart_client(ttak_net_endpoint_t *payload, uint64_t now);
static ttak_io_status_t ttak_net_endpoint_restart_server(ttak_net_endpoint_t *payload, uint64_t now);

static pthread_once_t endpoint_port_once = PTHREAD_ONCE_INIT;
static ttak_net_driver_ops_t endpoint_ops;
static ttak_net_os_t endpoint_os;
static void endpoint_port_bootstrap(void) {
    ttak_net_driver_detect(&endpoint_ops, &endpoint_os, NULL);
}

ttak_shared_net_endpoint_t *ttak_net_endpoint_create(ttak_owner_t *owner, uint64_t now) {
    if (!owner) return NULL;
    pthread_once(&endpoint_port_once, endpoint_port_bootstrap);
    ttak_shared_net_endpoint_t *shared = malloc(sizeof(*shared));
    if (!shared) return NULL;
    memset(shared, 0, sizeof(*shared));
    ttak_shared_init(&shared->base);
    if (ttak_shared_net_endpoint_allocate(shared, TTAK_SHARED_LEVEL_3) != TTAK_OWNER_SUCCESS) {
        ttak_shared_destroy(&shared->base);
        free(shared);
        return NULL;
    }
    if (shared->base.add_owner(&shared->base, owner) != TTAK_OWNER_SUCCESS) {
        ttak_shared_destroy(&shared->base);
        free(shared);
        return NULL;
    }

    ttak_shared_result_t res = 0;
    ttak_net_endpoint_t *payload = ttak_shared_net_endpoint_access(shared, owner, &res);
    if (!payload || res != TTAK_OWNER_SUCCESS) {
        ttak_shared_destroy(&shared->base);
        free(shared);
        return NULL;
    }

    memset(payload, 0, sizeof(*payload));
    payload->fd = -1;
    payload->generation_id = now;
    payload->type = TTAK_NET_ENDPOINT_IPV4;
    payload->domain = AF_UNSPEC;
    payload->socktype = SOCK_STREAM;
    payload->protocol = 0;
    payload->role_flags = TTAK_NET_ROLE_CLIENT;
    payload->restart = ttak_net_endpoint_restart_client;
    payload->listen_backlog = SOMAXCONN;
    payload->restart_ctx = NULL;
    payload->lattice = ttak_net_lattice_get_default();
    payload->guard.fd = -1;
    payload->guard.ttl_ns = 0;
    payload->guard.expires_at = now;
    payload->guard.last_used = now;
    payload->guard.closed = true;
    payload->guard.owner = owner;
    payload->addr.len = 0;
    memset(payload->addr.storage, 0, sizeof(payload->addr.storage));
    ttak_shared_net_endpoint_release(shared);

    return shared;
}

void ttak_net_endpoint_destroy(ttak_shared_net_endpoint_t *endpoint,
                               ttak_owner_t *owner,
                               uint64_t now) {
    if (!endpoint) return;
    if (owner) {
        ttak_net_endpoint_close(endpoint, owner, now);
    }
    ttak_shared_destroy(&endpoint->base);
    free(endpoint);
}

static ttak_io_status_t ttak_net_endpoint_access(ttak_shared_net_endpoint_t *endpoint,
                                                 ttak_owner_t *owner,
                                                 ttak_net_endpoint_t **out_ep,
                                                 uint64_t now,
                                                 bool allow_expired) {
    if (!endpoint || !owner || !out_ep) return TTAK_IO_ERR_INVALID_ARGUMENT;
    ttak_shared_result_t res = 0;
    ttak_net_endpoint_t *payload = ttak_shared_net_endpoint_access(endpoint, owner, &res);
    if (!payload || res != TTAK_OWNER_SUCCESS) {
        return TTAK_IO_ERR_INVALID_ARGUMENT;
    }

    if (!allow_expired && payload->guard.fd >= 0 &&
        !ttak_io_guard_valid(&payload->guard, now)) {
        ttak_shared_net_endpoint_release(endpoint);
        return TTAK_IO_ERR_EXPIRED_GUARD;
    }

    *out_ep = payload;
    return TTAK_IO_SUCCESS;
}

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
                                           uint64_t now) {
    if (!endpoint || fd < 0 || addr_len > sizeof(((ttak_net_endpoint_t *)0)->addr.storage)) {
        return TTAK_IO_ERR_INVALID_ARGUMENT;
    }
    ttak_net_endpoint_t *payload = NULL;
    ttak_io_status_t status = ttak_net_endpoint_access(endpoint, owner, &payload, now, true);
    if (status != TTAK_IO_SUCCESS) {
        return status;
    }

    payload->fd = fd;
    payload->domain = domain;
    payload->socktype = socktype;
    payload->protocol = protocol;
    payload->type = type;
    payload->generation_id++;
    payload->addr.len = addr_len;
    if (addr_len > 0 && addr_bytes) {
        memcpy(payload->addr.storage, addr_bytes, addr_len);
    }

    status = ttak_io_guard_init(&payload->guard, fd, owner, ttl_ns, now);

    ttak_shared_net_endpoint_release(endpoint);
    return status;
}

void ttak_net_endpoint_set_role(ttak_shared_net_endpoint_t *endpoint,
                                const ttak_net_endpoint_attr_t *attr,
                                ttak_owner_t *owner,
                                uint64_t now) {
    (void)now;
    if (!endpoint) return;
    ttak_net_endpoint_t *payload = NULL;
    if (ttak_net_endpoint_access(endpoint, owner, &payload, now, true) != TTAK_IO_SUCCESS) {
        return;
    }
    ttak_net_endpoint_attr_t defaults = {
        .role_flags = TTAK_NET_ROLE_CLIENT,
        .listen_backlog = SOMAXCONN,
        .restart_ctx = NULL,
        .restart_cb = NULL
    };
    if (!attr) attr = &defaults;
    payload->role_flags = attr->role_flags ? attr->role_flags : TTAK_NET_ROLE_CLIENT;
    payload->listen_backlog = (attr->listen_backlog > 0) ? attr->listen_backlog : SOMAXCONN;
    payload->restart_ctx = attr->restart_ctx;
    if (attr->restart_cb) {
        payload->restart = attr->restart_cb;
    } else if (payload->role_flags & TTAK_NET_ROLE_SERVER) {
        payload->restart = ttak_net_endpoint_restart_server;
    } else {
        payload->restart = ttak_net_endpoint_restart_client;
    }
    ttak_shared_net_endpoint_release(endpoint);
}

ttak_io_status_t ttak_net_endpoint_close(ttak_shared_net_endpoint_t *endpoint,
                                         ttak_owner_t *owner,
                                         uint64_t now) {
    ttak_net_endpoint_t *payload = NULL;
    ttak_io_status_t status = ttak_net_endpoint_access(endpoint, owner, &payload, now, true);
    if (status != TTAK_IO_SUCCESS) {
        return status;
    }

    status = ttak_io_guard_close(&payload->guard, now);
    payload->fd = -1;
    payload->generation_id++;
    payload->addr.len = 0;
    ttak_shared_net_endpoint_release(endpoint);
    return status;
}

ttak_io_status_t ttak_net_endpoint_snapshot_guard(ttak_shared_net_endpoint_t *endpoint,
                                                  ttak_owner_t *owner,
                                                  ttak_net_guard_snapshot_t *snapshot,
                                                  uint64_t now) {
    if (!snapshot) return TTAK_IO_ERR_INVALID_ARGUMENT;
    ttak_net_endpoint_t *payload = NULL;
    ttak_io_status_t status = ttak_net_endpoint_access(endpoint, owner, &payload, now, false);
    if (status != TTAK_IO_SUCCESS) {
        return status;
    }
    snapshot->fd = payload->guard.fd;
    snapshot->ttl_ns = payload->guard.ttl_ns;
    snapshot->guard_generation = payload->generation_id;
    snapshot->endpoint = endpoint;
    snapshot->owner = owner;
    ttak_shared_net_endpoint_release(endpoint);
    return (snapshot->fd >= 0) ? TTAK_IO_SUCCESS : TTAK_IO_ERR_INVALID_ARGUMENT;
}

ttak_io_status_t ttak_net_endpoint_guard_commit(const ttak_net_guard_snapshot_t *snapshot,
                                                uint64_t now) {
    if (!snapshot || !snapshot->endpoint || !snapshot->owner) {
        return TTAK_IO_ERR_INVALID_ARGUMENT;
    }
    ttak_net_endpoint_t *payload = NULL;
    ttak_io_status_t status = ttak_net_endpoint_access(snapshot->endpoint,
                                                       snapshot->owner,
                                                       &payload,
                                                       now,
                                                       true);
    if (status != TTAK_IO_SUCCESS) {
        return status;
    }

    if (payload->generation_id == snapshot->guard_generation && payload->guard.fd >= 0) {
        ttak_io_guard_refresh(&payload->guard, now);
    }

    ttak_shared_net_endpoint_release(snapshot->endpoint);
    return TTAK_IO_SUCCESS;
}

static ttak_io_status_t ttak_net_endpoint_restart_client(ttak_net_endpoint_t *payload, uint64_t now) {
    if (!payload || payload->domain == AF_UNSPEC || payload->addr.len == 0) {
        return TTAK_IO_ERR_INVALID_ARGUMENT;
    }
    int fd = endpoint_ops.socket_open(payload->domain, payload->socktype, payload->protocol);
    if (fd < 0) {
        return TTAK_IO_ERR_SYS_FAILURE;
    }

    struct sockaddr *sa = (struct sockaddr *)payload->addr.storage;
    if (endpoint_ops.socket_connect(fd, sa, payload->addr.len) != 0) {
        endpoint_ops.socket_close(fd);
        return TTAK_IO_ERR_SYS_FAILURE;
    }

    ttak_io_guard_close(&payload->guard, now);
    payload->fd = fd;
    payload->generation_id++;
    ttak_io_status_t status = ttak_io_guard_init(&payload->guard,
                                                 fd,
                                                 payload->guard.owner,
                                                 payload->guard.ttl_ns,
                                                 now);
    if (status != TTAK_IO_SUCCESS) {
        endpoint_ops.socket_close(fd);
        payload->fd = -1;
    }
    return status;
}

static ttak_io_status_t ttak_net_endpoint_restart_server(ttak_net_endpoint_t *payload, uint64_t now) {
    if (!payload || payload->domain == AF_UNSPEC || payload->addr.len == 0) {
        return TTAK_IO_ERR_INVALID_ARGUMENT;
    }
    int fd = endpoint_ops.socket_open(payload->domain, payload->socktype, payload->protocol);
    if (fd < 0) {
        return TTAK_IO_ERR_SYS_FAILURE;
    }
    struct sockaddr *sa = (struct sockaddr *)payload->addr.storage;
    if (endpoint_ops.socket_bind(fd, sa, payload->addr.len) != 0) {
        endpoint_ops.socket_close(fd);
        return TTAK_IO_ERR_SYS_FAILURE;
    }
    int backlog = (payload->listen_backlog > 0) ? payload->listen_backlog : SOMAXCONN;
    if (endpoint_ops.socket_listen(fd, backlog) != 0) {
        endpoint_ops.socket_close(fd);
        return TTAK_IO_ERR_SYS_FAILURE;
    }

    ttak_io_guard_close(&payload->guard, now);
    payload->fd = fd;
    payload->generation_id++;
    ttak_io_status_t status = ttak_io_guard_init(&payload->guard,
                                                 fd,
                                                 payload->guard.owner,
                                                 payload->guard.ttl_ns,
                                                 now);
    if (status != TTAK_IO_SUCCESS) {
        endpoint_ops.socket_close(fd);
        payload->fd = -1;
    }
    return status;
}

ttak_io_status_t ttak_net_endpoint_force_restart(ttak_shared_net_endpoint_t *endpoint,
                                                 ttak_owner_t *owner,
                                                 uint64_t now) {
    ttak_net_endpoint_t *payload = NULL;
    ttak_io_status_t status = ttak_net_endpoint_access(endpoint, owner, &payload, now, true);
    if (status != TTAK_IO_SUCCESS) {
        return status;
    }
    ttak_net_restart_op restart = payload->restart;
    if (!restart) {
        restart = (payload->role_flags & TTAK_NET_ROLE_SERVER)
                  ? ttak_net_endpoint_restart_server
                  : ttak_net_endpoint_restart_client;
    }
    status = restart(payload, now);
    ttak_shared_net_endpoint_release(endpoint);
    return status;
}

ttak_io_status_t ttak_net_endpoint_set_nonblocking(ttak_shared_net_endpoint_t *endpoint,
                                                   ttak_owner_t *owner,
                                                   bool nonblocking,
                                                   uint64_t now) {
    ttak_net_endpoint_t *payload = NULL;
    ttak_io_status_t status = ttak_net_endpoint_access(endpoint, owner, &payload, now, true);
    if (status != TTAK_IO_SUCCESS) return status;

    int fd = payload->guard.fd;
    if (fd < 0) {
        ttak_shared_net_endpoint_release(endpoint);
        return TTAK_IO_ERR_INVALID_ARGUMENT;
    }

#if defined(_WIN32)
    u_long mode = nonblocking ? 1 : 0;
    int rc = ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        ttak_shared_net_endpoint_release(endpoint);
        return TTAK_IO_ERR_SYS_FAILURE;
    }
    if (nonblocking) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    int rc = fcntl(fd, F_SETFL, flags);
#endif

    ttak_shared_net_endpoint_release(endpoint);
    return (rc == 0) ? TTAK_IO_SUCCESS : TTAK_IO_ERR_SYS_FAILURE;
}

ttak_io_status_t ttak_net_endpoint_set_nodelay(ttak_shared_net_endpoint_t *endpoint,
                                               ttak_owner_t *owner,
                                               bool nodelay,
                                               uint64_t now) {
    ttak_net_endpoint_t *payload = NULL;
    ttak_io_status_t status = ttak_net_endpoint_access(endpoint, owner, &payload, now, true);
    if (status != TTAK_IO_SUCCESS) return status;

    int fd = payload->guard.fd;
    if (fd < 0) {
        ttak_shared_net_endpoint_release(endpoint);
        return TTAK_IO_ERR_INVALID_ARGUMENT;
    }

    int optval = nodelay ? 1 : 0;
    int rc = endpoint_ops.socket_setopt(fd, IPPROTO_TCP, &optval, sizeof(optval));
    if (rc != 0) {
        /* Fallback direct setsockopt if driver returned error or no-op */
#if defined(_WIN32)
        rc = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&optval, sizeof(optval));
#else
        rc = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));
#endif
    }

    ttak_shared_net_endpoint_release(endpoint);
    return (rc == 0) ? TTAK_IO_SUCCESS : TTAK_IO_ERR_SYS_FAILURE;
}

ttak_io_status_t ttak_net_endpoint_set_reuse(ttak_shared_net_endpoint_t *endpoint,
                                             ttak_owner_t *owner,
                                             bool reuse_addr,
                                             bool reuse_port,
                                             uint64_t now) {
    ttak_net_endpoint_t *payload = NULL;
    ttak_io_status_t status = ttak_net_endpoint_access(endpoint, owner, &payload, now, true);
    if (status != TTAK_IO_SUCCESS) return status;

    int fd = payload->guard.fd;
    if (fd < 0) {
        ttak_shared_net_endpoint_release(endpoint);
        return TTAK_IO_ERR_INVALID_ARGUMENT;
    }

    int optval = reuse_addr ? 1 : 0;
#if defined(_WIN32)
    int rc = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&optval, sizeof(optval));
    (void)reuse_port;
#else
    int rc = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
#if defined(SO_REUSEPORT)
    if (rc == 0 && reuse_port) {
        int val_port = 1;
        rc = setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &val_port, sizeof(val_port));
    }
#else
    (void)reuse_port;
#endif
#endif

    ttak_shared_net_endpoint_release(endpoint);
    return (rc == 0) ? TTAK_IO_SUCCESS : TTAK_IO_ERR_SYS_FAILURE;
}

