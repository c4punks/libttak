#ifndef TTAK_MATH_CALCULUS_H
#define TTAK_MATH_CALCULUS_H

#include <ttak/math/bigreal.h>
#include <ttak/priority/scheduler.h>

/**
 * @brief Function signature for numerical methods.
 * @param res Destination for the function value.
 * @param x Input value.
 * @param ctx User context.
 * @param now Timestamp.
 * @return true on success.
 */
typedef _Bool (*ttak_math_func_t)(ttak_bigreal_t *res, const ttak_bigreal_t *x, void *ctx, uint64_t now);

/**
 * @brief Multivariable function signature for partial derivatives.
 * @param res Destination for scalar output.
 * @param x_vec Input vector array of bigreals.
 * @param dim Number of dimensions in x_vec.
 * @param ctx User context.
 * @param now Timestamp.
 * @return true on success.
 */
typedef _Bool (*ttak_math_vec_func_t)(ttak_bigreal_t *res, const ttak_bigreal_t *x_vec, uint8_t dim, void *ctx, uint64_t now);

/**
 * @brief Numerical differentiation at point x.
 */
_Bool ttak_calculus_diff(ttak_bigreal_t *res, ttak_math_func_t f, const ttak_bigreal_t *x, void *ctx, uint64_t now);

/**
 * @brief Partial differentiation of f with respect to component `target_dim`.
 *
 * @param res Destination for the partial derivative.
 * @param f Multivariable function f(x_vec, dim).
 * @param x_vec Input point vector.
 * @param dim Total dimensionality of x_vec.
 * @param target_dim Index of variable to differentiate with respect to (0 <= target_dim < dim).
 * @param ctx User context.
 * @param now Timestamp.
 * @return true on success, false on failure or out-of-range target_dim.
 */
_Bool ttak_calculus_partial_diff(ttak_bigreal_t *res, ttak_math_vec_func_t f, const ttak_bigreal_t *x_vec, uint8_t dim, uint8_t target_dim, void *ctx, uint64_t now);

/**
 * @brief Numerical definite integration over [a, b].
 * Uses adaptive methods and can be parallelized.
 */
_Bool ttak_calculus_integrate(ttak_bigreal_t *res, ttak_math_func_t f, const ttak_bigreal_t *a, const ttak_bigreal_t *b, void *ctx, uint64_t now);

/**
 * @brief RK4 (Runge-Kutta 4th Order) ODE solver step.
 * Solves dy/dt = f(t, y)
 * @param y_next Next state.
 * @param f Derivative function.
 * @param t Current time.
 * @param y Current state.
 * @param h Time step.
 * @param ctx User context.
 * @param now Timestamp.
 * @return true on success.
 */
_Bool ttak_calculus_rk4_step(ttak_bigreal_t *y_next, ttak_math_func_t f, const ttak_bigreal_t *t, const ttak_bigreal_t *y, const ttak_bigreal_t *h, void *ctx, uint64_t now);

#endif // TTAK_MATH_CALCULUS_H
