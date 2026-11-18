#include "assembly_test_support.hpp"
int main() {
    return run([] {
        auto b = base();
        b.layers[0].copper = CopperSpec{"Cu", .35, CopperPolygons{"copper.bin", "Top"}};
        b.layers[0].grading = 1.4;
        b.layers[0].n_elem_min = 3;
        auto m = merge(b, {part("ring", .0005, .001)}, .001);
        check(m.stack.layers.size() == 2, "split layer count");
        for (size_t i = 0; i < 2; ++i) {
            auto& l = m.stack.layers[i];
            check(l.name == "Core#" + std::to_string(i), "split name");
            check(l.kind == b.layers[0].kind && l.body == "board" && l.material == "core" &&
                      l.grading == 1.4 && l.n_elem_min == 3,
                  "layer fields copied");
            check(l.copper.has_value(), "copper retained");
            auto c = *l.copper;
            check(c.metal == "Cu" && c.fraction == .35 && c.polygons &&
                      c.polygons->file == "copper.bin" && c.polygons->layer == "Top",
                  "all copper fields copied");
        }
    });
}
