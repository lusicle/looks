#include "util/numerics.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "util/hash.h"

namespace looks::util {

void m3_mul(const double a[9], const double b[9], double out[9]) {
    double t[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            t[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] +
                           a[r * 3 + 2] * b[6 + c];
    std::memcpy(out, t, sizeof(t));
}

void m3_mul_v(const double m[9], const double v[3], double out[3]) {
    double t[3];
    for (int r = 0; r < 3; ++r)
        t[r] = m[r * 3] * v[0] + m[r * 3 + 1] * v[1] + m[r * 3 + 2] * v[2];
    std::memcpy(out, t, sizeof(t));
}

void m3_transpose(const double m[9], double out[9]) {
    double t[9] = {m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]};
    std::memcpy(out, t, sizeof(t));
}

double m3_det(const double m[9]) {
    return m[0] * (m[4] * m[8] - m[5] * m[7]) -
           m[1] * (m[3] * m[8] - m[5] * m[6]) +
           m[2] * (m[3] * m[7] - m[4] * m[6]);
}

bool m3_invert(const double m[9], double out[9]) {
    double adj[9] = {m[4] * m[8] - m[5] * m[7], m[2] * m[7] - m[1] * m[8],
                     m[1] * m[5] - m[2] * m[4], m[5] * m[6] - m[3] * m[8],
                     m[0] * m[8] - m[2] * m[6], m[2] * m[3] - m[0] * m[5],
                     m[3] * m[7] - m[4] * m[6], m[1] * m[6] - m[0] * m[7],
                     m[0] * m[4] - m[1] * m[3]};
    const double det = m[0] * adj[0] + m[1] * adj[3] + m[2] * adj[6];
    if (std::fabs(det) < 1.0e-14) return false;
    for (int i = 0; i < 9; ++i) out[i] = adj[i] / det;
    return true;
}

void rodrigues(const double aa[3], double r_out[9]) {
    const double th =
        std::sqrt(aa[0] * aa[0] + aa[1] * aa[1] + aa[2] * aa[2]);
    if (th < 1.0e-12) {
        const double eye[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        std::memcpy(r_out, eye, sizeof(eye));
        return;
    }
    const double kx = aa[0] / th, ky = aa[1] / th, kz = aa[2] / th;
    const double c = std::cos(th), s = std::sin(th), v = 1.0 - c;
    r_out[0] = c + kx * kx * v;
    r_out[1] = kx * ky * v - kz * s;
    r_out[2] = kx * kz * v + ky * s;
    r_out[3] = ky * kx * v + kz * s;
    r_out[4] = c + ky * ky * v;
    r_out[5] = ky * kz * v - kx * s;
    r_out[6] = kz * kx * v - ky * s;
    r_out[7] = kz * ky * v + kx * s;
    r_out[8] = c + kz * kz * v;
}

void rodrigues_jac(const double aa[3], double r_out[9], double jac[27]) {
    // Analytic derivative is fiddly near theta = 0; a fixed-step central
    // difference on doubles is deterministic, symmetric, and accurate to
    // ~1e-9 - well under the solver's tolerance. The step never adapts,
    // so the same inputs always produce the same Jacobian bytes.
    rodrigues(aa, r_out);
    constexpr double kStep = 1.0e-6;
    for (int k = 0; k < 3; ++k) {
        double ap[3] = {aa[0], aa[1], aa[2]};
        double am[3] = {aa[0], aa[1], aa[2]};
        ap[k] += kStep;
        am[k] -= kStep;
        double rp[9], rm[9];
        rodrigues(ap, rp);
        rodrigues(am, rm);
        for (int e = 0; e < 9; ++e)
            jac[e * 3 + k] = (rp[e] - rm[e]) / (2.0 * kStep);
    }
}

void rodrigues_inv(const double r[9], double aa[3]) {
    // Angle from atan2 of the skew magnitude against the trace cosine
    // (well-conditioned at both ends, unlike acos near pi).
    const double sx = r[7] - r[5];
    const double sy = r[2] - r[6];
    const double sz = r[3] - r[1];
    const double s2 = 0.5 * std::sqrt(sx * sx + sy * sy + sz * sz);
    const double c = std::min(
        1.0, std::max(-1.0, (r[0] + r[4] + r[8] - 1.0) * 0.5));
    const double th = std::atan2(s2, c);
    if (s2 > 1.0e-6) {
        // aa = th * (skew vector / its magnitude): no cancellation.
        const double k = th / (2.0 * s2);
        aa[0] = k * sx;
        aa[1] = k * sy;
        aa[2] = k * sz;
        return;
    }
    if (c > 0.0) {   // theta ~ 0: first-order skew read
        aa[0] = 0.5 * sx;
        aa[1] = 0.5 * sy;
        aa[2] = 0.5 * sz;
        return;
    }
    // theta at pi: R = I + 2K^2, so a_i^2 = (R_ii + 1)/2 and
    // a_i a_j = R_ij / 2. Anchor on the largest diagonal.
    double ax, ay, az;
    if (r[0] >= r[4] && r[0] >= r[8]) {
        ax = std::sqrt(std::max(0.0, (r[0] + 1.0) * 0.5));
        ay = r[1] / (2.0 * std::max(ax, 1.0e-12));
        az = r[2] / (2.0 * std::max(ax, 1.0e-12));
    } else if (r[4] >= r[8]) {
        ay = std::sqrt(std::max(0.0, (r[4] + 1.0) * 0.5));
        ax = r[1] / (2.0 * std::max(ay, 1.0e-12));
        az = r[5] / (2.0 * std::max(ay, 1.0e-12));
    } else {
        az = std::sqrt(std::max(0.0, (r[8] + 1.0) * 0.5));
        ax = r[2] / (2.0 * std::max(az, 1.0e-12));
        ay = r[5] / (2.0 * std::max(az, 1.0e-12));
    }
    aa[0] = th * ax;
    aa[1] = th * ay;
    aa[2] = th * az;
}

void jacobi_eigen_sym(const double* a, int n, double* eigvals,
                      double* eigvecs) {
    double m[81];
    std::memcpy(m, a, sizeof(double) * static_cast<size_t>(n) * n);
    double v[81] = {};
    for (int i = 0; i < n; ++i) v[i * n + i] = 1.0;
    // Fixed sweep count, fixed (p, q) order: deterministic and plenty
    // for n <= 9 (Jacobi converges quadratically).
    constexpr int kSweeps = 24;
    for (int sweep = 0; sweep < kSweeps; ++sweep) {
        for (int p = 0; p < n - 1; ++p)
            for (int q = p + 1; q < n; ++q) {
                const double apq = m[p * n + q];
                if (std::fabs(apq) < 1.0e-15) continue;
                const double app = m[p * n + p];
                const double aqq = m[q * n + q];
                const double tau = (aqq - app) / (2.0 * apq);
                const double t =
                    (tau >= 0.0 ? 1.0 : -1.0) /
                    (std::fabs(tau) + std::sqrt(1.0 + tau * tau));
                const double c = 1.0 / std::sqrt(1.0 + t * t);
                const double s = t * c;
                for (int k = 0; k < n; ++k) {
                    const double mkp = m[k * n + p];
                    const double mkq = m[k * n + q];
                    m[k * n + p] = c * mkp - s * mkq;
                    m[k * n + q] = s * mkp + c * mkq;
                }
                for (int k = 0; k < n; ++k) {
                    const double mpk = m[p * n + k];
                    const double mqk = m[q * n + k];
                    m[p * n + k] = c * mpk - s * mqk;
                    m[q * n + k] = s * mpk + c * mqk;
                }
                for (int k = 0; k < n; ++k) {
                    const double vpk = v[p * n + k];
                    const double vqk = v[q * n + k];
                    v[p * n + k] = c * vpk - s * vqk;
                    v[q * n + k] = s * vpk + c * vqk;
                }
            }
    }
    // Sort ascending, stable order for ties.
    int order[9];
    for (int i = 0; i < n; ++i) order[i] = i;
    std::stable_sort(order, order + n, [&](int x, int y) {
        return m[x * n + x] < m[y * n + y];
    });
    for (int i = 0; i < n; ++i) {
        eigvals[i] = m[order[i] * n + order[i]];
        for (int k = 0; k < n; ++k)
            eigvecs[i * n + k] = v[order[i] * n + k];
    }
}

void svd3(const double mm[9], double u[9], double s[3], double vt[9]) {
    // Eigen of M^T M gives V and the squared singular values; U comes
    // from M V / s with degenerate columns rebuilt by cross products.
    double mtm[9];
    double mt[9];
    m3_transpose(mm, mt);
    m3_mul(mt, mm, mtm);
    double evals[3], evecs[9];
    jacobi_eigen_sym(mtm, 3, evals, evecs);
    // Descending singular values.
    for (int i = 0; i < 3; ++i) {
        const double ev = evals[2 - i];
        s[i] = ev > 0.0 ? std::sqrt(ev) : 0.0;
        for (int k = 0; k < 3; ++k) vt[i * 3 + k] = evecs[(2 - i) * 3 + k];
    }
    // Right-handed V (det +1) keeps downstream decompositions sane.
    double vt_det[9];
    std::memcpy(vt_det, vt, sizeof(vt_det));
    if (m3_det(vt_det) < 0.0)
        for (int k = 0; k < 3; ++k) vt[2 * 3 + k] = -vt[2 * 3 + k];
    for (int i = 0; i < 3; ++i) {
        const double vcol[3] = {vt[i * 3], vt[i * 3 + 1], vt[i * 3 + 2]};
        double mv[3];
        m3_mul_v(mm, vcol, mv);
        if (s[i] > 1.0e-12) {
            u[0 * 3 + i] = mv[0] / s[i];
            u[1 * 3 + i] = mv[1] / s[i];
            u[2 * 3 + i] = mv[2] / s[i];
        } else {
            // Rebuild as the cross of the earlier columns.
            const double a0 = u[0], a1 = u[3], a2 = u[6];
            const double b0 = u[1], b1 = u[4], b2 = u[7];
            double cx = a1 * b2 - a2 * b1;
            double cy = a2 * b0 - a0 * b2;
            double cz = a0 * b1 - a1 * b0;
            const double len =
                std::sqrt(cx * cx + cy * cy + cz * cz);
            if (len > 1.0e-12) {
                cx /= len;
                cy /= len;
                cz /= len;
            } else {
                cx = 0.0;
                cy = 0.0;
                cz = 1.0;
            }
            u[0 * 3 + i] = cx;
            u[1 * 3 + i] = cy;
            u[2 * 3 + i] = cz;
        }
    }
}

bool sym_solve(double* a, int n, double* b) {
    // In-place Cholesky a = L·Lᵀ (lower triangle), then two triangular
    // substitutions.
    for (int j = 0; j < n; ++j) {
        double d = a[j * n + j];
        for (int k = 0; k < j; ++k) d -= a[j * n + k] * a[j * n + k];
        if (d <= 0.0) return false;
        const double lj = std::sqrt(d);
        a[j * n + j] = lj;
        for (int i = j + 1; i < n; ++i) {
            double s = a[i * n + j];
            for (int k = 0; k < j; ++k) s -= a[i * n + k] * a[j * n + k];
            a[i * n + j] = s / lj;
        }
    }
    for (int i = 0; i < n; ++i) {
        double s = b[i];
        for (int k = 0; k < i; ++k) s -= a[i * n + k] * b[k];
        b[i] = s / a[i * n + i];
    }
    for (int i = n - 1; i >= 0; --i) {
        double s = b[i];
        for (int k = i + 1; k < n; ++k) s -= a[k * n + i] * b[k];
        b[i] = s / a[i * n + i];
    }
    return true;
}

bool ransac_pick(uint64_t seed, uint32_t iteration, uint32_t n, int k,
                 int* out) {
    if (n < static_cast<uint32_t>(k)) return false;
    for (int i = 0; i < k; ++i) {
        for (uint32_t attempt = 0;; ++attempt) {
            const uint64_t h = hash_combine(
                hash_combine(seed, iteration),
                (static_cast<uint64_t>(i) << 32) | attempt);
            const int pick = static_cast<int>(h % n);
            bool dup = false;
            for (int j = 0; j < i; ++j)
                if (out[j] == pick) dup = true;
            if (!dup) {
                out[i] = pick;
                break;
            }
            if (attempt > 64) return false;   // n too small in practice
        }
    }
    return true;
}

}  // namespace looks::util
