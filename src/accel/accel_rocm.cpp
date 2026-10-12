#include "ttak/ttak_accelerator.h"

#ifdef ENABLE_ROCM
#include <hip/hip_runtime.h>
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER) && !defined(__cplusplus)
typedef unsigned char _Bool;
#endif

/* Provide __forceinline__ for host compilers that do not define it
   (e.g. MSVC or plain GCC/Clang when not using nvcc/hipcc). */
#ifndef __forceinline__
#if defined(_MSC_VER)
#define __forceinline__ __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define __forceinline__ __attribute__((always_inline)) inline
#else
#define __forceinline__ inline
#endif
#endif

#define TTAK_ACCEL_FACTOR_MAX 64
#define TTAK_HIP_SMALL_PRIME_COUNT 168
#define TTAK_HIP_POLLARD_STACK_MAX 64

extern "C" {
ttak_result_t ttak_accel_run_cpu(
    const ttak_accel_batch_item_t *items,
    size_t item_count,
    const ttak_accel_config_t *config);
}

typedef struct {
    uint64_t prime;
    uint32_t exponent;
    uint32_t reserved;
} ttak_accel_factor_slot_t;

typedef struct {
    uint64_t value;
    uint32_t factor_count;
    uint32_t checksum;
    uint32_t reserved;
    ttak_accel_factor_slot_t slots[TTAK_ACCEL_FACTOR_MAX];
} ttak_accel_factor_record_t;

typedef struct {
    uint32_t guard;
    uint32_t record_count;
    uint32_t payload_checksum;
    uint32_t reserved;
} ttak_accel_record_prefix_t;

typedef struct {
    uint64_t state;
} ttak_hip_rng_t;

#ifdef ENABLE_ROCM

__device__ __constant__ uint16_t ttak_hip_small_primes[TTAK_HIP_SMALL_PRIME_COUNT] = {
    2,  3,  5,  7, 11, 13, 17, 19, 23, 29,
    31, 37, 41, 43, 47, 53, 59, 61, 67, 71,
    73, 79, 83, 89, 97,101,103,107,109,113,
    127,131,137,139,149,151,157,163,167,173,
    179,181,191,193,197,199,211,223,227,229,
    233,239,241,251,257,263,269,271,277,281,
    283,293,307,311,313,317,331,337,347,349,
    353,359,367,373,379,383,389,397,401,409,
    419,421,431,433,439,443,449,457,461,463,
    467,479,487,491,499,503,509,521,523,541,
    547,557,563,569,571,577,587,593,599,601,
    607,613,617,619,631,641,643,647,653,659,
    661,673,677,683,691,701,709,719,727,733,
    739,743,751,757,761,769,773,787,797,809,
    811,821,823,827,829,839,853,857,859,863,
    877,881,883,887,907,911,919,929,937,941,
    947,953,967,971,977,983,991,997
};

static inline uint32_t ttak_guard_word(const ttak_accel_config_t *config,
                                       const ttak_accel_batch_item_t *item) {
    uint32_t guard = config->integrity_mask ^ item->mask_seed;
    guard |= 0x01010101u;
    return guard;
}

static inline uint32_t ttak_checksum_seed(const ttak_accel_batch_item_t *item) {
    return (item->checksum_salt == 0u) ? 2166136261u : item->checksum_salt;
}

static bool ttak_hip_context_ready(void) {
    static bool checked = false;
    static bool ready = false;
    if (checked) {
        return ready;
    }

    int device_count = 0;
    hipError_t err = hipInit(0);
    if (err == hipSuccess) {
        err = hipGetDeviceCount(&device_count);
    }
    ready = false;
    if (err == hipSuccess && device_count > 0) {
        err = hipSetDevice(0);
        if (err == hipSuccess) {
            ready = true;
        } else {
            (void)hipGetLastError();
        }
    } else if (err != hipSuccess) {
        (void)hipGetLastError();
    }

    checked = true;
    return ready;
}

static inline uint32_t ttak_fnv1a32(const void *data, size_t len, uint32_t seed) {
    const uint8_t *ptr = (const uint8_t *)data;
    uint32_t hash = (seed == 0u) ? 2166136261u : seed;
    for (size_t i = 0; i < len; ++i) {
        hash ^= ptr[i];
        hash *= 16777619u;
    }
    return hash;
}

static inline void ttak_mask_payload(uint8_t *buf, size_t len, uint32_t guard) {
    const uint8_t lanes[4] = {
        (uint8_t)(guard & 0xFFu),
        (uint8_t)((guard >> 8) & 0xFFu),
        (uint8_t)((guard >> 16) & 0xFFu),
        (uint8_t)((guard >> 24) & 0xFFu)
    };
    for (size_t i = 0; i < len; ++i) {
        buf[i] ^= lanes[i & 0x3u];
    }
}

__device__ __forceinline__ uint64_t ttak_device_abs_diff(uint64_t a, uint64_t b) {
    return (a > b) ? (a - b) : (b - a);
}

__device__ __forceinline__ uint64_t ttak_device_gcd(uint64_t a, uint64_t b) {
    while (b != 0ULL) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

__device__ __host__ __forceinline__ uint64_t ttak_device_mulmod(uint64_t a,
                                                               uint64_t b,
                                                               uint64_t mod) {
#if defined(__HIP_DEVICE_COMPILE__)
    a %= mod;
    b %= mod;
    uint64_t res = 0;
    while (a > 0) {
        if (a & 1) {
            res = (res + b) % mod;
        }
        a >>= 1;
        b = (b << 1) % mod;
    }
    return res;
#elif defined(__SIZEOF_INT128__)
    unsigned __int128 res = (unsigned __int128)a * b;
    return (uint64_t)(res % mod);
#elif defined(_MSC_VER)
    a %= mod;
    b %= mod;
    uint64_t res = 0;
    while (a > 0) {
        if (a & 1) {
            res = (res + b) % mod;
        }
        a >>= 1;
        b = (b << 1) % mod;
    }
    return res;
#else
    return (uint64_t)(((unsigned __int128)a * b) % mod);
#endif
}

__device__ __forceinline__ uint64_t ttak_device_powmod(uint64_t base,
                                                       uint64_t exp,
                                                       uint64_t mod) {
    uint64_t result = 1ULL % mod;
    uint64_t x = base % mod;
    while (exp > 0ULL) {
        if (exp & 1ULL) {
            result = ttak_device_mulmod(result, x, mod);
        }
        x = ttak_device_mulmod(x, x, mod);
        exp >>= 1ULL;
    }
    return result;
}

__device__ __forceinline__ uint64_t ttak_device_rng_next(ttak_hip_rng_t *rng) {
    uint64_t z = (rng->state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

__device__ __forceinline__ uint64_t ttak_device_rng_between(ttak_hip_rng_t *rng,
                                                            uint64_t min_inclusive,
                                                            uint64_t max_inclusive) {
    if (max_inclusive <= min_inclusive) return min_inclusive;
    uint64_t span = max_inclusive - min_inclusive + 1ULL;
    return min_inclusive + (ttak_device_rng_next(rng) % span);
}

__device__ bool ttak_device_miller_rabin(uint64_t n) {
    if (n < 2ULL) return false;
    for (int i = 0; i < TTAK_HIP_SMALL_PRIME_COUNT; ++i) {
        uint64_t p = (uint64_t)ttak_hip_small_primes[i];
        if (n == p) return true;
        if (n % p == 0ULL) return false;
    }

    uint64_t d = n - 1ULL;
    uint32_t s = 0;
    while ((d & 1ULL) == 0ULL) {
        d >>= 1ULL;
        ++s;
    }

    const uint64_t bases[] = {2ULL, 3ULL, 5ULL, 7ULL, 11ULL, 13ULL};
    for (int i = 0; i < (int)(sizeof(bases) / sizeof(bases[0])); ++i) {
        uint64_t a = bases[i] % n;
        if (a == 0ULL) continue;
        uint64_t x = ttak_device_powmod(a, d, n);
        if (x == 1ULL || x == n - 1ULL) continue;
        bool witness = true;
        for (uint32_t r = 1; r < s; ++r) {
            x = ttak_device_mulmod(x, x, n);
            if (x == n - 1ULL) {
                witness = false;
                break;
            }
        }
        if (witness) {
            return false;
        }
    }
    return true;
}

__device__ uint64_t ttak_device_pollard_rho(uint64_t n, ttak_hip_rng_t *rng) {
    if ((n & 1ULL) == 0ULL) return 2ULL;
    uint64_t c = ttak_device_rng_between(rng, 1ULL, n - 1ULL);
    uint64_t y = ttak_device_rng_between(rng, 1ULL, n - 1ULL);
    uint64_t m = 128ULL;
    uint64_t g = 1ULL;
    uint64_t r = 1ULL;
    uint64_t q = 1ULL;
    uint64_t ys = 0ULL;
    uint64_t x = 0ULL;

    while (g == 1ULL) {
        x = y;
        for (uint64_t i = 0; i < r; ++i) {
            y = (ttak_device_mulmod(y, y, n) + c) % n;
        }
        uint64_t k = 0ULL;
        while (k < r && g == 1ULL) {
            ys = y;
            uint64_t limit = (m < (r - k)) ? m : (r - k);
            for (uint64_t i = 0; i < limit; ++i) {
                y = (ttak_device_mulmod(y, y, n) + c) % n;
                uint64_t diff = ttak_device_abs_diff(x, y);
                if (diff == 0ULL) continue;
                q = ttak_device_mulmod(q, diff, n);
            }
            g = ttak_device_gcd(q, n);
            k += limit;
        }
        r <<= 1ULL;
    }

    if (g == n) {
        do {
            ys = (ttak_device_mulmod(ys, ys, n) + c) % n;
            uint64_t diff = ttak_device_abs_diff(x, ys);
            g = ttak_device_gcd(diff, n);
        } while (g == 1ULL);
    }

    return g;
}

__device__ bool ttak_device_insert_factor(uint64_t prime,
                                          ttak_accel_factor_record_t *record) {
    int count = (int)record->factor_count;
    for (int i = 0; i < count; ++i) {
        uint64_t slot_prime = record->slots[i].prime;
        if (slot_prime == prime) {
            record->slots[i].exponent++;
            return true;
        }
        if (slot_prime > prime) {
            if (count >= TTAK_ACCEL_FACTOR_MAX) return false;
            for (int j = count; j > i; --j) {
                record->slots[j] = record->slots[j - 1];
            }
            record->slots[i].prime = prime;
            record->slots[i].exponent = 1;
            record->factor_count++;
            return true;
        }
    }

    if (count >= TTAK_ACCEL_FACTOR_MAX) return false;
    record->slots[count].prime = prime;
    record->slots[count].exponent = 1;
    record->factor_count++;
    return true;
}

__device__ void ttak_device_factor_number(uint64_t value,
                                          ttak_hip_rng_t *rng,
                                          ttak_accel_factor_record_t *record) {
    record->value = value;
    record->factor_count = 0;
    record->checksum = 0;
    record->reserved = 0;
    for (int i = 0; i < TTAK_ACCEL_FACTOR_MAX; ++i) {
        record->slots[i].prime = 0ULL;
        record->slots[i].exponent = 0;
        record->slots[i].reserved = 0;
    }
    if (value <= 1ULL) {
        return;
    }

    uint64_t n = value;
    for (int i = 0; i < TTAK_HIP_SMALL_PRIME_COUNT; ++i) {
        uint64_t p = (uint64_t)ttak_hip_small_primes[i];
        if (p * p > n) break;
        while (n % p == 0ULL) {
            if (!ttak_device_insert_factor(p, record)) {
                return;
            }
            n /= p;
        }
    }

    if (n == 1ULL) {
        return;
    }

    uint64_t stack[TTAK_HIP_POLLARD_STACK_MAX];
    int sp = 0;
    stack[sp++] = n;
    while (sp > 0) {
        uint64_t current = stack[--sp];
        if (current == 1ULL) continue;
        if (ttak_device_miller_rabin(current)) {
            ttak_device_insert_factor(current, record);
            continue;
        }

        uint64_t factor = 0ULL;
        for (int attempt = 0; attempt < 64; ++attempt) {
            uint64_t candidate = ttak_device_pollard_rho(current, rng);
            if (candidate > 1ULL && candidate < current) {
                factor = candidate;
                break;
            }
        }

        if (factor == 0ULL || factor == current) {
            ttak_device_insert_factor(current, record);
            continue;
        }

        if (sp + 2 <= TTAK_HIP_POLLARD_STACK_MAX) {
            stack[sp++] = factor;
            stack[sp++] = current / factor;
        } else {
            ttak_device_insert_factor(current, record);
        }
    }
}

static size_t ttak_accel_record_active_size(const ttak_accel_factor_record_t *record) {
    size_t used_slots = record->factor_count;
    if (used_slots > TTAK_ACCEL_FACTOR_MAX) used_slots = TTAK_ACCEL_FACTOR_MAX;
    return sizeof(record->value) + sizeof(record->factor_count) +
           sizeof(record->checksum) +
           used_slots * sizeof(record->slots[0]);
}

static void ttak_finalize_record(ttak_accel_factor_record_t *record,
                                 uint32_t checksum_seed,
                                 uint32_t ordinal) {
    record->reserved = 0;
    uint32_t seed = checksum_seed ^ (ordinal * 0x9E3779B1u);
    uint32_t hash = ttak_fnv1a32(&record->value,
                                 sizeof(record->value) + sizeof(record->factor_count) + sizeof(record->reserved),
                                 seed);
    size_t used_slots = record->factor_count;
    if (used_slots > TTAK_ACCEL_FACTOR_MAX) used_slots = TTAK_ACCEL_FACTOR_MAX;
    hash = ttak_fnv1a32(record->slots, used_slots * sizeof(record->slots[0]), hash);
    record->checksum = hash;
}

static ttak_result_t ttak_finalize_output(const ttak_accel_batch_item_t *item,
                                          uint32_t guard,
                                          size_t record_count,
                                          uint32_t checksum_seed) {
    size_t payload_offset = sizeof(ttak_accel_record_prefix_t);
    uint8_t *payload = item->output + payload_offset;
    uint32_t payload_checksum = checksum_seed;
    for (size_t i = 0; i < record_count; ++i) {
        uint8_t *record_bytes = payload + i * sizeof(ttak_accel_factor_record_t);
        const ttak_accel_factor_record_t *record = (const ttak_accel_factor_record_t *)record_bytes;
        payload_checksum = ttak_fnv1a32(record_bytes,
                                        ttak_accel_record_active_size(record),
                                        payload_checksum);
    }

    ttak_accel_record_prefix_t prefix;
    prefix.guard = guard;
    prefix.record_count = (uint32_t)record_count;
    prefix.payload_checksum = payload_checksum;
    prefix.reserved = sizeof(ttak_accel_factor_record_t);
    memcpy(item->output, &prefix, sizeof(prefix));
    for (size_t i = 0; i < record_count; ++i) {
        uint8_t *record_bytes = payload + i * sizeof(ttak_accel_factor_record_t);
        const ttak_accel_factor_record_t *record = (const ttak_accel_factor_record_t *)record_bytes;
        ttak_mask_payload(record_bytes, ttak_accel_record_active_size(record), guard);
    }
    if (item->checksum_out != NULL) {
        *(item->checksum_out) = payload_checksum;
    }
    return TTAK_RESULT_OK;
}

__global__ void ttak_hip_factor_kernel(const uint64_t *values,
                                       ttak_accel_factor_record_t *records,
                                       size_t count,
                                       uint64_t seed_base) {
    size_t idx = (blockIdx.x * blockDim.x) + threadIdx.x;
    if (idx >= count) return;

    uint64_t n = values[idx];
    ttak_accel_factor_record_t record;
    ttak_hip_rng_t rng;
    rng.state = seed_base ^ n ^ (uint64_t)idx;
    ttak_device_factor_number(n, &rng, &record);
    records[idx] = record;
}

static ttak_result_t ttak_hip_process_item(const ttak_accel_batch_item_t *item,
                                           const ttak_accel_config_t *config) {
    if (item->input == NULL || item->output == NULL) {
        return TTAK_RESULT_ERR_ARGUMENT;
    }
    if (item->input_len == 0 || (item->input_len % sizeof(uint64_t)) != 0) {
        return TTAK_RESULT_ERR_ARGUMENT;
    }

    size_t record_count = item->input_len / sizeof(uint64_t);
    if (record_count == 0 || record_count > UINT32_MAX) {
        return TTAK_RESULT_ERR_ARGUMENT;
    }
    if (record_count > (SIZE_MAX - sizeof(ttak_accel_record_prefix_t)) /
                           sizeof(ttak_accel_factor_record_t)) {
        return TTAK_RESULT_ERR_ARGUMENT;
    }

    size_t payload_size = record_count * sizeof(ttak_accel_factor_record_t);
    size_t needed = sizeof(ttak_accel_record_prefix_t) + payload_size;
    if (item->output_len < needed) {
        return TTAK_RESULT_ERR_ARGUMENT;
    }

    uint32_t guard = ttak_guard_word(config, item);
    uint32_t checksum_seed = ttak_checksum_seed(item);

    size_t value_bytes = record_count * sizeof(uint64_t);
    uint64_t *host_values = (uint64_t *)malloc(value_bytes);
    ttak_accel_factor_record_t *host_records =
        (ttak_accel_factor_record_t *)malloc(payload_size);
    if (!host_values || !host_records) {
        free(host_values);
        free(host_records);
        return TTAK_RESULT_ERR_EXECUTION;
    }

    memcpy(host_values, item->input, value_bytes);

    uint64_t *device_values = NULL;
    ttak_accel_factor_record_t *device_records = NULL;
    hipError_t err = hipMalloc((void **)&device_values, value_bytes);
    if (err != hipSuccess) {
        free(host_values);
        free(host_records);
        return TTAK_RESULT_ERR_EXECUTION;
    }
    err = hipMalloc((void **)&device_records, payload_size);
    if (err != hipSuccess) {
        hipFree(device_values);
        free(host_values);
        free(host_records);
        return TTAK_RESULT_ERR_EXECUTION;
    }

    err = hipMemcpy(device_values, host_values, value_bytes, hipMemcpyHostToDevice);
    if (err != hipSuccess) {
        hipFree(device_values);
        hipFree(device_records);
        free(host_values);
        free(host_records);
        return TTAK_RESULT_ERR_EXECUTION;
    }

    uint64_t seed_base = ((uint64_t)guard << 32) ^ (uint64_t)record_count ^ (uint64_t)(uintptr_t)item->input;
    const int threads = 256;
    const int blocks = (int)((record_count + threads - 1) / threads);
    hipLaunchKernelGGL(ttak_hip_factor_kernel, dim3(blocks), dim3(threads), 0, 0,
                       device_values, device_records, record_count, seed_base);
    err = hipDeviceSynchronize();
    if (err != hipSuccess) {
        hipFree(device_values);
        hipFree(device_records);
        free(host_values);
        free(host_records);
        return TTAK_RESULT_ERR_EXECUTION;
    }

    err = hipGetLastError();
    if (err != hipSuccess) {
        hipFree(device_values);
        hipFree(device_records);
        free(host_values);
        free(host_records);
        return TTAK_RESULT_ERR_EXECUTION;
    }

    err = hipMemcpy(host_records,
                    device_records,
                    payload_size,
                    hipMemcpyDeviceToHost);
    hipFree(device_values);
    hipFree(device_records);
    free(host_values);
    if (err != hipSuccess) {
        free(host_records);
        return TTAK_RESULT_ERR_EXECUTION;
    }

    for (size_t idx = 0; idx < record_count; ++idx) {
        ttak_finalize_record(&host_records[idx], checksum_seed, (uint32_t)idx);
        memcpy(item->output + sizeof(ttak_accel_record_prefix_t) +
                   idx * sizeof(ttak_accel_factor_record_t),
               &host_records[idx],
               sizeof(ttak_accel_factor_record_t));
    }

    free(host_records);
    return ttak_finalize_output(item, guard, record_count, checksum_seed);
}

#endif /* ENABLE_ROCM */

extern "C" ttak_result_t ttak_accel_run_rocm(
    const ttak_accel_batch_item_t *items,
    size_t item_count,
    const ttak_accel_config_t *config) {
#ifdef ENABLE_ROCM
    if (items == NULL || config == NULL) {
        return TTAK_RESULT_ERR_ARGUMENT;
    }

    if (!ttak_hip_context_ready()) {
        return ttak_accel_run_cpu(items, item_count, config);
    }

    for (size_t idx = 0; idx < item_count; ++idx) {
        ttak_result_t status = ttak_hip_process_item(&items[idx], config);
        if (status != TTAK_RESULT_OK) {
            return ttak_accel_run_cpu(items + idx, item_count - idx, config);
        }
    }

    return TTAK_RESULT_OK;
#else
    (void)items;
    (void)item_count;
    (void)config;
    return ttak_accel_run_cpu(items, item_count, config);
#endif
}
