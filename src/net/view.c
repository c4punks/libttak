#include <ttak/net/view.h>
#include <ttak/mols_control.h>
#include <ttak/io/io.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

// Include basetsd to import _SSIZE_T_ macro
#ifdef __has_include
#  if __has_include(<basetsd.h>)
#    include <basetsd.h>
#  elif __has_include(<BaseTsd.h>)
#    include <BaseTsd.h>
#  endif
#endif

#if defined(_WIN32)
#  if !defined(_SSIZE_T_DEFINED) && !defined(_SSIZE_T_)
#    include <stddef.h>
     typedef ptrdiff_t ssize_t;
#    define _SSIZE_T_DEFINED
#    define _SSIZE_T_
#  endif
#endif
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif


void ttak_net_view_init(ttak_net_view_t *view) {
    if (!view) return;
    view->data = NULL;
    view->len = 0;
    view->birth_ns = 0;
    view->slot = NULL;
    view->slot_lattice = NULL;
    ttak_io_zerocopy_region_init(&view->region);
}

ttak_io_status_t ttak_net_view_from_endpoint(ttak_net_view_t *view,
                                             ttak_shared_net_endpoint_t *endpoint,
                                             ttak_owner_t *owner,
                                             size_t max_len,
                                             int flags,
                                             uint64_t now) {
    if (!view || !endpoint || !owner) {
        return TTAK_IO_ERR_INVALID_ARGUMENT;
    }
    view->slot = NULL;
    view->slot_lattice = NULL;

    ttak_shared_result_t res;
    ttak_net_endpoint_t *payload = ttak_shared_net_endpoint_access(endpoint, owner, &res);
    if (!payload || res != TTAK_OWNER_SUCCESS) {
        return TTAK_IO_ERR_INVALID_ARGUMENT;
    }

    ttak_net_lattice_t *lat = payload->lattice;
    int fd = payload->guard.fd;
    uint32_t tid = ttak_net_lattice_get_worker_id();

    /* Transparent optimization: use Latin-square isolation if lattice is present and enabled */
    if (lat && (payload->role_flags & TTAK_NET_ROLE_LATTICE_ACCEL)) {
        uint32_t mask = lat->mask;
        uint32_t my_tid = tid & mask;

        for (ttak_net_lattice_t *node = lat; node; ) {
            uint32_t dim = node->dim;
            for (uint32_t r = 0; r < dim; r++) {
                for (uint32_t c = 0; c < dim; c++) {
                    const uint16_t node_id =
                        (uint16_t)(((r & (uint32_t)TTAK_MOLS_SYMBOL_MASK) << TTAK_MOLS_COORD_SHIFT) |
                                   (c & (uint32_t)TTAK_MOLS_SYMBOL_MASK));
                    uint32_t lane = ttak_apply_mols_control(node_id, my_tid) & mask;
                    if (((r + c) & mask) == lane) {
                        ttak_net_lattice_slot_t *slot = &node->slots[r * dim + c];
                        if (ttak_atomic_read64(&slot->state) == 0) {
                            ttak_atomic_write64(&slot->state, 1);
                            ssize_t valread = recv(fd, slot->data, TTAK_LATTICE_SLOT_SIZE, flags);
                            if (valread > 0) {
                                slot->len = (uint32_t)valread;
                                slot->timestamp = now;
                                slot->seq++;
                                ttak_atomic_write64(&slot->state, 2);
                                ttak_atomic_add64(&node->total_ingress, 1);
                                ttak_net_lattice_mark_slot_acquired(node, now);

                                /* Wire the view directly to the lattice slot */
                                view->data = slot->data;
                                view->len = (size_t)valread;
                                view->birth_ns = now;
                                view->slot = slot;
                                view->slot_lattice = node;
                                /* We mark it as 'reading' so other won't overwrite while view is active */
                                ttak_atomic_write64(&slot->state, 3); 
                                
                                ttak_shared_net_endpoint_release(endpoint);
                                return TTAK_IO_SUCCESS;
                            }
                            ttak_atomic_write64(&slot->state, 0);
                        }
                    }
                }
            }

            ttak_net_lattice_t *next = node->next;
            if (!next) {
                next = ttak_net_lattice_ensure_next(node, now);
            }
            node = next;
        }
    }

    ttak_shared_net_endpoint_release(endpoint);

    ttak_net_guard_snapshot_t snap;
    ttak_io_status_t status = ttak_net_endpoint_snapshot_guard(endpoint, owner, &snap, now);
    if (status != TTAK_IO_SUCCESS) {
        return status;
    }

    status = ttak_io_zerocopy_recv_fd(snap.fd, &view->region, max_len, flags, now);
    if (status == TTAK_IO_SUCCESS) {
        view->data = view->region.data;
        view->len = view->region.len;
        view->birth_ns = now;
        ttak_net_endpoint_guard_commit(&snap, now);
    } else {
        ttak_io_zerocopy_release(&view->region);
        ttak_net_view_init(view);
    }
    return status;
}

const uint8_t *ttak_net_view_data(const ttak_net_view_t *view) {
    return view ? view->data : NULL;
}

void ttak_net_view_release(ttak_net_view_t *view) {
    if (!view) return;

    /* If this view was backed by a lattice slot, release it */
    if (view->slot && view->slot_lattice) {
        ttak_atomic_write64(&view->slot->state, 0);
        ttak_net_lattice_mark_slot_released(view->slot_lattice);
    } else {
        ttak_net_lattice_t *default_lat = ttak_net_lattice_get_default();
        if (default_lat && view->data) {
            _Bool released = false;
            for (ttak_net_lattice_t *node = default_lat; node && !released; node = node->next) {
                uint32_t dim = node->dim;
                for (uint32_t i = 0; i < dim * dim; i++) {
                    if (node->slots[i].data == view->data) {
                        ttak_atomic_write64(&node->slots[i].state, 0);
                        ttak_net_lattice_mark_slot_released(node);
                        released = true;
                        break;
                    }
                }
            }
        }
    }

    ttak_io_zerocopy_release(&view->region);
    view->data = NULL;
    view->len = 0;
    view->birth_ns = 0;
    view->slot = NULL;
    view->slot_lattice = NULL;
}

ttak_io_status_t ttak_net_view_readv(ttak_shared_net_endpoint_t *endpoint,
                                     ttak_owner_t *owner,
                                     const ttak_net_view_iovec_t *iov,
                                     size_t iovcnt,
                                     size_t *bytes_read,
                                     int flags,
                                     uint64_t now) {
    if (!endpoint || !owner || !iov || iovcnt == 0 || !bytes_read) {
        return TTAK_IO_ERR_INVALID_ARGUMENT;
    }
    *bytes_read = 0;

    ttak_net_guard_snapshot_t snap;
    ttak_io_status_t status = ttak_net_endpoint_snapshot_guard(endpoint, owner, &snap, now);
    if (status != TTAK_IO_SUCCESS) return status;

    int fd = snap.fd;
    if (fd < 0) return TTAK_IO_ERR_INVALID_ARGUMENT;

#if defined(_WIN32)
    /* Emulate readv on Windows using WSARecv or iterative recv */
    size_t total = 0;
    for (size_t i = 0; i < iovcnt; ++i) {
        if (!iov[i].iov_base || iov[i].iov_len == 0) continue;
        int rc = recv(fd, (char *)iov[i].iov_base, (int)iov[i].iov_len, flags);
        if (rc < 0) {
            if (total > 0) break;
            return TTAK_IO_ERR_SYS_FAILURE;
        }
        if (rc == 0) break; /* EOF */
        total += (size_t)rc;
        if ((size_t)rc < iov[i].iov_len) break; /* Short read */
    }
    *bytes_read = total;
    ttak_net_endpoint_guard_commit(&snap, now);
    return TTAK_IO_SUCCESS;
#else
    /* Use recvmsg on POSIX */
    enum { MAX_STACK_IOV = 32 };
    struct iovec stack_iov[MAX_STACK_IOV];
    struct iovec *posix_iov = stack_iov;
    if (iovcnt > MAX_STACK_IOV) {
        posix_iov = malloc(iovcnt * sizeof(struct iovec));
        if (!posix_iov) return TTAK_IO_ERR_SYS_FAILURE;
    }

    for (size_t i = 0; i < iovcnt; ++i) {
        posix_iov[i].iov_base = iov[i].iov_base;
        posix_iov[i].iov_len = iov[i].iov_len;
    }

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = posix_iov;
    msg.msg_iovlen = (int)iovcnt;

    ssize_t rc = recvmsg(fd, &msg, flags);
    if (posix_iov != stack_iov) free(posix_iov);

    if (rc < 0) {
        return TTAK_IO_ERR_SYS_FAILURE;
    }

    *bytes_read = (size_t)rc;
    ttak_net_endpoint_guard_commit(&snap, now);
    return TTAK_IO_SUCCESS;
#endif
}

