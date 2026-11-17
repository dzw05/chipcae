#include "occt_fixture.hpp"
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
        same(result, slice(input));
    });
}
