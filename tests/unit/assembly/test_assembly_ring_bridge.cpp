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
    });
}
