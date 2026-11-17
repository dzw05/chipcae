#pragma once

#include <cstdint>

#ifdef CHIPCAE_ENABLE_KOKKOS
#include <Kokkos_Core.hpp>
#define CHIPCAE_MESH_KOKKOS_INLINE_FUNCTION KOKKOS_INLINE_FUNCTION
#else
#define CHIPCAE_MESH_KOKKOS_INLINE_FUNCTION inline
#endif

namespace chipcae::mesh {

// Each finer level has twice as many cells per axis as its coarse level, so
// fine dense indices are 4 * coarse dense + r, where r = morton(i & 1, j & 1).
// At the finest level, dense indices equal the existing tensor_grid column
// Morton index, so elem_gid = dense * nz + k remains unchanged. Coarse grid
// lines are every other fine grid line; finest_axis_index gives their index on
// the finest axis.
enum class ColumnKind : std::uint8_t {
    Regular = 0, // A full column with no cut cells.
    Cut = 1,     // A column intersected by a cut surface.
    Partial = 2, // A partly cut or partly solid column.
    Void = 3     // A column with no degrees of freedom.
};

struct ColumnChildren {
    std::uint64_t d[4];
};

struct ColumnHierarchy {
    std::uint32_t nx0 = 0;
    std::uint32_t ny0 = 0;
    // Default 1 means a single level; GridOptions uses 0 to mean auto.
    std::uint32_t levels = 1;
};

// Interleave the low 31 bits of i and j, matching sfc.hpp's morton().
CHIPCAE_MESH_KOKKOS_INLINE_FUNCTION std::uint64_t morton2d(std::uint32_t i,
                                                           std::uint32_t j) noexcept {
    std::uint64_t code = 0;
    for (unsigned b = 0; b < 31; ++b) {
        code |= std::uint64_t((i >> b) & 1) << (2 * b);
        code |= std::uint64_t((j >> b) & 1) << (2 * b + 1);
    }
    return code;
}

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
    return scale * level0_dense(nx0, ny0, i >> level, j >> level) + morton2d(i & mask, j & mask);
}

CHIPCAE_MESH_KOKKOS_INLINE_FUNCTION constexpr std::uint64_t parent(std::uint64_t dense) noexcept {
    return dense >> 2;
}

CHIPCAE_MESH_KOKKOS_INLINE_FUNCTION constexpr ColumnChildren
children(std::uint64_t dense) noexcept {
    return {{4 * dense, 4 * dense + 1, 4 * dense + 2, 4 * dense + 3}};
}

CHIPCAE_MESH_KOKKOS_INLINE_FUNCTION constexpr std::uint32_t
child_coordinate(std::uint32_t parent_index, std::uint32_t child_bit) noexcept {
    return 2 * parent_index + child_bit;
}

// Map a coarse grid line to the corresponding every-other fine grid line.
CHIPCAE_MESH_KOKKOS_INLINE_FUNCTION constexpr std::uint32_t finest_axis_index(
    std::uint32_t level_index, std::uint32_t level, std::uint32_t finest_level) noexcept {
    return level_index << (finest_level - level);
}

} // namespace chipcae::mesh

#undef CHIPCAE_MESH_KOKKOS_INLINE_FUNCTION
