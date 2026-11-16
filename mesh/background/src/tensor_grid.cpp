#include "chipcae/mesh/background/tensor_grid.hpp"

#include "chipcae/mesh/background/column_partition.hpp"
#include "chipcae/mesh/background/sfc.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
#ifdef K1_HAVE_METIS
#include <metis.h>
#endif
namespace chipcae::mesh {
Expected<std::vector<Local>>
ColumnExtrudePartitioner::partition(Local nx, Local ny, Local nz, Local ranks, bool use_metis) {
    if (nx < 1 || ny < 1 || nz < 1 || ranks < 1)
        return Error{"partition", "positive dimensions required"};
    std::vector<Local> order(static_cast<std::size_t>(nx) * ny), owner(order.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](Local a, Local b) {
        return morton(a % nx, a / nx) < morton(b % nx, b / nx);
    });
    for (std::size_t i = 0; i < order.size(); ++i)
        owner[order[i]] = static_cast<Local>(i * static_cast<std::size_t>(ranks) / order.size());
#ifdef K1_HAVE_METIS
    if (use_metis && ranks > 1) {
        idx_t n = nx * ny, ncon = 1, np = ranks, obj = 0;
        std::vector<idx_t> offsets{0}, adj, weights(static_cast<std::size_t>(n), nz),
            part(static_cast<std::size_t>(n));
        for (Local y = 0; y < ny; ++y)
            for (Local x = 0; x < nx; ++x) {
                for (auto d :
                     std::array<std::array<Local, 2>, 4>{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}}) {
                    Local a = x + d[0], b = y + d[1];
                    if (a >= 0 && b >= 0 && a < nx && b < ny)
                        adj.push_back(a + nx * b);
                }
                offsets.push_back(static_cast<idx_t>(adj.size()));
            }
        idx_t options[METIS_NOPTIONS];
        METIS_SetDefaultOptions(options);
        options[METIS_OPTION_SEED] = 0;
        if (METIS_PartGraphKway(&n,
                                &ncon,
                                offsets.data(),
                                adj.data(),
                                weights.data(),
                                nullptr,
                                nullptr,
                                &np,
                                nullptr,
                                nullptr,
                                options,
                                &obj,
                                part.data()) != METIS_OK)
            return Error{"partition", "METIS failure"};
        std::copy(part.begin(), part.end(), owner.begin());
    }
#else
    (void)use_metis;
#endif
    std::vector<Local> result;
    result.reserve(owner.size() * static_cast<std::size_t>(nz));
    for (Local z = 0; z < nz; ++z)
        result.insert(result.end(), owner.begin(), owner.end());
    return result;
}
namespace {
bool contains(const Footprint& f, Real x, Real y) {
    if (x < f.box[0] || y < f.box[1] || x > f.box[2] || y > f.box[3])
        return false;
    if (f.polygon.empty())
        return true;
    bool in = false;
    for (std::size_t i = 0, j = f.polygon.size() - 1; i < f.polygon.size(); j = i++) {
        auto a = f.polygon[i], b = f.polygon[j];
        if ((a[1] > y) != (b[1] > y) && x < (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]) + a[0])
            in = !in;
    }
    return in;
}
} // namespace
Expected<HexMeshArrays> build_tensor_grid(const StackModel& s, const GridOptions& o) {
    if (o.nx < 1 || o.ny < 1 || o.ranks < 1 || o.cell_budget < 1 || !(o.snap_tol >= 0) ||
        !(o.max_ratio >= 1))
        return Error{"grid", "invalid options"};
    const auto& b = s.outline.box;
    if (!(b[2] > b[0] && b[3] > b[1]) || s.layers.empty())
        return Error{"outline", "positive box and nonempty layers required"};
    std::set<std::string> names;
    for (const auto& l : s.layers) {
        if (!names.insert(l.name).second)
            return Error{"layers", "duplicate layer or region name"};
        for (const auto& r : l.regions)
            if (!names.insert(r.name).second)
                return Error{"regions", "ambiguous cross-layer region name"};
    }
    HexMeshArrays m;
    auto& axes = m.axes;
    for (int a = 0; a < 2; ++a) {
        auto& v = axes[a];
        v = {b[a], b[a + 2]};
        for (const auto& l : s.layers)
            for (const auto& r : l.regions)
                for (int side : {0, 2})
                    v.push_back(r.footprint.box[a + side]);
        std::sort(v.begin(), v.end());
        std::vector<Real> snapped;
        for (auto x : v) {
            if (!std::isfinite(x) || x < b[a] || x > b[a + 2])
                return Error{"footprint", "outside outline or nonfinite"};
            if (!snapped.empty() && x - snapped.back() < o.snap_tol) {
                if (x != snapped.back())
                    m.snapped.push_back(
                        {a == 0 ? 'x' : 'y', x, snapped.back(), snapped.back() - x});
            } else if (snapped.empty() || x != snapped.back())
                snapped.push_back(x);
        }
        v.clear();
        Local count = a == 0 ? o.nx : o.ny;
        for (std::size_t j = 1; j < snapped.size(); ++j)
            for (Local k = 0; k < count; ++k)
                v.push_back(snapped[j - 1] + (snapped[j] - snapped[j - 1]) * k / count);
        v.push_back(snapped.back());
    }
    axes[2] = {0};
    std::vector<Local> cell_layer;
    Real z = 0, last_dz = 0;
    for (std::size_t li = 0; li < s.layers.size(); ++li) {
        const auto& l = s.layers[li];
        if (!(l.thickness > 0) || !std::isfinite(l.thickness) || l.n_elem_min < 1 ||
            !(l.grading > 0) || !std::isfinite(l.grading))
            return Error{"layers", "invalid thickness, count or grading"};
        if (std::max(l.grading, 1 / l.grading) > o.max_ratio)
            return Error{"layers/grading", "adjacent size ratio exceeds max_ratio"};
        Real sum = 0, weight = 1;
        for (Local k = 0; k < l.n_elem_min; ++k) {
            sum += weight;
            weight *= l.grading;
        }
        if (!std::isfinite(sum))
            return Error{"layers/grading", "grading overflow"};
        Real start = z;
        weight = 1;
        for (Local k = 0; k < l.n_elem_min; ++k) {
            Real dz = l.thickness * weight / sum;
            if (last_dz > 0 && std::max(dz / last_dz, last_dz / dz) > o.max_ratio)
                return Error{
                    "layers",
                    "interface adjacent size ratio exceeds max_ratio; refine layer counts"};
            last_dz = dz;
            z += dz;
            axes[2].push_back(k + 1 == l.n_elem_min ? start + l.thickness : z);
            cell_layer.push_back(static_cast<Local>(li));
            weight *= l.grading;
        }
        z = start + l.thickness;
    }
    auto nx = axes[0].size() - 1, ny = axes[1].size() - 1, nz = axes[2].size() - 1;
    if (nx > static_cast<std::size_t>(o.cell_budget) / ny / nz)
        return Error{"cell_budget",
                     "exceeded; reduce nx=" + std::to_string(nx) + ", ny=" + std::to_string(ny) +
                         ", nz=" + std::to_string(nz) + " explicitly"};
    if ((nx + 1) * (ny + 1) * (nz + 1) >
        static_cast<std::size_t>(std::numeric_limits<Local>::max()))
        return Error{"grid", "local int32 capacity exceeded"};
    auto node = [&](std::size_t x, std::size_t y, std::size_t k) {
        return static_cast<Local>(x + (nx + 1) * (y + (ny + 1) * k));
    };
    for (std::size_t k = 0; k <= nz; ++k)
        for (std::size_t y = 0; y <= ny; ++y)
            for (std::size_t x = 0; x <= nx; ++x) {
                m.vertices.insert(m.vertices.end(), {axes[0][x], axes[1][y], axes[2][k]});
                m.node_gid.push_back(static_cast<Index>(m.node_gid.size()));
                m.node_layer.push_back(k == nz ? static_cast<Local>(s.layers.size())
                                               : cell_layer[k]);
                m.node_column.push_back(static_cast<Local>(x + (nx + 1) * y));
            }
    std::vector<std::vector<Local>> attrs;
    for (const auto& l : s.layers) {
        std::vector<Local> row;
        auto add = [&](std::string region, std::string material, std::string body) {
            auto& t = m.attr_table;
            if (body.empty())
                body = region.empty() ? l.name : region;
            if (!t.materials.contains(material))
                t.materials[material] = static_cast<MaterialId>(t.materials.size() + 1);
            if (!t.bodies.contains(body))
                t.bodies[body] = static_cast<BodyId>(t.bodies.size() + 1);
            t.entries.push_back({l.name, region, t.materials[material], t.bodies[body]});
            auto a = static_cast<Local>(t.entries.size());
            for (auto ref : {"layer:" + l.name, "body:" + body})
                t.references[ref].push_back(a);
            if (!region.empty())
                t.references["region:" + region].push_back(a);
            row.push_back(a);
        };
        add("", l.material, l.body);
        for (const auto& r : l.regions)
            add(r.name, r.material, r.body);
        attrs.push_back(row);
    }
    for (std::size_t k = 0; k < nz; ++k)
        for (std::size_t y = 0; y < ny; ++y)
            for (std::size_t x = 0; x < nx; ++x) {
                std::array<Local, 8> h = {node(x, y, k),
                                          node(x + 1, y, k),
                                          node(x + 1, y + 1, k),
                                          node(x, y + 1, k),
                                          node(x, y, k + 1),
                                          node(x + 1, y, k + 1),
                                          node(x + 1, y + 1, k + 1),
                                          node(x, y + 1, k + 1)};
                m.hex.insert(m.hex.end(), h.begin(), h.end());
                auto li = cell_layer[k];
                Local attr = attrs[li][0];
                for (std::size_t ri = 0; ri < s.layers[li].regions.size(); ++ri) {
                    const auto& f = s.layers[li].regions[ri].footprint;
                    if (contains(f,
                                 (axes[0][x] + axes[0][x + 1]) / 2,
                                 (axes[1][y] + axes[1][y + 1]) / 2))
                        attr = attrs[li][ri + 1];
                    if (!f.polygon.empty() && axes[0][x] < f.box[2] && axes[0][x + 1] > f.box[0] &&
                        axes[1][y] < f.box[3] && axes[1][y + 1] > f.box[1])
                        m.pending_cut.push_back(static_cast<Local>(m.attribute.size()));
                }
                m.attribute.push_back(attr);
                m.elem_gid.push_back(static_cast<Index>(m.elem_gid.size()));
                auto face = [&](std::array<int, 4> ids, Local a) {
                    for (int i : ids)
                        m.bdr_quad.push_back(h[i]);
                    m.bdr_attribute.push_back(a);
                };
                if (x == 0)
                    face({0, 4, 7, 3}, 1);
                if (x + 1 == nx)
                    face({1, 2, 6, 5}, 2);
                if (y == 0)
                    face({0, 1, 5, 4}, 3);
                if (y + 1 == ny)
                    face({3, 7, 6, 2}, 4);
                if (k == 0)
                    face({0, 3, 2, 1}, 5);
                if (k + 1 == nz)
                    face({4, 5, 6, 7}, 6);
            }
    for (Local a = 1; a <= 6; ++a)
        m.face_sets["faceset:" + std::to_string(a)] = {a};
    for (std::size_t i = 1; i < s.layers.size(); ++i)
        m.face_sets["interface:" + s.layers[i - 1].name + "|" + s.layers[i].name] = {
            static_cast<Local>(6 + i)};
    // Compact away tuple records that have no cells, retaining exact inverse maps.
    AttributeTable compact;
    compact.materials = m.attr_table.materials;
    compact.bodies = m.attr_table.bodies;
    std::vector<Local> remap(m.attr_table.entries.size() + 1);
    for (std::size_t a = 1; a < remap.size(); ++a)
        if (std::find(m.attribute.begin(), m.attribute.end(), static_cast<Local>(a)) !=
            m.attribute.end()) {
            compact.entries.push_back(m.attr_table.entries[a - 1]);
            remap[a] = static_cast<Local>(compact.entries.size());
        }
    for (const auto& [ref, list] : m.attr_table.references) {
        auto& dst = compact.references[ref];
        for (auto a : list)
            if (remap[a])
                dst.push_back(remap[a]);
    }
    for (auto& a : m.attribute)
        a = remap[a];
    m.attr_table = std::move(compact);
    auto part = ColumnExtrudePartitioner::partition(static_cast<Local>(nx),
                                                    static_cast<Local>(ny),
                                                    static_cast<Local>(nz),
                                                    o.ranks,
                                                    o.use_metis);
    if (!part)
        return part.error();
    m.partitioning = std::move(part.value());
    return m;
}
} // namespace chipcae::mesh
