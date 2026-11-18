#include "occt_fixture.hpp"
using namespace slice_test;
int main() {
    return run([] {
        const auto input = fixture(BRepPrimAPI_MakeBox(40, 30, 0.5).Shape());
        const auto r = slice(input);
        const auto s = single(r);
        rectangle(s.sections[0].outer, 1e-12);
        require(s.sections[0].holes.empty(), "box has no holes");
        near(area(s), 0.04 * 0.03, 0.04 * 0.03 * 1e-12, "box relative area <= 1e-12");
        near(s.z0, 0, 1e-12, "box bottom");
        near(s.z1, 0.0005, 1e-12, "box top");
        same(r, slice(input));
    });
}
