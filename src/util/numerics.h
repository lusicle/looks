// Fixed-size numerics for the motion solver: 3-vector / 3x3 helpers,
// symmetric Jacobi eigendecomposition (the nullspace extractor behind
// the 8-point essential and DLT triangulation), a 3x3 SVD built on it,
// Rodrigues rotation with its analytic Jacobian (bundle adjustment),
// and counter-hashed RANSAC sampling. Everything runs a FIXED number of
// sweeps in a fixed order on doubles - deterministic by construction,
// no third-party math.

#pragma once

#include <cstdint>

namespace looks::util {

// Row-major 3x3.
void m3_mul(const double a[9], const double b[9], double out[9]);
void m3_mul_v(const double m[9], const double v[3], double out[3]);
void m3_transpose(const double m[9], double out[9]);
double m3_det(const double m[9]);
bool m3_invert(const double m[9], double out[9]);

// Rodrigues: axis-angle (angle = |aa|) to rotation matrix, plus the
// 9x3 Jacobian dR/d(aa) (row-major: dR[r*3+c][k] at jac[(r*3+c)*3+k]).
void rodrigues(const double aa[3], double r_out[9]);
void rodrigues_jac(const double aa[3], double r_out[9], double jac[27]);
// Rotation matrix back to axis-angle, stable through the theta -> 0
// and theta -> pi limits (the pi-side sign pick is deterministic).
void rodrigues_inv(const double r[9], double aa[3]);

// Symmetric Jacobi eigendecomposition, n <= 9: fills eigenvalues
// (ascending) and the matching eigenvectors as ROWS of eigvecs
// (n x n row-major). Fixed sweep count keeps it deterministic.
void jacobi_eigen_sym(const double* a, int n, double* eigvals,
                      double* eigvecs);

// 3x3 SVD via the eigen of M^T M: m = u * diag(s) * vt, singular values
// descending, u/vt proper handling of rank deficiency.
void svd3(const double m[9], double u[9], double s[3], double vt[9]);

// k distinct indices out of n from (seed, iteration) counter hashing -
// the shared deterministic RANSAC sampler. Returns false when n < k.
bool ransac_pick(uint64_t seed, uint32_t iteration, uint32_t n, int k,
                 int* out);

// Solves the symmetric positive-definite system a·x = b (row-major
// n x n): `a` is destroyed, `b` becomes x. Cholesky without pivoting in
// a fixed order - callers damp the diagonal (Levenberg-Marquardt) so
// the factorization stays positive. False on a non-positive pivot.
bool sym_solve(double* a, int n, double* b);

}  // namespace looks::util
