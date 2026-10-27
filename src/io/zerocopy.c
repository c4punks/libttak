/**
 * @file zerocopy.c
 * @brief Zero-copy receive window backed by a detachable arena.
 *
 * Allocates a contiguous segment-aligned buffer in a detachable arena so
 * that received data can be handed off to consumers without copying.
 * Segment occupancy is tracked with a 256-bit bitmap for O(1) iteration.
 */

#include <ttak/io/zerocopy.h>

#include <ttak/mem/mem.h>
#include "../../internal/ttak/io_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
typedef int ttak_io_ssize_t;
#elif TTAK_NET_LWIP
#include "../../internal/ttak/net_lwip.h"
typedef ssize_t ttak_io_ssize_t;
#else
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
typedef ssize_t ttak_io_ssize_t;
#endif

void ttak_io_zerocopy_region_init(ttak_io_zerocopy_region_t *region) {
    if (!region) return;
    region->data = NULL;
    region->len = 0;
    region->capacity = 0;
    region->read_only = true;
    memset(region->segment_mask, 0, sizeof(region->segment_mask));
    region->arena = NULL;
    region->allocation.data = NULL;
    region->allocation.size = 0;
}

ttak_io_status_t ttak_io_zerocopy_recv_fd(int fd,
                                          ttak_io_zerocopy_region_t *region,
                                          size_t max_len,
                                          int flags,
                                          uint64_t now) {
    if (fd < 0 || !region) {
        return TTAK_IO_ERR_INVALID_ARGUMENT;
    }
    if (max_len == 0) {
        ttak_io_zerocopy_region_init(region);
        return TTAK_IO_SUCCESS;
    }
    if (max_len > (size_t)TTAK_IO_ZC_MAX_SEGMENTS * (size_t)TTAK_IO_ZC_SEG_BYTES) {
        max_len = (size_t)TTAK_IO_ZC_MAX_SEGMENTS * (size_t)TTAK_IO_ZC_SEG_BYTES;
    }

    region->arena = ttak_io_arena();
    region->allocation = ttak_detachable_mem_alloc(region->arena, max_len, now);
    if (!region->allocation.data) {
        ttak_io_zerocopy_region_init(region);
        return TTAK_IO_ERR_SYS_FAILURE;
    }

    uint8_t *buffer = (uint8_t *)region->allocation.data;
    region->data = buffer;
    region->capacity = max_len;
    memset(region->segment_mask, 0, sizeof(region->segment_mask));
    region->len = 0;

    size_t total = 0;
#ifndef _WIN32
    struct iovec iov[TTAK_IO_ZC_MAX_IOV];
#endif

    while (total < max_len) {
        size_t remaining = max_len - total;
        size_t chunk_total = remaining;
        if (chunk_total > (size_t)TTAK_IO_ZC_SEG_BYTES * (size_t)TTAK_IO_ZC_MAX_IOV) {
            chunk_total = (size_t)TTAK_IO_ZC_SEG_BYTES * (size_t)TTAK_IO_ZC_MAX_IOV;
        }
#ifndef _WIN32
        int iovcnt = 0;
        size_t covered = 0;
        while (covered < chunk_total && iovcnt < (int)TTAK_IO_ZC_MAX_IOV) {
            size_t span = chunk_total - covered;
            if (span > TTAK_IO_ZC_SEG_BYTES) {
                span = TTAK_IO_ZC_SEG_BYTES;
            }
            size_t aligned = (span + TTAK_IO_ZC_SEG_MASK) & ~((size_t)TTAK_IO_ZC_SEG_MASK);
            if (aligned > span) aligned = span;
            iov[iovcnt].iov_base = buffer + total + covered;
            iov[iovcnt].iov_len = aligned;
            covered += aligned;
            iovcnt++;
        }
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = iov;
        msg.msg_iovlen = iovcnt;
        ttak_io_ssize_t rc = recvmsg(fd, &msg, flags);
#else
        size_t span = chunk_total;
        if (span > TTAK_IO_ZC_SEG_BYTES) span = TTAK_IO_ZC_SEG_BYTES;
        ttak_io_ssize_t rc = recv(fd, (char *)(buffer + total), (int)span, flags);
#endif
        if (rc < 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            bool retry = (err == WSAEINTR || err == WSAEWOULDBLOCK);
#else
            int err = errno;
            bool retry = (err == EINTR || err == EAGAIN);
#endif
            if (total == 0) {
                ttak_detachable_mem_free(region->arena, &region->allocation);
                ttak_io_zerocopy_region_init(region);
                return retry ? TTAK_IO_ERR_NEEDS_RETRY : TTAK_IO_ERR_SYS_FAILURE;
            }
            break;
        }
        if (rc == 0) break;

        size_t written = (size_t)rc;
        size_t seg_start = total >> TTAK_IO_ZC_SEG_SHIFT;
        size_t seg_count = (written + TTAK_IO_ZC_SEG_MASK) >> TTAK_IO_ZC_SEG_SHIFT;
        size_t seg_end = seg_start + seg_count;
        if (seg_end > TTAK_IO_ZC_MAX_SEGMENTS) {
            seg_end = TTAK_IO_ZC_MAX_SEGMENTS;
        }
        size_t seg_idx = seg_start;
        while (seg_idx < seg_end) {
            size_t word = seg_idx / TTAK_IO_ZC_SEGMENT_WORD_BITS;
            size_t bit_offset = seg_idx % TTAK_IO_ZC_SEGMENT_WORD_BITS;
            size_t run = seg_end - seg_idx;
            size_t available = TTAK_IO_ZC_SEGMENT_WORD_BITS - bit_offset;
            if (run > available) {
                run = available;
            }
            uint32_t mask = (run == TTAK_IO_ZC_SEGMENT_WORD_BITS)
                                ? 0xFFFFFFFFu
                                : ((1u << run) - 1u);
            region->segment_mask[word] |= (mask << bit_offset);
            seg_idx += run;
        }

        total += written;
        if ((size_t)rc < chunk_total) break;
    }

    region->len = total;
    region->read_only = true;
    return TTAK_IO_SUCCESS;
}

void ttak_io_zerocopy_release(ttak_io_zerocopy_region_t *region) {
    if (!region) return;
    if (region->arena && region->allocation.data) {
        ttak_detachable_mem_free(region->arena, &region->allocation);
    }
    ttak_io_zerocopy_region_init(region);
}
