#include "assembly_test_support.hpp"
int main() {
    return run([] {
        auto b = base();
        b.outline.box = {-.03, -.03, .06, .06};
        auto p = part("ring");
        p.slabs[0].sections = {
            {rect(-.02, -.02, .04, .04).outer, {rect(-.015, -.015, .03, .03).outer}}};
        auto m = merge(b, {p});
        const auto& rs = m.stack.layers.back().regions;
        const std::vector<std::array<double, 4>> expected = {{-.02, -.02, -.015, .02},
                                                             {-.015, -.02, .015, -.015},
                                                             {-.015, .015, .015, .02},
                                                             {.015, -.02, .02, .02}};
        check(rs.size() == 4, "ring must produce four x-strip boxes");
        double sum = 0;
        for (size_t i = 0; i < 4; ++i) {
            check(rs[i].footprint.box == expected[i], "exact sorted ring box");
            check(rs[i].footprint.polygon.empty() && rs[i].body == "ring", "ring boxes and body");
            sum += (expected[i][2] - expected[i][0]) * (expected[i][3] - expected[i][1]);
        }
        near(sum, .04 * .04 - .03 * .03, (.04 * .04 - .03 * .03) * 1e-12);
        p.slabs[0].sections = {ell()};
        auto l = merge(b, {p});
        const auto& lr = l.stack.layers.back().regions;
        check(lr.size() == 2, "L must produce two boxes");
        check(lr[0].footprint.box == std::array<double, 4>{0, 0, .01, .04} &&
                  lr[1].footprint.box == std::array<double, 4>{.01, 0, .04, .01},
              "exact L boxes");
        for (auto& r : lr)
            check(r.footprint.polygon.empty(), "L boxes only");
    });
}
