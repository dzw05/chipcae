#include "assembly_test_support.hpp"
int main() {
    return run([] {
        auto p = part("ring");
        auto s = rect(.002, .002, .012, .012);
        s.holes = {Ring{{{.004, .004}, {.004, .006}, {.006, .006}, {.006, .004}}}};
        p.slabs[0].sections = {s};
        auto m = merge(base(), {p});
        double sum = 0;
        for (auto& v : m.stack.layers.back().regions) {
            check(v.body == "ring", "ring body");
            check(!v.footprint.polygon.empty(), "ring must be polygon");
            sum += std::abs(area(v.footprint.polygon));
        }
        near(sum, .00014, .00014 * 1e-12);
        // Sloped outer/hole edges exercise nonrectangular bridge fragments.
        auto diamond = part("diamond");
        diamond.slabs[0].sections = {
            {Ring{{{.002, .008}, {.008, .002}, {.014, .008}, {.008, .014}}},
             {Ring{{{.005, .008}, {.008, .011}, {.011, .008}, {.008, .005}}}}}};
        auto d = merge(base(), {diamond});
        double diamond_area = 0;
        for (const auto& region : d.stack.layers.back().regions) {
            diamond_area += std::abs(area(region.footprint.polygon));
            check(!covers(region.footprint, .008, .008), "hole center stays empty");
        }
        near(diamond_area, .000054, .000054 * 1e-12);
    });
}
