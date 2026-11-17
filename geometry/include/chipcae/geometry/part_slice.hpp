#pragma once
#include "chipcae/geometry/step_import.hpp"

#include <array>
#include <string_view>
#include <vector>

namespace chipcae::geometry {
struct Ring {
    std::vector<std::array<Real, 2>> pts;
};
struct SlabSection {
    Ring outer;
    std::vector<Ring> holes;
};
struct Slab {
    Real z0, z1;
    std::vector<SlabSection> sections;
};
struct SliceOptions {
    Real chord_tol;
    Real tau_a, tau_b;
};
struct SliceResult {
    std::vector<Slab> slabs;
    std::vector<Defect> defects;
};
SliceResult slice_prismatic(const StepImportResult&, std::string_view body, const SliceOptions&);
} // namespace chipcae::geometry
