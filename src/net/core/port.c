/**
 * @file port.c
 * @brief OS detection and socket vtable population (POSIX / WinSock / bare-metal).
 *
 * Called once at startup via ttak_net_driver_detect().  Selects the right
 * socket function pointers based on the compiled target and, for bare-metal,
 * installs the caller-supplied NIC hooks from @c ttak_net_baremetal_spec_t.
 */

#include <ttak/net/core/port.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#elif defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <unistd.h>
#include <poll.h>
#endif

#include <errno.h>

#include <string.h>
#include <stdbool.h>
#include <ttak/phys/mem/buddy.h>

#define TTAK_BM_MAX_SOCKETS 16
#define TTAK_BM_BUF_SIZE    4096

typedef struct ttak_bm_socket {
    bool used;
    int domain;
    int type;
    int protocol;
    bool listening;
    int backlog;
    int peer_fd;
    bool connected;
    uint8_t bind_addr[64];
    size_t bind_len;
    uint8_t rx_buf[TTAK_BM_BUF_SIZE];
    size_t rx_head;
    size_t rx_tail;
    size_t rx_count;
} ttak_bm_socket_t;

static ttak_bm_socket_t s_bm_sockets[TTAK_BM_MAX_SOCKETS];
static const ttak_net_baremetal_spec_t *s_bm_spec = NULL;

static int ttak_bm_socket_open(int domain, int type, int protocol) {
    for (int i = 0; i < TTAK_BM_MAX_SOCKETS; ++i) {
        if (!s_bm_sockets[i].used) {
            memset(&s_bm_sockets[i], 0, sizeof(ttak_bm_socket_t));
            s_bm_sockets[i].used = true;
            s_bm_sockets[i].domain = domain;
            s_bm_sockets[i].type = type;
            s_bm_sockets[i].protocol = protocol;
            s_bm_sockets[i].peer_fd = -1;
            return i;
        }
    }
    errno = EMFILE;
    return -1;
}

static int ttak_bm_socket_close(int fd) {
    if (fd < 0 || fd >= TTAK_BM_MAX_SOCKETS || !s_bm_sockets[fd].used) {
        errno = EBADF;
        return -1;
    }
    int peer = s_bm_sockets[fd].peer_fd;
    if (peer >= 0 && peer < TTAK_BM_MAX_SOCKETS && s_bm_sockets[peer].used) {
        if (s_bm_sockets[peer].peer_fd == fd) {
            s_bm_sockets[peer].peer_fd = -1;
            s_bm_sockets[peer].connected = false;
        }
    }
    s_bm_sockets[fd].used = false;
    return 0;
}

static int ttak_bm_socket_bind(int fd, const void *addr, size_t len) {
    if (fd < 0 || fd >= TTAK_BM_MAX_SOCKETS || !s_bm_sockets[fd].used) {
        errno = EBADF;
        return -1;
    }
    if (!addr || len == 0 || len > sizeof(s_bm_sockets[fd].bind_addr)) {
        errno = EINVAL;
        return -1;
    }
    for (int i = 0; i < TTAK_BM_MAX_SOCKETS; ++i) {
        if (i != fd && s_bm_sockets[i].used && s_bm_sockets[i].bind_len == len) {
            if (memcmp(s_bm_sockets[i].bind_addr, addr, len) == 0) {
                errno = EADDRINUSE;
                return -1;
            }
        }
    }
    memcpy(s_bm_sockets[fd].bind_addr, addr, len);
    s_bm_sockets[fd].bind_len = len;
    return 0;
}

static int ttak_bm_socket_listen(int fd, int backlog) {
    if (fd < 0 || fd >= TTAK_BM_MAX_SOCKETS || !s_bm_sockets[fd].used) {
        errno = EBADF;
        return -1;
    }
    s_bm_sockets[fd].listening = true;
    s_bm_sockets[fd].backlog = backlog > 0 ? backlog : 1;
    return 0;
}

static int ttak_bm_socket_connect(int fd, const void *addr, size_t len) {
    if (fd < 0 || fd >= TTAK_BM_MAX_SOCKETS || !s_bm_sockets[fd].used) {
        errno = EBADF;
        return -1;
    }
    if (!addr || len == 0) {
        errno = EINVAL;
        return -1;
    }
    int target_fd = -1;
    for (int i = 0; i < TTAK_BM_MAX_SOCKETS; ++i) {
        if (i != fd && s_bm_sockets[i].used && s_bm_sockets[i].bind_len == len) {
            if (memcmp(s_bm_sockets[i].bind_addr, addr, len) == 0) {
                target_fd = i;
                break;
            }
        }
    }
    if (target_fd < 0) {
        /* Self loopback fallback if unbound */
        target_fd = fd;
    }
    s_bm_sockets[fd].peer_fd = target_fd;
    s_bm_sockets[fd].connected = true;
    if (target_fd != fd) {
        s_bm_sockets[target_fd].peer_fd = fd;
        s_bm_sockets[target_fd].connected = true;
    }
    return 0;
}

static int ttak_bm_socket_send(int fd, const void *buf, size_t len) {
    if (fd < 0 || fd >= TTAK_BM_MAX_SOCKETS || !s_bm_sockets[fd].used) {
        errno = EBADF;
        return -1;
    }
    if (!buf && len > 0) {
        errno = EINVAL;
        return -1;
    }
    if (len == 0) return 0;

    int dest_fd = s_bm_sockets[fd].peer_fd >= 0 ? s_bm_sockets[fd].peer_fd : fd;
    ttak_bm_socket_t *dest = &s_bm_sockets[dest_fd];
    if (!dest->used) {
        errno = ENOTCONN;
        return -1;
    }

    size_t space = TTAK_BM_BUF_SIZE - dest->rx_count;
    if (space == 0) {
        errno = EAGAIN;
        return -1;
    }
    size_t to_write = (len < space) ? len : space;
    const uint8_t *src = (const uint8_t *)buf;
    for (size_t i = 0; i < to_write; ++i) {
        dest->rx_buf[dest->rx_tail] = src[i];
        dest->rx_tail = (dest->rx_tail + 1) % TTAK_BM_BUF_SIZE;
    }
    dest->rx_count += to_write;
    return (int)to_write;
}

static int ttak_bm_socket_recv(int fd, void *buf, size_t len) {
    if (fd < 0 || fd >= TTAK_BM_MAX_SOCKETS || !s_bm_sockets[fd].used) {
        errno = EBADF;
        return -1;
    }
    if (!buf && len > 0) {
        errno = EINVAL;
        return -1;
    }
    if (len == 0) return 0;

    ttak_bm_socket_t *sock = &s_bm_sockets[fd];
    if (sock->rx_count == 0) {
        if (!sock->connected && sock->peer_fd < 0) {
            return 0; /* EOF */
        }
        errno = EAGAIN;
        return -1;
    }

    size_t to_read = (len < sock->rx_count) ? len : sock->rx_count;
    uint8_t *dst = (uint8_t *)buf;
    for (size_t i = 0; i < to_read; ++i) {
        dst[i] = sock->rx_buf[sock->rx_head];
        sock->rx_head = (sock->rx_head + 1) % TTAK_BM_BUF_SIZE;
    }
    sock->rx_count -= to_read;
    return (int)to_read;
}

static int ttak_bm_socket_setopt(int fd, int opt, const void *val, size_t len) {
    (void)opt;
    (void)val;
    (void)len;
    if (fd < 0 || fd >= TTAK_BM_MAX_SOCKETS || !s_bm_sockets[fd].used) {
        errno = EBADF;
        return -1;
    }
    return 0;
}

static int ttak_bm_poll_wait(int fd, uint32_t events, int timeout_ms) {
    (void)timeout_ms;
    if (fd < 0 || fd >= TTAK_BM_MAX_SOCKETS || !s_bm_sockets[fd].used) {
        errno = EBADF;
        return -1;
    }
    ttak_bm_socket_t *sock = &s_bm_sockets[fd];
    uint32_t revents = 0;
    if ((events & 0x0001) && sock->rx_count > 0) { /* POLLIN */
        revents |= 0x0001;
    }
    if (events & 0x0004) { /* POLLOUT */
        revents |= 0x0004;
    }
    return (revents != 0) ? 1 : 0;
}

void ttak_net_driver_get_baremetal(ttak_net_driver_ops_t *ops,
                                   const ttak_net_baremetal_spec_t *bm) {
    if (!ops) return;
    s_bm_spec = bm;
    if (bm && bm->driver_ops) {
        *ops = *bm->driver_ops;
        return;
    }
    ops->socket_open = ttak_bm_socket_open;
    ops->socket_close = ttak_bm_socket_close;
    ops->socket_bind = ttak_bm_socket_bind;
    ops->socket_listen = ttak_bm_socket_listen;
    ops->socket_connect = ttak_bm_socket_connect;
    ops->socket_send = ttak_bm_socket_send;
    ops->socket_recv = ttak_bm_socket_recv;
    ops->socket_setopt = ttak_bm_socket_setopt;
    ops->poll_wait = ttak_bm_poll_wait;
}

#if defined(_WIN32)
static int ttak_win_socket_open(int domain, int type, int protocol) {
    return (int)WSASocket(domain, type, protocol, NULL, 0, 0);
}
static int ttak_win_socket_close(int fd) {
    return closesocket(fd);
}
static int ttak_win_socket_bind(int fd, const void *addr, size_t len) {
    return bind(fd, (const struct sockaddr *)addr, (int)len);
}
static int ttak_win_socket_listen(int fd, int backlog) {
    return listen(fd, backlog);
}
static int ttak_win_socket_connect(int fd, const void *addr, size_t len) {
    return connect(fd, (const struct sockaddr *)addr, (int)len);
}
static int ttak_win_socket_send(int fd, const void *buf, size_t len) {
    return send(fd, buf, (int)len, 0);
}
static int ttak_win_socket_recv(int fd, void *buf, size_t len) {
    return recv(fd, buf, (int)len, 0);
}
static int ttak_win_poll_wait(int fd, uint32_t events, int timeout_ms) {
    WSAPOLLFD pfd = { .fd = fd, .events = (SHORT)events };
    return WSAPoll(&pfd, 1, timeout_ms);
}
#endif

#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
static int ttak_posix_socket_open(int domain, int type, int protocol) {
    return socket(domain, type, protocol);
}
static int ttak_posix_socket_close(int fd) { return close(fd); }
static int ttak_posix_socket_bind(int fd, const void *addr, size_t len) {
    return bind(fd, (const struct sockaddr *)addr, len);
}
static int ttak_posix_socket_listen(int fd, int backlog) {
    return listen(fd, backlog);
}
static int ttak_posix_socket_connect(int fd, const void *addr, size_t len) {
    return connect(fd, (const struct sockaddr *)addr, len);
}
static int ttak_posix_socket_send(int fd, const void *buf, size_t len) {
    return (int)send(fd, buf, len, 0);
}
static int ttak_posix_socket_recv(int fd, void *buf, size_t len) {
    return (int)recv(fd, buf, len, 0);
}
static int ttak_posix_poll_wait(int fd, uint32_t events, int timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = (short)events };
    return poll(&pfd, 1, timeout_ms);
}
#endif

void ttak_net_driver_detect(ttak_net_driver_ops_t *ops,
                            ttak_net_os_t *os,
                            const ttak_net_baremetal_spec_t *bm) {
    if (!ops || !os) return;
#if defined(__BAREMETAL__)
    *os = TTAK_NET_OS_BAREMETAL;
    ttak_net_driver_get_baremetal(ops, bm);
#elif defined(_WIN32)
    (void)bm;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    *os = TTAK_NET_OS_WINDOWS;
    ops->socket_open = ttak_win_socket_open;
    ops->socket_close = ttak_win_socket_close;
    ops->socket_bind = ttak_win_socket_bind;
    ops->socket_listen = ttak_win_socket_listen;
    ops->socket_connect = ttak_win_socket_connect;
    ops->socket_send = ttak_win_socket_send;
    ops->socket_recv = ttak_win_socket_recv;
    ops->socket_setopt = (int (*)(int, int, const void *, size_t))setsockopt;
    ops->poll_wait = ttak_win_poll_wait;
#elif defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
    (void)bm;
    *os = TTAK_NET_OS_POSIX;
    ops->socket_open = ttak_posix_socket_open;
    ops->socket_close = ttak_posix_socket_close;
    ops->socket_bind = ttak_posix_socket_bind;
    ops->socket_listen = ttak_posix_socket_listen;
    ops->socket_connect = ttak_posix_socket_connect;
    ops->socket_send = ttak_posix_socket_send;
    ops->socket_recv = ttak_posix_socket_recv;
    ops->socket_setopt = (int (*)(int, int, const void *, size_t))setsockopt;
    ops->poll_wait = ttak_posix_poll_wait;
#else
    *os = TTAK_NET_OS_BAREMETAL;
    ttak_net_driver_get_baremetal(ops, bm);
#endif
}
