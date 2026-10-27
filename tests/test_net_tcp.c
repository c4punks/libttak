/* TCP over the loopback interface through the endpoint, poller, view, io
 * and session layers. Runs on POSIX hosts and, with NET=lwip, on lwIP. */
#include <string.h>
#include <malloc.h>

#include <ttak/compat/baremetal.h>
#include <ttak/net/endpoint.h>
#include <ttak/net/poller.h>
#include <ttak/net/session.h>
#include <ttak/net/view.h>
#include <ttak/io/sync.h>
#include <ttak/mem/mem.h>
#include <ttak/mem/owner.h>
#include <ttak/timing/timing.h>
#include "test_macros.h"

#if TTAK_NET_LWIP
#include <unistd.h>
#include "lwip/sockets.h"
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

typedef struct {
    ttak_owner_t *owner;
    int client_fd;
    int server_fd;
    ttak_shared_net_endpoint_t *client;
    ttak_shared_net_endpoint_t *server;
    ttak_io_guard_t client_io;
} conn_t;

/* No address: closing an endpoint would otherwise reconnect to it. The
 * test closes the sockets itself. */
static ttak_shared_net_endpoint_t *wrap(ttak_owner_t *owner, int fd, uint64_t now) {
    ttak_shared_net_endpoint_t *ep = ttak_net_endpoint_create(owner, now);
    ASSERT(ep != NULL);
    ASSERT(ttak_net_endpoint_bind_fd(ep, owner, fd, AF_INET, SOCK_STREAM, 0, NULL, 0,
                                     TTAK_NET_ENDPOINT_IPV4, UINT64_MAX, now) == TTAK_IO_SUCCESS);
    return ep;
}

/* A connected client/server pair on 127.0.0.1 with an ephemeral port. */
static conn_t connect_pair(void) {
    uint64_t now = ttak_get_tick_count();
    conn_t c;
    memset(&c, 0, sizeof(c));
    c.owner = ttak_owner_create(TTAK_OWNER_SAFE_DEFAULT);
    ASSERT(c.owner != NULL);

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT(listener >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ASSERT(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    ASSERT(listen(listener, 1) == 0);
    socklen_t len = sizeof(addr);
    ASSERT(getsockname(listener, (struct sockaddr *)&addr, &len) == 0);

    c.client_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT(c.client_fd >= 0);
    ASSERT(connect(c.client_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    c.server_fd = accept(listener, NULL, NULL);
    ASSERT(c.server_fd >= 0);
    close(listener);

    c.client = wrap(c.owner, c.client_fd, now);
    c.server = wrap(c.owner, c.server_fd, now);
    ASSERT(ttak_io_guard_init(&c.client_io, c.client_fd, c.owner, UINT64_MAX, now) == TTAK_IO_SUCCESS);
    return c;
}

/* ttak_io_sync_* only take buffers from ttak_mem_alloc(). */
static void *io_buf(size_t len) {
    void *p = ttak_mem_alloc(len, __TTAK_UNSAFE_MEM_FOREVER__, ttak_get_tick_count());
    ASSERT(p != NULL);
    return p;
}

static void send_all(conn_t *c, const void *data, size_t len) {
    void *buf = io_buf(len);
    memcpy(buf, data, len);
    size_t written = 0;
    ASSERT(ttak_io_sync_write(&c->client_io, buf, len, &written, ttak_get_tick_count()) == TTAK_IO_SUCCESS);
    ASSERT(written == len);
    ttak_mem_free(buf);
}

static void close_pair(conn_t *c) {
    uint64_t now = ttak_get_tick_count();
    ttak_net_endpoint_destroy(c->client, c->owner, now);
    ttak_net_endpoint_destroy(c->server, c->owner, now);
    ttak_owner_destroy(c->owner);
    if (c->client_fd >= 0) close(c->client_fd);
    close(c->server_fd);
}

/* Waits until the server side has something to read. */
static void wait_readable(ttak_shared_net_endpoint_t *ep, ttak_owner_t *owner) {
    uint64_t now = ttak_get_tick_count();
    ttak_net_poller_t poller;
    ASSERT(ttak_net_poller_init(&poller, 4, now) == TTAK_IO_SUCCESS);
    ASSERT(ttak_net_poller_add(&poller, ep, TTAK_NET_EVENT_READ, NULL, owner, now) == TTAK_IO_SUCCESS);
    ttak_net_poller_event_t ev;
    ASSERT(ttak_net_poller_wait(&poller, &ev, 1, 2000, now) == 1);
    ASSERT(ev.endpoint == ep && (ev.events & TTAK_NET_EVENT_READ));
    ASSERT(ttak_net_poller_del(&poller, ep, owner, now) == TTAK_IO_SUCCESS);
    ttak_net_poller_destroy(&poller, now);
}

static void test_socket_options(void) {
    conn_t c = connect_pair();
    uint64_t now = ttak_get_tick_count();
    ASSERT(ttak_net_endpoint_set_nonblocking(c.server, c.owner, true, now) == TTAK_IO_SUCCESS);
    ASSERT(ttak_net_endpoint_set_nonblocking(c.server, c.owner, false, now) == TTAK_IO_SUCCESS);
    ASSERT(ttak_net_endpoint_set_nodelay(c.client, c.owner, true, now) == TTAK_IO_SUCCESS);
    ASSERT(ttak_net_endpoint_set_reuse(c.client, c.owner, true, true, now) == TTAK_IO_SUCCESS);
    ASSERT(ttak_net_endpoint_set_buffer_sizes(c.server, c.owner, 8192, 8192, now) == TTAK_IO_SUCCESS);
    ASSERT(ttak_net_endpoint_set_keepalive(c.client, c.owner, true, now) == TTAK_IO_SUCCESS);
    close_pair(&c);
}

/* Nonblocking: an empty socket polls quiet, then readable once data lands. */
static void test_poller_and_readv(void) {
    conn_t c = connect_pair();
    uint64_t now = ttak_get_tick_count();
    ASSERT(ttak_net_endpoint_set_nonblocking(c.server, c.owner, true, now) == TTAK_IO_SUCCESS);

    ttak_net_poller_t poller;
    ASSERT(ttak_net_poller_init(&poller, 4, now) == TTAK_IO_SUCCESS);
    ASSERT(ttak_net_poller_add(&poller, c.server, TTAK_NET_EVENT_READ, (void *)0x1234,
                               c.owner, now) == TTAK_IO_SUCCESS);
    ttak_net_poller_event_t evs[2];
    ASSERT(ttak_net_poller_wait(&poller, evs, 2, 0, now) == 0);

    send_all(&c, "HELLO_WORLD_SG", 14);
    ASSERT(ttak_net_poller_wait(&poller, evs, 2, 2000, now) == 1);
    ASSERT(evs[0].endpoint == c.server);
    ASSERT(evs[0].events & TTAK_NET_EVENT_READ);
    ASSERT((uintptr_t)evs[0].user_data == 0x1234);

    char a[6], b[8];
    ttak_net_view_iovec_t iov[2] = { { a, sizeof(a) }, { b, sizeof(b) } };
    size_t got = 0;
    ASSERT(ttak_net_view_readv(c.server, c.owner, iov, 2, &got, 0, now) == TTAK_IO_SUCCESS);
    ASSERT(got == 14);
    ASSERT(memcmp(a, "HELLO_", 6) == 0 && memcmp(b, "WORLD_SG", 8) == 0);

    ASSERT(ttak_net_poller_del(&poller, c.server, c.owner, now) == TTAK_IO_SUCCESS);
    ttak_net_poller_destroy(&poller, now);
    close_pair(&c);
}

/* Blocking reads, and a payload bigger than one TCP segment. */
static void test_sync_bulk_transfer(void) {
    enum { BULK = 6000 };
    unsigned char *out = io_buf(BULK), *in = io_buf(BULK);
    for (size_t i = 0; i < BULK; ++i) out[i] = (unsigned char)(i * 7u + 3u);
    conn_t c = connect_pair();
    uint64_t now = ttak_get_tick_count();
    ttak_io_guard_t server_io;
    ASSERT(ttak_io_guard_init(&server_io, c.server_fd, c.owner, UINT64_MAX, now) == TTAK_IO_SUCCESS);

    send_all(&c, out, BULK);
    size_t total = 0;
    while (total < BULK) {
        size_t got = 0;
        ASSERT(ttak_io_sync_read(&server_io, in + total, BULK - total, &got, now) == TTAK_IO_SUCCESS);
        ASSERT(got > 0);
        total += got;
    }
    ASSERT(memcmp(in, out, BULK) == 0);
    ttak_mem_free(in);
    ttak_mem_free(out);
    close_pair(&c);
}

static size_t heap_in_use(void) {
#if defined(__GLIBC__)
    return mallinfo2().uordblks;
#else
    return (size_t)mallinfo().uordblks;
#endif
}

/* Each sync read/write stages its data in a detachable buffer; those used
 * to be retired to the epoch and never reclaimed (about 1 KiB per call). */
static void test_sync_io_returns_memory(void) {
    enum { ROUNDS = 1000, LEN = 1024 };
    conn_t c = connect_pair();
    uint64_t now = ttak_get_tick_count();
    /* Otherwise Nagle waits out lwIP's delayed ACK on every round. */
    ASSERT(ttak_net_endpoint_set_nodelay(c.client, c.owner, true, now) == TTAK_IO_SUCCESS);
    ttak_io_guard_t server_io;
    ASSERT(ttak_io_guard_init(&server_io, c.server_fd, c.owner, UINT64_MAX, now) == TTAK_IO_SUCCESS);
    unsigned char *buf = io_buf(LEN);
    memset(buf, 0x5a, LEN);

    size_t before = 0;
    for (int i = 0; i < ROUNDS; ++i) {
        if (i == 100) before = heap_in_use(); /* after caches have warmed up */
        size_t n = 0;
        ASSERT(ttak_io_sync_write(&c.client_io, buf, LEN, &n, now) == TTAK_IO_SUCCESS && n == LEN);
        for (size_t total = 0; total < LEN; total += n) {
            ASSERT(ttak_io_sync_read(&server_io, buf + total, LEN - total, &n, now) == TTAK_IO_SUCCESS && n > 0);
        }
    }
    size_t growth = heap_in_use() - before;
    ASSERT_MSG(growth < 64 * 1024, "heap grew by %lu bytes over %d round trips",
               (unsigned long)growth, ROUNDS - 100);
    ttak_mem_free(buf);
    close_pair(&c);
}

static void test_zerocopy_view(void) {
    conn_t c = connect_pair();
    uint64_t now = ttak_get_tick_count();
    unsigned char msg[64];
    for (size_t i = 0; i < sizeof(msg); ++i) msg[i] = (unsigned char)i;
    send_all(&c, msg, sizeof(msg));
    wait_readable(c.server, c.owner);

    ttak_net_view_t view;
    ttak_net_view_init(&view);
    ASSERT(ttak_net_view_from_endpoint(&view, c.server, c.owner, sizeof(msg), 0, now) == TTAK_IO_SUCCESS);
    ASSERT(view.region.len == sizeof(msg));
    ASSERT(memcmp(ttak_net_view_data(&view), msg, sizeof(msg)) == 0);
    ttak_net_view_release(&view);
    close_pair(&c);
}

/* The peer closing shows up as readable with a zero-byte read. */
static void test_peer_close(void) {
    conn_t c = connect_pair();
    close(c.client_fd);
    c.client_fd = -1;
    wait_readable(c.server, c.owner);
    char buf[4];
    ASSERT(recv(c.server_fd, buf, sizeof(buf), 0) == 0);
    close_pair(&c);
}

static void test_session_lifecycle(void) {
    conn_t c = connect_pair();
    uint64_t now = ttak_get_tick_count();
    ttak_net_session_mgr_t mgr;
    ttak_net_session_mgr_init(&mgr, false);
    ttak_net_session_t *session = ttak_net_session_mgr_create(&mgr, c.server, NULL, c.owner, now);
    ASSERT(session != NULL);
    ttak_net_session_mgr_tick(&mgr, now);
    ttak_net_session_mgr_close(&mgr, session, now);
    ttak_net_session_mgr_destroy(&mgr, now);
    close_pair(&c);
}

int main(void) {
    RUN_TEST(test_socket_options);
    RUN_TEST(test_poller_and_readv);
    RUN_TEST(test_sync_bulk_transfer);
    RUN_TEST(test_sync_io_returns_memory);
    RUN_TEST(test_zerocopy_view);
    RUN_TEST(test_peer_close);
    RUN_TEST(test_session_lifecycle);
    return 0;
}
