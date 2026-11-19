#pragma once

#include "chipcae/schema/stack.hpp"

#include <array>
#include <optional>
#include <vector>

namespace chipcae::mesh::cut::detail {
using Point2 = std::array<Real, 2>;
using Ring2 = std::vector<Point2>;

Expected<Ring2> normalize_ring(const Ring2& ring);
bool is_convex(const Ring2& ccw);
// Expected<T> stores variant<T, Error> and cannot instantiate void.
// nullopt denotes success, as permitted by the convex-pieces brief.
std::optional<Error> check_simple(const Ring2& ccw);

struct ConvexPieces {
    std::vector<Ring2> pieces;
    std::vector<std::vector<bool>> internal_edge;
};

Expected<ConvexPieces> convex_pieces(const Ring2& ccw);
Real signed_distance(const Ring2& ccw, Point2 p);
} // namespace chipcae::mesh::cut::detail
