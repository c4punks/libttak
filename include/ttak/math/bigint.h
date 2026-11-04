#ifndef TTAK_MATH_BIGINT_H
#define TTAK_MATH_BIGINT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdalign.h>
#include <ttak/types/ttak_align.h>
#include <ttak/types/fixed.h>

/**
 * @brief Platform-optimized word type for Raspberry Pi (ARMv7/v8).
 */
typedef uint32_t limb_t;

#define TTAK_BIGINT_SSO_LIMIT 4 // 4 * 32-bit = 128-bit

/**
 * @brief Arbitrary-precision integer engine with Small Stack Optimization (SSO).
 */
typedef struct ttak_bigint {
    size_t  capacity;
    size_t  used;
    bool    is_negative;
    bool    is_dynamic;
    union {
        limb_t *dyn_ptr;
        limb_t sso_buf[TTAK_BIGINT_SSO_LIMIT];
    } data;
#ifndef _MSC_VER
    alignas(alignof(ttak_max_align_t)) char padding[0];
#endif
} ttak_bigint_t;

typedef ttak_bigint_t __tt_big_i_t;

/**
 * @brief Initializes a bigint.
 */
void ttak_bigint_init(ttak_bigint_t *bi, uint64_t now);
void ttak_bigint_init_u64(ttak_bigint_t *bi, uint64_t value, uint64_t now);
void ttak_bigint_init_copy(ttak_bigint_t *dst, const ttak_bigint_t *src, uint64_t now);


/**
 * @brief Performs modulo operation optimized for Mersenne primes (2^p - 1).
 */
void ttak_bigint_mersenne_mod(ttak_bigint_t *bi, int p, uint64_t now);

/**
 * @brief Frees the bigint resources.
 */
void ttak_bigint_free(ttak_bigint_t *bi, uint64_t now);

/**
 * @brief Assigns an unsigned 64-bit value to the bigint.
 *
 * @return true on success, false on allocation failure.
 */
bool ttak_bigint_set_u64(ttak_bigint_t *bi, uint64_t value, uint64_t now);
bool ttak_bigint_set_u128(ttak_bigint_t *bi, ttak_u128_t value, uint64_t now);
bool ttak_bigint_set_u256(ttak_bigint_t *bi, ttak_u256_t value, uint64_t now);
bool ttak_bigint_copy(ttak_bigint_t *dst, const ttak_bigint_t *src, uint64_t now);

int ttak_bigint_cmp(const ttak_bigint_t *lhs, const ttak_bigint_t *rhs);
int ttak_bigint_cmp_u64(const ttak_bigint_t *lhs, uint64_t rhs);
bool ttak_bigint_is_zero(const ttak_bigint_t *bi);
limb_t sub_limbs(limb_t *u, const limb_t *v, size_t n);

/**
 * @brief Adds two non-negative bigints: dst = lhs + rhs.
 *
 * The destination must be initialized. Sign handling is limited to positive inputs.
 *
 * @return true on success, false on allocation failure.
 */
bool ttak_bigint_add(ttak_bigint_t *dst, const ttak_bigint_t *lhs, const ttak_bigint_t *rhs, uint64_t now);
bool ttak_bigint_sub(ttak_bigint_t *dst, const ttak_bigint_t *lhs, const ttak_bigint_t *rhs, uint64_t now);
bool ttak_bigint_mul(ttak_bigint_t *dst, const ttak_bigint_t *lhs, const ttak_bigint_t *rhs, uint64_t now);
bool ttak_bigint_div(ttak_bigint_t *q, ttak_bigint_t *r, const ttak_bigint_t *n, const ttak_bigint_t *d, uint64_t now);
bool ttak_bigint_mod(ttak_bigint_t *r, const ttak_bigint_t *n, const ttak_bigint_t *d, uint64_t now);

bool ttak_bigint_add_u64(ttak_bigint_t *dst, const ttak_bigint_t *lhs, uint64_t rhs, uint64_t now);
bool ttak_bigint_mul_u64(ttak_bigint_t *dst, const ttak_bigint_t *lhs, uint64_t rhs, uint64_t now);
bool ttak_bigint_div_u64(ttak_bigint_t *q, ttak_bigint_t *r, const ttak_bigint_t *n, uint64_t d, uint64_t now);
bool ttak_bigint_mod_u64(ttak_bigint_t *r, const ttak_bigint_t *n, uint64_t d, uint64_t now);

size_t ttak_bigint_get_bit_length(const ttak_bigint_t *bi);
char* ttak_bigint_to_string(const ttak_bigint_t *bi, uint64_t now);
bool ttak_bigint_export_u64(const ttak_bigint_t *bi, uint64_t *value_out);
bool ttak_bigint_export_u128(const ttak_bigint_t *bi, ttak_u128_t *value_out);
bool ttak_bigint_export_u256(const ttak_bigint_t *bi, ttak_u256_t *value_out);
void ttak_bigint_to_hex_hash(const ttak_bigint_t *bi, char out[65]);
void ttak_bigint_format_prefix(const ttak_bigint_t *bi, char *dest, size_t dest_cap);
void ttak_bigint_hash(const ttak_bigint_t *bi, uint8_t out[32]);


#endif // TTAK_MATH_BIGINT_H
