#include "support.hpp"
using namespace sp5;
int main() {
    return run([] {
        // The box-outline exception also covers the specified chamfered lid.
        auto s = stack();
        s.outline.box = {0, 0, .056, .044};
        auto lid = box("chamfer", 0, 0, .056, .044);
        lid.footprint.polygon = {{.002, 0},
                                 {.054, 0},
                                 {.056, .002},
                                 {.056, .042},
                                 {.054, .044},
                                 {.002, .044},
                                 {0, .042},
                                 {0, .002}};
        s.layers[0].regions = {lid};
        HexMeshArrays m;
        m.axes[0] = {0, .001, .002, .028, .054, .055, .056};
        m.axes[1] = {0, .001, .002, .022, .042, .043, .044};
        for (auto x : m.axes[0])
            for (auto y : m.axes[1]) {
                m.vertices.insert(m.vertices.end(), {x, y, .5});
                m.node_gid.push_back(static_cast<chipcae::Index>(m.node_gid.size()));
            }
        auto ls = levels(s, m);
        for (std::size_t n = 0; n < m.node_gid.size(); ++n) {
            double x = m.vertices[3 * n], y = m.vertices[3 * n + 1];
            if ((x == 0 || x == .056) && y >= .002 && y <= .042)
                require(ls[0].phi[n] <= 0, "chamfer straight side inside");
            if ((y == 0 || y == .044) && x >= .002 && x <= .054)
                require(ls[0].phi[n] <= 0, "chamfer straight side inside");
            if (x == 0 && y == 0)
                require(std::abs(ls[0].phi[n] - .002 / std::sqrt(2.)) <= 1e-12,
                        "chamfer corner distance");
        }
        // A polygonal outline uses boundary distance, not containment distance.
        s = stack();
        s.outline.polygon = {{0, 0}, {8, 0}, {8, 8}, {0, 8}};
        s.layers[0].regions = {square("interior", 2, 2, 6, 6)};
        m = samples(32);
        require(levels(s, m).size() == 1, "well separated polygon outline accepted");

        // Coarse cells far from the gap must not raise its local 2h threshold.
        s = stack();
        s.layers[0].regions = {square("a", 2, 2, 3, 3), square("b", 3.625, 2, 4.625, 3)};
        m = samples(32);
        m.axes[0] = {0, 2};
        for (double x = 2.25; x < 5; x += .25)
            m.axes[0].push_back(x);
        m.axes[0].insert(m.axes[0].end(), {5, 8});
        require(levels(s, m).size() == 2, "local gap ignores distant coarse cells");

        // First body occurrence includes exact box regions in the same group.
        s = stack();
        auto first = box("first_box", 2, 2, 3, 3);
        first.body = "first";
        auto second = square("second_polygon", 5, 5, 6, 6);
        auto last = square("first_polygon", 2, 2, 3, 3);
        last.body = "first";
        s.layers[0].regions = {first, second, last};
        m = samples(32);
        ls = levels(s, m);
        require(ls.size() == 2 && ls[0].body == m.attr_table.bodies.at("first") &&
                    ls[1].body == m.attr_table.bodies.at("second_polygon"),
                "polygon-bearing groups follow first body occurrence including boxes");

        // Retain crossing cells whose centers are outside a triangular region.
        s = stack();
        auto triangle = square("triangle", 2, 2, 6, 6);
        triangle.footprint.polygon = {{2, 2}, {6, 2}, {2, 6}};
        s.layers[0].regions = {triangle};
        m = grid(s);
        bool crossing = false;
        for (auto e : m.pending_cut) {
            double x = 0, y = 0;
            for (int v = 0; v < 8; ++v) {
                x += m.vertices[3 * m.hex[8 * e + v]] / 8;
                y += m.vertices[3 * m.hex[8 * e + v] + 1] / 8;
            }
            if (x + y >= 8)
                crossing = true;
        }
        require(crossing, "polygon crossing cells must remain eligible for cutting");
        compact(m);
    });
}
