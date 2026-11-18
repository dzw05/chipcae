#include "occt_fixture.hpp"

#include <BRepPrimAPI_MakeCylinder.hxx>
using namespace slice_test;
int main() {
    return run([] {
        const auto input =
            fixture(BRepAlgoAPI_Cut(BRepPrimAPI_MakeBox(40, 40, 0.5).Shape(),
                                    BRepPrimAPI_MakeBox(gp_Pnt(5, 5, -1), 30, 30, 3).Shape())
                        .Shape());
        const auto result = slice(input);
        const auto s = single(result);
        require(s.sections[0].holes.size() == 1, "one outer and one hole");
        const double expected = 0.04 * 0.04 - 0.03 * 0.03;
        near(area(s), expected, expected * 1e-12, "square ring relative area and orientation");
        const auto& section = s.sections[0];
        near(section.outer.pts.front()[0], 0, 1e-12, "outer starts at minimum x");
        near(section.outer.pts.front()[1], 0, 1e-12, "outer starts at minimum y among minimum x");
        near(section.holes[0].pts.front()[0], 0.005, 1e-12, "hole starts at minimum x");
        near(section.holes[0].pts.front()[1],
             0.005,
             1e-12,
             "clockwise hole starts at minimum y among minimum x");
        same(result, slice(input));

        // A cylindrical wall intersects the slicing plane in a single closed
        // edge. Its opposite endpoint must count as its own closure partner.
        const auto circular = fixture(
            BRepAlgoAPI_Cut(
                BRepPrimAPI_MakeCylinder(20, 0.5).Shape(),
                BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, -1), gp_Dir(0, 0, 1)), 15, 3).Shape())
                .Shape());
        const auto circular_result = slice(circular);
        const auto circular_slab = single(circular_result);
        require(circular_slab.sections[0].holes.size() == 1, "closed circular outer and hole");
        const double circular_area = std::acos(-1.0) * (0.02 * 0.02 - 0.015 * 0.015);
        near(area(circular_slab), circular_area, circular_area * 1e-4, "circular ring area");
        same(circular_result, slice(circular));
    });
}
