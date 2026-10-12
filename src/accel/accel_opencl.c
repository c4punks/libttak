/**
 * @file accel_opencl.c
 * @brief OpenCL acceleration backend for LibTTAK compute kernels.
 *
 * Manages OpenCL context/queue lifetime, compiles kernels on first use,
 * and dispatches bigint multiply and hash workloads to the GPU.
 */

#include "ttak/ttak_accelerator.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern ttak_result_t ttak_accel_run_cpu(
    const ttak_accel_batch_item_t *items,
    size_t item_count,
    const ttak_accel_config_t *config);

#ifdef ENABLE_OPENCL
#include <CL/cl.h>

#define TTAK_STR_HELPER(x) #x
#define TTAK_STR(x) TTAK_STR_HELPER(x)

#define TTAK_ACCEL_FACTOR_MAX 64

static inline uint32_t ttak_guard_word(const ttak_accel_config_t *config,
                                       const ttak_accel_batch_item_t *item) {
    uint32_t guard = config->integrity_mask ^ item->mask_seed;
    guard |= 0x01010101u;
    return guard;
}

static inline uint32_t ttak_checksum_seed(const ttak_accel_batch_item_t *item) {
    return (item->checksum_salt == 0u) ? 2166136261u : item->checksum_salt;
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

    ttak_accel_record_prefix_t prefix = {
        .guard = guard,
        .record_count = (uint32_t)record_count,
        .payload_checksum = payload_checksum,
        .reserved = sizeof(ttak_accel_factor_record_t)
    };
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

static const char *kFactorKernelSrc =
"typedef struct { ulong prime; uint exponent; uint reserved; } accel_slot;\n"
"typedef struct {\n"
"    ulong value;\n"
"    uint factor_count;\n"
"    uint checksum;\n"
"    uint reserved;\n"
"    accel_slot slots[" TTAK_STR(TTAK_ACCEL_FACTOR_MAX) "];\n"
"} accel_record;\n"
"\n"
"__constant uint k_small_primes[168] = {\n"
"    2,3,5,7,11,13,17,19,23,29,31,37,41,43,47,53,59,61,67,71,\n"
"    73,79,83,89,97,101,103,107,109,113,127,131,137,139,149,151,157,163,167,173,\n"
"    179,181,191,193,197,199,211,223,227,229,233,239,241,251,257,263,269,271,277,281,\n"
"    283,293,307,311,313,317,331,337,347,349,353,359,367,373,379,383,389,397,401,409,\n"
"    419,421,431,433,439,443,449,457,461,463,467,479,487,491,499,503,509,521,523,541,\n"
"    547,557,563,569,571,577,587,593,599,601,607,613,617,619,631,641,643,647,653,659,\n"
"    661,673,677,683,691,701,709,719,727,733,739,743,751,757,761,769,773,787,797,809,\n"
"    811,821,823,827,829,839,853,857,859,863,877,881,883,887,907,911,919,929,937,941,\n"
"    947,953,967,971,977,983,991,997\n"
"};\n"
"\n"
"inline ulong ocl_mulmod(ulong a, ulong b, ulong mod) {\n"
"    a %= mod; b %= mod;\n"
"    ulong res = 0;\n"
"    while (a > 0) {\n"
"        if (a & 1UL) res = (res + b) % mod;\n"
"        a >>= 1;\n"
"        b = (b << 1) % mod;\n"
"    }\n"
"    return res;\n"
"}\n"
"\n"
"inline ulong ocl_powmod(ulong base, ulong exp, ulong mod) {\n"
"    ulong result = 1 % mod;\n"
"    ulong x = base % mod;\n"
"    while (exp > 0) {\n"
"        if (exp & 1UL) result = ocl_mulmod(result, x, mod);\n"
"        x = ocl_mulmod(x, x, mod);\n"
"        exp >>= 1;\n"
"    }\n"
"    return result;\n"
"}\n"
"\n"
"inline ulong ocl_gcd(ulong a, ulong b) {\n"
"    while (b != 0) {\n"
"        ulong t = a % b;\n"
"        a = b;\n"
"        b = t;\n"
"    }\n"
"    return a;\n"
"}\n"
"\n"
"inline ulong ocl_abs_diff(ulong a, ulong b) {\n"
"    return (a > b) ? (a - b) : (b - a);\n"
"}\n"
"\n"
"inline ulong ocl_rng_next(ulong *state) {\n"
"    ulong z = (*state += 0x9E3779B97F4A7C15UL);\n"
"    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9UL;\n"
"    z = (z ^ (z >> 27)) * 0x94D049BB133111EBUL;\n"
"    return z ^ (z >> 31);\n"
"}\n"
"\n"
"inline ulong ocl_rng_between(ulong *state, ulong lo, ulong hi) {\n"
"    if (hi <= lo) return lo;\n"
"    ulong span = hi - lo + 1;\n"
"    return lo + (ocl_rng_next(state) % span);\n"
"}\n"
"\n"
"inline int ocl_miller_rabin(ulong n) {\n"
"    if (n < 2) return 0;\n"
"    for (uint i = 0; i < 168; ++i) {\n"
"        ulong p = (ulong)k_small_primes[i];\n"
"        if (n == p) return 1;\n"
"        if (n % p == 0) return 0;\n"
"    }\n"
"    ulong d = n - 1;\n"
"    uint s = 0;\n"
"    while ((d & 1UL) == 0UL) {\n"
"        d >>= 1;\n"
"        ++s;\n"
"    }\n"
"    const ulong bases[6] = {2UL,3UL,5UL,7UL,11UL,13UL};\n"
"    for (uint i = 0; i < 6; ++i) {\n"
"        ulong a = bases[i] % n;\n"
"        if (a == 0) continue;\n"
"        ulong x = ocl_powmod(a, d, n);\n"
"        if (x == 1 || x == n - 1) continue;\n"
"        int witness = 1;\n"
"        for (uint r = 1; r < s; ++r) {\n"
"            x = ocl_mulmod(x, x, n);\n"
"            if (x == n - 1) {\n"
"                witness = 0;\n"
"                break;\n"
"            }\n"
"        }\n"
"        if (witness) return 0;\n"
"    }\n"
"    return 1;\n"
"}\n"
"\n"
"inline ulong ocl_pollard_rho(ulong n, ulong *state) {\n"
"    if ((n & 1UL) == 0UL) return 2UL;\n"
"    ulong c = ocl_rng_between(state, 1, n - 1);\n"
"    ulong y = ocl_rng_between(state, 1, n - 1);\n"
"    ulong m = 128;\n"
"    ulong g = 1;\n"
"    ulong r = 1;\n"
"    ulong q = 1;\n"
"    ulong ys = 0;\n"
"    ulong x = 0;\n"
"    while (g == 1) {\n"
"        x = y;\n"
"        for (ulong i = 0; i < r; ++i) {\n"
"            y = (ocl_mulmod(y, y, n) + c) % n;\n"
"        }\n"
"        ulong k = 0;\n"
"        while (k < r && g == 1) {\n"
"            ys = y;\n"
"            ulong limit = (m < (r - k)) ? m : (r - k);\n"
"            for (ulong i = 0; i < limit; ++i) {\n"
"                y = (ocl_mulmod(y, y, n) + c) % n;\n"
"                ulong diff = ocl_abs_diff(x, y);\n"
"                if (diff == 0) continue;\n"
"                q = ocl_mulmod(q, diff, n);\n"
"            }\n"
"            g = ocl_gcd(q, n);\n"
"            k += limit;\n"
"        }\n"
"        r <<= 1;\n"
"    }\n"
"    if (g == n) {\n"
"        do {\n"
"            ys = (ocl_mulmod(ys, ys, n) + c) % n;\n"
"            ulong diff = ocl_abs_diff(x, ys);\n"
"            g = ocl_gcd(diff, n);\n"
"        } while (g == 1);\n"
"    }\n"
"    return g;\n"
"}\n"
"\n"
"inline int ocl_insert_factor(ulong prime, accel_record *rec) {\n"
"    uint count = rec->factor_count;\n"
"    for (uint i = 0; i < count; ++i) {\n"
"        if (rec->slots[i].prime == prime) {\n"
"            rec->slots[i].exponent++;\n"
"            return 1;\n"
"        }\n"
"        if (rec->slots[i].prime > prime) {\n"
"            if (count >= " TTAK_STR(TTAK_ACCEL_FACTOR_MAX) ") return 0;\n"
"            for (uint j = count; j > i; --j) {\n"
"                rec->slots[j] = rec->slots[j - 1];\n"
"            }\n"
"            rec->slots[i].prime = prime;\n"
"            rec->slots[i].exponent = 1;\n"
"            rec->factor_count++;\n"
"            return 1;\n"
"        }\n"
"    }\n"
"    if (count >= " TTAK_STR(TTAK_ACCEL_FACTOR_MAX) ") return 0;\n"
"    rec->slots[count].prime = prime;\n"
"    rec->slots[count].exponent = 1;\n"
"    rec->factor_count++;\n"
"    return 1;\n"
"}\n"
"\n"
"inline void ocl_factor_number(ulong value, ulong seed, accel_record *rec) {\n"
"    rec->value = value;\n"
"    rec->factor_count = 0;\n"
"    rec->checksum = 0;\n"
"    rec->reserved = 0;\n"
"    for (int i = 0; i < " TTAK_STR(TTAK_ACCEL_FACTOR_MAX) "; ++i) {\n"
"        rec->slots[i].prime = 0UL;\n"
"        rec->slots[i].exponent = 0;\n"
"        rec->slots[i].reserved = 0;\n"
"    }\n"
"    if (value <= 1) return;\n"
"    ulong n = value;\n"
"    for (uint i = 0; i < 168; ++i) {\n"
"        ulong p = (ulong)k_small_primes[i];\n"
"        if (p * p > n) break;\n"
"        while (n % p == 0) {\n"
"            if (!ocl_insert_factor(p, rec)) return;\n"
"            n /= p;\n"
"        }\n"
"    }\n"
"    if (n == 1) return;\n"
"    ulong stack[64];\n"
"    int sp = 0;\n"
"    stack[sp++] = n;\n"
"    while (sp > 0) {\n"
"        ulong cur = stack[--sp];\n"
"        if (cur == 1) continue;\n"
"        if (ocl_miller_rabin(cur)) {\n"
"            ocl_insert_factor(cur, rec);\n"
"            continue;\n"
"        }\n"
"        ulong factor = 0;\n"
"        for (int attempt = 0; attempt < 64; ++attempt) {\n"
"            ulong cand = ocl_pollard_rho(cur, &seed);\n"
"            if (cand > 1 && cand < cur) {\n"
"                factor = cand;\n"
"                break;\n"
"            }\n"
"        }\n"
"        if (factor == 0 || factor == cur) {\n"
"            ocl_insert_factor(cur, rec);\n"
"            continue;\n"
"        }\n"
"        if (sp + 2 <= 64) {\n"
"            stack[sp++] = factor;\n"
"            stack[sp++] = cur / factor;\n"
"        } else {\n"
"            ocl_insert_factor(cur, rec);\n"
"        }\n"
"    }\n"
"}\n"
"\n"
"__kernel void factor_kernel(__global const ulong *values,\n"
"                            __global accel_record *records,\n"
"                            ulong count,\n"
"                            ulong seed_base) {\n"
"    ulong idx = get_global_id(0);\n"
"    if (idx >= count) return;\n"
"    ulong n = values[idx];\n"
"    accel_record rec;\n"
"    ulong seed = seed_base ^ n ^ idx;\n"
"    ocl_factor_number(n, seed, &rec);\n"
"    records[idx] = rec;\n"
"}\n";

typedef struct {
    cl_context context;
    cl_command_queue queue;
    cl_program program;
    cl_kernel kernel;
    cl_device_id device;
    bool ready;
} ttak_opencl_context_t;

static ttak_opencl_context_t g_ocl = {0};
static bool g_ocl_probe_failed = false;

static bool ttak_opencl_mark_failed(void) {
    g_ocl_probe_failed = true;
    return false;
}

static void ttak_opencl_release(void) {
    if (g_ocl.kernel) {
        clReleaseKernel(g_ocl.kernel);
        g_ocl.kernel = NULL;
    }
    if (g_ocl.program) {
        clReleaseProgram(g_ocl.program);
        g_ocl.program = NULL;
    }
    if (g_ocl.queue) {
        clReleaseCommandQueue(g_ocl.queue);
        g_ocl.queue = NULL;
    }
    if (g_ocl.context) {
        clReleaseContext(g_ocl.context);
        g_ocl.context = NULL;
    }
    g_ocl.ready = false;
}

static bool ttak_opencl_build(void) {
    if (g_ocl.ready) return true;
    if (g_ocl_probe_failed) return false;

    cl_int err = CL_SUCCESS;
    cl_uint platform_count = 0;
    if (clGetPlatformIDs(0, NULL, &platform_count) != CL_SUCCESS || platform_count == 0) {
        return ttak_opencl_mark_failed();
    }

    cl_platform_id platform = NULL;
    if (clGetPlatformIDs(1, &platform, NULL) != CL_SUCCESS) {
        return ttak_opencl_mark_failed();
    }

    cl_uint device_count = 0;
    err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &g_ocl.device, &device_count);
    if (err != CL_SUCCESS) {
        err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_CPU, 1, &g_ocl.device, &device_count);
        if (err != CL_SUCCESS) {
            return ttak_opencl_mark_failed();
        }
    }

    g_ocl.context = clCreateContext(NULL, 1, &g_ocl.device, NULL, NULL, &err);
    if (err != CL_SUCCESS || g_ocl.context == NULL) {
        ttak_opencl_release();
        return ttak_opencl_mark_failed();
    }

#if defined(CL_TARGET_OPENCL_VERSION) && (CL_TARGET_OPENCL_VERSION >= 200)
    g_ocl.queue = clCreateCommandQueueWithProperties(g_ocl.context, g_ocl.device, NULL, &err);
#else
    g_ocl.queue = clCreateCommandQueue(g_ocl.context, g_ocl.device, 0, &err);
#endif
    if (err != CL_SUCCESS || g_ocl.queue == NULL) {
        ttak_opencl_release();
        return ttak_opencl_mark_failed();
    }

    const char *src = kFactorKernelSrc;
    size_t len = strlen(kFactorKernelSrc);
    g_ocl.program = clCreateProgramWithSource(g_ocl.context, 1, &src, &len, &err);
    if (err != CL_SUCCESS || g_ocl.program == NULL) {
        ttak_opencl_release();
        return ttak_opencl_mark_failed();
    }

    err = clBuildProgram(g_ocl.program, 1, &g_ocl.device, NULL, NULL, NULL);
    if (err != CL_SUCCESS) {
        ttak_opencl_release();
        return ttak_opencl_mark_failed();
    }

    g_ocl.kernel = clCreateKernel(g_ocl.program, "factor_kernel", &err);
    if (err != CL_SUCCESS || g_ocl.kernel == NULL) {
        ttak_opencl_release();
        return ttak_opencl_mark_failed();
    }

    g_ocl.ready = true;
    return true;
}

static ttak_result_t ttak_opencl_process_item(const ttak_accel_batch_item_t *item,
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

    for (size_t idx = 0; idx < record_count; ++idx) {
        memcpy(&host_values[idx],
               item->input + idx * sizeof(uint64_t),
               sizeof(uint64_t));
    }

    cl_int err = CL_SUCCESS;
    cl_mem values_buf = clCreateBuffer(g_ocl.context,
                                       CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                       value_bytes,
                                       host_values,
                                       &err);
    if (err != CL_SUCCESS || values_buf == NULL) {
        free(host_values);
        free(host_records);
        return TTAK_RESULT_ERR_EXECUTION;
    }

    cl_mem records_buf = clCreateBuffer(g_ocl.context,
                                        CL_MEM_WRITE_ONLY,
                                        payload_size,
                                        NULL,
                                        &err);
    if (err != CL_SUCCESS || records_buf == NULL) {
        clReleaseMemObject(values_buf);
        free(host_values);
        free(host_records);
        return TTAK_RESULT_ERR_EXECUTION;
    }

    err = clSetKernelArg(g_ocl.kernel, 0, sizeof(cl_mem), &values_buf);
    err |= clSetKernelArg(g_ocl.kernel, 1, sizeof(cl_mem), &records_buf);
    cl_ulong count_arg = (cl_ulong)record_count;
    err |= clSetKernelArg(g_ocl.kernel, 2, sizeof(cl_ulong), &count_arg);
    cl_ulong seed_base = ((cl_ulong)guard << 32) ^ (cl_ulong)record_count ^ (cl_ulong)(uintptr_t)item->input;
    err |= clSetKernelArg(g_ocl.kernel, 3, sizeof(cl_ulong), &seed_base);
    if (err != CL_SUCCESS) {
        clReleaseMemObject(values_buf);
        clReleaseMemObject(records_buf);
        free(host_values);
        free(host_records);
        return TTAK_RESULT_ERR_EXECUTION;
    }

    size_t local = 64;
    size_t global = ((record_count + local - 1) / local) * local;
    err = clEnqueueNDRangeKernel(g_ocl.queue, g_ocl.kernel, 1, NULL, &global, &local, 0, NULL, NULL);
    if (err != CL_SUCCESS) {
        clReleaseMemObject(values_buf);
        clReleaseMemObject(records_buf);
        free(host_values);
        free(host_records);
        return TTAK_RESULT_ERR_EXECUTION;
    }

    err = clFinish(g_ocl.queue);
    if (err != CL_SUCCESS) {
        clReleaseMemObject(values_buf);
        clReleaseMemObject(records_buf);
        free(host_values);
        free(host_records);
        return TTAK_RESULT_ERR_EXECUTION;
    }

    err = clEnqueueReadBuffer(g_ocl.queue,
                              records_buf,
                              CL_TRUE,
                              0,
                              payload_size,
                              host_records,
                              0,
                              NULL,
                              NULL);
    clReleaseMemObject(values_buf);
    clReleaseMemObject(records_buf);
    free(host_values);
    if (err != CL_SUCCESS) {
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

static ttak_result_t ttak_opencl_dispatch(const ttak_accel_batch_item_t *items,
                                          size_t item_count,
                                          const ttak_accel_config_t *config) {
    if (!ttak_opencl_build()) {
        return TTAK_RESULT_ERR_EXECUTION;
    }

    for (size_t idx = 0; idx < item_count; ++idx) {
        ttak_result_t status = ttak_opencl_process_item(&items[idx], config);
        if (status != TTAK_RESULT_OK) {
            return ttak_accel_run_cpu(items + idx, item_count - idx, config);
        }
    }
    return TTAK_RESULT_OK;
}

ttak_result_t ttak_accel_run_opencl(
    const ttak_accel_batch_item_t *items,
    size_t item_count,
    const ttak_accel_config_t *config) {
    if (items == NULL || config == NULL) {
        return TTAK_RESULT_ERR_ARGUMENT;
    }

    ttak_result_t status = ttak_opencl_dispatch(items, item_count, config);
    if (status != TTAK_RESULT_OK) {
        return ttak_accel_run_cpu(items, item_count, config);
    }
    return status;
}

#else

ttak_result_t ttak_accel_run_opencl(
    const ttak_accel_batch_item_t *items,
    size_t item_count,
    const ttak_accel_config_t *config) {
    return ttak_accel_run_cpu(items, item_count, config);
}

#endif
