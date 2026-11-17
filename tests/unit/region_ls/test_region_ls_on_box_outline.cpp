#include "support.hpp"
using namespace sp5;
int main() {
    return run([] {
        auto s = stack();
        s.outline.box = {0, 0, .056, .044};
        auto lid = box("lid", 0, 0, .056, .044);
        lid.footprint.polygon = {{.002, 0},
                                 {.054, 0},
                                 {.056, .002},
                                 {.056, .042},
                                 {.054, .044},
                                 {.002, .044},
                                 {0, .042},
                                 {0, .002}};
        s.layers[0].regions = {lid};
        auto background = grid(s, 8);
        require(!background.elem_gid.empty(), "coincident box outline accepted by grid validation");
        HexMeshArrays m;
        for (int j = 0; j <= 44; ++j)
            for (int i = 0; i <= 56; ++i) {
                m.vertices.insert(m.vertices.end(), {.001 * i, .001 * j, .5});
                m.node_gid.push_back(m.node_gid.size());
            }
        for (int i = 0; i <= 56; ++i)
            m.axes[0].push_back(.001 * i);
        for (int j = 0; j <= 44; ++j)
            m.axes[1].push_back(.001 * j);
        m.axes[2] = {0, .5, 1};
        auto ls = levels(s, m);
        require(ls.size() == 1, "outline polygon LS");
        for (std::size_t n = 0; n < m.node_gid.size(); ++n) {
            double x = m.vertices[3 * n], y = m.vertices[3 * n + 1];
            // Chamfered rectangle is the intersection of eight analytic half planes.
            bool inside = x >= 0 && x <= .056 && y >= 0 && y <= .044 && x + y >= .002 &&
                          x - y <= .054 && x + y <= .098 && y - x <= .042;
            double distance = std::numeric_limits<double>::infinity();
            const auto& polygon = lid.footprint.polygon;
            for (std::size_t e = 0; e < polygon.size(); ++e) {
                auto a = polygon[e], b = polygon[(e + 1) % polygon.size()];
                double dx = b[0] - a[0], dy = b[1] - a[1];
                double t =
                    std::clamp(((x - a[0]) * dx + (y - a[1]) * dy) / (dx * dx + dy * dy), 0., 1.);
                distance = std::min(distance, std::hypot(x - a[0] - t * dx, y - a[1] - t * dy));
            }
            double exact = inside ? -distance : distance;
            require(std::abs(ls[0].phi[n] - exact) <= 1e-12, "outline analytic distance");
            if (((x == 0 || x == .056) && y >= .002 && y <= .042) ||
                ((y == 0 || y == .044) && x >= .002 && x <= .054))
                require(ls[0].phi[n] <= 0, "coincident edges classified inside");
        }
    });
}
