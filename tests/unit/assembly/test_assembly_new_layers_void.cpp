#include "assembly_test_support.hpp"
int main() {
    return run([] {
        auto m = merge(base(), {part()});
        auto& l = m.stack.layers.back();
        check(l.kind == "Explicit" && l.material.empty(), "new layer is void");
        check(l.regions.size() == 1 && l.regions[0].body == "lid" && l.regions[0].name == "lid#0#0",
              "part provenance");
        check(l.regions[0].footprint.polygon.empty(), "rectangle box only");
    });
}
