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

// NonZero union area in m², using native int64 coordinates without grid scaling.
// Requires finite positive dbu_m and valid, strictly increasing ring offsets.
// With ECAD disabled, throws the same runtime_error as rasterize_copper.
Real copper_union_area_m2(const PolysetLayer& layer, Real dbu_m);

// Per-cell copper fraction from an independent Clipper2 reference.
// Output layout is identical to rasterize_copper on the same axes.
// For each cell, returns Area(Union(NonZero)(layer) intersect cell) / Area(cell).
// Uses native int64 paths and dbu_m, without the rasterizer's lattice or calls.
// Shares copper_union_area_m2's full-array ring-offset validation.
// Clips the native integer union against fractional-DBU cell boundaries using
// long-double arithmetic, preserving signed ring areas for holes.
// Coordinates outside Clipper2's signed 62-bit range throw invalid_argument.
// With ECAD disabled, throws the same runtime_error text as rasterize_copper.
std::vector<Real> copper_column_ref_phi(const std::vector<Real>& x_lines,
                                        const std::vector<Real>& y_lines,
                                        const PolysetLayer& layer,
                                        Real dbu_m);

// Aggregate a dense Morton-ordered fine field into `level` coarser levels.
// Fine cell widths are taken from the axes; children use dense >> 2.
std::vector<Real> aggregate_phi(const std::vector<Real>& fine,
                                const std::vector<Real>& x_lines,
                                const std::vector<Real>& y_lines,
                                Local level);

} // namespace chipcae::mesh
