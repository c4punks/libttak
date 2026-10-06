#include <ttak/math/matrix.h>
#include <ttak/mem/mem.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/**
 * Matrix-vector lane multiplication using independent lane processing.
 * Each variable lane is processed independently to maximize throughput.
 */
static void ttak_math_lane_mul(ttak_bigreal_t *res, const ttak_bigreal_t *mat_elements, const ttak_vector_t *vec, uint8_t cols, uint64_t now) {
    ttak_bigreal_t sum, prod;
    ttak_bigreal_init_u64(&sum, 0, now);
    ttak_bigreal_init(&prod, now);
    
    for (uint8_t j = 0; j < cols; j++) {
        /* Each variable lane is processed independently */
        ttak_bigreal_mul(&prod, &mat_elements[j], &vec->elements[j], now);
        ttak_bigreal_add(&sum, &sum, &prod, now);
    }
    ttak_bigreal_copy(res, &sum, now);
    
    ttak_bigreal_free(&sum, now);
    ttak_bigreal_free(&prod, now);
}

static void matrix_payload_cleanup(void *data) {
    if (!data) return;
    ttak_matrix_t *m = (ttak_matrix_t *)data;
    for (int i = 0; i < 16; i++) {
        ttak_bigreal_free(&m->elements[i], 0);
    }
}

tt_shared_matrix_t* ttak_matrix_create(uint8_t rows, uint8_t cols, tt_owner_t *owner, uint64_t now) {
    if (rows > 4 || cols > 4) return NULL;
    
    tt_shared_matrix_t *sm = malloc(sizeof(tt_shared_matrix_t));
    if (!sm) return NULL;
    
    ttak_shared_init(&sm->base);
    sm->base.cleanup = matrix_payload_cleanup;
    
    ttak_shared_result_t res = sm->base.allocate_typed(&sm->base, sizeof(ttak_matrix_t), "ttak_matrix_t", TTAK_SHARED_LEVEL_3);
    if (res != TTAK_OWNER_SUCCESS) {
        free(sm);
        return NULL;
    }
    
    sm->base.add_owner(&sm->base, owner);
    
    ttak_matrix_t *m = (ttak_matrix_t *)sm->base.access(&sm->base, owner, &res);
    if (!m) {
        ttak_shared_destroy(&sm->base);
        free(sm);
        return NULL;
    }
    
    m->rows = rows;
    m->cols = cols;
    for (int i = 0; i < 16; i++) {
        ttak_bigreal_init(&m->elements[i], now);
    }
    sm->base.release(&sm->base);
    
    return sm;
}

ttak_bigreal_t* ttak_matrix_get(tt_shared_matrix_t *sm, tt_owner_t *owner, uint8_t row, uint8_t col, uint64_t now) {
    if (!sm || !owner) return NULL;
    (void)now;
    
    ttak_shared_result_t res;
    ttak_matrix_t *m = (ttak_matrix_t *)sm->base.access(&sm->base, owner, &res);
    if (!m) return NULL;
    
    if (row >= m->rows || col >= m->cols) {
        sm->base.release(&sm->base);
        return NULL;
    }
    
    return &m->elements[row * m->cols + col];
}

_Bool ttak_matrix_set(tt_shared_matrix_t *sm, tt_owner_t *owner, uint8_t row, uint8_t col, const ttak_bigreal_t *val, uint64_t now) {
    if (!sm || !owner || !val) return false;
    
    ttak_shared_result_t res;
    ttak_matrix_t *m = (ttak_matrix_t *)sm->base.access(&sm->base, owner, &res);
    if (!m) return false;
    
    if (row >= m->rows || col >= m->cols) {
        sm->base.release(&sm->base);
        return false;
    }
    
    _Bool ok = ttak_bigreal_copy(&m->elements[row * m->cols + col], val, now);
    sm->base.release(&sm->base);
    return ok;
}

_Bool ttak_matrix_multiply_vec(tt_shared_vector_t *res, tt_shared_matrix_t *m, tt_shared_vector_t *v, tt_owner_t *owner, uint64_t now) {
    ttak_shared_result_t rm, rv, rr;
    ttak_matrix_t *mat = (ttak_matrix_t *)m->base.access(&m->base, owner, &rm);
    ttak_vector_t *vec = (ttak_vector_t *)v->base.access(&v->base, owner, &rv);
    ttak_vector_t *v_res = (ttak_vector_t *)res->base.access(&res->base, owner, &rr);
    
    if (!mat || !vec || !v_res || mat->cols != vec->dim || mat->rows != v_res->dim) {
        if (mat) m->base.release(&m->base);
        if (vec) v->base.release(&v->base);
        if (v_res) res->base.release(&res->base);
        return false;
    }
    
    for (uint8_t i = 0; i < mat->rows; i++) {
        /* Independent lane processing for matrix-vector multiply */
        ttak_math_lane_mul(&v_res->elements[i], &mat->elements[i * mat->cols], vec, mat->cols, now);
    }
    
    m->base.release(&m->base);
    v->base.release(&v->base);
    res->base.release(&res->base);
    
    return true;
}

_Bool ttak_matrix_multiply(tt_shared_matrix_t *res, tt_shared_matrix_t *a, tt_shared_matrix_t *b, tt_owner_t *owner, uint64_t now) {
    ttak_shared_result_t ra, rb, rr;
    ttak_matrix_t *ma = (ttak_matrix_t *)a->base.access(&a->base, owner, &ra);
    ttak_matrix_t *mb = (ttak_matrix_t *)b->base.access(&b->base, owner, &rb);
    ttak_matrix_t *mr = (ttak_matrix_t *)res->base.access(&res->base, owner, &rr);
    
    if (!ma || !mb || !mr || ma->cols != mb->rows || mr->rows != ma->rows || mr->cols != mb->cols) {
        if (ma) a->base.release(&a->base);
        if (mb) b->base.release(&b->base);
        if (mr) res->base.release(&res->base);
        return false;
    }
    
    ttak_bigreal_t sum, prod;
    ttak_bigreal_init(&sum, now);
    ttak_bigreal_init(&prod, now);
    
    for (uint8_t i = 0; i < ma->rows; i++) {
        for (uint8_t j = 0; j < mb->cols; j++) {
            ttak_bigreal_init_u64(&sum, 0, now);
            for (uint8_t k = 0; k < ma->cols; k++) {
                ttak_bigreal_mul(&prod, &ma->elements[i * ma->cols + k], &mb->elements[k * mb->cols + j], now);
                ttak_bigreal_add(&sum, &sum, &prod, now);
            }
            ttak_bigreal_copy(&mr->elements[i * mr->cols + j], &sum, now);
        }
    }
    
    ttak_bigreal_free(&sum, now);
    ttak_bigreal_free(&prod, now);
    
    a->base.release(&a->base);
    b->base.release(&b->base);
    res->base.release(&res->base);
    
    return true;
}

_Bool ttak_matrix_set_rotation(tt_shared_matrix_t *m, tt_owner_t *owner, uint8_t axis, const ttak_bigreal_t *angle, uint64_t now) {
    ttak_shared_result_t res;
    ttak_matrix_t *mat = (ttak_matrix_t *)m->base.access(&m->base, owner, &res);
    if (!mat) return false;

    /* Initialize to identity */
    for (int i = 0; i < 16; i++) {
        ttak_bigint_set_u64(&mat->elements[i].mantissa, 0, now);
        mat->elements[i].exponent = 0;
    }
    for (int i = 0; i < 4; i++) {
        ttak_bigint_set_u64(&mat->elements[i * 4 + i].mantissa, 1, now);
        mat->elements[i * 4 + i].exponent = 0;
    }

    ttak_bigreal_t s, c;
    ttak_bigreal_init(&s, now);
    ttak_bigreal_init(&c, now);

    /* Compute sine and cosine approximations for stable rotation entries */
    ttak_math_approx_sin(&s, angle, now);
    ttak_math_approx_cos(&c, angle, now);

    if (axis == 0) { /* X-axis */
        ttak_bigreal_copy(&mat->elements[5], &c, now);
        ttak_bigreal_copy(&mat->elements[6], &s, now);
        mat->elements[6].mantissa.is_negative = !mat->elements[6].mantissa.is_negative;
        ttak_bigreal_copy(&mat->elements[9], &s, now);
        ttak_bigreal_copy(&mat->elements[10], &c, now);
    } else if (axis == 1) { /* Y-axis */
        ttak_bigreal_copy(&mat->elements[0], &c, now);
        ttak_bigreal_copy(&mat->elements[2], &s, now);
        ttak_bigreal_copy(&mat->elements[8], &s, now);
        mat->elements[8].mantissa.is_negative = !mat->elements[8].mantissa.is_negative;
        ttak_bigreal_copy(&mat->elements[10], &c, now);
    } else { /* Z-axis */
        ttak_bigreal_copy(&mat->elements[0], &c, now);
        ttak_bigreal_copy(&mat->elements[1], &s, now);
        mat->elements[1].mantissa.is_negative = !mat->elements[1].mantissa.is_negative;
        ttak_bigreal_copy(&mat->elements[4], &s, now);
        ttak_bigreal_copy(&mat->elements[5], &c, now);
    }

    ttak_bigreal_free(&s, now);
    ttak_bigreal_free(&c, now);
    m->base.release(&m->base);
    return true;
}

_Bool ttak_matrix_set_shearing(tt_shared_matrix_t *m, tt_owner_t *owner, uint8_t axis, const ttak_bigreal_t *factor, uint64_t now) {
    if (!m || !owner || !factor) return false;
    ttak_shared_result_t res;
    ttak_matrix_t *mat = (ttak_matrix_t *)m->base.access(&m->base, owner, &res);
    if (!mat) return false;

    // Reset to identity
    for (int i = 0; i < 16; i++) {
        ttak_bigint_set_u64(&mat->elements[i].mantissa, 0, now);
        mat->elements[i].exponent = 0;
    }
    for (int i = 0; i < mat->rows && i < mat->cols; i++) {
        ttak_bigint_set_u64(&mat->elements[i * mat->cols + i].mantissa, 1, now);
        mat->elements[i * mat->cols + i].exponent = 0;
    }

    if (mat->rows >= 2 && mat->cols >= 2) {
        if (axis == 0) {
            // Shearing X along Y: row 0, col 1 = factor
            ttak_bigreal_copy(&mat->elements[0 * mat->cols + 1], factor, now);
        } else if (axis == 1) {
            // Shearing Y along X: row 1, col 0 = factor
            ttak_bigreal_copy(&mat->elements[1 * mat->cols + 0], factor, now);
        }
    }

    m->base.release(&m->base);
    return true;
}

_Bool ttak_matrix_determinant(ttak_bigreal_t *det, tt_shared_matrix_t *m, tt_owner_t *owner, uint64_t now) {
    if (!det || !m || !owner) return false;
    ttak_shared_result_t res;
    ttak_matrix_t *mat = (ttak_matrix_t *)m->base.access(&m->base, owner, &res);
    if (!mat || mat->rows != mat->cols) {
        if (mat) m->base.release(&m->base);
        return false;
    }

    uint8_t n = mat->rows;
    if (n == 1) {
        ttak_bigreal_copy(det, &mat->elements[0], now);
        m->base.release(&m->base);
        return true;
    }
    if (n == 2) {
        // det = a*d - b*c
        ttak_bigreal_t ad, bc;
        ttak_bigreal_init(&ad, now);
        ttak_bigreal_init(&bc, now);
        _Bool ok = ttak_bigreal_mul(&ad, &mat->elements[0], &mat->elements[3], now) &&
                   ttak_bigreal_mul(&bc, &mat->elements[1], &mat->elements[2], now) &&
                   ttak_bigreal_sub(det, &ad, &bc, now);
        ttak_bigreal_free(&ad, now);
        ttak_bigreal_free(&bc, now);
        m->base.release(&m->base);
        return ok;
    }

    // Copy matrix to a local working array of size n x n
    ttak_bigreal_t a[4][4];
    for (uint8_t i = 0; i < n; i++) {
        for (uint8_t j = 0; j < n; j++) {
            ttak_bigreal_init(&a[i][j], now);
            ttak_bigreal_copy(&a[i][j], &mat->elements[i * n + j], now);
        }
    }

    _Bool ok = true;
    int sign = 1;
    ttak_bigreal_t det_val, zero, factor, tmp, prod;
    ttak_bigreal_init(&det_val, now);
    ttak_bigreal_init_u64(&det_val, 1, now);
    ttak_bigreal_init(&zero, now);
    ttak_bigreal_init_u64(&zero, 0, now);
    ttak_bigreal_init(&factor, now);
    ttak_bigreal_init(&tmp, now);
    ttak_bigreal_init(&prod, now);

    for (uint8_t i = 0; i < n; i++) {
        // Find pivot
        int pivot = -1;
        for (uint8_t r = i; r < n; r++) {
            if (ttak_bigreal_cmp(&a[r][i], &zero, now) != 0) {
                pivot = r;
                break;
            }
        }
        if (pivot == -1) {
            ttak_bigreal_init_u64(det, 0, now);
            goto cleanup_det;
        }

        if (pivot != i) {
            for (uint8_t c = 0; c < n; c++) {
                ttak_bigreal_copy(&tmp, &a[i][c], now);
                ttak_bigreal_copy(&a[i][c], &a[pivot][c], now);
                ttak_bigreal_copy(&a[pivot][c], &tmp, now);
            }
            sign = -sign;
        }

        ttak_bigreal_mul(&det_val, &det_val, &a[i][i], now);

        for (uint8_t r = i + 1; r < n; r++) {
            if (ttak_bigreal_cmp(&a[r][i], &zero, now) == 0) continue;
            if (!ttak_bigreal_div(&factor, &a[r][i], &a[i][i], now)) {
                ok = false;
                goto cleanup_det;
            }
            for (uint8_t c = i; c < n; c++) {
                ttak_bigreal_mul(&prod, &factor, &a[i][c], now);
                ttak_bigreal_sub(&a[r][c], &a[r][c], &prod, now);
            }
        }
    }

    if (sign < 0) {
        det_val.mantissa.is_negative = !det_val.mantissa.is_negative;
    }
    ttak_bigreal_copy(det, &det_val, now);

cleanup_det:
    ttak_bigreal_free(&det_val, now);
    ttak_bigreal_free(&zero, now);
    ttak_bigreal_free(&factor, now);
    ttak_bigreal_free(&tmp, now);
    ttak_bigreal_free(&prod, now);
    for (uint8_t i = 0; i < n; i++) {
        for (uint8_t j = 0; j < n; j++) {
            ttak_bigreal_free(&a[i][j], now);
        }
    }
    m->base.release(&m->base);
    return ok;
}

_Bool ttak_matrix_invert(tt_shared_matrix_t *inv, tt_shared_matrix_t *m, tt_owner_t *owner, uint64_t now) {
    if (!inv || !m || !owner) return false;
    ttak_shared_result_t rm, ri;
    ttak_matrix_t *mat = (ttak_matrix_t *)m->base.access(&m->base, owner, &rm);
    ttak_matrix_t *out = (ttak_matrix_t *)inv->base.access(&inv->base, owner, &ri);
    if (!mat || !out || mat->rows != mat->cols || out->rows != mat->rows || out->cols != mat->cols) {
        if (mat) m->base.release(&m->base);
        if (out) inv->base.release(&inv->base);
        return false;
    }

    uint8_t n = mat->rows;
    if (n == 1) {
        ttak_bigreal_t one;
        ttak_bigreal_init_u64(&one, 1, now);
        _Bool ok = ttak_bigreal_div(&out->elements[0], &one, &mat->elements[0], now);
        ttak_bigreal_free(&one, now);
        m->base.release(&m->base);
        inv->base.release(&inv->base);
        return ok;
    }
    if (n == 2) {
        // [a, b; c, d]^-1 = 1/det * [d, -b; -c, a]
        ttak_bigreal_t det, ad, bc, inv_det, one;
        ttak_bigreal_init(&det, now);
        ttak_bigreal_init(&ad, now);
        ttak_bigreal_init(&bc, now);
        ttak_bigreal_init(&inv_det, now);
        ttak_bigreal_init_u64(&one, 1, now);

        _Bool ok = ttak_bigreal_mul(&ad, &mat->elements[0], &mat->elements[3], now) &&
                   ttak_bigreal_mul(&bc, &mat->elements[1], &mat->elements[2], now) &&
                   ttak_bigreal_sub(&det, &ad, &bc, now) &&
                   ttak_bigreal_div(&inv_det, &one, &det, now);

        if (ok) {
            ttak_bigreal_t d, neg_b, neg_c, a_val;
            ttak_bigreal_init(&d, now);
            ttak_bigreal_init(&neg_b, now);
            ttak_bigreal_init(&neg_c, now);
            ttak_bigreal_init(&a_val, now);

            ttak_bigreal_copy(&d, &mat->elements[3], now);
            ttak_bigreal_copy(&neg_b, &mat->elements[1], now);
            neg_b.mantissa.is_negative = !neg_b.mantissa.is_negative;
            ttak_bigreal_copy(&neg_c, &mat->elements[2], now);
            neg_c.mantissa.is_negative = !neg_c.mantissa.is_negative;
            ttak_bigreal_copy(&a_val, &mat->elements[0], now);

            ttak_bigreal_mul(&out->elements[0], &d, &inv_det, now);
            ttak_bigreal_mul(&out->elements[1], &neg_b, &inv_det, now);
            ttak_bigreal_mul(&out->elements[2], &neg_c, &inv_det, now);
            ttak_bigreal_mul(&out->elements[3], &a_val, &inv_det, now);

            ttak_bigreal_free(&d, now);
            ttak_bigreal_free(&neg_b, now);
            ttak_bigreal_free(&neg_c, now);
            ttak_bigreal_free(&a_val, now);
        }

        ttak_bigreal_free(&det, now);
        ttak_bigreal_free(&ad, now);
        ttak_bigreal_free(&bc, now);
        ttak_bigreal_free(&inv_det, now);
        ttak_bigreal_free(&one, now);

        m->base.release(&m->base);
        inv->base.release(&inv->base);
        return ok;
    }

    ttak_bigreal_t a[4][4];
    ttak_bigreal_t b[4][4];

    for (uint8_t i = 0; i < n; i++) {
        for (uint8_t j = 0; j < n; j++) {
            ttak_bigreal_init(&a[i][j], now);
            ttak_bigreal_init(&b[i][j], now);
            ttak_bigreal_copy(&a[i][j], &mat->elements[i * n + j], now);
            ttak_bigreal_init_u64(&b[i][j], (i == j) ? 1 : 0, now);
        }
    }

    ttak_bigreal_t zero, pivot_val, factor, tmp, prod;
    ttak_bigreal_init(&zero, now);
    ttak_bigreal_init_u64(&zero, 0, now);
    ttak_bigreal_init(&pivot_val, now);
    ttak_bigreal_init(&factor, now);
    ttak_bigreal_init(&tmp, now);
    ttak_bigreal_init(&prod, now);

    _Bool ok = true;
    for (uint8_t i = 0; i < n; i++) {
        int pivot = -1;
        for (uint8_t r = i; r < n; r++) {
            if (ttak_bigreal_cmp(&a[r][i], &zero, now) != 0) {
                pivot = r;
                break;
            }
        }
        if (pivot == -1) {
            ok = false;
            goto cleanup_inv;
        }

        if (pivot != i) {
            for (uint8_t c = 0; c < n; c++) {
                ttak_bigreal_copy(&tmp, &a[i][c], now);
                ttak_bigreal_copy(&a[i][c], &a[pivot][c], now);
                ttak_bigreal_copy(&a[pivot][c], &tmp, now);

                ttak_bigreal_copy(&tmp, &b[i][c], now);
                ttak_bigreal_copy(&b[i][c], &b[pivot][c], now);
                ttak_bigreal_copy(&b[pivot][c], &tmp, now);
            }
        }

        ttak_bigreal_copy(&pivot_val, &a[i][i], now);
        for (uint8_t c = 0; c < n; c++) {
            ttak_bigreal_div(&a[i][c], &a[i][c], &pivot_val, now);
            ttak_bigreal_div(&b[i][c], &b[i][c], &pivot_val, now);
        }

        for (uint8_t r = 0; r < n; r++) {
            if (r == i || ttak_bigreal_cmp(&a[r][i], &zero, now) == 0) continue;
            ttak_bigreal_copy(&factor, &a[r][i], now);
            for (uint8_t c = 0; c < n; c++) {
                ttak_bigreal_mul(&prod, &factor, &a[i][c], now);
                ttak_bigreal_sub(&a[r][c], &a[r][c], &prod, now);

                ttak_bigreal_mul(&prod, &factor, &b[i][c], now);
                ttak_bigreal_sub(&b[r][c], &b[r][c], &prod, now);
            }
        }
    }

    for (uint8_t i = 0; i < n; i++) {
        for (uint8_t j = 0; j < n; j++) {
            ttak_bigreal_copy(&out->elements[i * n + j], &b[i][j], now);
        }
    }

cleanup_inv:
    ttak_bigreal_free(&zero, now);
    ttak_bigreal_free(&pivot_val, now);
    ttak_bigreal_free(&factor, now);
    ttak_bigreal_free(&tmp, now);
    ttak_bigreal_free(&prod, now);
    for (uint8_t i = 0; i < n; i++) {
        for (uint8_t j = 0; j < n; j++) {
            ttak_bigreal_free(&a[i][j], now);
            ttak_bigreal_free(&b[i][j], now);
        }
    }
    m->base.release(&m->base);
    inv->base.release(&inv->base);
    return ok;
}

_Bool ttak_matrix_set_flip(tt_shared_matrix_t *m, tt_owner_t *owner, uint8_t axis, uint64_t now) {
    ttak_shared_result_t res;
    ttak_matrix_t *mat = (ttak_matrix_t *)m->base.access(&m->base, owner, &res);
    if (!mat) return false;
    
    // Set identity first
    for (int i = 0; i < 16; i++) {
        ttak_bigint_set_u64(&mat->elements[i].mantissa, 0, now);
        mat->elements[i].exponent = 0;
    }
    for (int i = 0; i < mat->rows && i < mat->cols; i++) {
        ttak_bigint_set_u64(&mat->elements[i * mat->cols + i].mantissa, (i == axis) ? 1 : 1, now);
        mat->elements[i * mat->cols + i].exponent = 0;
        mat->elements[i * mat->cols + i].mantissa.is_negative = (i == axis);
    }
    
    m->base.release(&m->base);
    return true;
}

_Bool ttak_matrix_set_ols_magic_square_4x4(tt_shared_matrix_t *m, tt_owner_t *owner, uint64_t now) {
    ttak_shared_result_t res;
    ttak_matrix_t *mat = (ttak_matrix_t *)m->base.access(&m->base, owner, &res);
    if (!mat) return false;

    /* Two mutually orthogonal 4x4 Latin squares.  The upper two bits encode
     * the first square and the lower two bits encode the second square.
     * When composed (value = (primary << 2) | secondary) the grid becomes a
     * normal 0-15 magic square whose rows, columns, and both diagonals sum to 30.
     *
     *   Primary (Latin A)        Secondary (Latin B)       Packed magic square
     *   0 1 2 3                  0 1 3 2                  0  5 11 14
     *   2 3 0 1                  2 3 1 0                  10 15 1  4
     *   1 0 3 2                  3 2 0 1                  7  2 12  9
     *   3 2 1 0                  1 0 2 3                  13 8 6  3
     */
    static const uint8_t latin_primary[16] = {
        0, 1, 2, 3,
        2, 3, 0, 1,
        1, 0, 3, 2,
        3, 2, 1, 0
    };
    static const uint8_t latin_secondary[16] = {
        0, 1, 3, 2,
        2, 3, 1, 0,
        3, 2, 0, 1,
        1, 0, 2, 3
    };

    mat->rows = 4;
    mat->cols = 4;
    for (int i = 0; i < 16; i++) {
        uint8_t val = (latin_primary[i] << 2) | latin_secondary[i];
        ttak_bigint_set_u64(&mat->elements[i].mantissa, val, now);
        mat->elements[i].exponent = 0;
    }

    m->base.release(&m->base);
    return true;
}
