#include "support.hpp"
using namespace sp5;
int main() {
    return run([] {
        auto s = stack();
        s.layers[0].regions = {square("A", 2, 2, 6, 6)};
        auto upper = s.layers[0];
        upper.name = "upper";
        upper.regions = {square("B", 2, 2, 6, 6)};
        s.layers.push_back(upper);
        auto m = samples(32);
        m.vertices.insert(m.vertices.end(), {4, 4, 1.5});
        m.node_gid.push_back(static_cast<chipcae::Index>(m.node_gid.size()));
        auto ls = levels(s, m);
        require(ls.size() == 2, "one LS per distinct polygon-source body");
        require(ls[0].body == m.attr_table.bodies.at("A") &&
                    ls[1].body == m.attr_table.bodies.at("B"),
                "LS order and body table IDs");
        for (std::size_t n = 0; n < m.node_gid.size(); ++n) {
            double z = m.vertices[3 * n + 2],
                   exact = rect(m.vertices[3 * n], m.vertices[3 * n + 1], 2, 2, 6, 6);
            for (int l = 0; l < 2; ++l) {
                double phi = ls[l].phi[n];
                if (z >= l && z <= l + 1)
                    require(std::abs(phi - exact) <= 1e-12, "square analytic signed distance");
                else
                    require(std::isinf(phi) && phi > 0, "outside layer must be positive infinity");
            }
        }
    });
}
