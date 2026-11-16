#include "chipcae/mesh/layered_hex_mesh.hpp"

#include <stdexcept>
#include <utility>

namespace chipcae::mesh {

LayeredHexMesh::LayeredHexMesh(std::array<Index, 2> nxy,
                               std::array<Real, 2> size_xy,
                               std::vector<LayerSpec> layers)
    : nxy_(nxy), size_xy_(size_xy), layers_(std::move(layers)) {
    if (nxy_[0] <= 0 || nxy_[1] <= 0) {
        throw std::invalid_argument("LayeredHexMesh in-plane divisions must be positive");
    }
    if (size_xy_[0] <= 0 || size_xy_[1] <= 0) {
        throw std::invalid_argument("LayeredHexMesh in-plane size must be positive");
    }
    if (layers_.empty()) {
        throw std::invalid_argument("LayeredHexMesh requires at least one layer");
    }
    nz_ = 0;
    for (const auto& layer : layers_) {
        if (layer.thickness <= 0 || layer.divisions <= 0) {
            throw std::invalid_argument("Layer thickness and divisions must be positive");
        }
        nz_ += layer.divisions;
    }
}

Index LayeredHexMesh::node_count() const noexcept {
    return (nxy_[0] + 1) * (nxy_[1] + 1) * (nz_ + 1);
}

Index LayeredHexMesh::element_count() const noexcept {
    return nxy_[0] * nxy_[1] * nz_;
}

} // namespace chipcae::mesh
