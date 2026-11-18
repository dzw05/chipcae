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
                const auto input = fixture(loft.Shape());
                const auto r = slice(input);
                same(r, slice(input));
                // Keep the end sections beyond the imported face tolerances so
                // OCCT does not snap their vertices onto the end-cap edges.
                auto measured_options = options();
                measured_options.tau_a *= 5;
                const auto measured = slice_prismatic(input, input.body_names[0], measured_options);
                require(measured.defects.size() == 1, "draft reports measured deviation");
                const double expected =
                    std::sqrt(2.0) * inset / 1000 * (0.5 - measured_options.tau_a / 0.0005);
                near(measured.defects[0].size,
                     expected,
                     1e-10,
                     "whole section Hausdorff distance at tau_a inset");
                auto thin_options = options();
                thin_options.tau_a = 0.0005 / 2;
                thin_options.tau_b = thin_options.tau_a;
                const auto thin = slice_prismatic(input, input.body_names[0], thin_options);
                require(tier(thin) == Tier::A && !thin.slabs.empty(),
                        "slabs thinner than three tau_a use midpoint only");
                if (factor < 1)
                    require(!r.slabs.empty(), "accepted B draft retains slabs");
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
