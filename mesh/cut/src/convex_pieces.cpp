#include "chipcae/mesh/cut/detail/convex_pieces.hpp"

#include <limits>

namespace chipcae::mesh::cut::detail {
// Test-first link stubs only. No caller is wired to these helpers yet.
Expected<Ring2> normalize_ring(const Ring2&) {
    return Error{"outline.unsupported", "convex pieces not implemented (test-first stub)"};
}

bool is_convex(const Ring2&) {
    return false;
}

std::optional<Error> check_simple(const Ring2&) {
    return Error{"outline.unsupported", "convex pieces not implemented (test-first stub)"};
}

Expected<ConvexPieces> convex_pieces(const Ring2&) {
    return Error{"outline.unsupported", "convex pieces not implemented (test-first stub)"};
}

Real signed_distance(const Ring2&, Point2) {
    return std::numeric_limits<Real>::quiet_NaN();
}
} // namespace chipcae::mesh::cut::detail
