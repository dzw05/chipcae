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
        const auto sheared =
            fixture(BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(square(0, 0)).Face(),
                                          gp_Vec(0.05, 0, 0.5))
                        .Shape());
        const auto shear_result = slice(sheared);
        require(shear_result.defects.size() == 1 &&
                    shear_result.defects[0].reason == DefectReason::PART_TILTED &&
                    tier(shear_result) == Tier::C,
                "tilted extrusion with horizontal caps is PART_TILTED");
        tr.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0)), 0.001);
        const auto column = fixture(
            BRepBuilderAPI_Transform(BRepPrimAPI_MakeBox(1, 1, 40).Shape(), tr, true).Shape());
        auto column_options = options();
        column_options.tau_a = column_options.tau_b = 0.0001;
        const auto tilted = slice_prismatic(column, column.body_names[0], column_options);
        require(tilted.defects.size() == 1 && tilted.defects[0].reason == DefectReason::PART_TILTED,
                "largest planar face normal controls tilt even with inertia axis near z");
    });
}
