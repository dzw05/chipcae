#include "occt_fixture.hpp"
using namespace slice_test;
int main() {
    return run([] {
        const auto input =
            fixture(BRepAlgoAPI_Cut(BRepPrimAPI_MakeBox(40, 40, 3).Shape(),
                                    BRepPrimAPI_MakeBox(gp_Pnt(5, 5, -1), 30, 30, 3).Shape())
                        .Shape());
        const auto r = slice(input);
        require(tier(r) == Tier::A, "hat tier A");
        require(r.slabs.size() == 2, "hat has two slabs");
        require(r.slabs[0].sections.size() == 1 && r.slabs[1].sections.size() == 1,
                "one connected section in each hat slab");
        require(r.slabs[0].sections[0].holes.size() == 1, "flange has one hole");
        require(r.slabs[1].sections[0].holes.empty(), "lid has no hole");
        same(r, slice(input));
        near(r.slabs[0].z0, 0, 1e-12, "flange bottom");
        near(r.slabs[0].z1, 0.002, 1e-12, "flange top");
        near(r.slabs[1].z0, 0.002, 1e-12, "lid bottom");
        near(r.slabs[1].z1, 0.003, 1e-12, "lid top");
        near(area(r.slabs[0]), 0.0007, 0.0007 * 1e-12, "flange area");
        near(area(r.slabs[1]), 0.0016, 0.0016 * 1e-12, "lid area");
    });
}
