// tests/t03_mandel.cpp —— T3：Mandel 约定
//   1) σ:ε = σ_M·ε_M
//   2) Voigt(Abaqus) ↔ Mandel 往返（向量与 6x6 切线）
//   3) Q_M(R) 正交、det=+1、保持内积
// 相对误差判据 ≤ 1e-14；固定种子，失败时打印种子与输入。
#include "chipcae/material/mandel.hpp"

#include <Kokkos_Core.hpp>
#include <cmath>
#include <cstdio>
#include <random>
#include <utility>

using namespace chipcae::material;

namespace {

constexpr Real kTol = 1e-14;
Real max_error = 0;
constexpr std::uint64_t kSeed = 20261116ull;

Real rel_error(Real got, Real want) {
    const Real denom = Kokkos::max(Kokkos::abs(got), Kokkos::abs(want));
    return (denom == 0.0) ? Kokkos::abs(got - want) : Kokkos::abs(got - want) / denom;
}

bool check_rel(const char* what, Real got, Real want, std::uint64_t seed, int iter) {
    const Real err = rel_error(got, want);
    max_error = std::max(max_error, err);
    if (!(err <= kTol)) {
        std::printf("FAIL %s seed=%llu iter=%d got=%.17g want=%.17g rel=%.3e\n",
                    what,
                    static_cast<unsigned long long>(seed),
                    iter,
                    got,
                    want,
                    err);
        return false;
    }
    return true;
}

bool check_abs(const char* what, Real err, Real tol, std::uint64_t seed, int iter) {
    max_error = std::max(max_error, err);
    if (!(err <= tol)) {
        std::printf("FAIL %s seed=%llu iter=%d abs_err=%.3e tol=%.1e\n",
                    what,
                    static_cast<unsigned long long>(seed),
                    iter,
                    err,
                    tol);
        return false;
    }
    return true;
}

Mat3 random_rotation(std::mt19937_64& rng) {
    std::uniform_real_distribution<Real> u(-1.0, 1.0);
    // 四元数 → 旋转矩阵，天然正交且 det=+1
    Real q[4];
    Real norm = 0.0;
    do {
        norm = 0.0;
        for (Real& c : q) {
            c = u(rng);
            norm += c * c;
        }
    } while (norm < 1e-6);
    norm = std::sqrt(norm);
    for (Real& c : q)
        c /= norm;
    const Real w = q[0], x = q[1], y = q[2], z = q[3];
    return Mat3{1 - 2 * (y * y + z * z),
                2 * (x * y - z * w),
                2 * (x * z + y * w),
                2 * (x * y + z * w),
                1 - 2 * (x * x + z * z),
                2 * (y * z - x * w),
                2 * (x * z - y * w),
                2 * (y * z + x * w),
                1 - 2 * (x * x + y * y)};
}

Real determinant6(const Mat6& A) {
    Real a[6][6];
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 6; ++j)
            a[i][j] = A[i * 6 + j];
    Real det = 1.0;
    for (int col = 0; col < 6; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 6; ++r)
            if (std::abs(a[r][col]) > std::abs(a[pivot][col]))
                pivot = r;
        if (a[pivot][col] == 0.0)
            return 0.0;
        if (pivot != col) {
            for (int j = 0; j < 6; ++j)
                std::swap(a[col][j], a[pivot][j]);
            det = -det;
        }
        det *= a[col][col];
        for (int r = col + 1; r < 6; ++r) {
            const Real f = a[r][col] / a[col][col];
            for (int j = col; j < 6; ++j)
                a[r][j] -= f * a[col][j];
        }
    }
    return det;
}

} // namespace

int main(int argc, char** argv) {
    Kokkos::ScopeGuard guard(argc, argv);
    std::mt19937_64 rng(kSeed);
    std::uniform_real_distribution<Real> u(-1.0, 1.0);
    std::uniform_real_distribution<Real> u01(0.0, 1.0); // 避免双点积在随机正负号下大量抵消

    bool ok = true;
    for (int iter = 0; iter < 200 && ok; ++iter) {
        Vec6 a{}, b{};
        for (int i = 0; i < 6; ++i) {
            a[i] = u01(rng);
            b[i] = u01(rng);
        }

        // a、b 先视为张量分量 (11,22,33,23,13,12)，转成 Mandel 后点积应等于张量缩并
        const Vec6 ma{
            a[0], a[1], a[2], kMandelSqrt2 * a[3], kMandelSqrt2 * a[4], kMandelSqrt2 * a[5]};
        const Vec6 mb{
            b[0], b[1], b[2], kMandelSqrt2 * b[3], kMandelSqrt2 * b[4], kMandelSqrt2 * b[5]};
        const Real tensor_dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] +
                                2.0 * (a[3] * b[3] + a[4] * b[4] + a[5] * b[5]);
        ok &= check_rel("inner", mandel_inner(ma, mb), tensor_dot, kSeed, iter);

        // 2a) Voigt ↔ Mandel 向量往返
        const Vec6 back = voigt_to_mandel(mandel_to_voigt(a));
        for (int i = 0; i < 6 && ok; ++i)
            ok &= check_rel("voigt_roundtrip", back[i], a[i], kSeed, iter);

        // 2b) 6x6 切线往返
        Mat6 C{};
        for (int i = 0; i < 36; ++i)
            C[i] = u(rng);
        const Mat6 Cback = voigt_stiffness_to_mandel(mandel_stiffness_to_voigt(C));
        Real nD = 0.0, nC = 0.0;
        for (int i = 0; i < 36; ++i) {
            nD += (Cback[i] - C[i]) * (Cback[i] - C[i]);
            nC += C[i] * C[i];
        }
        ok &= check_abs("stiffness_roundtrip", std::sqrt(nD) / std::sqrt(nC), kTol, kSeed, iter);

        // 3) Q_M 正交 / det / 内积不变
        const Mat3 R = random_rotation(rng);
        const Mat6 Q = mandel_rotation_matrix(R);
        const Mat6 QtQ = mat6_mul(mat6_transpose(Q), Q);
        const Mat6 I = mandel_identity6();
        Real nQ = 0.0;
        for (int i = 0; i < 36; ++i)
            nQ += (QtQ[i] - I[i]) * (QtQ[i] - I[i]);
        ok &= check_abs("Q_orthogonal", std::sqrt(nQ), kTol, kSeed, iter);
        ok &= check_rel("Q_det", determinant6(Q), 1.0, kSeed, iter);

        const Vec6 ar = rotate_mandel(a, R);
        ok &= check_rel("rotate_inner", mandel_inner(ar, ar), mandel_inner(a, a), kSeed, iter);
        ok &= check_rel("rotate_norm", mandel_norm(ar), mandel_norm(a), kSeed, iter);
    }

    if (!ok) {
        std::printf("T3 FAILED (seed=%llu)\n", static_cast<unsigned long long>(kSeed));
        return 1;
    }
    std::printf("T3 PASSED (seed=%llu, 200 random cases)\n",
                static_cast<unsigned long long>(kSeed));
    std::printf(
        "METRIC {\"id\":\"T3\",\"measured_error\":%.17g,\"tolerance\":1e-14,\"seed\":20261116}\n",
        max_error);
    return 0;
}
