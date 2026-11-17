#include "assembly_test_support.hpp"
int main() {
    return run([] {
        auto b = base();
        b.layers[0].regions.push_back(
            {"existing", "seed", "board", {{.001, .001, .002, .002}, {}}});
        auto a = part("A", .0005, .001);
        a.slabs.push_back({.0005, .001, {rect(.007, .007)}});
        auto c = part("a", .0005, .001);
        auto m = merge(b, {a, c}, .001);
        auto& rs = m.stack.layers.back().regions;
        check(rs.size() == 4, "exact region count");
        check(rs[0].name == "existing" && rs[1].name == "A#0#0" && rs[2].name == "A#1#0" &&
                  rs[3].name == "a#0#0",
              "base then part then slab order and case");
        check(rs[1].body == "A" && rs[2].body == "A" && rs[3].body == "a", "body identity");
        auto spanningBase = base();
        auto upper = spanningBase.layers[0];
        upper.name = "Upper";
        spanningBase.layers.push_back(upper);
        auto spanning = merge(spanningBase, {part("span", 0, .002)}, .002);
        check(spanning.stack.layers.size() == 2, "existing interfaces retained");
        for (auto& layer : spanning.stack.layers) {
            check(layer.regions.size() == 1 && layer.regions[0].name == "span#0#0" &&
                      layer.regions[0].body == "span",
                  "original slab index repeated across layers");
        }
        for (auto& r : rs)
            check(r.footprint.polygon.empty(), "box rectangles");
    });
}
