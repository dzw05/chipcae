#pragma once

#include "chipcae/schema/stack.hpp"

#include <array>
#include <vector>

namespace chipcae::mesh {

using CopperPoint = std::array<Real, 2>;
using CopperPath = std::vector<CopperPoint>;

// Paths form a NonZero-fill polygon set (hole rings have opposite winding).
// Output is row-major in the supplied axes: x changes fastest.
std::vector<Real> rasterize_copper(const std::vector<Real>& x_lines,
                                   const std::vector<Real>& y_lines,
                                   const std::vector<CopperPath>& paths);

// Aggregate a dense Morton-ordered fine field into `level` coarser levels.
// Fine cell widths are taken from the axes; children use dense >> 2.
std::vector<Real> aggregate_phi(const std::vector<Real>& fine,
                                const std::vector<Real>& x_lines,
                                const std::vector<Real>& y_lines,
                                Local level);

} // namespace chipcae::mesh
