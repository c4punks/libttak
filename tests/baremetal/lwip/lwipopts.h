/* lwIP configuration for the NET=lwip test images: sockets over the
 * loopback interface only, so the tests need no NIC driver. */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                          0
#define SYS_LIGHTWEIGHT_PROT            1
#define LWIP_TCPIP_CORE_LOCKING         1

/* Required by libttak (see internal/ttak/net_lwip.h). */
#define LWIP_SOCKET                     1
#define LWIP_NETCONN                    1
#define LWIP_COMPAT_SOCKETS             1
#define LWIP_POSIX_SOCKETS_IO_NAMES     1
#define LWIP_SOCKET_POLL                1
/* Used by ttak_net_endpoint_set_reuse/_set_buffer_sizes/_set_keepalive. */
#define SO_REUSE                        1
#define LWIP_SO_RCVBUF                  1
#define LWIP_TCP_KEEPALIVE              1

#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ARP                        0
#define LWIP_ETHERNET                   0
#define LWIP_DHCP                       0
#define LWIP_DNS                        0
#define LWIP_IGMP                       0
#define LWIP_HAVE_LOOPIF                1
#define LWIP_NETIF_LOOPBACK             1
#define LWIP_STATS                      0

#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (24 * 1024)
#define MEMP_NUM_NETCONN                8
#define MEMP_NUM_TCP_PCB                8
#define MEMP_NUM_TCP_PCB_LISTEN         4
#define PBUF_POOL_SIZE                  16
#define TCP_MSS                         1460
#define TCP_WND                         (4 * TCP_MSS)
#define TCP_SND_BUF                     (4 * TCP_MSS)

#define TCPIP_THREAD_NAME               "tcpip"
#define TCPIP_THREAD_STACKSIZE          (4 * 1024) /* bytes */
#define TCPIP_THREAD_PRIO               3
#define TCPIP_MBOX_SIZE                 16
#define DEFAULT_ACCEPTMBOX_SIZE         8
#define DEFAULT_TCP_RECVMBOX_SIZE       16
#define DEFAULT_UDP_RECVMBOX_SIZE       8
#define DEFAULT_RAW_RECVMBOX_SIZE       8

#endif /* LWIPOPTS_H */
