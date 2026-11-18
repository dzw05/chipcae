#include "region_cut_support.hpp"
using namespace sp5b;
int main() {
    return run([] {
        constexpr double tol = 1e-14;
        struct Case {
            double coordinate;
            std::vector<Real> candidates;
            double selected;
        };
        // Positive-normal edge: nearest need not be the first admissible axis.
        // Subtracting zero preserves the exact double used by the default snap
        // tolerance, so inclusive-boundary cases do not depend on rounding.
        const std::vector<Case> positive = {
            {0, {-8e-15, 2e-15}, 2e-15},
            {0, {-2e-15, 8e-15}, -2e-15},
            {0, {tol}, tol},
            {0, {-tol}, -tol},
            {0, {std::nextafter(tol, std::numeric_limits<double>::infinity())}, 0},
            {.01, {.01 + 4 * tol}, .01}};
        for (int axis : {0, 1}) {
            for (const auto& c : positive) {
                auto s = stack();
                auto r = square("snap", -.02, -.02, 0, 0);
                r.body = "lid";
                for (auto& p : r.footprint.polygon)
                    if (p[axis] == 0)
                        p[axis] = c.coordinate;
                s.layers[0].regions = {r};
                auto m = registered_mesh();
                m.axes[axis] = c.candidates;
                const auto g = geometry(s, m, "positive-normal snapping");
                require(g.planes.size() == 4, "snap fixture retains four edges");
                const auto& p = g.planes[axis == 0 ? 1 : 2];
                std::array<Real, 3> normal{};
                normal[axis] = 1;
                require(p.n == normal && p.d == c.selected, "exact nearest signed axis offset");
            }
            // Both outward normal signs must snap. A selected coordinate of
            // zero is unambiguous under both readings of the frozen d wording.
            for (double coordinate : {0., tol, -tol, 2e-15}) {
                auto s = stack();
                auto r = square("negative-snap", 0, 0, .02, .02);
                r.body = "lid";
                for (auto& p : r.footprint.polygon)
                    if (p[axis] == 0)
                        p[axis] = coordinate;
                s.layers[0].regions = {r};
                auto m = registered_mesh();
                m.axes[axis] = {0, 9e-15};
                // With +tol, 9e-15 would be nearest: use only zero to exercise
                // the inclusive tolerance boundary without a tie.
                if (coordinate == tol)
                    m.axes[axis] = {0};
                const auto g = geometry(s, m, "negative-normal snapping to zero");
                require(g.planes.size() == 4, "negative snap fixture retains four edges");
                const auto& p = g.planes[axis == 0 ? 3 : 0];
                std::array<Real, 3> normal{};
                normal[axis] = -1;
                require(p.n == normal && p.d == 0, "negative unit normal snaps to exact zero");
            }
        }
    });
}
