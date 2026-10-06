/**
 * @file port.h
 * @brief Platform-agnostic network driver operations and OS detection.
 *
 * Defines the @c ttak_net_driver_ops_t vtable that abstracts socket calls
 * across POSIX, Windows, and bare-metal targets.  Call
 * ttak_net_driver_detect() once at startup to populate the vtable.
 */

#ifndef TTAK_NET_CORE_PORT_H
#define TTAK_NET_CORE_PORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Target operating environment detected at runtime. */
typedef enum ttak_net_os {
    TTAK_NET_OS_POSIX,      /**< Linux / macOS / BSD with POSIX sockets. */
    TTAK_NET_OS_WINDOWS,    /**< Windows with WinSock2. */
    TTAK_NET_OS_BAREMETAL   /**< No OS; uses caller-supplied NIC hooks. */
} ttak_net_os_t;

/**
 * @brief Vtable of platform socket primitives.
 *
 * Populated by ttak_net_driver_detect().  All function pointers follow the
 * semantics of their POSIX equivalents and return -1 on failure.
 */
typedef struct ttak_net_driver_ops {
    int  (*socket_open)(int domain, int type, int protocol); /**< Create socket. */
    int  (*socket_close)(int fd);                            /**< Close socket. */
    int  (*socket_bind)(int fd, const void *addr, size_t len);   /**< Bind address. */
    int  (*socket_listen)(int fd, int backlog);              /**< Start listening. */
    int  (*socket_connect)(int fd, const void *addr, size_t len);/**< Connect. */
    int  (*socket_send)(int fd, const void *buf, size_t len);    /**< Send data. */
    int  (*socket_recv)(int fd, void *buf, size_t len);          /**< Receive data. */
    int  (*socket_setopt)(int fd, int opt, const void *val, size_t len); /**< Set option. */
    int  (*poll_wait)(int fd, uint32_t events, int timeout_ms);  /**< Poll/select. */
} ttak_net_driver_ops_t;

/**
 * @brief Hooks for bare-metal NIC drivers (used when @c os == TTAK_NET_OS_BAREMETAL).
 */
typedef struct ttak_net_baremetal_spec {
    const ttak_net_driver_ops_t *driver_ops; /**< Optional override vtable. */
    void *(*nic_map_io)(uintptr_t base, size_t len);  /**< Map MMIO region. */
    void  (*nic_unmap_io)(void *addr, size_t len);    /**< Unmap MMIO region. */
    void *(*buddy_alloc)(size_t bytes);               /**< DMA-safe allocator. */
    void  (*buddy_free)(void *ptr);                   /**< DMA-safe free. */
} ttak_net_baremetal_spec_t;

/**
 * @brief Detects the runtime OS and fills the driver vtable accordingly.
 *
 * @param ops Output vtable to populate.
 * @param os  Receives the detected OS kind.
 * @param bm  Bare-metal spec (pass NULL on POSIX/Windows targets).
 */
void ttak_net_driver_detect(ttak_net_driver_ops_t *ops,
                            ttak_net_os_t *os,
                            const ttak_net_baremetal_spec_t *bm);

/**
 * @brief Explicitly fills driver vtable with bare-metal memory/NIC implementation.
 *
 * @param ops Output vtable to populate.
 * @param bm  Bare-metal spec with optional overrides/allocators.
 */
void ttak_net_driver_get_baremetal(ttak_net_driver_ops_t *ops,
                                   const ttak_net_baremetal_spec_t *bm);

#ifdef __cplusplus
}
#endif

#endif /* TTAK_NET_CORE_PORT_H */
#if !defined(__BAREMETAL__)
#include <ttak/phys/mem/buddy.h>
#endif
