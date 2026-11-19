#pragma once

#include "chipcae/mesh/background/hex_mesh_arrays.hpp"
#include "chipcae/mesh/cut/cut_layer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace chipcae::solvers::fvm {
// Test-first interface scaffold. Reuses the existing error carrier; coded error
// transport is still an open contract question in the implementation brief.
using mesh::Expected;

struct FvFace {
    std::int64_t lo = -1, hi = -1;
    int axis = -1, side = -1;
    double area = 0;
    std::array<double, 3> area_vector{}, centroid{};
};
struct FvGrid {
    std::array<std::vector<double>, 3> axes;
    std::size_t nz = 0;
    std::vector<std::int64_t> cv_elem, cv_elem_gid;
    std::vector<int> cv_attribute, cv_layer, cv_kind, cv_rank;
    std::vector<double> cv_volume, cv_volfrac, cv_copper_fraction;
    std::vector<std::array<double, 3>> cv_centroid;
    std::vector<FvFace> faces;
    std::vector<std::int64_t> cv_face_off{0}, cv_faces;
    double max_identity_defect = 0;
};
Expected<FvGrid> build_grid(const mesh::HexMeshArrays&,
                            const mesh::cut::CutLayerArrays* cut = nullptr);
std::vector<double> copper_fraction(const FvGrid&, const mesh::HexMeshArrays&);
} // namespace chipcae::solvers::fvm
