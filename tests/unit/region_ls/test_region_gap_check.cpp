#include "support.hpp"
using namespace sp5;
int main() {
    return run([] {
        auto check = [](StackModel s, bool reject, const char* first, const char* second) {
            // Conservative polygon bounds keep a uniform h=8/32 independent of the gap.
            // Polygon vertices, rather than these bounds, define its geometry.
            for (auto& layer : s.layers)
                for (auto& r : layer.regions)
                    r.footprint.box = s.outline.box;
            GridOptions o;
            o.nx = o.ny = 32;
            o.levels = 1;
            auto result = build_tensor_grid(s, o);
            require(bool(result) != reject, "build_tensor_grid gap acceptance threshold");
            if (!result) {
                std::string reason;
                if (s.layers[0].material.size())
                    reason = "in_solid: polygon in material layer";
                else if (s.layers[0].regions[0].name == "edge")
                    reason = "edge outline: spacing < 2h";
                else
                    reason = "a b: spacing < 2h";
                require(result.error().reason == reason, "exact cut precondition reason");
                require(result.error().path == "CUT_UNSUPPORTED", "exact error code");
                require(result.error().reason.find(first) != std::string::npos &&
                            result.error().reason.find(second) != std::string::npos,
                        "gap diagnostic names offending regions");
            }
            // Validation belongs solely to build_tensor_grid, even for unsupported geometry.
            auto m = samples(32);
            require(!levels(s, m).empty(), "level sets do not perform cut preconditions");
        };
        for (bool same_material : {false, true})
            for (double gap : {.375, .625}) {
                auto s = stack();
                auto a = square("a", 2, 2, 3, 3), b = square("b", 3 + gap, 2, 4 + gap, 3);
                b.material = same_material ? a.material : "other";
                s.layers[0].regions = {a, b};
                check(s, gap == .375, "a", "b");
                auto upper = s.layers[0];
                upper.name = "upper";
                upper.regions = {b};
                s.layers[0].regions = {a};
                s.layers.push_back(upper);
                check(s, false, "a", "b");
                // Even coincident different-body polygons are independent across layers.
                s.layers[1].regions[0].footprint = a.footprint;
                check(s, false, "a", "b");
                s.layers[1].regions[0].body = a.body;
                check(s, false, "a", "b");
            }
        auto s = stack();
        s.outline.polygon = {{0, 0}, {8, 0}, {8, 8}, {0, 8}};
        s.layers[0].regions = {square("edge", .375, 2, 2, 4)};
        check(s, true, "edge", "outline");
        for (bool reverse : {false, true})
            for (int side = 0; side < 4; ++side)
                for (double gap : {.375, .5, .625}) {
                    s = stack();
                    auto r = square("edge", gap, 2, 2, 4);
                    for (auto& p : r.footprint.polygon)
                        for (int turn = 0; turn < side; ++turn)
                            p = {8 - p[1], p[0]};
                    if (reverse)
                        std::reverse(r.footprint.polygon.begin(), r.footprint.polygon.end());
                    s.layers[0].regions = {r};
                    check(s, gap < .5, "edge", "outline");
                }
        // Sharing one outline edge must not exempt a different nearby edge.
        s = stack();
        s.layers[0].regions = {square("edge", 0, .375, 2, 4)};
        check(s, true, "edge", "outline");
        s.layers[0].regions = {square("edge", 0, 2, 2, 4)};
        check(s, false, "edge", "outline");
        s = stack();
        s.layers[0].material = "background";
        s.layers[0].regions = {square("in_solid", 2, 2, 6, 6)};
        check(s, true, "", "");
    });
}
