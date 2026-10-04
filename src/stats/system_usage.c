#include <ttak/stats/system_usage.h>

#include <ttak/compat/pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#include <dirent.h>
#endif

typedef struct {
    unsigned long long idle;
    unsigned long long total;
    _Bool valid;
} ttak_cpu_snapshot_t;

static pthread_mutex_t g_cpu_lock = PTHREAD_MUTEX_INITIALIZER;
static ttak_cpu_snapshot_t *g_prev_cores = NULL;
static size_t g_prev_core_count = 0;
static ttak_cpu_snapshot_t g_prev_total = {0};

static _Bool ttak_read_proc_stat(ttak_cpu_snapshot_t *total_out,
                                 ttak_cpu_snapshot_t **cores_out,
                                 size_t *core_count_out) {
#ifdef _WIN32
    (void)total_out;
    (void)cores_out;
    (void)core_count_out;
    return 0;
#else
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 0;

    char line[512];
    ttak_cpu_snapshot_t total = {0};
    ttak_cpu_snapshot_t *cores = NULL;
    size_t cap = 0;
    size_t len = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu", 3) != 0) continue;
        char name[32] = {0};
        unsigned long long user = 0, nice = 0, system = 0, idle = 0, iowait = 0;
        unsigned long long irq = 0, softirq = 0, steal = 0, guest = 0, guest_nice = 0;
        int n = sscanf(line, "%31s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                       name, &user, &nice, &system, &idle, &iowait, &irq, &softirq,
                       &steal, &guest, &guest_nice);
        if (n < 5) continue;

        ttak_cpu_snapshot_t snap = {0};
        snap.idle = idle + iowait;
        snap.total = user + nice + system + idle + iowait + irq + softirq + steal + guest + guest_nice;
        snap.valid = 1;

        if (strcmp(name, "cpu") == 0) {
            total = snap;
            continue;
        }

        if (len == cap) {
            size_t new_cap = cap ? cap * 2 : 8;
            ttak_cpu_snapshot_t *tmp = realloc(cores, new_cap * sizeof(*tmp));
            if (!tmp) {
                free(cores);
                fclose(f);
                return 0;
            }
            cores = tmp;
            cap = new_cap;
        }
        cores[len++] = snap;
    }

    fclose(f);
    if (!total.valid) {
        free(cores);
        return 0;
    }

    *total_out = total;
    *cores_out = cores;
    *core_count_out = len;
    return 1;
#endif
}

static double ttak_cpu_usage_delta(ttak_cpu_snapshot_t prev, ttak_cpu_snapshot_t now) {
    if (!prev.valid || !now.valid) return 0.0;
    unsigned long long totald = now.total - prev.total;
    unsigned long long idled = now.idle - prev.idle;
    if (totald == 0) return 0.0;
    return (double)(totald - idled) * 100.0 / (double)totald;
}

/**
 * @brief Parse /proc/stat once and atomically update the shared previous
 * snapshot store, computing usage deltas against the previous snapshot
 * under the same lock.
 *
 * Both total and per-core usage derive from a single parse and the same
 * previous-snapshot baseline, so interleaving the two public functions no
 * longer corrupts the delta baseline. Pass core < 0 to skip per-core.
 */
static _Bool ttak_cpu_snapshot_usage(int core,
                                     double *total_usage,
                                     double *core_usage) {
    ttak_cpu_snapshot_t now_total = {0};
    ttak_cpu_snapshot_t *now_cores = NULL;
    size_t now_core_count = 0;
    if (!ttak_read_proc_stat(&now_total, &now_cores, &now_core_count)) {
        return 0;
    }

    pthread_mutex_lock(&g_cpu_lock);
    if (total_usage) {
        *total_usage = ttak_cpu_usage_delta(g_prev_total, now_total);
    }
    if (core_usage) {
        if ((size_t)core < now_core_count && (size_t)core < g_prev_core_count) {
            *core_usage = ttak_cpu_usage_delta(g_prev_cores[core], now_cores[core]);
        } else if ((size_t)core < now_core_count) {
            *core_usage = 0.0;
        } else {
            *core_usage = -1.0;
        }
    }
    g_prev_total = now_total;
    g_prev_total.valid = 1;
    free(g_prev_cores);
    g_prev_cores = now_cores;
    g_prev_core_count = now_core_count;
    pthread_mutex_unlock(&g_cpu_lock);
    return 1;
}

double ttak_get_cpu_usage_total(void) {
    double usage = 0.0;
    if (!ttak_cpu_snapshot_usage(-1, &usage, NULL)) {
        return -1.0;
    }
    return usage;
}

double ttak_get_cpu_usage_per_core(int core) {
    if (core < 0) return -1.0;

    double usage = -1.0;
    if (!ttak_cpu_snapshot_usage(core, NULL, &usage)) {
        return -1.0;
    }
    return usage;
}

double ttak_get_gpu_usage_total(void) {
#ifdef _WIN32
    return -1.0;
#else
    DIR *drm = opendir("/sys/class/drm");
    if (!drm) return -1.0;
    struct dirent *de = NULL;
    double sum = 0.0;
    int count = 0;
    while ((de = readdir(drm)) != NULL) {
        if (strncmp(de->d_name, "card", 4) != 0) continue;
        char path[512];
        snprintf(path, sizeof(path), "/sys/class/drm/%s/device/gpu_busy_percent", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        double v = 0.0;
        if (fscanf(f, "%lf", &v) == 1) {
            sum += v;
            count++;
        }
        fclose(f);
    }
    closedir(drm);
    if (count == 0) return -1.0;
    return sum / (double)count;
#endif
}

double ttak_get_gpu_usage_per_cu(int cu) {
    (void)cu;
    return -1.0;
}

size_t ttak_get_rss_footprint(void) {
#ifdef _WIN32
    return 0;
#else
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long size_pages = 0;
    unsigned long resident_pages = 0;
    if (fscanf(f, "%lu %lu", &size_pages, &resident_pages) != 2) {
        fclose(f);
        return 0;
    }
    fclose(f);
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return 0;
    return (size_t)resident_pages * (size_t)page_size;
#endif
}

char *ttak_get_rss_footprint_full(void) {
    size_t rss_bytes = ttak_get_rss_footprint();
    size_t vmrss_kb = 0;
    size_t vmsize_kb = 0;

#ifndef _WIN32
    FILE *f = fopen("/proc/self/status", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "VmRSS: %zu kB", &vmrss_kb) == 1) continue;
            if (sscanf(line, "VmSize: %zu kB", &vmsize_kb) == 1) continue;
        }
        fclose(f);
    }
#endif

    char *json = malloc(256);
    if (!json) return NULL;
    snprintf(json, 256,
             "{\"rss_bytes\":%zu,\"vmrss_kb\":%zu,\"vmsize_kb\":%zu}",
             rss_bytes, vmrss_kb, vmsize_kb);
    return json;
}
