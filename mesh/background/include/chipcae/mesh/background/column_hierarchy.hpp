#pragma once

#include "chipcae/mesh/background/sfc.hpp"

#include <array>
#include <cstdint>

#ifdef CHIPCAE_ENABLE_KOKKOS
#include <Kokkos_Core.hpp>
#define CHIPCAE_MESH_KOKKOS_INLINE_FUNCTION KOKKOS_INLINE_FUNCTION
#else
#define CHIPCAE_MESH_KOKKOS_INLINE_FUNCTION inline
#endif

namespace chipcae::mesh {

enum class ColumnKind : std::uint8_t { Regular = 0, Cut = 1, Partial = 2, Void = 3 };

struct ColumnHierarchy {
    std::uint32_t nx0 = 0;
    std::uint32_t ny0 = 0;
    std::uint32_t levels = 1;
};

// Count values below n whose high (31 - bits) bits equal prefix.
CHIPCAE_MESH_KOKKOS_INLINE_FUNCTION constexpr std::uint64_t
prefix_count(std::uint32_t n, std::uint32_t prefix, unsigned bits) noexcept {
    const std::uint64_t width = std::uint64_t{1} << (31 - bits);
    const std::uint64_t begin = std::uint64_t{prefix} * width;
    return n > begin ? ((n < begin + width ? n : begin + width) - begin) : 0;
}

// Dense rank in Morton order for a possibly rectangular, non-power-of-two grid.
CHIPCAE_MESH_KOKKOS_INLINE_FUNCTION constexpr std::uint64_t
level0_dense(std::uint32_t nx, std::uint32_t ny, std::uint32_t i, std::uint32_t j) noexcept {
    std::uint64_t rank = 0;
    std::uint32_t px = 0, py = 0;
    for (int b = 30; b >= 0; --b) {
        const auto bit = std::uint32_t{1} << b;
        const unsigned fixed = static_cast<unsigned>(30 - b);
        if ((j & bit) != 0) {
            rank += prefix_count(nx, px, fixed) * prefix_count(ny, py << 1, fixed + 1);
        }
        py = (py << 1) | ((j & bit) != 0);
        if ((i & bit) != 0) {
            rank += prefix_count(nx, px << 1, fixed + 1) * prefix_count(ny, py, fixed + 1);
        }
        px = (px << 1) | ((i & bit) != 0);
    }
    return rank;
}

CHIPCAE_MESH_KOKKOS_INLINE_FUNCTION std::uint64_t dense_index(std::uint32_t nx0,
                                                              std::uint32_t ny0,
                                                              std::uint32_t level,
                                                              std::uint32_t i,
                                                              std::uint32_t j) noexcept {
    const std::uint32_t mask = level == 0 ? 0 : (std::uint32_t{1} << level) - 1;
    const std::uint64_t scale = std::uint64_t{1} << (2 * level);
    return scale * level0_dense(nx0, ny0, i >> level, j >> level) + morton(i & mask, j & mask);
}

CHIPCAE_MESH_KOKKOS_INLINE_FUNCTION constexpr std::uint64_t parent(std::uint64_t dense) noexcept {
    return dense >> 2;
}

CHIPCAE_MESH_KOKKOS_INLINE_FUNCTION constexpr std::array<std::uint64_t, 4>
children(std::uint64_t dense) noexcept {
    return {4 * dense, 4 * dense + 1, 4 * dense + 2, 4 * dense + 3};
}

CHIPCAE_MESH_KOKKOS_INLINE_FUNCTION constexpr std::uint32_t
child_coordinate(std::uint32_t parent_index, std::uint32_t child_bit) noexcept {
    return 2 * parent_index + child_bit;
}

// A level-k grid line is the same line at this index on the finest axis.
CHIPCAE_MESH_KOKKOS_INLINE_FUNCTION constexpr std::uint32_t finest_axis_index(
    std::uint32_t level_index, std::uint32_t level, std::uint32_t finest_level) noexcept {
    return level_index << (finest_level - level);
}

} // namespace chipcae::mesh

#undef CHIPCAE_MESH_KOKKOS_INLINE_FUNCTION
