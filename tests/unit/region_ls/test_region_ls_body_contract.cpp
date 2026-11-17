#include "support.hpp"
using namespace sp5;
int main() {
    return run([] {
        auto s = stack();
        auto a = square("lower", 1, 1, 3, 3);
        a.body = "shared";
        auto b = square("other", 5, 5, 7, 7);
        s.layers[0].regions = {a, b, box("box_only", 0, 0, 1, 1)};
        auto upper = s.layers[0];
        upper.name = "upper";
        a.footprint = square("unused", 2, 2, 6, 6).footprint;
        upper.regions = {a};
        s.layers.push_back(upper);
        auto m = samples(16);
        m.vertices.insert(m.vertices.end(), {4, 4, 1.5, 4, 4, 2.5});
        m.node_gid.push_back(m.node_gid.size());
        m.node_gid.push_back(m.node_gid.size());
        m.attr_table.bodies = {{"shared", 91}, {"other", 3}, {"box_only", 1}};
        auto ls = region_level_sets(s, m, {});
        require(ls.size() == 2, "one entry per polygon-source body across layers; boxes excluded");
        require(ls[0].body == 3 && ls[1].body == 91,
                "entries sorted by BodyId, not name or occurrence");
        for (const auto& l : ls) {
            require(l.phi.size() == m.node_gid.size(), "all node values present");
            for (std::size_t n = 0; n < m.node_gid.size(); ++n) {
                double x = m.vertices[3 * n], y = m.vertices[3 * n + 1], z = m.vertices[3 * n + 2];
                double exact = std::numeric_limits<double>::infinity();
                if (l.body == 3 && z >= 0 && z <= 1)
                    exact = rect(x, y, 5, 5, 7, 7);
                if (l.body == 91) {
                    if (z >= 0 && z <= 1)
                        exact = rect(x, y, 1, 1, 3, 3);
                    if (z >= 1 && z <= 2)
                        exact = std::min(exact, rect(x, y, 2, 2, 6, 6));
                }
                require(std::isinf(exact) ? std::isinf(l.phi[n]) && l.phi[n] > 0
                                          : std::abs(l.phi[n] - exact) <= 1e-12,
                        "closed layer intervals and pointwise min across body sources");
            }
        }
    });
}
