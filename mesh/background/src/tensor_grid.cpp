#include "chipcae/mesh/background/tensor_grid.hpp"

#include "chipcae/mesh/background/activation.hpp"
#include "chipcae/mesh/background/column_hierarchy.hpp"
#include "chipcae/mesh/background/copper_map.hpp"
#include "chipcae/mesh/background/sfc.hpp"
#include "chipcae/par/column_extrude_partitioner.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>
namespace chipcae::mesh {
Expected<Real> detail::column_partition_imbalance(
    const std::vector<std::vector<Local>>& column_ranks, std::size_t nz, int ranks) {
    if (column_ranks.empty() || column_ranks.back().empty() || nz == 0 || ranks <= 0)
        return Error{"partition", "invalid column partition dimensions"};
    std::vector<std::size_t> loads(static_cast<std::size_t>(ranks));
    for (auto rank : column_ranks.back()) {
        if (rank < 0 || rank >= ranks)
            return Error{"partition", "column rank is out of range"};
        loads[static_cast<std::size_t>(rank)] += nz;
    }
    const auto cells = column_ranks.back().size() * nz;
    const Real imbalance = Real(*std::max_element(loads.begin(), loads.end())) * ranks / cells - 1;
    if (imbalance > .25)
        return Error{"partition", "fine-cell partition imbalance exceeds 0.25"};
    return imbalance;
}
namespace {
Expected<std::vector<Local>> expand_column_partition(HexMeshArrays::GridReport& report,
                                                     std::size_t nx0,
                                                     std::size_t ny0,
                                                     std::size_t nz,
                                                     int ranks,
                                                     std::span<const std::int32_t> weights = {}) {
    try {
        const auto L = report.levels - 1;
        auto structured = par::ColumnExtrudePartitioner::partition(nx0, ny0, nz, ranks, weights);
        auto& coarse = report.column_ranks.emplace_back(nx0 * ny0);
        for (std::size_t j = 0; j < ny0; ++j)
            for (std::size_t i = 0; i < nx0; ++i)
                coarse[level0_dense(nx0, ny0, i, j)] = structured(j * nx0 + i);
        for (int l = 1; l <= L; ++l) {
            const auto parent = report.column_ranks.back();
            auto& child = report.column_ranks.emplace_back(parent.size() * 4);
            for (std::size_t d = 0; d < child.size(); ++d)
                child[d] = parent[d >> 2];
        }
        const auto& fine = report.column_ranks.back();
        std::vector<Local> result(fine.size() * nz);
        for (std::size_t d = 0; d < fine.size(); ++d)
            for (std::size_t k = 0; k < nz; ++k) {
                result[d * nz + k] = fine[d];
            }
        auto imbalance = detail::column_partition_imbalance(report.column_ranks, nz, ranks);
        if (!imbalance)
            return imbalance.error();
        report.partition_imbalance = imbalance.value();
        // TODO: Count fine-cell weights here when alpha weights are available.
        for (std::size_t l = 1; l < report.column_ranks.size(); ++l) {
            const auto nx = nx0 << l, ny = ny0 << l;
            for (std::size_t j = 0; j < ny; ++j)
                for (std::size_t i = 0; i < nx; ++i) {
                    const auto child = dense_index(nx0, ny0, l, i, j);
                    const auto parent = dense_index(nx0, ny0, l - 1, i >> 1, j >> 1);
                    if (report.column_ranks[l][child] != report.column_ranks[l - 1][parent])
                        return Error{"HIERARCHY_INVALID", "child and parent ranks differ"};
                }
        }
        for (std::size_t d = 0; d < fine.size(); ++d)
            for (std::size_t k = 1; k < nz; ++k)
                if (result[d * nz + k] != result[d * nz])
                    return Error{"COLUMN_SPLIT_ACROSS_RANKS", "column crosses ranks"};
        return result;
    } catch (const std::exception& e) {
        return Error{"partition", e.what()};
    }
}
struct AxisSegments {
    std::vector<Real> lines;
    std::vector<std::size_t> counts;
};
bool pad_axis(AxisSegments& axis, std::size_t divisor, Real ratio) {
    auto total = std::accumulate(axis.counts.begin(), axis.counts.end(), std::size_t{0});
    auto width = [&](std::size_t i) {
        return (axis.lines[i + 1] - axis.lines[i]) / axis.counts[i];
    };
    while (total % divisor) {
        std::size_t largest = 0;
        for (std::size_t i = 1; i < axis.counts.size(); ++i)
            if (width(i) > width(largest))
                largest = i;
        bool accepted = false;
        // Try the largest physical cells first, then its neighboring segments.
        for (std::size_t distance = 0; distance <= 1 && !accepted; ++distance)
            for (int direction : {-1, 1}) {
                if (distance == 0 && direction == 1)
                    continue;
                auto candidate = static_cast<std::ptrdiff_t>(largest) +
                                 direction * static_cast<std::ptrdiff_t>(distance);
                if (candidate < 0 || static_cast<std::size_t>(candidate) >= axis.counts.size())
                    continue;
                ++axis.counts[candidate];
                bool valid = true;
                for (std::size_t i = 1; i < axis.counts.size(); ++i)
                    if (std::max(width(i) / width(i - 1), width(i - 1) / width(i)) >
                        ratio * (1 + 16 * std::numeric_limits<Real>::epsilon()))
                        valid = false;
                if (valid) {
                    accepted = true;
                    break;
                }
                --axis.counts[candidate];
            }
        if (!accepted)
            return false;
        ++total;
    }
    for (std::size_t i = 1; i < axis.counts.size(); ++i)
        if (std::max(width(i) / width(i - 1), width(i - 1) / width(i)) >
            ratio * (1 + 16 * std::numeric_limits<Real>::epsilon()))
            return false;
    return true;
}
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
Expected<std::array<AxisSegments, 2>>
plan_axes(const StackModel& s, const GridOptions& o, HexMeshArrays& m) {
    if (!s.symmetry.empty() && s.symmetry != "none")
        return Error{"symmetry", "symmetry other than none is not implemented"};
    if (o.nx < 1 || o.ny < 1 || o.ranks < 1 || o.cell_budget < 1 || !(o.snap_tol >= 0) ||
        !(o.max_ratio >= 1) || !std::isfinite(o.snap_tol) || !std::isfinite(o.max_ratio))
        return Error{"grid", "invalid options"};
    const auto& b = s.outline.box;
    if (!(b[2] > b[0] && b[3] > b[1]) ||
        !std::all_of(b.begin(), b.end(), [](Real x) { return std::isfinite(x); }) ||
        s.layers.empty())
        return Error{"outline", "positive box and nonempty layers required"};
    std::set<std::string> names;
    for (const auto& l : s.layers) {
        if (!names.insert(l.name).second)
            return Error{"layers", "duplicate layer or region name"};
        for (const auto& r : l.regions)
            if (!names.insert(r.name).second)
                return Error{"regions", "ambiguous cross-layer region name"};
    }
    if (o.nx > o.cell_budget || o.ny > o.cell_budget || Index(o.nx) > o.cell_budget / o.ny)
        return Error{"cell_budget",
                     "requested xy resolution exceeds budget; reduce nx/ny explicitly"};
    Index minimum_z = 0;
    for (const auto& l : s.layers) {
        if (l.n_elem_min < 1 || l.n_elem_min > o.cell_budget - minimum_z)
            return Error{"cell_budget",
                         "minimum layer counts exceed budget; revise n_elem_min explicitly"};
        minimum_z += l.n_elem_min;
    }
    if (Index(o.nx) * o.ny > o.cell_budget / minimum_z)
        return Error{"cell_budget",
                     "requested xyz resolution exceeds budget; revise nx/ny/n_elem_min explicitly"};
    if (o.levels < 0 || o.levels > 31)
        return Error{"HIERARCHY_INVALID", "invalid hierarchy options"};
    std::array<AxisSegments, 2> segments;
    auto& axes = m.axes;
    for (int a = 0; a < 2; ++a) {
        auto& v = axes[a];
        v = {b[a], b[a + 2]};
        for (const auto& l : s.layers)
            for (const auto& r : l.regions)
                for (int side : {0, 2})
                    v.push_back(r.footprint.box[a + side]);
        for (auto& x : v)
            if (x != b[a] && x != b[a + 2]) {
                Real target = std::abs(x - b[a]) < o.snap_tol       ? b[a]
                              : std::abs(x - b[a + 2]) < o.snap_tol ? b[a + 2]
                                                                    : x;
                if (x != target) {
                    m.snapped.push_back({a == 0 ? 'x' : 'y', x, target, target - x});
                    x = target;
                }
            }
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
        segments[a].lines = snapped;
        v.clear();
        Local minimum = a == 0 ? o.nx : o.ny;
        Real spacing = b[a + 2] - b[a];
        for (std::size_t j = 1; j < snapped.size(); ++j)
            spacing = std::min(spacing, (snapped[j] - snapped[j - 1]) / minimum);
        if (o.max_ratio > 1)
            spacing *= std::min(Real(1), o.max_ratio - 1);
        std::size_t total = 0;
        for (std::size_t j = 1; j < snapped.size(); ++j) {
            Real proposed =
                std::max(Real(minimum), std::ceil((snapped[j] - snapped[j - 1]) / spacing));
            if (proposed > Real(o.cell_budget) ||
                proposed > Real(std::numeric_limits<Local>::max()) || !std::isfinite(proposed) ||
                proposed > Real(static_cast<std::size_t>(o.cell_budget) - total) ||
                proposed >=
                    Real(static_cast<std::size_t>(std::numeric_limits<Local>::max()) - total))
                return Error{"cell_budget",
                             "xy grading refinement exceeds budget; revise feature spacing or "
                             "nx/ny explicitly"};
            Local count = std::max(minimum, static_cast<Local>(proposed));
            segments[a].counts.push_back(count);
            total += static_cast<std::size_t>(count);
        }
    }
    const auto& original = segments;
    for (const auto& axis : original)
        for (std::size_t i = 1; i < axis.counts.size(); ++i) {
            const Real left = (axis.lines[i] - axis.lines[i - 1]) / axis.counts[i - 1];
            const Real right = (axis.lines[i + 1] - axis.lines[i]) / axis.counts[i];
            if (std::max(left / right, right / left) >
                o.max_ratio * (1 + 16 * std::numeric_limits<Real>::epsilon()))
                return Error{"grid/grading",
                             "max_ratio=1 requires exactly commensurate feature intervals"};
        }
    return segments;
}
Expected<bool> plan_z(const StackModel& s,
                      const GridOptions& o,
                      HexMeshArrays& m,
                      std::vector<Local>& cell_layer,
                      bool materialize = true) {
    auto& axes = m.axes;
    if (materialize)
        axes[2] = {0};
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
            if (materialize) {
                axes[2].push_back(k + 1 == l.n_elem_min ? start + l.thickness : z);
                cell_layer.push_back(static_cast<Local>(li));
            }
            weight *= l.grading;
        }
        z = start + l.thickness;
    }
    return true;
}
std::size_t count(const AxisSegments& axis) {
    return std::accumulate(axis.counts.begin(), axis.counts.end(), std::size_t{0});
}
enum class PrepareFailure { None, Budget, MaxRatio };
PrepareFailure prepare(const std::array<AxisSegments, 2>& original,
                       const GridOptions& o,
                       int L,
                       std::array<AxisSegments, 2>& candidate) {
    candidate = original;
    const auto divisor = std::size_t{1} << L;
    for (auto& axis : candidate) {
        const auto n = count(axis);
        if (n > std::numeric_limits<std::size_t>::max() - (divisor - 1))
            return PrepareFailure::Budget;
        const auto blocks = (n + divisor - 1) / divisor;
        if (blocks > std::numeric_limits<std::size_t>::max() / divisor)
            return PrepareFailure::Budget;
        const auto padded = blocks * divisor;
        if (padded > static_cast<std::size_t>(o.cell_budget) ||
            padded >= static_cast<std::size_t>(std::numeric_limits<Local>::max()))
            return PrepareFailure::Budget;
        if (!pad_axis(axis, divisor, o.max_ratio))
            return PrepareFailure::MaxRatio;
    }
    return PrepareFailure::None;
}
bool within_budget(const std::array<AxisSegments, 2>& axes, std::size_t nz, const GridOptions& o) {
    return count(axes[0]) <= static_cast<std::size_t>(o.cell_budget) / count(axes[1]) / nz;
}
} // namespace
Expected<std::vector<LevelCandidate>> hierarchy_candidates(const StackModel& s,
                                                           const GridOptions& o) {
    HexMeshArrays planning;
    auto original = plan_axes(s, o, planning);
    if (!original)
        return original.error();
    std::vector<Local> cell_layer;
    auto z = plan_z(s, o, planning, cell_layer, false);
    if (!z)
        return z.error();
    std::size_t nz = 0;
    for (const auto& layer : s.layers)
        nz += static_cast<std::size_t>(layer.n_elem_min);
    std::vector<LevelCandidate> result;
    for (int L = 0; L <= 30; ++L) {
        std::array<AxisSegments, 2> axes;
        auto failure = prepare(original.value(), o, L, axes);
        const auto divisor = std::size_t{1} << L;
        // Base axes have fewer than INT32_MAX cells, and divisor <= 2^30.
        // These rounded counts and the sum of additions fit even 32-bit size_t.
        // Infeasible rows describe the requested padding, not a partial retry.
        const auto nx = (count(original.value()[0]) + divisor - 1) / divisor * divisor;
        const auto ny = (count(original.value()[1]) + divisor - 1) / divisor * divisor;
        result.push_back({L,
                          nx / divisor,
                          ny / divisor,
                          nx - count(original.value()[0]) + ny - count(original.value()[1]),
                          failure == PrepareFailure::None && within_budget(axes, nz, o)});
    }
    return result;
}
Expected<HexMeshArrays>
build_tensor_grid(const StackModel& s, const GridOptions& o, const Polyset* polyset) {
    HexMeshArrays m;
    auto planned = plan_axes(s, o, m);
    if (!planned)
        return planned.error();
    const auto original = planned.value();
    std::array<AxisSegments, 2> segments;
    auto& axes = m.axes;
    std::vector<Local> cell_layer;
    auto z_plan = plan_z(s, o, m, cell_layer);
    if (!z_plan)
        return z_plan.error();
    int L = o.levels ? o.levels - 1 : 0;
    LevelChoice choice;
    if (!o.levels) {
        auto candidates = hierarchy_candidates(s, o);
        if (!candidates)
            return candidates.error();
        choice = select_levels(candidates.value(), cell_layer.size(), o.ranks);
        L = choice.L;
        m.grid_report.warnings = choice.warnings;
        m.grid_report.rule = choice.rule;
        m.grid_report.auto_selected = true;
    }
    const auto failure = prepare(original, o, L, segments);
    if (failure != PrepareFailure::None || !within_budget(segments, cell_layer.size(), o))
        return Error{"HIERARCHY_INVALID", "hierarchy padding violates budget or max_ratio"};
    const auto divisor = std::size_t{1} << L;
    const auto nx0 = count(segments[0]) / divisor, ny0 = count(segments[1]) / divisor;
    if (o.levels && nx0 * ny0 < static_cast<std::size_t>(o.ranks))
        return Error{"hierarchy/levels", "coarsest columns below ranks"};
    if (o.levels && nx0 * ny0 / 16 < static_cast<std::size_t>(o.ranks))
        m.grid_report.warnings.push_back("coarse columns below 16*np");
    const LevelCandidate selected{0, nx0, ny0, 0, true};
    m.grid_report.N_L = o.levels
                            ? select_levels(std::span(&selected, 1), cell_layer.size(), o.ranks).N_L
                            : choice.N_L;
    m.grid_report.levels = L + 1;
    for (int a = 0; a < 2; ++a) {
        auto& v = axes[a];
        m.grid_report.padding[a] = count(segments[a]) - count(original[a]);
        if (count(segments[a]) > static_cast<std::size_t>(o.cell_budget) ||
            count(segments[a]) >= static_cast<std::size_t>(std::numeric_limits<Local>::max()))
            return Error{"cell_budget", "hierarchy padding exceeds capacity"};
        for (std::size_t j = 0; j < segments[a].counts.size(); ++j)
            for (std::size_t k = 0; k < segments[a].counts[j]; ++k)
                v.push_back(segments[a].lines[j] +
                            (segments[a].lines[j + 1] - segments[a].lines[j]) * k /
                                segments[a].counts[j]);
        v.push_back(segments[a].lines.back());
    }
    if (!o.levels)
        std::clog << "levels=auto(L=" << L << ", rule=" << choice.rule << ")\n";
    std::clog << "run_config levels=" << L + 1 << " n0=" << nx0 << 'x' << ny0
              << " part=" << (par::ColumnExtrudePartitioner::uses_metis() ? "metis" : "serpentine")
              << " np=" << o.ranks << " coarse=" << nx0 * ny0 << '\n';
    for (const auto& warning : m.grid_report.warnings)
        std::clog << "warning: " << warning << '\n';
    auto nx = axes[0].size() - 1, ny = axes[1].size() - 1, nz = axes[2].size() - 1;
    m.copper_layer_of_layer.assign(s.layers.size(), -1);
    std::size_t copper_layers = 0;
    for (std::size_t li = 0; li < s.layers.size(); ++li)
        if (s.layers[li].copper) {
            if (s.layers[li].copper->polygons && !polyset)
                return Error{"copper", "polygon sidecar data is not available to tensor_grid"};
            m.copper_layer_of_layer[li] = static_cast<Local>(copper_layers++);
        }
    m.copper_phi.assign(copper_layers * nx * ny, 0);
    for (std::size_t li = 0; li < s.layers.size(); ++li)
        if (s.layers[li].copper) {
            const auto offset = static_cast<std::size_t>(m.copper_layer_of_layer[li]) * nx * ny;
            if (s.layers[li].copper->fraction) {
                std::fill_n(m.copper_phi.begin() + static_cast<std::ptrdiff_t>(offset),
                            nx * ny,
                            *s.layers[li].copper->fraction);
            } else if (s.layers[li].copper->polygons) {
                const auto& name = s.layers[li].copper->polygons->layer;
                auto source = std::find_if(polyset->layers.begin(),
                                           polyset->layers.end(),
                                           [&](const auto& p) { return p.name == name; });
                if (source == polyset->layers.end())
                    return Error{"copper", "polyset does not contain layer " + name};
                if (!(polyset->dbu_m > 0) || source->ring_offsets.size() < 2 ||
                    source->coordinates.size() % 2 != 0)
                    return Error{"copper", "invalid polyset geometry"};
                std::vector<CopperPath> paths;
                for (std::size_t r = 0; r + 1 < source->ring_offsets.size(); ++r) {
                    const auto begin = source->ring_offsets[r], end = source->ring_offsets[r + 1];
                    if (begin < 0 || end < begin ||
                        static_cast<std::uint64_t>(end) * 2 > source->coordinates.size())
                        return Error{"copper", "invalid polyset ring offsets"};
                    auto& path = paths.emplace_back();
                    path.reserve(static_cast<std::size_t>(end - begin));
                    for (auto point = begin; point < end; ++point)
                        path.push_back({source->coordinates[2 * point] * polyset->dbu_m,
                                        source->coordinates[2 * point + 1] * polyset->dbu_m});
                }
                try {
                    auto phi = rasterize_copper(axes[0], axes[1], paths);
                    std::copy(phi.begin(),
                              phi.end(),
                              m.copper_phi.begin() + static_cast<std::ptrdiff_t>(offset));
                } catch (const std::exception& e) {
                    return Error{"copper", e.what()};
                }
            }
        }
    if (nx > static_cast<std::size_t>(o.cell_budget) / ny / nz)
        return Error{"cell_budget",
                     "exceeded; reduce nx=" + std::to_string(nx) + ", ny=" + std::to_string(ny) +
                         ", nz=" + std::to_string(nz) + " explicitly"};
    if (nx + 1 > static_cast<std::size_t>(std::numeric_limits<Local>::max()) / (ny + 1) / (nz + 1))
        return Error{"grid", "local int32 capacity exceeded"};
    std::vector<std::size_t> columns(nx * ny), node_columns((nx + 1) * (ny + 1));
    std::iota(columns.begin(), columns.end(), 0);
    std::iota(node_columns.begin(), node_columns.end(), 0);
    auto sort_columns = [](auto& c, std::size_t width) {
        std::sort(c.begin(), c.end(), [&](auto a, auto b) {
            return morton(a % width, a / width) < morton(b % width, b / width);
        });
    };
    sort_columns(columns, nx);
    sort_columns(node_columns, nx + 1);
    std::vector<Local> node_index(node_columns.size());
    for (std::size_t i = 0; i < node_columns.size(); ++i)
        node_index[node_columns[i]] = static_cast<Local>(i);
    auto node = [&](std::size_t x, std::size_t y, std::size_t k) {
        return static_cast<Local>(node_index[x + (nx + 1) * y] * (nz + 1) + k);
    };
    for (auto column : node_columns)
        for (std::size_t k = 0; k <= nz; ++k) {
            auto x = column % (nx + 1), y = column / (nx + 1);
            m.vertices.insert(m.vertices.end(), {axes[0][x], axes[1][y], axes[2][k]});
            m.node_gid.push_back(static_cast<Index>(m.node_gid.size()));
            m.node_layer.push_back(k == 0 ? 0 : cell_layer[k - 1]);
            m.node_column.push_back(node_index[column]);
        }
    std::vector<std::vector<Local>> attrs;
    for (const auto& l : s.layers) {
        m.attr_table.layers[l.name] = static_cast<Local>(m.attr_table.layers.size() + 1);
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
    for (auto column : columns)
        for (std::size_t k = 0; k < nz; ++k) {
            auto x = column % nx, y = column / nx;
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
                if (contains(
                        f, (axes[0][x] + axes[0][x + 1]) / 2, (axes[1][y] + axes[1][y + 1]) / 2))
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
    // Compact away tuple records that have no cells, retaining exact inverse maps.
    AttributeTable compact;
    compact.layers = m.attr_table.layers;
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
    for (int l = 0; l <= L; ++l) {
        auto& level_axes = m.grid_report.level_axes.emplace_back();
        std::array<Real, 3> smallest, largest{};
        smallest.fill(std::numeric_limits<Real>::max());
        for (int a = 0; a < 3; ++a) {
            const auto step = a == 2 ? 1u : std::size_t{1} << (L - l);
            for (std::size_t i = 0; i < axes[a].size(); i += step) {
                if (a < 2)
                    level_axes[a].push_back(axes[a][i]);
                if (i) {
                    const auto width = axes[a][i] - axes[a][i - step];
                    smallest[a] = std::min(smallest[a], width);
                    largest[a] = std::max(largest[a], width);
                }
            }
        }
        Real aspect = 1;
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b)
                if (a != b)
                    aspect = std::max(aspect, largest[a] / smallest[b]);
        m.grid_report.max_aspect_ratio.push_back(aspect);
    }
    for (auto threshold : {o.ar_warn, o.ar_max})
        if (m.grid_report.max_aspect_ratio.back() > threshold) {
            m.grid_report.warnings.push_back("finest aspect ratio exceeds " +
                                             std::to_string(threshold));
            std::clog << "warning: " << m.grid_report.warnings.back() << '\n';
        }
    // Uncut coarse columns all carry 4^L*nz fine cells. Equal scaling is
    // omitted for the unweighted fallback; the weights span supports future alpha weights.
    std::vector<std::int32_t> weights;
    if (par::ColumnExtrudePartitioner::uses_metis()) {
        const auto weight = divisor * divisor * nz;
        if (weight > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
            return Error{"HIERARCHY_INVALID", "coarse column weight exceeds int32"};
        weights.assign(nx0 * ny0, static_cast<std::int32_t>(weight));
    }
    auto part = expand_column_partition(m.grid_report, nx0, ny0, nz, o.ranks, weights);
    if (!part)
        return part.error();
    m.partitioning = std::move(part.value());
    // Every actual body transition receives a dedicated aligned interface attribute.
    constexpr int fi[6][4] = {
        {0, 3, 2, 1}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}, {4, 5, 6, 7}};
    std::map<std::array<Local, 4>, std::pair<std::size_t, int>> shared;
    Local next_boundary = 7;
    for (std::size_t e = 0; e < m.attribute.size(); ++e)
        for (int f = 0; f < 6; ++f) {
            std::array<Local, 4> q;
            for (int j = 0; j < 4; ++j)
                q[j] = m.hex[8 * e + fi[f][j]];
            auto key = q;
            std::sort(key.begin(), key.end());
            auto [it, inserted] = shared.emplace(key, std::pair{e, f});
            if (inserted)
                continue;
            auto a = m.attr_table.entries[m.attribute[it->second.first] - 1].body;
            auto bbody = m.attr_table.entries[m.attribute[e] - 1].body;
            if (a == bbody)
                continue;
            std::string an, bn;
            for (const auto& [name, id] : m.attr_table.bodies) {
                if (id == a)
                    an = name;
                if (id == bbody)
                    bn = name;
            }
            auto name = canonical_interface("body:" + an, "body:" + bn);
            auto& list = m.face_sets[name];
            if (list.empty()) {
                list.push_back(next_boundary++);
                HexMeshArrays::InterfaceReport report;
                report.name = name;
                m.preparation_report.push_back(report);
                fill_interface_sides(m.preparation_report.back(), m);
            }
            // Face on first element points towards second element.
            for (int j = 0; j < 4; ++j)
                m.bdr_quad.push_back(m.hex[8 * it->second.first + fi[it->second.second][j]]);
            m.bdr_attribute.push_back(list[0]);
            Real area = 1;
            std::array<Real, 3> span{};
            for (int d = 0; d < 3; ++d) {
                Real lo = m.vertices[3 * q[0] + d], hi = lo;
                for (auto v : q) {
                    lo = std::min(lo, m.vertices[3 * v + d]);
                    hi = std::max(hi, m.vertices[3 * v + d]);
                }
                span[d] = hi - lo;
            }
            for (auto v : span)
                if (v > 0)
                    area *= v;
            for (auto& r : m.preparation_report)
                if (r.name == name) {
                    ++r.aligned_faces;
                    r.aligned_area += area;
                }
        }
    m.internal_interfaces = !m.preparation_report.empty();
    for (const auto& [name, list] : m.face_sets)
        for (auto a : list)
            m.attr_table.boundary_names[a] = name;
    m.attr_frame.resize(9 * m.attr_table.entries.size());
    for (std::size_t a = 0; a < m.attr_table.entries.size(); ++a)
        for (int d = 0; d < 3; ++d)
            m.attr_frame[9 * a + 4 * d] = 1;

    for (const auto& [name, members] : s.activation_groups) {
        auto group = resolve_activation(s, m, nullptr, members);
        if (!group)
            return group.error();
        m.attr_table.references["group:" + name] = group.value().cells.attributes;
    }
    return m;
}
Expected<HexMeshArrays> build_tensor_grid(const StackModel& s, const GridOptions& o) {
    return build_tensor_grid(s, o, static_cast<const Polyset*>(nullptr));
}
Expected<HexMeshArrays>
build_tensor_grid(const StackModel& s, const GridOptions& o, const Polyset& polyset) {
    return build_tensor_grid(s, o, &polyset);
}
} // namespace chipcae::mesh
