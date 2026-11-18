#pragma once
#include "chipcae/mesh/background/hex_mesh_arrays.hpp"
#include "chipcae/mesh/cut/cut_layer.hpp"
#include "chipcae/schema/stack.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <optional>

namespace chipcae::mesh::cut {
inline constexpr std::array<Real, 4> kSmallVfBins{1e-8, 1e-6, 1e-4, 1e-2};

struct CutStats {
    std::size_t total_cells = 0;
    std::size_t full_cells = 0;
    std::size_t cut_cells = 0;
    std::size_t sub_cells = 0;
    std::size_t void_cells = 0;
    std::size_t small_subs_recorded = 0;
    std::array<std::size_t, 4> small_vf_cumulative{};
    std::optional<Real> min_sub_volfrac;
    Real solid_volume = 0;
    std::map<BodyId, Real> body_volume;
    std::optional<Real> max_vq_relative_error;
};

// Pair the cut arrays with the mesh as it was left by the same build_cut_layer call.
inline Expected<CutStats> cut_stats(const HexMeshArrays&, const CutLayerArrays&) {
    // Test-first stub until statistics and validation are implemented.
    return CutStats{};
}
} // namespace chipcae::mesh::cut
