#pragma once
#include "chipcae/core/types.hpp"

#include <array>
#include <vector>
namespace chipcae::mesh::cut {
// SI volumes/areas; zero-based CSR. Layout only: no merging or redistribution.
struct CvLayout {
    std::vector<std::int32_t> elem_cv_off, cv_elem, cv_attribute;
    std::vector<Index> cv_gid;
    std::vector<RegionId> cv_region;
    std::vector<MaterialId> cv_material;
    std::vector<BodyId> cv_body;
    std::vector<Real> cv_volume;
    std::array<std::vector<Real>, 3> cv_centroid;
    std::array<std::vector<std::int32_t>, 2> face_cv;
    std::array<std::vector<Real>, 3> face_normal;
    std::vector<Real> face_area;
};
} // namespace chipcae::mesh::cut
