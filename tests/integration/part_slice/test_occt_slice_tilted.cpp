#include "occt_fixture.hpp"
using namespace slice_test;
int main() {
    return run([] {
        gp_Trsf tr;
        tr.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0)), 5 * std::acos(-1.0) / 180);
        const auto input = fixture(
            BRepBuilderAPI_Transform(BRepPrimAPI_MakeBox(40, 30, 0.5).Shape(), tr, true).Shape());
        const auto r = slice(input);
        bool found = false;
        for (const auto& d : r.defects)
            found |= d.tier == Tier::C && d.reason == DefectReason::PART_TILTED;
        require(found, "five degree tilt C PART_TILTED");
        same(r, slice(input));
    });
}
