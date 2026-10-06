#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <ttak/net/endpoint.h>
#include <ttak/net/poller.h>
#include <ttak/net/view.h>
#include <ttak/timing/timing.h>

int main(void) {
    uint64_t now = ttak_get_tick_count();
    ttak_owner_t *owner = ttak_owner_create(TTAK_OWNER_SAFE_DEFAULT);
    assert(owner != NULL);

    /* 1. Create a socketpair for testing */
    int sv[2];
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(rc == 0);

    /* 2. Wrap both ends in ttak_shared_net_endpoint_t */
    ttak_shared_net_endpoint_t *ep1 = ttak_net_endpoint_create(owner, now);
    ttak_shared_net_endpoint_t *ep2 = ttak_net_endpoint_create(owner, now);
    assert(ep1 && ep2);

    struct sockaddr un_addr;
    memset(&un_addr, 0, sizeof(un_addr));
    un_addr.sa_family = AF_UNIX;

    assert(ttak_net_endpoint_bind_fd(ep1, owner, sv[0], AF_UNIX, SOCK_STREAM, 0,
                                     &un_addr, sizeof(un_addr), TTAK_NET_ENDPOINT_UNIX,
                                     UINT64_MAX, now) == TTAK_IO_SUCCESS);
    assert(ttak_net_endpoint_bind_fd(ep2, owner, sv[1], AF_UNIX, SOCK_STREAM, 0,
                                     &un_addr, sizeof(un_addr), TTAK_NET_ENDPOINT_UNIX,
                                     UINT64_MAX, now) == TTAK_IO_SUCCESS);

    /* Test socket options */
    assert(ttak_net_endpoint_set_nonblocking(ep1, owner, true, now) == TTAK_IO_SUCCESS);
    assert(ttak_net_endpoint_set_nonblocking(ep2, owner, true, now) == TTAK_IO_SUCCESS);

    /* 3. Initialize poller and register ep2 for read events */
    ttak_net_poller_t poller;
    assert(ttak_net_poller_init(&poller, 16, now) == TTAK_IO_SUCCESS);

    uintptr_t tag = 0x1234;
    assert(ttak_net_poller_add(&poller, ep2, TTAK_NET_EVENT_READ, (void *)tag, owner, now) == TTAK_IO_SUCCESS);

    /* Poller wait with timeout 0 should return 0 (no data yet) */
    ttak_net_poller_event_t evs[4];
    int ready = ttak_net_poller_wait(&poller, evs, 4, 0, now);
    assert(ready == 0);

    /* Send data from ep1: 2 buffers in scatter-gather */
    const char *msg_part1 = "HELLO_";
    const char *msg_part2 = "WORLD_SG";
    write(sv[0], msg_part1, strlen(msg_part1));
    write(sv[0], msg_part2, strlen(msg_part2));

    /* Wait on poller */
    ready = ttak_net_poller_wait(&poller, evs, 4, 100, now);
    assert(ready == 1);
    assert(evs[0].endpoint == ep2);
    assert(evs[0].events & TTAK_NET_EVENT_READ);
    assert((uintptr_t)evs[0].user_data == tag);

    /* 4. Test scatter-gather receive (ttak_net_view_readv) */
    char buf1[8];
    char buf2[16];
    memset(buf1, 0, sizeof(buf1));
    memset(buf2, 0, sizeof(buf2));

    ttak_net_view_iovec_t iov[2];
    iov[0].iov_base = buf1;
    iov[0].iov_len = strlen(msg_part1);
    iov[1].iov_base = buf2;
    iov[1].iov_len = strlen(msg_part2);

    size_t bytes_read = 0;
    assert(ttak_net_view_readv(ep2, owner, iov, 2, &bytes_read, 0, now) == TTAK_IO_SUCCESS);
    assert(bytes_read == strlen(msg_part1) + strlen(msg_part2));
    assert(memcmp(buf1, msg_part1, strlen(msg_part1)) == 0);
    assert(memcmp(buf2, msg_part2, strlen(msg_part2)) == 0);

    /* Clean up poller and endpoints */
    assert(ttak_net_poller_del(&poller, ep2, owner, now) == TTAK_IO_SUCCESS);
    ttak_net_poller_destroy(&poller, now);

    ttak_net_endpoint_destroy(ep1, owner, now);
    ttak_net_endpoint_destroy(ep2, owner, now);
    ttak_owner_destroy(owner);

    printf("[PASS] test_net_poller_and_sg\n");
    return 0;
}
