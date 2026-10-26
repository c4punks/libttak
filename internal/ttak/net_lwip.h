/**
 * @file net_lwip.h
 * @brief lwIP socket API for the net and io modules (TTAK_NET_LWIP=1).
 *
 * lwIP's compatibility names map socket(), recvmsg(), poll(), fcntl(),
 * read(), write(), close() and the rest onto lwip_*(), so the modules keep
 * their POSIX code. Each module includes this header in place of the POSIX
 * socket headers.
 *
 * The macros would rewrite newlib's own prototypes (fcntl() would no
 * longer match), so newlib headers come first and fcntl.h not at all:
 * lwIP's F_GETFL/O_NONBLOCK values differ from newlib's, and the modules
 * switch blocking mode with ioctlsocket(FIONBIO) instead.
 */

#ifndef TTAK_INTERNAL_NET_LWIP_H
#define TTAK_INTERNAL_NET_LWIP_H

#include <unistd.h>
#include "lwip/sockets.h"

#if !LWIP_SOCKET || !LWIP_COMPAT_SOCKETS || !LWIP_POSIX_SOCKETS_IO_NAMES || !LWIP_SOCKET_POLL
#error "libttak needs LWIP_SOCKET, LWIP_COMPAT_SOCKETS, LWIP_POSIX_SOCKETS_IO_NAMES and LWIP_SOCKET_POLL"
#endif

#ifndef SOMAXCONN
#define SOMAXCONN TCP_DEFAULT_LISTEN_BACKLOG
#endif

#endif /* TTAK_INTERNAL_NET_LWIP_H */
