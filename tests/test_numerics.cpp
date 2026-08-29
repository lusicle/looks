#include "util/numerics.h"

#include <cmath>
#include <cstring>

#include "test_framework.h"

using namespace looks::util;

namespace {

bool near9(const double* a, const double* b, double tol) {
    for (int i = 0; i < 9; ++i)
        if (std::fabs(a[i] - b[i]) > tol) return false;
    return true;
}

}  // namespace

TEST(numerics_m3_basics) {
    const double m[9] = {2, 0, 1, 0, 3, 0, 1, 0, 2};
    CHECK(std::fabs(m3_det(m) - 9.0) < 1e-12);
    double inv[9], prod[9];
    CHECK(m3_invert(m, inv));
    m3_mul(m, inv, prod);
    const double eye[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    CHECK(near9(prod, eye, 1e-12));
}

TEST(numerics_rodrigues_and_jacobian) {
    // 90 degrees about z maps x onto y.
    const double aa[3] = {0.0, 0.0, 1.5707963267948966};
    double r[9];
    rodrigues(aa, r);
    const double x[3] = {1, 0, 0};
    double y[3];
    m3_mul_v(r, x, y);
    CHECK(std::fabs(y[0]) < 1e-9);
    CHECK(std::fabs(y[1] - 1.0) < 1e-9);
    double rt[9], rr[9];
    m3_transpose(r, rt);
    m3_mul(rt, r, rr);
    const double eye[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    CHECK(near9(rr, eye, 1e-12));
    double r2[9], jac[27], jac2[27];
    rodrigues_jac(aa, r2, jac);
    rodrigues_jac(aa, r2, jac2);
    CHECK(std::memcmp(jac, jac2, sizeof(jac)) == 0);
    // At this pose dR00/dz = -sin = -1 and dR01/dz = 0.
    CHECK(std::fabs(jac[0 * 3 + 2] - (-1.0)) < 1e-5);
    CHECK(std::fabs(jac[1 * 3 + 2] - (-0.0)) < 1e-5);
}

TEST(numerics_jacobi_eigen_and_svd3) {
    const double aa[3] = {0.3, -0.2, 0.5};
    double r[9], rt[9];
    rodrigues(aa, r);
    m3_transpose(r, rt);
    const double d[9] = {1, 0, 0, 0, 2, 0, 0, 0, 4};
    double tmp[9], sym[9];
    m3_mul(r, d, tmp);
    m3_mul(tmp, rt, sym);
    double evals[3], evecs[9];
    jacobi_eigen_sym(sym, 3, evals, evecs);
    CHECK(std::fabs(evals[0] - 1.0) < 1e-9);
    CHECK(std::fabs(evals[1] - 2.0) < 1e-9);
    CHECK(std::fabs(evals[2] - 4.0) < 1e-9);

    const double m[9] = {3, 1, 0, 1, 2, 1, 0, 1, 1};
    double u[9], s[3], vt[9];
    svd3(m, u, s, vt);
    CHECK(s[0] >= s[1] && s[1] >= s[2]);
    double us[9] = {u[0] * s[0], u[1] * s[1], u[2] * s[2],
                    u[3] * s[0], u[4] * s[1], u[5] * s[2],
                    u[6] * s[0], u[7] * s[1], u[8] * s[2]};
    double rec[9];
    m3_mul(us, vt, rec);
    CHECK(near9(rec, m, 1e-9));
}

TEST(numerics_rodrigues_inv_roundtrip) {
    const double cases[4][3] = {{0.3, -0.2, 0.5},
                                {0.0, 0.0, 1.0e-9},
                                {2.0, 1.0, -0.5},
                                {3.14159, 0.0, 0.0}};   // near pi
    for (const double* aa : cases) {
        double r[9], back[9], aa2[3];
        rodrigues(aa, r);
        rodrigues_inv(r, aa2);
        rodrigues(aa2, back);
        CHECK(near9(r, back, 1e-6));
    }
}

TEST(numerics_sym_solve) {
    const double L[9] = {2, 0, 0, 1, 3, 0, -1, 2, 4};
    double a[9], lt[9];
    m3_transpose(L, lt);
    m3_mul(L, lt, a);
    const double x_true[3] = {1.5, -2.0, 0.5};
    double b[3];
    m3_mul_v(a, x_true, b);
    CHECK(sym_solve(a, 3, b));
    for (int i = 0; i < 3; ++i) CHECK(std::fabs(b[i] - x_true[i]) < 1e-10);
    double bad[4] = {1, 2, 2, 1};   // eigenvalues 3 and -1: not SPD
    double rhs[2] = {1, 1};
    CHECK(!sym_solve(bad, 2, rhs));
}

TEST(numerics_ransac_pick_deterministic) {
    int a[4], b[4];
    CHECK(ransac_pick(0x1234, 7, 100, 4, a));
    CHECK(ransac_pick(0x1234, 7, 100, 4, b));
    for (int i = 0; i < 4; ++i) CHECK_EQ(a[i], b[i]);
    for (int i = 0; i < 4; ++i)
        for (int j = i + 1; j < 4; ++j) CHECK(a[i] != a[j]);
    CHECK(!ransac_pick(1, 1, 3, 4, a));   // n < k refuses
    int c[4];
    CHECK(ransac_pick(0x1234, 8, 100, 4, c));
    bool same = true;
    for (int i = 0; i < 4; ++i) same = same && a[i] == c[i];
    CHECK(!same);
}
