#include "support.hpp"
using namespace sp5;
int main() {
    return run([] {
        auto s = stack();
        s.layers[0].regions = {box("lid", 2, 2, 6, 6)};
        auto m = grid(s, 4, 3);
        s.layers[0].material = "background";
        auto full = grid(s, 4, 3);
        require(m.axes == full.axes, "void filtering must preserve candidate axes");
        std::map<std::array<double, 3>, std::size_t> cells;
        for (std::size_t e = 0; e < full.elem_gid.size(); ++e)
            cells[center(full, e)] = e;
        require(m.partitioning.size() == m.elem_gid.size(), "one partition per retained cell");
        const auto nz = m.axes[2].size() - 1;
        // Independently rank columns by interleaved Morton bits (dense, including void columns).
        std::vector<std::pair<unsigned long long, std::array<double, 2>>> columns;
        for (std::size_t j = 0; j + 1 < m.axes[1].size(); ++j)
            for (std::size_t i = 0; i + 1 < m.axes[0].size(); ++i) {
                unsigned long long code = 0;
                for (int b = 0; b < 24; ++b) {
                    code |= ((i >> b) & 1ULL) << (2 * b);
                    code |= ((j >> b) & 1ULL) << (2 * b + 1);
                }
                columns.push_back({code,
                                   {(m.axes[0][i] + m.axes[0][i + 1]) / 2,
                                    (m.axes[1][j] + m.axes[1][j + 1]) / 2}});
            }
        std::sort(columns.begin(), columns.end());
        for (std::size_t e = 0; e < m.elem_gid.size(); ++e) {
            auto c = center(m, e);
            auto original = cells.at(c);
            require(m.elem_gid[e] == full.elem_gid[original],
                    "void filtering must preserve elem_gid");
            require(m.partitioning[e] == full.partitioning[original],
                    "partition follows retained cell");
            auto col = std::find_if(columns.begin(),
                                    columns.end(),
                                    [&](const auto& p) {
                                        return p.second == std::array<double, 2>{c[0], c[1]};
                                    }) -
                       columns.begin();
            require(m.elem_gid[e] / nz == static_cast<std::size_t>(col), "dense Morton column GID");
            auto k =
                std::upper_bound(m.axes[2].begin(), m.axes[2].end(), c[2]) - m.axes[2].begin() - 1;
            require(m.elem_gid[e] % nz == static_cast<std::size_t>(k), "GID retains z index");
        }
        std::map<std::array<double, 3>, chipcae::Index> nodes;
        for (std::size_t n = 0; n < full.node_gid.size(); ++n)
            nodes[{full.vertices[3 * n], full.vertices[3 * n + 1], full.vertices[3 * n + 2]}] =
                full.node_gid[n];
        for (std::size_t n = 0; n < m.node_gid.size(); ++n)
            require(m.node_gid[n] ==
                        nodes.at({m.vertices[3 * n], m.vertices[3 * n + 1], m.vertices[3 * n + 2]}),
                    "void filtering must preserve node_gid at every coordinate");
        require(m.grid_report.void_cells ==
                    static_cast<std::int64_t>(full.elem_gid.size() - m.elem_gid.size()),
                "exact void count");
        require(m.node_gid.size() < full.node_gid.size(),
                "nodes belonging only to void cells removed");
        require(std::is_sorted(m.elem_gid.begin(), m.elem_gid.end()), "retained GIDs ascending");
        compact(m);
    });
}
