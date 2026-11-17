#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef CHIPCAE_ENABLE_KOKKOS
#include <Kokkos_Core.hpp>
#define CHIPCAE_MESH_KOKKOS_INLINE_FUNCTION KOKKOS_INLINE_FUNCTION
#else
#define CHIPCAE_MESH_KOKKOS_INLINE_FUNCTION inline
#endif

namespace chipcae::mesh {

struct LevelCandidate {
    int L;
    std::size_t nx0, ny0, padded_cells;
    bool feasible;
};
struct LevelChoice {
    int L = 0, rule = 3;
    std::size_t C_L = 0, N_L = 0;
    std::vector<std::string> warnings;
};

// Invalid solver parameters throw invalid_argument. Candidates whose arithmetic
// cannot be represented in size_t are ineligible (including for direct metrics).
inline LevelChoice select_levels(std::span<const LevelCandidate> candidates,
                                 std::size_t nz,
                                 int np,
                                 std::size_t n_cap = 50000,
                                 int eq_per_node = 3,
                                 int p = 2) {
    if (np <= 0 || p <= 0 || eq_per_node <= 0)
        throw std::invalid_argument("np, p and eq_per_node must be positive");
    const auto limit = std::numeric_limits<std::size_t>::max();
    auto multiply = [=](std::size_t a, std::size_t b, std::size_t& out) {
        if (b && a > limit / b)
            return false;
        out = a * b;
        return true;
    };
    LevelChoice first, last, direct;
    bool found_first = false, found_last = false;
    std::size_t minimum = 0, preferred = 0;
    const bool valid_minimum = multiply(16, np, minimum);
    const bool valid_preferred = multiply(64, np, preferred);
    for (const auto& c : candidates) {
        if (!c.feasible || c.L < 0 || c.L > 30 || !c.nx0 || !c.ny0)
            continue;
        LevelChoice choice;
        choice.L = c.L;
        std::size_t x, y, z, xy, xyz;
        if (!multiply(c.nx0, c.ny0, choice.C_L) || !multiply(p, c.nx0, x) || x == limit ||
            !multiply(p, c.ny0, y) || y == limit || !multiply(p, nz, z) || z == limit ||
            !multiply(x + 1, y + 1, xy) || !multiply(xy, z + 1, xyz) ||
            !multiply(eq_per_node, xyz, choice.N_L))
            continue;
        if (c.L == 0) {
            direct = choice;
            continue;
        }
        if (!valid_minimum || choice.C_L < minimum)
            continue;
        if (!found_last || c.L > last.L) {
            last = choice;
            found_last = true;
        }
        if (choice.N_L <= n_cap && (!found_first || c.L < first.L)) {
            first = choice;
            found_first = true;
        }
    }
    if (found_first) {
        first.rule = 1;
        if (!valid_preferred || first.C_L < preferred)
            first.warnings.push_back("coarse columns below 64*np");
        return first;
    }
    if (found_last) {
        last.rule = 2;
        last.warnings.push_back("coarse equations exceed n_cap: L=" + std::to_string(last.L) +
                                " C_L=" + std::to_string(last.C_L) + " N_L=" +
                                std::to_string(last.N_L) + " n_cap=" + std::to_string(n_cap));
        return last;
    }
    direct.rule = 3;
    direct.warnings.push_back(
        "direct solve: no feasible L>=1 with C_L>=16*np and representable equation count");
    return direct;
}

// Each finer level has twice as many cells per axis as its coarse level, so
// fine dense indices are 4 * coarse dense + r, where r = morton(i & 1, j & 1).
// At the finest level, dense indices equal the existing tensor_grid column
// Morton index, so elem_gid = dense * nz + k remains unchanged. Coarse grid
// lines are every other fine grid line; finest_axis_index gives their index on
// the finest axis.
enum class ColumnKind : std::uint8_t {
    Regular = 0, // All nz cells active; none in cut_elem.
    Cut = 1,     // At least one cell in cut_elem.
    Partial = 2, // No cut cells, but at least one cell void or inactive.
    Void = 3     // All cells void or inactive; no degrees of freedom.
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
