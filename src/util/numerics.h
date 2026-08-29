// All routines use doubles and a fixed operation order for determinism.

#pragma once

#include <cstdint>

namespace looks::util {

// Row-major 3x3.
void m3_mul(const double a[9], const double b[9], double out[9]);
void m3_mul_v(const double m[9], const double v[3], double out[3]);
void m3_transpose(const double m[9], double out[9]);
double m3_det(const double m[9]);
bool m3_invert(const double m[9], double out[9]);

// The angle is the magnitude of aa.
// jac holds dR[r*3+c]/d(aa[k]) at jac[(r*3+c)*3+k].
void rodrigues(const double aa[3], double r_out[9]);
void rodrigues_jac(const double aa[3], double r_out[9], double jac[27]);
// The sign choice at theta = pi is deterministic.
void rodrigues_inv(const double r[9], double aa[3]);

// Eigenvalues are ascending. Eigenvectors are the ROWS of eigvecs.
// n must be 9 or less. The sweep count is fixed for determinism.
void jacobi_eigen_sym(const double* a, int n, double* eigvals,
                      double* eigvecs);

// m = u * diag(s) * vt. The singular values are descending.
void svd3(const double m[9], double u[9], double s[3], double vt[9]);

// The result is deterministic for a given seed and iteration.
bool ransac_pick(uint64_t seed, uint32_t iteration, uint32_t n, int k,
                 int* out);

// This destroys a and writes the solution x into b.
// The caller must damp the diagonal to keep the pivots positive.
bool sym_solve(double* a, int n, double* b);

}  // namespace looks::util
