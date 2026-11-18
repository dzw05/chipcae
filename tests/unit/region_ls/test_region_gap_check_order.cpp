#include "support.hpp"
using namespace sp5;
int main() {
    return run([] {
        auto s = stack();
        s.layers[0].regions = {square("b_ring", 2, 2, 3, 3), square("a_lid", 3.375, 2, 4.375, 3)};
        for (auto& r : s.layers[0].regions)
            r.footprint.box = s.outline.box;
        GridOptions o;
        o.nx = o.ny = 32;
        o.levels = 1;
        for (bool reverse : {false, true}) {
            if (reverse)
                std::reverse(s.layers[0].regions.begin(), s.layers[0].regions.end());
            auto result = build_tensor_grid(s, o);
            require(!result, "precondition failure must return before emitting owned cells");
            require(result.error().path == "CUT_UNSUPPORTED", "grid owns cut validation");
            require(result.error().reason == "a_lid b_ring: spacing < 2h",
                    "lexical diagnostic independent of input order");
        }
    });
}
