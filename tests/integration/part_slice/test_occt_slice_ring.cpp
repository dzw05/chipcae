#include "occt_fixture.hpp"
using namespace slice_test;
int main() {
    return run([] {
        const auto input =
            fixture(BRepAlgoAPI_Cut(BRepPrimAPI_MakeBox(40, 40, 0.5).Shape(),
                                    BRepPrimAPI_MakeBox(gp_Pnt(5, 5, -1), 30, 30, 3).Shape())
                        .Shape());
        const auto s = single(slice(input));
        require(s.sections[0].holes.size() == 1, "one outer and one hole");
        near(area(s), 0.04 * 0.04 - 0.03 * 0.03, 1e-15, "square ring area and orientation");
    });
}
