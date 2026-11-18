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
                    3 * std::max(2., m.axes[0][i]) + 4 * std::max(2., m.axes[1][j]) < 26) {
                    const auto cx = (m.axes[0][i] + m.axes[0][i + 1]) / 2;
                    const auto cy = (m.axes[1][j] + m.axes[1][j + 1]) / 2;
                    ++expected_columns;
                    if (3 * cx + 4 * cy > 26)
                        ++outside_centers;
                }
        for (std::size_t e = 0; e < m.elem_gid.size(); ++e) {
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
            const bool box_owned = xmin < 3 && xmax > 2 && ymin < 3 && ymax > 2;
            if (box_owned) {
                ++box_cells;
                require(attr.region == "box_priority",
                        "box ownership takes precedence over later polygons");
            } else {
                require(attr.region == "last_polygon", "last positive-area polygon owns cell");
            }
            require(std::count(m.pending_cut.begin(), m.pending_cut.end(), static_cast<Local>(e)) ==
                        (box_owned ? 0 : 1),
                    "pending exactly once for polygon ownership, absent for box ownership");
        }
        require(m.elem_gid.size() == expected_columns * nz, "exactly positive-area cells retained");
        require(outside_centers > 0, "fixture exercises centers outside polygon");
        require(box_cells > 0, "fixture exercises box priority");

        // Snapping a box side onto another feature can leave a narrow covered
        // strip in a cell whose center is outside that box.
        for (bool box_last : {false, true}) {
            s = stack();
            auto polygon = square("polygon", 2, 2, 6, 6);
            auto priority = box("snapped_box", 2, 2, 3.04, 3);
            auto anchor = box("anchor", 2, 2, 3, 3);
            s.layers[0].regions = box_last ? std::vector<Inclusion>{anchor, polygon, priority}
                                           : std::vector<Inclusion>{anchor, priority, polygon};
            GridOptions o;
            o.nx = o.ny = 1;
            o.levels = 1;
            o.snap_tol = .1;
            auto result = build_tensor_grid(s, o);
            require(bool(result), "snapped box fixture builds");
            const auto& snapped = result.value();
            bool exercised = false;
            for (std::size_t e = 0; e < snapped.elem_gid.size(); ++e) {
                const auto c = center(snapped, e);
                if (c[0] > 3.04 && c[0] < 4 && c[1] > 2 && c[1] < 3) {
                    exercised = true;
                    require(snapped.attr_table.entries[snapped.attribute[e] - 1].region ==
                                "snapped_box",
                            "box overlap overrides polygon independent of center and order");
                    require(!std::binary_search(
                                snapped.pending_cut.begin(), snapped.pending_cut.end(), Local(e)),
                            "box-owned cells do not become pending polygon cuts");
                }
            }
            require(exercised, "fixture covers a snapped box strip with center outside");
            // Retention must use that same box coverage even without a polygon.
            s.layers[0].regions = {anchor, priority};
            auto boxes = build_tensor_grid(s, o);
            require(bool(boxes), "snapped box-only fixture builds");
            bool retained_strip = false;
            const auto& box_mesh = boxes.value();
            for (std::size_t e = 0; e < box_mesh.elem_gid.size(); ++e) {
                const auto c = center(box_mesh, e);
                if (c[0] > 3.04 && c[0] < 4 && c[1] > 2 && c[1] < 3) {
                    retained_strip = true;
                    require(box_mesh.attr_table.entries[box_mesh.attribute[e] - 1].region ==
                                "snapped_box",
                            "box-only overlap retains box attribute");
                }
            }
            require(retained_strip, "box-only positive-area strip must be retained");
            require(box_mesh.pending_cut.empty(), "box-only cells need no polygon cuts");
        }
    });
}
