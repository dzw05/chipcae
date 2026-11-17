#include "support.hpp"
using namespace sp5;
int main() {
    return run([] {
        auto s = stack();
        auto triangle = square("triangle", 2, 2, 6, 5);
        triangle.footprint.polygon = {{2, 2}, {6, 2}, {2, 5}};
        // A duplicate source of the same body must not duplicate pending_cut.
        auto duplicate = triangle;
        duplicate.name = "last_polygon";
        s.layers[0].regions = {box("box_priority", 2, 2, 3, 3), triangle, duplicate};
        auto m = grid(s, 8);
        require(std::is_sorted(m.pending_cut.begin(), m.pending_cut.end()) &&
                    std::adjacent_find(m.pending_cut.begin(), m.pending_cut.end()) ==
                        m.pending_cut.end(),
                "pending_cut must be strictly ascending and unique");
        std::size_t outside_centers = 0, box_cells = 0;
        const auto nz = m.axes[2].size() - 1;
        std::size_t expected_columns = 0;
        for (std::size_t j = 0; j + 1 < m.axes[1].size(); ++j)
            for (std::size_t i = 0; i + 1 < m.axes[0].size(); ++i)
                if (m.axes[0][i + 1] > 2 && m.axes[1][j + 1] > 2 &&
                    3 * std::max(2., m.axes[0][i]) + 4 * std::max(2., m.axes[1][j]) < 26)
                    ++expected_columns;
        for (std::size_t e = 0; e < m.elem_gid.size(); ++e) {
            auto c = center(m, e);
            double xmin = 8, ymin = 8, xmax = 0, ymax = 0;
            for (int v = 0; v < 8; ++v) {
                auto n = m.hex[8 * e + v];
                xmin = std::min(xmin, m.vertices[3 * n]);
                xmax = std::max(xmax, m.vertices[3 * n]);
                ymin = std::min(ymin, m.vertices[3 * n + 1]);
                ymax = std::max(ymax, m.vertices[3 * n + 1]);
            }
            // Exact positive-area criterion for this right triangle; equality is vertex/edge
            // contact.
            require(xmax > 2 && ymax > 2 && 3 * std::max(2., xmin) + 4 * std::max(2., ymin) < 26,
                    "zero-area polygon contacts must be omitted");
            const auto& attr = m.attr_table.entries.at(m.attribute[e] - 1);
            if (c[0] > 2 && c[0] < 3 && c[1] > 2 && c[1] < 3) {
                ++box_cells;
                require(attr.region == "box_priority",
                        "box ownership takes precedence over later polygons");
            } else {
                require(attr.region == "last_polygon", "last positive-area polygon owns cell");
            }
            require(std::count(m.pending_cut.begin(), m.pending_cut.end(), static_cast<Local>(e)) ==
                        1,
                    "every positive-area polygon cell pending exactly once");
            if (3 * c[0] + 4 * c[1] > 26)
                ++outside_centers;
        }
        require(m.elem_gid.size() == expected_columns * nz, "all positive-area cells retained");
        require(outside_centers > 0, "fixture exercises centers outside polygon");
        require(box_cells > 0, "fixture exercises box priority");
    });
}
