#pragma once

#include "chipcae/schema/stack.hpp"

#include <array>
#include <optional>
#include <vector>

namespace chipcae::mesh::cut::detail {
using Point2 = std::array<Real, 2>;
using Ring2 = std::vector<Point2>;

// Cleanup uses GridOptions' snap tolerance and region_cut's relative collinearity
// rule. Degenerate/nonfinite input returns the outline degeneracy error.
// The result is CCW, starting at its minimum (y,x) vertex.
Expected<Ring2> normalize_ring(const Ring2& ring);
// Uses outline_cut's absolute 1e-12 half-space distance criterion.
bool is_convex(const Ring2& ccw);
// Expected<T> stores variant<T, Error> and cannot instantiate void.
// nullopt denotes success, as permitted by the convex-pieces brief.
// Crossings, contacts, overlaps and nonfinite coordinates return a simplicity
// error. Topology uses extended-precision orientation without a contact tolerance.
std::optional<Error> check_simple(const Ring2& ccw);
// cutgeom-nc 1.0.4 (I11 region and I10 PieceUnion entry classification): true iff
// every coordinate is finite, the ring keeps >= 3 points after closing-point removal
// and clean_edges, its translated-fan signed area is exactly 0, and the raw ring with
// adjacent duplicates and closing points removed fails check_simple. Callers report
// "self-intersecting polygon" when true, "degenerate polygon" otherwise.
// normalize_ring is unchanged and still returns the outline degeneracy error.
bool zero_area_nonsimple(const Ring2& raw);

struct ConvexPieces {
    std::vector<Ring2> pieces;
    std::vector<std::vector<bool>> internal_edge;
};

// Requires a ring that passed normalize_ring and check_simple. Pieces preserve
// boundary subdivisions and are sorted by their canonical (y,x) sequences.
Expected<ConvexPieces> convex_pieces(const Ring2& ccw);
// Negative inside, positive outside, exactly zero on the boundary. Empty or
// nonfinite input/query has undefined distance and returns NaN.
Real signed_distance(const Ring2& ccw, Point2 p);
} // namespace chipcae::mesh::cut::detail
