#include "region_cut_support.hpp"
using namespace sp5b;
int main() {
    return run([] {
        auto s = stack();
        s.layers[0].regions = {box("square", 0, 0, 8, 8)};
        const auto m = grid(s);
        const auto axes_before = m.axes;
        const auto bodies_before = m.attr_table.bodies;
        const double x0 = m.axes[0].front(), x1 = m.axes[0].back();
        const double y0 = m.axes[1].front(), y1 = m.axes[1].back();
        require(x0 == 0 && y0 == 0 && x1 - x0 == y1 - y0, "generated square axes");
        s.layers[0].regions[0].footprint.polygon = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
        const auto polygon_before = s.layers[0].regions[0].footprint.polygon;
        const auto body = m.attr_table.bodies.at("square");
        const auto g = geometry(s, m, "axis-aligned square");
        // Zero lower coordinates avoid the unresolved negative-normal offset
        // wording. Nonzero negative offsets need the architect's ruling.
        const CutGeometry expected{{{body, {0, -1, 0}, 0},
                                    {body, {1, 0, 0}, x1},
                                    {body, {0, 1, 0}, y1},
                                    {body, {-1, 0, 0}, 0}}};
        exact_planes(g, expected);
        require(m.axes == axes_before && m.attr_table.bodies == bodies_before &&
                    s.layers[0].regions[0].footprint.polygon == polygon_before,
                "plane extraction leaves axes, body registry and polygon unchanged");
    });
}
