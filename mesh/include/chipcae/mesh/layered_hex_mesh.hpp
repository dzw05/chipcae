#pragma once

#include "chipcae/core/types.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace chipcae::mesh {

struct LayerSpec {
    Real thickness{1};
    MaterialId material_id{0};
    Index divisions{1};
};

/// Structured hexahedral mesh stacked along z, intended for package warpage FEM.
class LayeredHexMesh {
public:
    LayeredHexMesh() = default;
    LayeredHexMesh(std::array<Index, 2> nxy,
                   std::array<Real, 2> size_xy,
                   std::vector<LayerSpec> layers);

    Index nx() const noexcept { return nxy_[0]; }
    Index ny() const noexcept { return nxy_[1]; }
    Index nz() const noexcept { return nz_; }
    Index node_count() const noexcept;
    Index element_count() const noexcept;
    std::size_t layer_count() const noexcept { return layers_.size(); }

private:
    std::array<Index, 2> nxy_{0, 0};
    std::array<Real, 2> size_xy_{1, 1};
    std::vector<LayerSpec> layers_;
    Index nz_{0};
};

} // namespace chipcae::mesh
