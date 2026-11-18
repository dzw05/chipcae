#include "support.hpp"
using namespace sp5;
int main() {
    return run([] {
        // Convex sloping edges, horizontal ray/vertex hits, both windings.
        for (bool reverse : {false, true}) {
            auto s = stack();
            auto r = square("diamond", 1, 1, 7, 7);
            r.footprint.polygon = {{4, 1}, {7, 4}, {4, 7}, {1, 4}};
            if (reverse)
                std::reverse(r.footprint.polygon.begin(), r.footprint.polygon.end());
            s.layers[0].regions = {r};
            auto m = grid(s, 6);
            auto ls = levels(s, m);
            for (std::size_t n = 0; n < m.node_gid.size(); ++n) {
                const auto x = m.vertices[3 * n], y = m.vertices[3 * n + 1];
                const auto side = std::abs(x - 4) + std::abs(y - 4) - 3;
                if (std::abs(side) < 1e-14)
                    require(ls[0].phi[n] == 0, "general polygon edges and vertices are closed");
                else
                    require((ls[0].phi[n] < 0) == (side < 0),
                            "signed distance matches containment");
            }
            for (std::size_t e = 0; e < m.elem_gid.size(); ++e) {
                const auto c = center(m, e);
                // The minimum L1 distance over a cell rectangle is separable.
                double dx = 8, dy = 8;
                double xmin = 8, xmax = 0, ymin = 8, ymax = 0;
                for (int v = 0; v < 8; ++v) {
                    const auto n = m.hex[8 * e + v];
                    xmin = std::min(xmin, m.vertices[3 * n]);
                    xmax = std::max(xmax, m.vertices[3 * n]);
                    ymin = std::min(ymin, m.vertices[3 * n + 1]);
                    ymax = std::max(ymax, m.vertices[3 * n + 1]);
                }
                dx = std::max({xmin - 4, 4 - xmax, 0.});
                dy = std::max({ymin - 4, 4 - ymax, 0.});
                require(dx + dy < 3, "edge/vertex-only contacts do not retain cells");
                require(std::binary_search(m.pending_cut.begin(), m.pending_cut.end(), Local(e)),
                        "all overlap-owned cells are pending");
                if (std::abs(c[0] - 4) + std::abs(c[1] - 4) <= 3)
                    require(m.attr_table.entries[m.attribute[e] - 1].region == "diamond",
                            "closed polygon center implies polygon ownership");
            }
        }
        // Numerically coincident outline nodes need the configured local-h tolerance.
        auto s = stack();
        s.layers[0].regions = {square("lid", 0, 0, 8, 8)};
        auto m = samples(8);
        const double delta = 2e-13;
        for (std::size_t n = 0; n < m.node_gid.size(); ++n)
            if (m.vertices[3 * n] == 8)
                m.vertices[3 * n] += delta;
        auto ls = levels(s, m);
        for (std::size_t n = 0; n < m.node_gid.size(); ++n)
            if (m.vertices[3 * n] > 8)
                require(ls[0].phi[n] <= 0, "near-coincident box-outline nodes are inside");
        RegionLSOptions strict;
        strict.inside_tol_rel = 0;
        const auto outside = region_level_sets(s, m, strict);
        require(std::any_of(
                    outside[0].phi.begin(), outside[0].phi.end(), [](double p) { return p > 0; }),
                "outside nodes exceed roundoff tolerance");
    });
}
