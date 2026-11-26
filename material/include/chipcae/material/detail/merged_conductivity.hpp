#pragma once

#include <cstdint>
#include <limits>

#if defined(__CUDACC__) || defined(__HIPCC__)
#define CHIPCAE_MERGED_K_INLINE __host__ __device__ inline
#else
#define CHIPCAE_MERGED_K_INLINE inline
#endif

namespace chipcae::material::detail {
static_assert(sizeof(double) == sizeof(std::uint64_t) && std::numeric_limits<double>::is_iec559 &&
              std::numeric_limits<double>::digits == 53);
inline constexpr int kMergedConductivityMaxMembers = 8;
enum class MergedConductivityStatus : int {
    Ok = 0,
    EmptyGroup = 1,
    TooManyMembers = 2,
    NullPointer = 3,
    InvalidVolume = 4,
    InvalidConductivity = 5
};
enum class MergedConductivityKind : int { Uniform = 0, Laminate = 1, ReussBound = 2 };
struct MergedConductivity {
    double k[6];
    MergedConductivityKind kind;
    int axis;
};

// Character access copies the representation without aliasing violations, inactive
// union reads, or a host-only library call. CUDA and HIP use the same binary64 layout.
CHIPCAE_MERGED_K_INLINE std::uint64_t merged_k_bits(double value) {
    std::uint64_t result = 0;
    const auto* source = reinterpret_cast<const unsigned char*>(&value);
    auto* destination = reinterpret_cast<unsigned char*>(&result);
    for (unsigned i = 0; i < sizeof(value); ++i)
        destination[i] = source[i];
    return result;
}

CHIPCAE_MERGED_K_INLINE bool merged_k_positive_finite(double value) {
    const auto bits = merged_k_bits(value);
    return (bits >> 63) == 0 && bits != 0 &&
           (bits & 0x7ff0000000000000ULL) != 0x7ff0000000000000ULL;
}

CHIPCAE_MERGED_K_INLINE bool merged_k_same(const double* k, int i, int j) {
    for (int c = 0; c < 3; ++c)
        if (merged_k_bits(k[3 * i + c]) != merged_k_bits(k[3 * j + c]))
            return false;
    return true;
}

CHIPCAE_MERGED_K_INLINE bool
merged_k_less(int i, int j, const double* k, const double* volume, const int* ijk) {
    for (int a = 0; a < 3; ++a) {
        if (ijk[3 * i + a] != ijk[3 * j + a])
            return ijk[3 * i + a] < ijk[3 * j + a];
    }
    for (int c = 0; c < 3; ++c) {
        const auto left = merged_k_bits(k[3 * i + c]);
        const auto right = merged_k_bits(k[3 * j + c]);
        if (left != right)
            return left < right;
    }
    return merged_k_bits(volume[i]) < merged_k_bits(volume[j]);
}

// Store products before adding: callers need not inherit the material library's
// private FP flags. Volatile forces binary64 rounding and prevents FMA contraction.
CHIPCAE_MERGED_K_INLINE double merged_k_product(double a, double b) {
    volatile double product = a * b;
    return product;
}

// Positive finite value = mantissa * 2^exponent, with 1 <= mantissa < 2.
// Normalize subnormals exactly before inspecting their exponent.
CHIPCAE_MERGED_K_INLINE double merged_k_split(double value, int& exponent) {
    auto bits = merged_k_bits(value);
    int adjustment = 0;
    if ((bits >> 52) == 0) {
        bits = merged_k_bits(merged_k_product(value, 0x1p52));
        adjustment = -52;
    }
    exponent = static_cast<int>(bits >> 52) - 1023 + adjustment;
    return 1.0 + static_cast<double>(bits & 0x000fffffffffffffULL) * 0x1p-52;
}

CHIPCAE_MERGED_K_INLINE double merged_k_scale(double value, int exponent) {
    // Chunking keeps powers representable and rounds only when the final result
    // becomes subnormal. Terms too small to affect a normalized sum may reach zero.
    while (exponent > 512) {
        value = merged_k_product(value, 0x1p512);
        exponent -= 512;
    }
    while (exponent < -512) {
        value = merged_k_product(value, 0x1p-512);
        exponent += 512;
    }
    const auto bits = static_cast<std::uint64_t>(exponent + 1023) << 52;
    double power = 0.0;
    const auto* source = reinterpret_cast<const unsigned char*>(&bits);
    auto* destination = reinterpret_cast<unsigned char*>(&power);
    for (unsigned i = 0; i < sizeof(power); ++i)
        destination[i] = source[i];
    return merged_k_product(value, power);
}

// Fallback when normalized weights or conductivity ratios underflow. Form V*k
// or V/k in mantissa/exponent form *before* normalization, preserving a tiny
// phase with significant resistance even when V/max(V) is unrepresentable.
CHIPCAE_MERGED_K_INLINE double merged_k_extreme_mean(
    int n, const int* order, const double* k, const double* volume, int c, bool arithmetic) {
    double vm[kMergedConductivityMaxMembers], term[kMergedConductivityMaxMembers];
    int ve[kMergedConductivityMaxMembers], te[kMergedConductivityMaxMembers];
    int max_ve = -4096, max_te = -4096;
    for (int i = 0; i < n; ++i) {
        int ke = 0;
        vm[i] = merged_k_split(volume[order[i]], ve[i]);
        const double km = merged_k_split(k[3 * order[i] + c], ke);
        term[i] = arithmetic ? merged_k_product(vm[i], km) : vm[i] / km;
        te[i] = arithmetic ? ve[i] + ke : ve[i] - ke;
        if (ve[i] > max_ve)
            max_ve = ve[i];
        if (te[i] > max_te)
            max_te = te[i];
    }
    double v_sum = 0.0, term_sum = 0.0;
    for (int i = 0; i < n; ++i) {
        v_sum += merged_k_scale(vm[i], ve[i] - max_ve);
        term_sum += merged_k_scale(term[i], te[i] - max_te);
    }
    return arithmetic ? merged_k_scale(term_sum / v_sum, max_te - max_ve)
                      : merged_k_scale(v_sum / term_sum, max_ve - max_te);
}

// Internal only. Arrays are member-major; k is xx, yy, zz, yz, xz, xy.
CHIPCAE_MERGED_K_INLINE MergedConductivityStatus
merged_conductivity_tensor(int n,
                           const double* k_diag,
                           const double* volume,
                           const int* cell_ijk,
                           MergedConductivity* out) {
    if (n <= 0)
        return MergedConductivityStatus::EmptyGroup;
    if (n > kMergedConductivityMaxMembers)
        return MergedConductivityStatus::TooManyMembers;
    if (!k_diag || !volume || !cell_ijk || !out)
        return MergedConductivityStatus::NullPointer;
    for (int i = 0; i < n; ++i)
        if (!merged_k_positive_finite(volume[i]))
            return MergedConductivityStatus::InvalidVolume;
    for (int i = 0; i < 3 * n; ++i)
        if (!merged_k_positive_finite(k_diag[i]))
            return MergedConductivityStatus::InvalidConductivity;

    int order[kMergedConductivityMaxMembers];
    for (int i = 0; i < n; ++i) {
        int j = i;
        while (j > 0 && merged_k_less(i, order[j - 1], k_diag, volume, cell_ijk)) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = i;
    }

    MergedConductivity result{{0.0, 0.0, 0.0, 0.0, 0.0, 0.0}, MergedConductivityKind::Uniform, -1};
    bool uniform = true;
    for (int i = 1; i < n; ++i)
        uniform = merged_k_same(k_diag, order[0], order[i]) && uniform;
    if (uniform) {
        for (int c = 0; c < 3; ++c)
            result.k[c] = k_diag[3 * order[0] + c];
        *out = result;
        return MergedConductivityStatus::Ok;
    }

    int candidates = 0;
    for (int a = 0; a < 3; ++a) {
        bool candidate = true;
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j)
                if (cell_ijk[3 * order[i] + a] == cell_ijk[3 * order[j] + a] &&
                    !merged_k_same(k_diag, order[i], order[j]))
                    candidate = false;
        if (candidate) {
            ++candidates;
            result.axis = a;
        }
    }
    result.kind =
        candidates == 1 ? MergedConductivityKind::Laminate : MergedConductivityKind::ReussBound;
    if (candidates != 1)
        result.axis = -1;

    double max_volume = volume[order[0]];
    for (int i = 1; i < n; ++i)
        if (volume[order[i]] > max_volume)
            max_volume = volume[order[i]];
    double scaled_volume[kMergedConductivityMaxMembers];
    double total = 0.0;
    for (int i = 0; i < n; ++i) {
        scaled_volume[i] = volume[order[i]] / max_volume;
        total += scaled_volume[i];
    }
    for (int c = 0; c < 3; ++c) {
        double minimum = k_diag[3 * order[0] + c];
        double maximum = minimum;
        for (int i = 1; i < n; ++i) {
            const double k = k_diag[3 * order[i] + c];
            if (k < minimum)
                minimum = k;
            if (k > maximum)
                maximum = k;
        }
        double sum = 0.0;
        bool extreme = false;
        const bool arithmetic = result.kind == MergedConductivityKind::Laminate && c != result.axis;
        for (int i = 0; i < n; ++i) {
            const double weight = scaled_volume[i] / total;
            const double k = k_diag[3 * order[i] + c];
            const double ratio = arithmetic ? k / maximum : minimum / k;
            const double term = merged_k_product(weight, ratio);
            extreme = extreme || term < 0x1p-1022;
            sum += term;
        }
        // Algebraically A=max(k)*sum(w*k/max(k)), H=min(k)/sum(w*min(k)/k).
        // Clamp only roundoff at the exact component extrema (also bounds overflow).
        const double value = extreme
                                 ? merged_k_extreme_mean(n, order, k_diag, volume, c, arithmetic)
                             : arithmetic ? merged_k_product(maximum, sum)
                                          : minimum / sum;
        result.k[c] = value < minimum ? minimum : value > maximum ? maximum : value;
    }
    *out = result;
    return MergedConductivityStatus::Ok;
}
} // namespace chipcae::material::detail

#undef CHIPCAE_MERGED_K_INLINE
