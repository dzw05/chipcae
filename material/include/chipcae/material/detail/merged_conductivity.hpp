#pragma once

#if defined(__CUDACC__) || defined(__HIPCC__)
#define CHIPCAE_MERGED_K_INLINE __host__ __device__ inline
#else
#define CHIPCAE_MERGED_K_INLINE inline
#endif

namespace chipcae::material::detail {
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

// TEST-FIRST stub only: keep acceptance executable buildable until implementation.
// Intentionally rejects every input and leaves output untouched.
CHIPCAE_MERGED_K_INLINE MergedConductivityStatus
merged_conductivity_tensor(int, const double*, const double*, const int*, MergedConductivity*) {
    return MergedConductivityStatus::EmptyGroup;
}
} // namespace chipcae::material::detail

#undef CHIPCAE_MERGED_K_INLINE
