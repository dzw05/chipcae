#include "region_cut_support.hpp"
using namespace sp5b;
int main() {
    return run([] {
        auto s = stack();
        auto rectangle = square("first-region", 0, 0, 4, 4);
        rectangle.body = "A";
        auto triangle = square("second-region", 0, 0, 2, 2);
        triangle.body = "z";
        triangle.footprint.polygon = {{2, 0}, {0, 2}, {0, 0}};
        s.layers[0].regions = {rectangle, triangle};
        HexMeshArrays m;
        m.axes = {{{0, 2, 4}, {0, 2, 4}, {0, 1}}};
        m.attr_table.bodies = {{"A", 91}, {"z", 3}};
        const auto g = geometry(s, m, "two bodies in reverse numeric order");
        require(g.planes.size() == 7, "three triangle edges plus four rectangle edges");
        for (std::size_t k = 0; k < g.planes.size(); ++k)
            require(g.planes[k].body == (k < 3 ? 3 : 91), "groups sorted by numeric BodyId");
        const double q = std::sqrt(.5);
        require(g.planes[0].n == std::array<Real, 3>{0, -1, 0} && g.planes[0].d == 0 &&
                    g.planes[2].n == std::array<Real, 3>{-1, 0, 0} && g.planes[2].d == 0,
                "triangle starts at lexicographic minimum in CCW order");
        require(std::abs(g.planes[1].n[0] - q) <= 1e-15 &&
                    std::abs(g.planes[1].n[1] - q) <= 1e-15 && g.planes[1].n[2] == 0 &&
                    std::abs(g.planes[1].d - std::sqrt(2.)) <= 2e-15,
                "triangle diagonal analytic plane");
        CutGeometry tail;
        tail.planes.assign(g.planes.begin() + 3, g.planes.end());
        exact_planes(
            tail,
            {{{91, {0, -1, 0}, 0}, {91, {1, 0, 0}, 4}, {91, {0, 1, 0}, 4}, {91, {-1, 0, 0}, 0}}});
        std::reverse(s.layers[0].regions.begin(), s.layers[0].regions.end());
        exact_planes(geometry(s, m, "reversed input order"), g);
    });
}
