#include "region_cut_support.hpp"
using namespace sp5b;
int main() {
    return run([] {
        auto s = chamfer_stack();
        const auto m = registered_mesh();
        const std::vector<Polygon> rings = {{{0, 0}},
                                            {{0, 0}, {1, 0}},
                                            {{1, 1}, {1, 1}, {1, 1}, {1, 1}},
                                            {{0, 0}, {1, 0}, {2, 0}},
                                            {{0, 0}, {1, 0}, {2, 0}, {1, 0}, {0, 0}},
                                            {{0, 0}, {1e-16, 0}, {1e-16, 1e-16}, {0, 1e-16}},
                                            {{0, 0}, {1e-14, 0}, {0, 1e-14}}};
        for (const auto& ring : rings) {
            s.layers[0].regions[0].footprint.polygon = ring;
            error(s, m, "CUT_UNSUPPORTED", "lid-region: degenerate polygon");
        }
        auto later = s.layers[0].regions[0];
        later.name = "later-degenerate";
        s = chamfer_stack();
        s.layers[0].regions.push_back(later);
        error(s, m, "CUT_UNSUPPORTED", "later-degenerate: degenerate polygon");
    });
}
