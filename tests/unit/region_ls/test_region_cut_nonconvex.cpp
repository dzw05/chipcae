#include "region_cut_support.hpp"
using namespace sp5b;
int main() {
    return run([] {
        auto s = stack();
        auto invalid = box("L-region", 1, 1, 7, 7);
        invalid.body = "lid";
        invalid.footprint.polygon = {{1, 1}, {7, 1}, {7, 3}, {3, 3}, {3, 7}, {1, 7}};
        s.layers[0].regions = {invalid};
        const auto m = registered_mesh();
        error(s, m, "CUT_UNSUPPORTED", "L-region: non-convex polygon");
        std::reverse(s.layers[0].regions[0].footprint.polygon.begin(),
                     s.layers[0].regions[0].footprint.polygon.end());
        error(s, m, "CUT_UNSUPPORTED", "L-region: non-convex polygon");
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
            // Reporting a multiple-polygon error first would hide an invalid
            // later source. Every source is validated before deduplication.
            error(s, m, "CUT_UNSUPPORTED", "L-region: non-convex polygon");
        }
        // A shallow inward notch exceeds 1e-12 * edge_length but would vanish
        // if vertices were snapped to y=0 before convexity validation.
        s = stack();
        invalid.name = "before-snap";
        invalid.footprint.polygon = {{0, 0}, {.001, 5e-15}, {.002, 0}, {.002, .002}, {0, .002}};
        s.layers[0].regions = {invalid};
        error(s, m, "CUT_UNSUPPORTED", "before-snap: non-convex polygon");
    });
}
