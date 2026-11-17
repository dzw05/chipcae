#include "support.hpp"
using namespace sp5;
int main() {
    return run([] {
        // A nearly tied upper connector must not hide the coarser lower band.
        // y widths are 2.1/8=.2625 below the feature and 5.9/23 above it.
        for (bool reverse : {false, true}) {
            auto s = stack();
            auto a = square("a", 2, 1, 3, 6);
            auto b = square("b", 3.52, 1, 4.52, 6);
            b.footprint.polygon[0][0] += 2e-15;
            b.footprint.polygon[3][0] -= 2e-15;
            a.footprint.box = b.footprint.box = s.outline.box;
            if (reverse) {
                std::reverse(a.footprint.polygon.begin(), a.footprint.polygon.end());
                std::reverse(b.footprint.polygon.begin(), b.footprint.polygon.end());
            }
            s.layers[0].regions = {a, b, box("grading", 0, 0, 8, 2.1)};
            GridOptions o;
            o.nx = 32;
            o.ny = 8;
            o.levels = 1;
            auto result = build_tensor_grid(s, o);
            require(!result && result.error().path == "CUT_UNSUPPORTED",
                    "all numerically tied connectors contribute to local h");
        }
        for (double delta : {-2e-15, 0., 2e-15}) {
            for (bool reverse : {false, true}) {
                auto s = stack();
                auto a = square("a", 2, 2, 3, 6);
                auto b = square("b", 3.5, 2, 4.5, 6);
                // Almost-parallel facing edges produce numerically tied connectors.
                b.footprint.polygon[0][0] += delta;
                b.footprint.polygon[3][0] -= delta;
                a.footprint.box = b.footprint.box = s.outline.box;
                if (reverse) {
                    std::reverse(a.footprint.polygon.begin(), a.footprint.polygon.end());
                    std::reverse(b.footprint.polygon.begin(), b.footprint.polygon.end());
                }
                s.layers[0].regions = {a, b};
                GridOptions o;
                o.nx = o.ny = 32;
                o.levels = 1;
                require(bool(build_tensor_grid(s, o)), "2h tied connectors tolerate roundoff");
                // Near-coincident and tangent polygons must always fail spacing.
                for (auto& p : s.layers[0].regions[1].footprint.polygon)
                    p[0] -= .5;
                auto result = build_tensor_grid(s, o);
                require(!result && result.error().path == "CUT_UNSUPPORTED",
                        "near-coincident polygons fail spacing in either winding");
            }
        }
    });
}
