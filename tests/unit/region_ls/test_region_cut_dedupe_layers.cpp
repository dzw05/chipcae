#include "region_cut_support.hpp"
using namespace sp5b;
int main() {
    return run([] {
        auto s = chamfer_stack();
        const auto m = registered_mesh();
        const auto single = geometry(s, m, "single polygon source");
        require(single.planes.size() == 8, "single source reference has eight planes");
        const auto original = s;
        for (bool same_layer : {false, true}) {
            s = original;
            auto duplicate = s.layers[0].regions[0];
            duplicate.name = "second-region";
            if (same_layer)
                s.layers[0].regions.push_back(duplicate);
            else {
                auto upper = s.layers[0];
                upper.name = "L#1";
                upper.regions = {duplicate};
                s.layers.push_back(upper);
            }
            auto& second = same_layer ? s.layers[0].regions[1] : s.layers[1].regions[0];
            exact_planes(geometry(s, m, "identical duplicate"), single);
            std::reverse(second.footprint.polygon.begin(), second.footprint.polygon.end());
            second.footprint.polygon.push_back(second.footprint.polygon.front());
            exact_planes(geometry(s, m, "duplicate normalized independently"), single);
            second.footprint.polygon = {{0, 0}, {.056, 0}, {.056, .044}, {0, .044}};
            error(s, m, "CUT_UNSUPPORTED", "lid: body has multiple distinct cut polygons");
        }
        // Exact ring comparison precedes snapping. Distinct rings that would
        // snap to the same rectangle must still be rejected.
        s = original;
        auto a = box("first", 0, 0, .056, .044);
        a.body = "lid";
        a.footprint.polygon = {{0, 0}, {.056, 0}, {.056, .044}, {0, .044}};
        auto b = a;
        b.name = "second";
        b.footprint.polygon[1][0] += 1e-16;
        b.footprint.polygon[2][0] += 1e-16;
        s.layers[0].regions = {a, b};
        error(s, m, "CUT_UNSUPPORTED", "lid: body has multiple distinct cut polygons");
    });
}
