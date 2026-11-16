#include "chipcae/mesh/cut_cell_mesh.hpp"

#include <stdexcept>
#include <utility>

namespace chipcae::mesh {

CutCellMesh::CutCellMesh(std::array<Index, 3> extents,
                         std::array<Real, 3> origin,
                         std::array<Real, 3> spacing)
    : extents_(extents), origin_(origin), spacing_(spacing) {
    if (extents_[0] <= 0 || extents_[1] <= 0 || extents_[2] <= 0) {
        throw std::invalid_argument("CutCellMesh extents must be positive");
    }
    if (spacing_[0] <= 0 || spacing_[1] <= 0 || spacing_[2] <= 0) {
        throw std::invalid_argument("CutCellMesh spacing must be positive");
    }
    const auto count = static_cast<std::size_t>(cell_count());
    offsets_.assign(count + 1, 0);
}

Index CutCellMesh::cell_count() const noexcept {
    return extents_[0] * extents_[1] * extents_[2];
}

void CutCellMesh::add_control_volume(Index cell, ControlVolume volume) {
    if (cell < 0 || cell >= cell_count()) {
        throw std::out_of_range("CutCellMesh cell index out of range");
    }
    const auto cell_index = static_cast<std::size_t>(cell);
    const std::size_t insert_at = offsets_[cell_index + 1];
    volumes_.insert(volumes_.begin() + static_cast<std::ptrdiff_t>(insert_at), volume);
    for (std::size_t i = cell_index + 1; i < offsets_.size(); ++i) {
        ++offsets_[i];
    }
}

std::span<const ControlVolume> CutCellMesh::control_volumes(Index cell) const {
    if (cell < 0 || cell >= cell_count()) {
        throw std::out_of_range("CutCellMesh cell index out of range");
    }
    const auto cell_index = static_cast<std::size_t>(cell);
    const std::size_t begin = offsets_[cell_index];
    const std::size_t end = offsets_[cell_index + 1];
    return std::span<const ControlVolume>(volumes_.data() + begin, end - begin);
}

} // namespace chipcae::mesh
