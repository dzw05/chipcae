#include "region_cut_support.hpp"

#include <numbers>
using namespace sp5b;
int main() {
    return run([] {
        auto s = stack();
        auto r = square("rotated-region", 2, 3, 6, 5);
        r.body = "lid";
        const double angle = std::numbers::pi / 12;
        for (auto& p : r.footprint.polygon) {
            const double x = p[0] - 4, y = p[1] - 4;
            p = {4 + x * std::cos(angle) - y * std::sin(angle),
                 4 + x * std::sin(angle) + y * std::cos(angle)};
        }
        s.layers[0].material = "substrate";
        s.layers[0].regions = {r};
        // Direct API call: tensor_grid already rejects this input upstream.
        error(s, registered_mesh(), "CUT_UNSUPPORTED", "rotated-region: polygon in material layer");
        auto upper = s.layers[0];
        upper.name = "L#1";
        s.layers[0].material.clear();
        s.layers.push_back(upper);
        error(s, registered_mesh(), "CUT_UNSUPPORTED", "rotated-region: polygon in material layer");
    });
}
