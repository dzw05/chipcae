// chipcae/material/mandel.hpp —— Mandel 记法与 Voigt(Abaqus) 转换、坐标旋转
//
// 约定（规范第 2 节）：
//   Mandel 分量顺序 11,22,33,23,13,12；剪切分量乘 √2（应力、应变一致）；
//   四阶张量为 6x6 行主序；Mandel 基是正交基，因此 σ:ε = σ_M·ε_M。
//   Voigt(Abaqus) 顺序 11,22,33,12,13,23：应变剪切分量为工程剪应变 γ_ij = 2ε_ij。
#pragma once

#include "chipcae/material/material_point.hpp"

#include <Kokkos_Core.hpp>

namespace chipcae::material {

inline constexpr Real kMandelSqrt2 = 1.414213562373095048801688724209698079;

// ---- 基本量 ----
KOKKOS_INLINE_FUNCTION
constexpr Vec6 mandel_m() {
    return Vec6{1.0, 1.0, 1.0, 0.0, 0.0, 0.0};
}

KOKKOS_INLINE_FUNCTION
constexpr Mat6 mandel_identity6() {
    Mat6 I{};
    for (int i = 0; i < 6; ++i)
        I[i * 6 + i] = 1.0;
    return I;
}

KOKKOS_INLINE_FUNCTION
constexpr Real mandel_trace(const Vec6& v) {
    return v[0] + v[1] + v[2];
}

KOKKOS_INLINE_FUNCTION
constexpr Real mandel_inner(const Vec6& a, const Vec6& b) {
    Real s = 0.0;
    for (int i = 0; i < 6; ++i)
        s += a[i] * b[i];
    return s;
}

KOKKOS_INLINE_FUNCTION
Real mandel_norm(const Vec6& v) {
    return Kokkos::sqrt(mandel_inner(v, v));
}

// 偏量：dev(v) = v − ⅓ tr(v) m
KOKKOS_INLINE_FUNCTION
constexpr Vec6 mandel_dev(const Vec6& v) {
    const Real p = mandel_trace(v) / 3.0;
    return Vec6{v[0] - p, v[1] - p, v[2] - p, v[3], v[4], v[5]};
}

// P_dev = I₆ − ⅓ m mᵀ
KOKKOS_INLINE_FUNCTION
constexpr Mat6 mandel_deviator_projector() {
    Mat6 P = mandel_identity6();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            P[i * 6 + j] -= 1.0 / 3.0;
    return P;
}

// ---- 四阶张量的 6x6 矩阵运算 ----
KOKKOS_INLINE_FUNCTION
constexpr Vec6 mat6_vec(const Mat6& C, const Vec6& v) {
    Vec6 r{};
    for (int i = 0; i < 6; ++i) {
        Real s = 0.0;
        for (int j = 0; j < 6; ++j)
            s += C[i * 6 + j] * v[j];
        r[i] = s;
    }
    return r;
}

KOKKOS_INLINE_FUNCTION
constexpr Mat6 mat6_mul(const Mat6& A, const Mat6& B) {
    Mat6 C{};
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 6; ++j) {
            Real s = 0.0;
            for (int k = 0; k < 6; ++k)
                s += A[i * 6 + k] * B[k * 6 + j];
            C[i * 6 + j] = s;
        }
    return C;
}

KOKKOS_INLINE_FUNCTION
constexpr Mat6 mat6_transpose(const Mat6& A) {
    Mat6 T{};
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 6; ++j)
            T[i * 6 + j] = A[j * 6 + i];
    return T;
}

// 6x6 行主序求逆（Gauss–Jordan，带部分主元）。奇异时返回全零；调用方应保证可逆。
KOKKOS_INLINE_FUNCTION
Mat6 mat6_inverse(const Mat6& A) {
    Real a[6][12]{};
    for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 6; ++j)
            a[i][j] = A[i * 6 + j];
        a[i][6 + i] = 1.0;
    }
    for (int col = 0; col < 6; ++col) {
        int pivot = col;
        Real best = Kokkos::abs(a[col][col]);
        for (int r = col + 1; r < 6; ++r) {
            const Real v = Kokkos::abs(a[r][col]);
            if (v > best) {
                best = v;
                pivot = r;
            }
        }
        if (best == 0.0)
            return Mat6{};
        if (pivot != col)
            for (int j = 0; j < 12; ++j) {
                const Real t = a[col][j];
                a[col][j] = a[pivot][j];
                a[pivot][j] = t;
            }
        const Real inv = 1.0 / a[col][col];
        for (int j = 0; j < 12; ++j)
            a[col][j] *= inv;
        for (int r = 0; r < 6; ++r) {
            if (r == col)
                continue;
            const Real f = a[r][col];
            if (f == 0.0)
                continue;
            for (int j = 0; j < 12; ++j)
                a[r][j] -= f * a[col][j];
        }
    }
    Mat6 out{};
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 6; ++j)
            out[i * 6 + j] = a[i][6 + j];
    return out;
}

// ---- Voigt(Abaqus) ↔ Mandel ----
// Abaqus 顺序 11,22,33,12,13,23；Mandel 顺序 11,22,33,23,13,12。
KOKKOS_INLINE_FUNCTION
constexpr int voigt_from_mandel_index(int i) {
    // {0,1,2,5,4,3}
    return (i < 3) ? i : (5 - (i - 3));
}

KOKKOS_INLINE_FUNCTION
constexpr Vec6 voigt_to_mandel(const Vec6& v) {
    return Vec6{v[0], v[1], v[2], kMandelSqrt2 * v[5], kMandelSqrt2 * v[4], kMandelSqrt2 * v[3]};
}

KOKKOS_INLINE_FUNCTION
constexpr Vec6 mandel_to_voigt(const Vec6& m) {
    const Real inv = 1.0 / kMandelSqrt2;
    return Vec6{m[0], m[1], m[2], inv * m[5], inv * m[4], inv * m[3]};
}

// 切线 Voigt(Abaqus, 工程剪应变) → Mandel：C_M = D_s P C_V D_e P，D = diag(1,1,1,√2,√2,√2)。
KOKKOS_INLINE_FUNCTION
constexpr Mat6 voigt_stiffness_to_mandel(const Mat6& Cv) {
    Mat6 Cm{};
    for (int i = 0; i < 6; ++i) {
        const Real ai = (i < 3) ? 1.0 : kMandelSqrt2;
        const int pi = voigt_from_mandel_index(i);
        for (int j = 0; j < 6; ++j) {
            const Real bj = (j < 3) ? 1.0 : kMandelSqrt2;
            const int pj = voigt_from_mandel_index(j);
            Cm[i * 6 + j] = ai * Cv[pi * 6 + pj] * bj;
        }
    }
    return Cm;
}

KOKKOS_INLINE_FUNCTION
constexpr Mat6 mandel_stiffness_to_voigt(const Mat6& Cm) {
    Mat6 Cv{};
    for (int i = 0; i < 6; ++i) {
        const Real ai = (i < 3) ? 1.0 : kMandelSqrt2;
        const int pi = voigt_from_mandel_index(i);
        for (int j = 0; j < 6; ++j) {
            const Real bj = (j < 3) ? 1.0 : kMandelSqrt2;
            const int pj = voigt_from_mandel_index(j);
            Cv[pi * 6 + pj] = Cm[i * 6 + j] / (ai * bj);
        }
    }
    return Cv;
}

// ---- 坐标旋转：v_local = R v_global（R 为 3x3 行主序）----
KOKKOS_INLINE_FUNCTION
constexpr Mat3 mat3_transpose(const Mat3& A) {
    return Mat3{A[0], A[3], A[6], A[1], A[4], A[7], A[2], A[5], A[8]};
}

KOKKOS_INLINE_FUNCTION
constexpr Mat3 mat3_mul(const Mat3& A, const Mat3& B) {
    Mat3 C{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            Real s = 0.0;
            for (int k = 0; k < 3; ++k)
                s += A[i * 3 + k] * B[k * 3 + j];
            C[i * 3 + j] = s;
        }
    return C;
}

// Mandel 基张量：第 a 个基。
KOKKOS_INLINE_FUNCTION
constexpr Mat3 mandel_basis(int a) {
    Mat3 E{};
    const Real inv = 1.0 / kMandelSqrt2;
    switch (a) {
    case 0:
        E[0] = 1.0;
        break;
    case 1:
        E[4] = 1.0;
        break;
    case 2:
        E[8] = 1.0;
        break;
    case 3:
        E[5] = inv;
        E[7] = inv;
        break; // 23
    case 4:
        E[2] = inv;
        E[6] = inv;
        break; // 13
    case 5:
        E[1] = inv;
        E[3] = inv;
        break; // 12
    default:
        break;
    }
    return E;
}

// 3x3 对称张量 → Mandel 6 向量
KOKKOS_INLINE_FUNCTION
constexpr Vec6 tensor_to_mandel(const Mat3& T) {
    return Vec6{T[0], T[4], T[8], kMandelSqrt2 * T[5], kMandelSqrt2 * T[2], kMandelSqrt2 * T[1]};
}

// Q_M(R)：Mandel 6x6 旋转矩阵，满足 eps_local_M = Q_M(R) eps_global_M。
// 对每个 Mandel 基张量做 R E Rᵀ，再取回 Mandel 分量作为一列。Q_M 正交，det = 1。
KOKKOS_INLINE_FUNCTION
Mat6 mandel_rotation_matrix(const Mat3& R) {
    const Mat3 Rt = mat3_transpose(R);
    Mat6 Q{};
    for (int a = 0; a < 6; ++a) {
        const Mat3 E = mandel_basis(a);
        const Vec6 v = tensor_to_mandel(mat3_mul(mat3_mul(R, E), Rt));
        for (int i = 0; i < 6; ++i)
            Q[i * 6 + a] = v[i];
    }
    return Q;
}

KOKKOS_INLINE_FUNCTION
Vec6 rotate_mandel(const Vec6& v, const Mat3& R) {
    return mat6_vec(mandel_rotation_matrix(R), v);
}

// C_local = Q C_global Qᵀ；C_global = Qᵀ C_local Q
KOKKOS_INLINE_FUNCTION
Mat6 rotate_stiffness_local(const Mat6& C_global, const Mat3& R) {
    const Mat6 Q = mandel_rotation_matrix(R);
    return mat6_mul(mat6_mul(Q, C_global), mat6_transpose(Q));
}

KOKKOS_INLINE_FUNCTION
Mat6 rotate_stiffness_global(const Mat6& C_local, const Mat3& R) {
    const Mat6 Q = mandel_rotation_matrix(R);
    return mat6_mul(mat6_mul(mat6_transpose(Q), C_local), Q);
}

} // namespace chipcae::material
