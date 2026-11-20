#include "region_cut_support.hpp"

#include <bit>
#include <cstdint>

using namespace sp5b;
namespace {
void same_ring(const Polygon& actual, const Polygon& expected) {
    require(actual.size() == expected.size(), "normalized ring vertex count");
    for (std::size_t i = 0; i < actual.size(); ++i)
        for (int d = 0; d < 2; ++d)
            require(std::bit_cast<std::uint64_t>(actual[i][d]) ==
                        std::bit_cast<std::uint64_t>(expected[i][d]),
                    "normalized unsnapped ring coordinate bits and order");
}
void distinct_error(const StackModel& s, const HexMeshArrays& m) {
    const auto result = region_cut_planes(s, m);
    require(!result, "distinct same-body rings must fail");
    require(result.error().path == "CUT_UNSUPPORTED" &&
                result.error().reason == "lid: body has multiple distinct cut polygons",
            "complete distinct-ring error path and reason");
}
} // namespace
int main() {
    return run([] {
        auto s = stack();
        auto invalid = box("L-region", 1, 1, 7, 7);
        invalid.body = "lid";
        invalid.footprint.polygon = {{1, 1}, {7, 1}, {7, 3}, {3, 3}, {3, 7}, {1, 7}};
        s.layers[0].regions = {invalid};
        const auto m = registered_mesh();
        const auto expected_ring = invalid.footprint.polygon;
        const auto check_l = [&] {
            const auto g = geometry(s, m, "simple L region");
            require(g.planes.empty(), "L region emits no legacy lid planes");
            require(g.unions.size() == 1 && g.unions[0].body == 17,
                    "L region emits exactly one lid union");
            const auto& u = g.unions[0];
            same_ring(u.ring, expected_ring);
            require(u.pieces.size() >= 2 && u.internal.size() == u.pieces.size(),
                    "L region convex decomposition and flags");
            double twice_area = 0;
            for (std::size_t i = 0; i < u.ring.size(); ++i) {
                const auto a = u.ring[i], b = u.ring[(i + 1) % u.ring.size()];
                twice_area += a[0] * b[1] - a[1] * b[0];
            }
            require(std::abs(twice_area / 2 - 20) <= 1e-12,
                    "normalized L ring is CCW with area 20");
        };
        check_l();
        std::reverse(s.layers[0].regions[0].footprint.polygon.begin(),
                     s.layers[0].regions[0].footprint.polygon.end());
        check_l();
        auto valid = square("valid-first", 1, 1, 7, 7);
        valid.body = "lid";
        for (bool same_layer : {false, true}) {
            s = stack();
            s.layers[0].regions = {valid};
            if (same_layer)
                s.layers[0].regions.push_back(invalid);
            else {
                auto upper = s.layers[0];
                upper.name = "L#1";
                upper.regions = {invalid};
                s.layers.push_back(upper);
            }
            distinct_error(s, m);
        }
        // A shallow inward notch exceeds 1e-12 * edge_length but would vanish
        // if vertices were snapped to y=0 before convexity validation.
        s = stack();
        invalid.name = "before-snap";
        invalid.footprint.polygon = {{0, 0}, {.001, 5e-15}, {.002, 0}, {.002, .002}, {0, .002}};
        s.layers[0].regions = {invalid};
        const auto g = geometry(s, m, "before-snap simple shallow notch");
        require(g.planes.empty() && g.unions.size() == 1,
                "shallow nonconvex notch produces one union");
        same_ring(g.unions[0].ring, invalid.footprint.polygon);
    });
}
