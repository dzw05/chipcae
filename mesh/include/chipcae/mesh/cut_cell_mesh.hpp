#pragma once

#include "chipcae/core/types.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace chipcae::mesh {

/// One control volume inside a cut cell. A cell may hold several of these.
struct ControlVolume {
    RegionId region_id{0};
    MaterialId material_id{0};
    BodyId body_id{0};
    Real volume{0};
};

/// Cartesian cut-cell mesh. Multi-material cells are represented as multiple
/// control volumes rather than a single cutting plane per cell.
class CutCellMesh {
public:
    CutCellMesh() = default;
    CutCellMesh(std::array<Index, 3> extents,
                std::array<Real, 3> origin,
                std::array<Real, 3> spacing);

    const std::array<Index, 3>& extents() const noexcept { return extents_; }
    Index cell_count() const noexcept;
    std::size_t control_volume_count() const noexcept { return volumes_.size(); }

    void add_control_volume(Index cell, ControlVolume volume);
    std::span<const ControlVolume> control_volumes(Index cell) const;

private:
    std::array<Index, 3> extents_{0, 0, 0};
    std::array<Real, 3> origin_{0, 0, 0};
    std::array<Real, 3> spacing_{1, 1, 1};
    std::vector<ControlVolume> volumes_;
    std::vector<std::size_t> offsets_{0};
};

} // namespace chipcae::mesh
