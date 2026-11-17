#include "occt_fixture.hpp"
using namespace slice_test;
int main() {
    return run([] {
        int failures = 0;
        for (const double factor : {0.5, 2.0}) {
            failures += run([&] {
                // Top and bottom offsets from the mid-height contour are factor*tau_b.
                const double inset = 2 * factor * options().tau_b * 1000;
                BRepOffsetAPI_ThruSections loft(true, true);
                loft.AddWire(square(0, 0));
                loft.AddWire(square(inset, 0.5));
                loft.Build();
                require(loft.IsDone(), "draft fixture built");
                const auto r = slice(fixture(loft.Shape()));
                require(tier(r) == (factor < 1 ? Tier::B : Tier::C), "draft threshold tier");
                if (factor > 1) {
                    bool found = false;
                    for (const auto& d : r.defects)
                        found |= d.tier == Tier::C && d.reason == DefectReason::PART_NOT_PRISMATIC;
                    require(found, "excessive draft C PART_NOT_PRISMATIC");
                }
            });
        }
        require(failures == 0, "both draft thresholds");
    });
}
