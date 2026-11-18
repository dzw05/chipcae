#include "assembly_test_support.hpp"
int main() {
    return run([] {
        auto p = part("ring");
        auto s = rect(.002, .002, .012, .012);
        s.holes = {Ring{{{.004, .004}, {.004, .006}, {.006, .006}, {.006, .004}}}};
        s.holes.push_back(Ring{{{.009, .009}, {.009, .011}, {.011, .011}, {.011, .009}}});
        p.slabs[0].sections = {s};
        auto m = merge(base(), {p});
        double sum = 0;
        for (auto& v : m.stack.layers.back().regions) {
            check(v.body == "ring", "ring body");
            check(v.footprint.polygon.empty(), "rectilinear cuts are boxes");
            sum += (v.footprint.box[2] - v.footprint.box[0]) *
                   (v.footprint.box[3] - v.footprint.box[1]);
        }
        near(sum, .000136, .000136 * 1e-12);
        auto n = merge(base(), {p});
        same_regions(m, n);
        auto& section = p.slabs[0].sections[0];
        auto reverse = [](Ring& r) {
            std::reverse(r.pts.begin(), r.pts.end());
            std::rotate(r.pts.begin(), r.pts.begin() + 1, r.pts.end());
        };
        reverse(section.outer);
        for (auto& h : section.holes)
            reverse(h);
        same_regions(m, merge(base(), {p}));
        check(m.stack.layers.size() == n.stack.layers.size(), "same layers");
        for (size_t k = 0; k < m.stack.layers.size(); ++k) {
            auto& a = m.stack.layers[k].regions;
            auto& b = n.stack.layers[k].regions;
            check(a.size() == b.size(), "same fragment count");
            for (size_t i = 0; i < a.size(); ++i)
                check(a[i].name == b[i].name && a[i].material == b[i].material &&
                          a[i].body == b[i].body && a[i].footprint.box == b[i].footprint.box &&
                          a[i].footprint.polygon == b[i].footprint.polygon,
                      "identical ordered fragments");
        }
    });
}
