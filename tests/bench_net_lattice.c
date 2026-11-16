#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <ttak/net/endpoint.h>
#include <ttak/net/view.h>
#include <ttak/mem/owner.h>
#include <ttak/timing/timing.h>

#define PAYLOAD_SIZE 1024

typedef struct {
    pthread_t sender;
    pthread_t receiver;
    ttak_owner_t *owner;
    ttak_shared_net_endpoint_t *endpoint;
    int fds[2];
    uint32_t role_flags;
    uint64_t receives;
    uint64_t bytes;
} bench_worker_t;

static atomic_bool stop_requested;

static double monotonic_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void *send_payloads(void *arg) {
    bench_worker_t *worker = arg;
    unsigned char payload[PAYLOAD_SIZE];
    memset(payload, 0x5a, sizeof(payload));
    while (!atomic_load_explicit(&stop_requested, memory_order_relaxed)) {
        ssize_t written = send(worker->fds[1], payload, sizeof(payload), 0);
        if (written == (ssize_t)sizeof(payload)) continue;
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            sched_yield();
            continue;
        }
        break;
    }
    return NULL;
}

static void *receive_payloads(void *arg) {
    bench_worker_t *worker = arg;
    ttak_net_lattice_set_worker_id((uint32_t)(uintptr_t)pthread_self());
    while (!atomic_load_explicit(&stop_requested, memory_order_relaxed)) {
        ttak_net_view_t view;
        ttak_net_view_init(&view);
        ttak_io_status_t status = ttak_net_view_from_endpoint(
            &view, worker->endpoint, worker->owner, PAYLOAD_SIZE, 0, ttak_get_tick_count());
        if (status == TTAK_IO_SUCCESS && view.len > 0) {
            worker->receives++;
            worker->bytes += view.len;
        } else if (status == TTAK_IO_ERR_NEEDS_RETRY) {
            sched_yield();
        } else {
            ttak_net_view_release(&view);
            break;
        }
        ttak_net_view_release(&view);
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s direct|lattice|auto WORKERS SECONDS LOAD_PERCENT\n", argv[0]);
        return 2;
    }
    int workers = atoi(argv[2]);
    double duration = atof(argv[3]);
    uint32_t load = (uint32_t)strtoul(argv[4], NULL, 10);
    if (workers < 1 || workers > 64 || duration <= 0.0) return 2;

    uint32_t accel = 0;
    if (strcmp(argv[1], "lattice") == 0) accel = TTAK_NET_ROLE_LATTICE_ACCEL;
    if (strcmp(argv[1], "auto") == 0) accel = TTAK_NET_ROLE_LATTICE_AUTO;
    if (strcmp(argv[1], "direct") != 0 && !accel) return 2;

    bench_worker_t *pool = calloc((size_t)workers, sizeof(*pool));
    if (!pool) return 1;
    atomic_store(&stop_requested, false);
    for (int i = 0; i < workers; ++i) {
        bench_worker_t *worker = &pool[i];
        worker->role_flags = TTAK_NET_ROLE_SERVER | accel;
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, worker->fds) != 0) return 1;
        int flags = fcntl(worker->fds[0], F_GETFL, 0);
        if (flags >= 0) fcntl(worker->fds[0], F_SETFL, flags | O_NONBLOCK);
        flags = fcntl(worker->fds[1], F_GETFL, 0);
        if (flags >= 0) fcntl(worker->fds[1], F_SETFL, flags | O_NONBLOCK);
        worker->owner = ttak_owner_create(TTAK_OWNER_SAFE_DEFAULT);
        if (!worker->owner) return 1;
        uint64_t now = ttak_get_tick_count();
        worker->endpoint = ttak_net_endpoint_create(worker->owner, now);
        if (!worker->endpoint) return 1;
        if (ttak_net_endpoint_bind_fd(worker->endpoint, worker->owner, worker->fds[0],
                                      AF_UNIX, SOCK_STREAM, 0, NULL, 0,
                                      TTAK_NET_ENDPOINT_UNIX, UINT64_MAX, now) != TTAK_IO_SUCCESS) {
            return 1;
        }
        ttak_net_endpoint_attr_t attr = {
            .role_flags = worker->role_flags,
            .listen_backlog = 0,
        };
        ttak_net_endpoint_set_role(worker->endpoint, &attr, worker->owner, now);
    }

    ttak_net_lattice_set_load(load, 100);
    double started = monotonic_sec();
    for (int i = 0; i < workers; ++i) {
        if (pthread_create(&pool[i].sender, NULL, send_payloads, &pool[i]) != 0) return 1;
        if (pthread_create(&pool[i].receiver, NULL, receive_payloads, &pool[i]) != 0) return 1;
    }
    struct timespec remaining = {
        .tv_sec = (time_t)duration,
        .tv_nsec = (long)((duration - (double)(time_t)duration) * 1000000000.0),
    };
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {}
    atomic_store_explicit(&stop_requested, true, memory_order_relaxed);
    for (int i = 0; i < workers; ++i) {
        pthread_join(pool[i].sender, NULL);
        pthread_join(pool[i].receiver, NULL);
    }
    double elapsed = monotonic_sec() - started;

    uint64_t receives = 0;
    uint64_t bytes = 0;
    for (int i = 0; i < workers; ++i) {
        receives += pool[i].receives;
        bytes += pool[i].bytes;
        ttak_net_endpoint_destroy(pool[i].endpoint, pool[i].owner, ttak_get_tick_count());
        ttak_owner_destroy(pool[i].owner);
        close(pool[i].fds[0]);
        close(pool[i].fds[1]);
    }
    printf("mode=%s workers=%d load=%u elapsed=%.3f receives=%llu bytes=%llu recv_ops_per_sec=%.0f mib_per_sec=%.2f\n",
           argv[1], workers, load, elapsed, (unsigned long long)receives,
           (unsigned long long)bytes, (double)receives / elapsed,
           (double)bytes / elapsed / (1024.0 * 1024.0));
    free(pool);
    return 0;
}
