#include <ttak/math/ntt.h>
#include <ttak/ttak_accelerator.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

const ttak_ntt_prime_t ttak_ntt_primes[TTAK_NTT_PRIME_COUNT] = {
    { 998244353ULL, 3ULL, 23U, 17450252288407896063ULL, 299560064ULL },
    { 1004535809ULL, 3ULL, 21U, 8214279848305098751ULL, 742115580ULL },
    { 469762049ULL, 3ULL, 26U, 18226067692438159359ULL, 118963808ULL }
};

/**
 * @brief Forward declarations for GPU-accelerated NTT backends.
 */
#ifdef ENABLE_CUDA
extern ttak_result_t ttak_accel_ntt_cuda(uint64_t *data, size_t n, const ttak_ntt_prime_t *prime, bool inverse);
#endif

#ifdef ENABLE_ROCM
extern ttak_result_t ttak_accel_ntt_rocm(uint64_t *data, size_t n, const ttak_ntt_prime_t *prime, bool inverse);
#endif

#ifdef ENABLE_OPENCL
extern ttak_result_t ttak_accel_ntt_opencl(uint64_t *data, size_t n, const ttak_ntt_prime_t *prime, bool inverse);
#endif

/**
 * @brief Compute (a + b) mod mod without overflow.
 *
 * @param a   Left operand.
 * @param b   Right operand.
 * @param mod Prime modulus.
 * @return Modular sum.
 */
uint64_t ttak_mod_add(uint64_t a, uint64_t b, uint64_t mod) {
    uint64_t sum = a + b;
    if (sum >= mod || sum < a) {
        sum -= mod;
    }
    return sum;
}

/**
 * @brief Compute (a - b) mod mod, wrapping into the range [0, mod).
 *
 * @param a   Left operand.
 * @param b   Right operand.
 * @param mod Prime modulus.
 * @return Modular difference.
 */
uint64_t ttak_mod_sub(uint64_t a, uint64_t b, uint64_t mod) {
    return (a >= b) ? (a - b) : (a + mod - b);
}

/**
 * @brief Multiply two residues modulo mod using 128-bit intermediates.
 *
 * @param a   Left operand.
 * @param b   Right operand.
 * @param mod Prime modulus.
 * @return Modular product.
 */
uint64_t ttak_mod_mul(uint64_t a, uint64_t b, uint64_t mod) {
    ttak_u128_t prod = ttak_u128_mul64(a, b);
    return ttak_u128_mod_u64(prod, mod);
}

/**
 * @brief Exponentiate a base modulo mod using square-and-multiply.
 *
 * @param base Base residue.
 * @param exp  Exponent.
 * @param mod  Prime modulus.
 * @return base^exp mod mod.
 */
uint64_t ttak_mod_pow(uint64_t base, uint64_t exp, uint64_t mod) {
    uint64_t result = 1 % mod;
    uint64_t factor = base % mod;
    while (exp) {
        if (exp & 1ULL) {
            result = ttak_mod_mul(result, factor, mod);
        }
        factor = ttak_mod_mul(factor, factor, mod);
        exp >>= 1ULL;
    }
    return result;
}

/**
 * @brief Compute the modular inverse using the extended Euclidean algorithm.
 *
 * Computes multiplicative inverse modulo mod for residue reduction in NTT and BigInt operations.
 *
 * @param value Input residue.
 * @param mod   Modulus.
 * @return Multiplicative inverse or 0 if none exists.
 */
uint64_t ttak_mod_inverse(uint64_t value, uint64_t mod) {
    if (mod == 0) return 0;
    if (mod == 1) return 0;
    
    int64_t m0 = (int64_t)mod;
    int64_t t, q;
    int64_t x0 = 0, x1 = 1;
    int64_t a = (int64_t)(value % mod);

    if (m0 == 1) return 0;

    /* Extended Euclidean algorithm loop */
    while (a > 1) {
        if (m0 == 0) return 0; // Should not happen with mod > 1
        q = a / m0;
        t = m0;

        /* m0 is the next divisor, a is the remainder */
        m0 = a % m0;
        a = t;
        t = x0;

        x0 = x1 - q * x0;
        x1 = t;
    }

    if (x1 < 0) x1 += (int64_t)mod;
    return (uint64_t)x1;
}

/**
 * @brief Reduce a 128-bit product using Montgomery arithmetic.
 *
 * @param value Intermediate product.
 * @param prime Prime parameters (including Montgomery constants).
 * @return Reduced residue.
 */
uint64_t ttak_montgomery_reduce(ttak_u128_t value, const ttak_ntt_prime_t *prime) {
    uint64_t m = ttak_u64_mul_lo(value.lo, prime->montgomery_inv);
    ttak_u128_t m_mod = ttak_u128_mul64(prime->modulus, m);
    ttak_u128_t sum = ttak_u128_add(value, m_mod);
    uint64_t result = sum.hi;
    if (result >= prime->modulus) result -= prime->modulus;
    return result;
}

/**
 * @brief Multiply two residues in Montgomery space.
 *
 * @param lhs   Left operand (Montgomery form).
 * @param rhs   Right operand (Montgomery form).
 * @param prime Prime definition.
 * @return Product in Montgomery form.
 */
uint64_t ttak_montgomery_mul(uint64_t lhs, uint64_t rhs, const ttak_ntt_prime_t *prime) {
    ttak_u128_t value = ttak_u128_mul64(lhs, rhs);
    return ttak_montgomery_reduce(value, prime);
}

/**
 * @brief Choose lattice tile size for the current butterfly width.
 */
static inline size_t ttak_ntt_lattice_dim(size_t len) {
    if (len >= 64) return 32;
    if (len >= 32) return 16;
    if (len >= 16) return 8;
    if (len >= 8)  return 4;
    if (len >= 4)  return 2;
    return 1;
}

static inline size_t ttak_ntt_lattice_shift(size_t dim) {
#if defined(__GNUC__) || defined(__clang__)
    return (size_t)__builtin_ctzll(dim);
#else
    size_t shift = 0;
    while ((1ULL << shift) < dim) ++shift;
    return shift;
#endif
}

/**
 * @brief Convert a standard residue into Montgomery representation.
 *
 * @param value Value in standard form.
 * @param prime Prime definition with R^2 precomputed.
 * @return Value represented in Montgomery space.
 */
uint64_t ttak_montgomery_convert(uint64_t value, const ttak_ntt_prime_t *prime) {
    uint64_t v = value % prime->modulus;
    ttak_u128_t converted = ttak_u128_mul64(v, prime->montgomery_r2);
    return ttak_montgomery_reduce(converted, prime);
}

/**
 * @brief Reorder the array into bit-reversed order.
 *
 * @param data Data array to permute.
 * @param n    Length of the array (power of two).
 */
static void bit_reverse(uint64_t *data, size_t n) {
    size_t j = 0;
    for (size_t i = 1; i < n; ++i) {
        size_t bit = n >> 1;
        while (j & bit) {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;
        if (i < j) {
            uint64_t tmp = data[i];
            data[i] = data[j];
            data[j] = tmp;
        }
    }
}

/**
 * @brief Convert every element into Montgomery space.
 *
 * @param data  Array of residues.
 * @param n     Number of elements.
 * @param prime Prime modulus descriptor.
 */
static void montgomery_array_convert(uint64_t *data, size_t n, const ttak_ntt_prime_t *prime) {
    for (size_t i = 0; i < n; ++i) {
        data[i] = ttak_montgomery_convert(data[i], prime);
    }
}

/**
 * @brief Convert every element from Montgomery space back to standard form.
 *
 * @param data  Array of residues.
 * @param n     Number of elements.
 * @param prime Prime modulus descriptor.
 */
static void montgomery_array_restore(uint64_t *data, size_t n, const ttak_ntt_prime_t *prime) {
    for (size_t i = 0; i < n; ++i) {
        ttak_u128_t value = ttak_u128_make(0, data[i]);
        data[i] = ttak_montgomery_reduce(value, prime);
    }
}

/**
 * @brief Perform an in-place NTT or inverse NTT over the provided data.
 *
 * @param data   Array of residues to transform (length must be power of two).
 * @param n      Number of elements.
 * @param prime  Prime modulus descriptor.
 * @param inverse Set to true for inverse transform.
 * @return true on success, false if parameters are invalid.
 */
_Bool ttak_ntt_transform(uint64_t *data, size_t n, const ttak_ntt_prime_t *prime, _Bool inverse) {
    if (!data || !prime || n == 0) return false;
    if ((n & (n - 1)) != 0) return false;
    if (n > (size_t)(1ULL << prime->max_power_two)) return false;

    // GPU Acceleration Dispatch for large transforms (N >= 4096)
    if (n >= 4096) {
#ifdef ENABLE_CUDA
        if (ttak_accel_ntt_cuda(data, n, prime, inverse) == TTAK_RESULT_OK) {
            return true;
        }
#endif
#ifdef ENABLE_ROCM
        if (ttak_accel_ntt_rocm(data, n, prime, inverse) == TTAK_RESULT_OK) {
            return true;
        }
#endif
#ifdef ENABLE_OPENCL
        if (ttak_accel_ntt_opencl(data, n, prime, inverse) == TTAK_RESULT_OK) {
            return true;
        }
#endif
        // Fall back to CPU if no GPU available or if execution failed.
    }

    uint64_t modulus = prime->modulus;

    bit_reverse(data, n);
    montgomery_array_convert(data, n, prime);

    // Precompute twiddle factors on host to avoid mod_pow inside loops
    uint64_t *twiddle = malloc(sizeof(uint64_t) * n);
    if (!twiddle) return false;

    uint64_t root = ttak_mod_pow(prime->primitive_root, (prime->modulus - 1) / n, modulus);
    if (inverse) {
        root = ttak_mod_inverse(root, modulus);
    }
    uint64_t root_mont = ttak_montgomery_convert(root, prime);
    uint64_t curr = ttak_montgomery_convert(1ULL, prime);
    for (size_t i = 0; i < n; ++i) {
        twiddle[i] = curr;
        curr = ttak_montgomery_mul(curr, root_mont, prime);
    }

    // Tiled/Hierarchical mixed Lattice NTT structure
    for (size_t len = 1; len < n; len <<= 1) {
        size_t step = len << 1;
        size_t twiddle_stride = n / step;
        size_t lattice_dim = ttak_ntt_lattice_dim(len);
        size_t lattice_shift = ttak_ntt_lattice_shift(lattice_dim);
        size_t lattice_rows = len >> lattice_shift;

        for (size_t i = 0; i < n; i += step) {
            for (size_t row = 0; row < lattice_rows; ++row) {
                size_t row_base = row << lattice_shift;
                for (size_t lane = 0; lane < lattice_dim; ++lane) {
                    size_t j = row_base | lane;
                    uint64_t w = twiddle[j * twiddle_stride];

                    uint64_t u = data[i + j];
                    uint64_t v = ttak_montgomery_mul(data[i + j + len], w, prime);

                    data[i + j] = ttak_mod_add(u, v, modulus);
                    data[i + j + len] = ttak_mod_sub(u, v, modulus);
                }
            }
        }
    }

    free(twiddle);

    if (inverse) {
        uint64_t inv_n = ttak_mod_inverse((uint64_t)n % modulus, modulus);
        uint64_t inv_n_mont = ttak_montgomery_convert(inv_n, prime);
        for (size_t i = 0; i < n; ++i) {
            data[i] = ttak_montgomery_mul(data[i], inv_n_mont, prime);
        }
    }

    montgomery_array_restore(data, n, prime);
    return true;
}

/**
 * @brief Multiply two transformed sequences element-wise.
 *
 * @param dst   Destination array.
 * @param lhs   Left operand.
 * @param rhs   Right operand.
 * @param n     Number of elements.
 * @param prime Prime modulus descriptor.
 */
void ttak_ntt_pointwise_mul(uint64_t *dst, const uint64_t *lhs, const uint64_t *rhs, size_t n, const ttak_ntt_prime_t *prime) {
    uint64_t mod = prime->modulus;
    for (size_t i = 0; i < n; ++i) {
        dst[i] = ttak_mod_mul(lhs[i], rhs[i], mod);
    }
}

/**
 * @brief Square each element of a transformed sequence.
 *
 * @param dst   Destination array.
 * @param src   Source array.
 * @param n     Number of elements.
 * @param prime Prime modulus descriptor.
 */
void ttak_ntt_pointwise_square(uint64_t *dst, const uint64_t *src, size_t n, const ttak_ntt_prime_t *prime) {
    uint64_t mod = prime->modulus;
    for (size_t i = 0; i < n; ++i) {
        dst[i] = ttak_mod_mul(src[i], src[i], mod);
    }
}

/**
 * @brief Round up to the next power of two.
 *
 * @param value Input value.
 * @return Next power of two >= value.
 */
size_t ttak_next_power_of_two(size_t value) {
    if (value == 0) return 1;
    value--;
    value |= value >> 1;
    value |= value >> 2;
    value |= value >> 4;
    value |= value >> 8;
    value |= value >> 16;
#if UINTPTR_MAX > 0xFFFFFFFFu
    value |= value >> 32;
#endif
    return value + 1;
}

static uint64_t mod128_u64(ttak_u128_t value, uint64_t mod) {
    return ttak_u128_mod_u64(value, mod);
}

static bool ttak_u128_mul_u64_checked(ttak_u128_t value, uint64_t factor, ttak_u128_t *out) {
    bool overflow = false;
    ttak_u128_t tmp = ttak_u128_mul_u64_wide(value, factor, &overflow);
    if (overflow) return false;
    if (out) *out = tmp;
    return true;
}

/**
 * @brief Combine residues via the Chinese Remainder Theorem.
 *
 * @param terms       Array of residue/modulus pairs.
 * @param count       Number of terms.
 * @param residue_out Resulting residue in mixed-radix form.
 * @param modulus_out Combined modulus.
 * @return true on success, false if inversion fails.
 */
_Bool ttak_crt_combine(const ttak_crt_term_t *terms, size_t count, ttak_u128_t *residue_out, ttak_u128_t *modulus_out) {
    if (!terms || count == 0 || !residue_out || !modulus_out) return false;

    ttak_u128_t result = ttak_u128_from_u64(terms[0].residue % terms[0].modulus);
    ttak_u128_t modulus = ttak_u128_from_u64(terms[0].modulus);

    for (size_t i = 1; i < count; ++i) {
        uint64_t mod_i = terms[i].modulus;
        uint64_t residue_i = terms[i].residue % mod_i;
        uint64_t current_mod = mod128_u64(modulus, mod_i);
        uint64_t inverse = ttak_mod_inverse(current_mod, mod_i);
        if (inverse == 0) return false;

        uint64_t current_residue = mod128_u64(result, mod_i);
        uint64_t delta = ttak_mod_sub(residue_i, current_residue, mod_i);
        uint64_t k = ttak_mod_mul(delta, inverse, mod_i);

        ttak_u128_t step;
        if (!ttak_u128_mul_u64_checked(modulus, k, &step)) return false;
        if (ttak_u128_add_overflow(result, step, &result)) return false;
        if (!ttak_u128_mul_u64_checked(modulus, mod_i, &modulus)) return false;
    }

    *residue_out = result;
    *modulus_out = modulus;
    return true;
}
