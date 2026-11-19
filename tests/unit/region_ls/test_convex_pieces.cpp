#include "chipcae/mesh/cut/detail/convex_pieces.hpp"
#include "support.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <numbers>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace cut = chipcae::mesh::cut::detail;
using chipcae::mesh::Error;
using cut::ConvexPieces;
using cut::Point2;
using cut::Ring2;
using Bits = std::array<std::uint64_t, 2>;
using Edge = std::pair<std::size_t, std::size_t>;

struct InternalEdge {
    Edge endpoints;
    std::size_t piece;
};

void require(bool ok, const std::string& message) {
    sp5::require(ok, message.c_str());
}

Bits bits(const Point2& p) {
    return {std::bit_cast<std::uint64_t>(p[0]), std::bit_cast<std::uint64_t>(p[1])};
}

bool same_ring(const Ring2& a, const Ring2& b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (bits(a[i]) != bits(b[i]))
            return false;
    return true;
}

bool yx_less(const Point2& a, const Point2& b) {
    return std::pair{a[1], a[0]} < std::pair{b[1], b[0]};
}

bool ring_less(const Ring2& a, const Ring2& b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), yx_less);
}

// Reference geometry uses extended precision and no production predicates.
long double turn(const Point2& a, const Point2& b, const Point2& c) {
    const auto ux = static_cast<long double>(b[0]) - a[0];
    const auto uy = static_cast<long double>(b[1]) - a[1];
    const auto vx = static_cast<long double>(c[0]) - a[0];
    const auto vy = static_cast<long double>(c[1]) - a[1];
    return ux * vy - uy * vx;
}

long double area(const Ring2& ring) {
    long double twice = 0;
    for (std::size_t i = 1; i + 1 < ring.size(); ++i)
        twice += turn(ring[0], ring[i], ring[i + 1]);
    return twice / 2;
}

const Ring2 l_shape = {
    {0, 0}, {0.02, 0}, {0.02, 0.0137}, {0.0113, 0.0137}, {0.0113, 0.02}, {0, 0.02}};
const Ring2 u_shape = {{0, 0},
                       {0.02, 0},
                       {0.02, 0.02},
                       {0.0129, 0.02},
                       {0.0129, 0.0123},
                       {0.0071, 0.0123},
                       {0.0071, 0.02},
                       {0, 0.02}};
const Ring2 v_shape = {
    {0, 0}, {0.0064, 0}, {0.0103, 0.0087}, {0.0142, 0}, {0.02, 0}, {0.02, 0.02}, {0, 0.02}};
const Ring2 r_shape = {{0, 0},
                       {0.0086, 0},
                       {0.0086, 0.0043},
                       {0.0043, 0.0043},
                       {0.0043, 0.0157},
                       {0.0157, 0.0157},
                       {0.0157, 0.0043},
                       {0.0117, 0.0043},
                       {0.0117, 0},
                       {0.02, 0},
                       {0.02, 0.02},
                       {0, 0.02}};
const Ring2 square = {{0, 0}, {0.02, 0}, {0.02, 0.02}, {0, 0.02}};

Ring2 g_shape() {
    Ring2 ring;
    constexpr std::size_t n = 2000;
    ring.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double radius = 0.01 + (i % 2 == 0 ? 0.003 : -0.003);
        const double theta = 2 * std::numbers::pi * static_cast<double>(i) / n;
        ring.push_back({0.01 + radius * std::cos(theta), 0.01 + radius * std::sin(theta)});
    }
    // This fixture is already CCW and simple; only select its canonical start.
    std::rotate(ring.begin(), std::min_element(ring.begin(), ring.end(), yx_less), ring.end());
    return ring;
}

Ring2 normalize(const Ring2& input) {
    const auto before = input;
    const auto result = cut::normalize_ring(input);
    require(same_ring(input, before), "normalize_ring must not mutate its input");
    if (!result)
        require(false,
                "normalize_ring must succeed: " + result.error().path + ": " +
                    result.error().reason);
    return result.value();
}

ConvexPieces decompose(const Ring2& ring) {
    const auto before = ring;
    const auto result = cut::convex_pieces(ring);
    require(same_ring(ring, before), "convex_pieces must not mutate its input");
    if (!result)
        require(false,
                "convex_pieces must succeed: " + result.error().path + ": " +
                    result.error().reason);
    return result.value();
}

void require_same(const ConvexPieces& actual, const ConvexPieces& expected) {
    require(actual.pieces.size() == expected.pieces.size(), "deterministic piece count");
    require(actual.internal_edge == expected.internal_edge, "deterministic internal-edge flags");
    for (std::size_t k = 0; k < actual.pieces.size(); ++k)
        require(same_ring(actual.pieces[k], expected.pieces[k]),
                "bitwise deterministic coordinates");
}

void require_error(const Error& error, const char* reason) {
    require(error.path == "outline.unsupported", "exact error path must be outline.unsupported");
    require(error.reason == reason,
            "expected reason '" + std::string(reason) + "', got '" + error.reason + "'");
}

bool opposite(long double a, long double b) {
    return (a < 0 && b > 0) || (a > 0 && b < 0);
}

bool proper_crossing(const Point2& a, const Point2& b, const Point2& c, const Point2& d) {
    return opposite(turn(a, b, c), turn(a, b, d)) && opposite(turn(c, d, a), turn(c, d, b));
}

bool strictly_inside(const Ring2& ccw, const Point2& p) {
    for (std::size_t i = 0; i < ccw.size(); ++i)
        if (turn(ccw[i], ccw[(i + 1) % ccw.size()], p) <= 0)
            return false;
    return true;
}

// An edge normal separates two convex interiors if every vertex of the other
// polygon is on its exterior side. This also catches coincident overlapping
// polygons, which crossing/strict-vertex-containment checks alone miss.
bool has_separating_edge(const Ring2& a, const Ring2& b) {
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto& p = a[i];
        const auto& q = a[(i + 1) % a.size()];
        bool separated = true;
        for (const auto& v : b) {
            if (turn(p, q, v) > 0) {
                separated = false;
                break;
            }
        }
        if (separated)
            return true;
    }
    return false;
}

std::array<double, 4> bounds(const Ring2& ring) {
    std::array<double, 4> box{ring[0][0], ring[0][1], ring[0][0], ring[0][1]};
    for (const auto& p : ring) {
        box[0] = std::min(box[0], p[0]);
        box[1] = std::min(box[1], p[1]);
        box[2] = std::max(box[2], p[0]);
        box[3] = std::max(box[3], p[1]);
    }
    return box;
}

void require_disjoint(const std::vector<Ring2>& pieces) {
    std::vector<std::array<double, 4>> boxes;
    for (const auto& p : pieces)
        boxes.push_back(bounds(p));
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        for (std::size_t j = i + 1; j < pieces.size(); ++j) {
            const auto& x = boxes[i];
            const auto& y = boxes[j];
            if (x[2] <= y[0] || y[2] <= x[0] || x[3] <= y[1] || y[3] <= x[1])
                continue;
            const auto& a = pieces[i];
            const auto& b = pieces[j];
            require(has_separating_edge(a, b) || has_separating_edge(b, a),
                    "piece interiors overlap (independent separating-axis check)");
            for (std::size_t u = 0; u < a.size(); ++u) {
                require(!strictly_inside(b, a[u]), "piece vertex strictly inside another piece");
                for (std::size_t v = 0; v < b.size(); ++v)
                    require(
                        !proper_crossing(a[u], a[(u + 1) % a.size()], b[v], b[(v + 1) % b.size()]),
                        "piece boundaries properly cross");
            }
            for (const auto& p : b)
                require(!strictly_inside(a, p), "piece vertex strictly inside another piece");
        }
    }
}

void require_partition(const Ring2& ring, const ConvexPieces& result, long double reference_area) {
    require(!result.pieces.empty(), "decomposition must not be empty");
    require(result.internal_edge.size() == result.pieces.size(), "one flag vector per piece");
    require(std::is_sorted(result.pieces.begin(), result.pieces.end(), ring_less),
            "pieces must be sorted by canonical (y,x) sequences");
    std::size_t reflex = 0;
    for (std::size_t i = 0; i < ring.size(); ++i)
        if (turn(ring[(i + ring.size() - 1) % ring.size()], ring[i], ring[(i + 1) % ring.size()]) <
            0)
            ++reflex;
    require(result.pieces.size() <= 2 * reflex + 1, "Hertel-Mehlhorn piece-count bound");

    std::map<Bits, std::size_t> ids;
    for (std::size_t i = 0; i < ring.size(); ++i)
        require(ids.emplace(bits(ring[i]), i).second, "reference ring vertices must be unique");
    std::map<Edge, std::size_t> boundary;
    std::map<Edge, std::vector<InternalEdge>> internal;
    long double sum = 0;
    for (std::size_t k = 0; k < result.pieces.size(); ++k) {
        const auto& p = result.pieces[k];
        require(p.size() >= 3, "piece needs at least three vertices");
        require(result.internal_edge[k].size() == p.size(), "one internal flag per directed edge");
        require(area(p) > 0, "piece must have positive area and CCW winding");
        require(cut::is_convex(p), "every piece must satisfy is_convex");
        require(std::min_element(p.begin(), p.end(), yx_less) == p.begin(),
                "piece starts at its minimum (y,x) vertex");
        sum += area(p);
        for (std::size_t i = 0; i < p.size(); ++i) {
            const auto from = ids.find(bits(p[i]));
            const auto to = ids.find(bits(p[(i + 1) % p.size()]));
            require(from != ids.end() && to != ids.end(), "piece uses only original vertices");
            const auto a = from->second;
            const auto b = to->second;
            require(a != b, "piece edge must have distinct endpoints");
            const bool diagonal = (a + 1) % ring.size() != b && (b + 1) % ring.size() != a;
            require(result.internal_edge[k][i] == diagonal,
                    "flag must identify an internal diagonal");
            if (diagonal)
                internal[{std::min(a, b), std::max(a, b)}].push_back({{a, b}, k});
            else
                ++boundary[{a, b}];
        }
    }
    require(std::abs(sum - reference_area) <= 1e-12L * reference_area,
            "sum of piece areas must match analytic area within relative 1e-12");
    require(boundary.size() == ring.size(),
            "unflagged edges must cover exactly the original boundary");
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const auto edge = boundary.find({i, (i + 1) % ring.size()});
        require(edge != boundary.end() && edge->second == 1,
                "every original directed boundary edge occurs exactly once unflagged");
    }
    for (const auto& [edge, occurrences] : internal) {
        (void)edge;
        require(occurrences.size() == 2, "every internal edge occurs exactly twice");
        require(occurrences[0].piece != occurrences[1].piece,
                "an internal edge must be shared by two different pieces");
        require(occurrences[0].endpoints.first == occurrences[1].endpoints.second &&
                    occurrences[0].endpoints.second == occurrences[1].endpoints.first,
                "paired internal edges have opposite directions");
    }
    require_disjoint(result.pieces);
}

// Catches lost area, overlapping pieces, broken adjacency/flags, nonconvex
// output, and failure to merge enough triangles, using only synthetic fixtures.
void pieces_analytic() {
    const std::array<std::pair<const Ring2*, long double>, 4> cases{{{&l_shape, 3.4519e-4L},
                                                                     {&u_shape, 3.5534e-4L},
                                                                     {&v_shape, 3.6607e-4L},
                                                                     {&r_shape, 2.5671e-4L}}};
    for (const auto& [ring, reference] : cases)
        require_partition(*ring, decompose(*ring), reference);
    const auto g = g_shape();
    // 2000 * (1/2) * r_even * r_odd * sin(2*pi/2000), independent of vertices.
    const long double reference =
        1000 * 0.013L * 0.007L * std::sin(2 * std::numbers::pi_v<long double> / 2000);
    require_partition(g, decompose(g), reference);
}

void convex_single_piece() {
    constexpr double c = 0.003;
    const Ring2 chamfer = {{c, 0},
                           {0.02 - c, 0},
                           {0.02, c},
                           {0.02, 0.02 - c},
                           {0.02 - c, 0.02},
                           {c, 0.02},
                           {0, 0.02 - c},
                           {0, c}};
    for (const auto& input : {square, chamfer}) {
        const auto result = decompose(input);
        require(result.pieces.size() == 1, "convex input must take the single-piece fast path");
        require(same_ring(result.pieces[0], input),
                "convex fast path preserves coordinates bitwise");
        require(same_ring(result.pieces[0], normalize(input)),
                "convex piece equals normalized ring");
        require(result.internal_edge.size() == 1 && result.internal_edge[0].size() == input.size(),
                "convex fast path flag dimensions");
        for (bool flag : result.internal_edge[0])
            require(!flag, "convex input has no internal edges");
        require_partition(input, result, input.size() == 4 ? 4e-4L : 3.82e-4L);
    }
}

Ring2 decorated(const Ring2& ring, bool collinear, bool duplicate) {
    Ring2 result;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const auto& a = ring[i];
        const auto& b = ring[(i + 1) % ring.size()];
        result.push_back(a);
        if (duplicate)
            result.push_back(a);
        if (collinear)
            result.push_back({std::midpoint(a[0], b[0]), std::midpoint(a[1], b[1])});
    }
    return result;
}

// Catches winding/start-sensitive ear choice, cleanup, output ordering, and
// const-input mutation. Bitwise comparison is explicitly required by the brief.
void determinism() {
    for (const auto& ring : {l_shape, r_shape, g_shape()}) {
        const auto expected = decompose(ring);
        std::vector<Ring2> variants{ring};
        auto reverse = ring;
        std::reverse(reverse.begin(), reverse.end());
        variants.push_back(reverse);
        auto rotate = ring;
        std::rotate(rotate.begin(),
                    rotate.begin() + static_cast<Ring2::difference_type>(ring.size() / 3),
                    rotate.end());
        variants.push_back(rotate);
        auto closed = ring;
        closed.push_back(closed.front());
        closed.push_back(closed.front());
        variants.push_back(closed);
        variants.push_back(decorated(ring, true, false));
        variants.push_back(decorated(ring, false, true));
        auto combined = decorated(ring, true, true);
        std::reverse(combined.begin(), combined.end());
        std::rotate(combined.begin(), combined.begin() + 1, combined.end());
        combined.push_back(combined.front());
        variants.push_back(combined);
        for (const auto& input : variants) {
            const auto normalized = normalize(input);
            require(same_ring(normalized, ring),
                    "variants normalize to identical reference vertices");
            const auto before = normalized;
            require(!cut::check_simple(normalized), "valid normalized ring must be simple");
            require(same_ring(normalized, before), "check_simple must not mutate its input");
            require_same(decompose(normalized), expected);
        }
    }
}

void signed_distance() {
    const std::array<std::pair<Point2, double>, 3> cases{
        {{{0.005, 0.005}, -0.005}, {{0.015, 0.015}, 0.0013}, {{0.0113, 0.0137}, 0}}};
    for (const auto& [p, expected] : cases) {
        const double actual = cut::signed_distance(l_shape, p);
        require(std::isfinite(actual) && std::abs(actual - expected) <= 1e-15,
                "L signed distance must match analytic value within absolute 1e-15");
    }
}

void errors() {
    const std::vector<Ring2> nonsimple = {
        {{0, 0}, {0.02, 0.02}, {0.02, 0}, {0, 0.02}},
        {{0, 0}, {0.02, 0}, {0.02, 0.02}, {0.01, 0}, {0, 0.02}},
        {{0, 0}, {0.01, 0}, {0.01, 0.01}, {0, 0}, {-0.01, 0.01}},
        {{0, 0}, {0.02, 0}, {0.02, 0.02}, {0.005, 0.02}, {0.005, 0}, {0.015, 0}, {0, 0.02}},
        {{0, 0}, {0.02, 0}, {0.01, 0}, {0.02, 0.02}, {0, 0.02}}};
    for (const auto& ring : nonsimple) {
        // In particular, the zero-area bow-tie is checked directly: normalization
        // has its own specified zero-area error, so it cannot precede this check.
        const auto error = cut::check_simple(ring);
        require(error.has_value(), "crossing, touching, and overlapping outlines must be rejected");
        require_error(*error, "self-intersecting outline");
    }
    for (const auto& ring :
         std::vector<Ring2>{{}, {{0, 0}}, {{0, 0}, {0.02, 0}}, {{0, 0}, {0.01, 0}, {0.02, 0}}}) {
        const auto result = cut::normalize_ring(ring);
        require(!result, "degenerate normalization must fail");
        require_error(result.error(), "outline must be a convex polygon");
    }
}

void cleanup_boundaries() {
    const Ring2 tiny = {{0, 0}, {2e-14, 0}, {2e-14, 2e-14}, {0, 2e-14}};
    require(same_ring(normalize(tiny), tiny), "edges strictly above snap_tol must survive");
    const auto collapsed = cut::normalize_ring({{0, 0}, {1e-14, 0}, {1e-14, 1e-14}, {0, 1e-14}});
    require(!collapsed, "edges at snap_tol must collapse");
    require_error(collapsed.error(), "outline must be a convex polygon");

    const Ring2 short_seam = {{0, 0}, {0.02, 0}, {0.02, 0.02}, {0, 0.02}, {0, 5e-15}};
    require(same_ring(normalize(short_seam), square),
            "short seam retains lexicographically smaller endpoint");
    const Ring2 collinear_seam = {{0, 0.01}, {0, 0}, {0.02, 0}, {0.02, 0.02}, {0, 0.02}, {0, 0.01}};
    require(same_ring(normalize(collinear_seam), square),
            "cleanup spans cyclic collinearity and closure");
    auto reversed = short_seam;
    std::reverse(reversed.begin(), reversed.end());
    require(same_ring(normalize(reversed), square), "short seam cleanup is winding independent");
}

void predicates() {
    require(cut::is_convex(square), "square must be convex");
    require(!cut::is_convex(l_shape), "L must be nonconvex");
    require(!cut::is_convex(g_shape()), "star must be nonconvex");
    // The outline criterion is an absolute signed-distance tolerance of 1e-12,
    // whereas region_cut's length-scaled criterion would reject the first notch.
    const Ring2 shallow = {{0, 0}, {0.001, 2e-13}, {0.002, 0}, {0.002, 0.002}, {0, 0.002}};
    const Ring2 deep = {{0, 0}, {0.001, 2e-12}, {0.002, 0}, {0.002, 0.002}, {0, 0.002}};
    require(cut::is_convex(shallow), "is_convex must use outline absolute tolerance");
    require(!cut::is_convex(deep), "is_convex must reject distances above 1e-12");
    for (const auto& ring : {square, l_shape, u_shape, v_shape, r_shape})
        require(!cut::check_simple(ring), "valid simple fixture must pass check_simple");
    const Ring2 close_edges = {{0, 0}, {0.02, 0}, {0.02, 2e-14}, {0, 2e-14}};
    require(!cut::check_simple(close_edges),
            "nearby distinct edges must not be classified as touching");
}

void perf_g() {
    const auto g = g_shape();
    const auto start = std::chrono::steady_clock::now();
    const auto result = cut::convex_pieces(g);
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "perf_g: " << elapsed << " s\n";
    require(static_cast<bool>(result), "perf_g must successfully decompose 2000 vertices");
#ifdef NDEBUG
    require(elapsed <= 1.0, "Release G decomposition must finish within 1 second");
#endif
    // Generation and independent geometry checks are deliberately outside timing.
    const long double reference =
        1000 * 0.013L * 0.007L * std::sin(2 * std::numbers::pi_v<long double> / 2000);
    require_partition(g, result.value(), reference);
}
} // namespace

int main() {
    return sp5::run([] {
        const std::array<std::pair<const char*, void (*)()>, 8> cases{
            {{"pieces_analytic", pieces_analytic},
             {"convex_single_piece", convex_single_piece},
             {"determinism", determinism},
             {"signed_distance", signed_distance},
             {"errors", errors},
             {"cleanup_boundaries", cleanup_boundaries},
             {"predicates", predicates},
             {"perf_g", perf_g}}};
        std::size_t failures = 0;
        for (const auto& [name, test] : cases) {
            try {
                test();
                std::cout << "PASS: " << name << '\n';
            } catch (const std::exception& e) {
                ++failures;
                std::cerr << "FAIL: " << name << ": " << e.what() << '\n';
            }
        }
        require(failures == 0, std::to_string(failures) + " convex_pieces acceptance cases failed");
    });
}
