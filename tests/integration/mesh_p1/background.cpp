#include "chipcae/mesh/background/activation.hpp"
#include "chipcae/mesh/background/column_hierarchy.hpp"
#include "chipcae/mesh/background/sfc.hpp"
#include "chipcae/par/column_extrude_partitioner.hpp"
#include "support.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
using namespace chipcae::mesh;
using k1::expect;
int main() {
#ifdef CHIPCAE_ENABLE_KOKKOS
    Kokkos::ScopeGuard guard;
#endif
    auto s = k1::stack();
    auto o = k1::options();
    o.nx = 4;
    o.ny = 4;
    o.ranks = 4;
    auto result = build_tensor_grid(s, o);
    expect(bool(result), "grid");
    auto& m = result.value();
    auto replay = build_tensor_grid(s, o);
    expect(bool(replay), "replay");
    expect(m.vertices == replay.value().vertices && m.hex == replay.value().hex &&
               m.attribute == replay.value().attribute &&
               m.partitioning == replay.value().partitioning,
           "bitwise replay");
    expect(std::set<chipcae::Index>(m.elem_gid.begin(), m.elem_gid.end()).size() ==
               m.elem_gid.size(),
           "element GID uniqueness");
    expect(std::set<chipcae::Index>(m.node_gid.begin(), m.node_gid.end()).size() ==
               m.node_gid.size(),
           "node GID uniqueness");
    for (int ranks : {1, 2, 4}) {
        auto opts = o;
        opts.ranks = ranks;
        auto other = build_tensor_grid(s, opts);
        expect(bool(other) && other.value().elem_gid == m.elem_gid &&
                   other.value().node_gid == m.node_gid && other.value().hex == m.hex &&
                   other.value().vertices == m.vertices,
               "SFC rank-independent geometric GIDs");
    }
    for (std::size_t n = 0; n < m.node_layer.size(); ++n) {
        auto z = m.vertices[3 * n + 2];
        expect(m.node_layer[n] == (z <= .5 ? 0 : 1), "interface and top node_layer");
    }
    auto device = upload<chipcae::HostSpace>(m);
    expect(device.node_column.extent(0) == m.node_gid.size(), "hex upload");
    double volume = 0;
    std::vector<int> counts(s.layers.size());
    for (std::size_t e = 0; e < m.attribute.size(); ++e) {
        auto a = m.hex[8 * e], b = m.hex[8 * e + 6];
        double v = 1;
        for (int d = 0; d < 3; ++d) {
            double dx = m.vertices[3 * b + d] - m.vertices[3 * a + d];
            expect(dx > 0, "eight corner affine Jacobians positive");
            v *= dx;
        }
        volume += v;
        auto l = m.attr_table.entries[static_cast<std::size_t>(m.attribute[e] - 1)].layer;
        ++counts[l == "base" ? 0 : 1];
    }
    expect(std::abs(volume - 1) <= k1::threshold("volume_relative"), "total volume");
    expect(std::find(m.axes[2].begin(), m.axes[2].end(), .5) != m.axes[2].end(),
           "interface alignment");
    for (std::size_t l = 0; l < 2; ++l)
        expect(counts[l] >= s.layers[l].n_elem_min, "minimum z elements");
    std::set<Local> attrs(m.attribute.begin(), m.attribute.end());
    Local next = 1;
    for (auto a : attrs)
        expect(a == next++, "compact attributes");
    std::size_t nx = m.axes[0].size() - 1, ny = m.axes[1].size() - 1, nz = m.axes[2].size() - 1;
    std::vector<int> load(4);
    for (std::size_t e = 0; e < m.attribute.size(); ++e) {
        expect(m.partitioning[e] == m.partitioning[(e / nz) * nz], "column ownership");
        ++load[m.partitioning[e]];
    }
    for (auto n : load)
        expect(std::abs(double(n) * 4 / m.attribute.size() - 1) <=
                   k1::threshold("partition_imbalance"),
               "rank balance");
    for (int ranks : {1, 2, 4}) {
        auto opts = o;
        opts.ranks = ranks;
        auto grid = build_tensor_grid(s, opts);
        expect(bool(grid), "partition grid at rank count");
        const auto& g = grid.value();
        std::vector<std::size_t> measured_loads(ranks);
        for (auto rank : g.partitioning)
            ++measured_loads[rank];
        std::cout << "K1 background np=" << ranks << " cells=" << g.attribute.size() << " loads=";
        for (auto load : measured_loads)
            std::cout << load << ',';
        std::cout << " imbalance="
                  << double(*std::max_element(measured_loads.begin(), measured_loads.end())) *
                             ranks / g.attribute.size() -
                         1
                  << '\n';

        auto structured = chipcae::par::ColumnExtrudePartitioner::partition(nx, ny, nz, ranks);
        std::vector<std::size_t> order(nx * ny);
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return morton(a % nx, a / nx) < morton(b % nx, b / nx);
        });
        expect(g.partitioning.size() == g.attribute.size() &&
                   g.elem_gid.size() == g.attribute.size(),
               "partition and GID lengths");
        for (std::size_t e = 0; e < g.attribute.size(); ++e) {
            auto mi = e / nz, k = e % nz;
            expect(g.elem_gid[e] == static_cast<chipcae::Index>(e),
                   "Morton column times nz plus k GID");
            expect(g.partitioning[e] == structured(k * (nx * ny) + order[mi]),
                   "element rank equals column rank");
        }
    }
    for (bool use_metis : {false, true})
        for (int ranks : {1, 2, 4}) {
            auto opts = o;
            opts.use_metis = use_metis;
            opts.ranks = ranks;
            auto grid = build_tensor_grid(s, opts);
            expect(bool(grid), "column ownership grid at rank count");
            const auto& g = grid.value();
            std::unordered_map<chipcae::Index, int> column_ranks;
            for (std::size_t e = 0; e < g.partitioning.size(); ++e) {
                const auto column = g.elem_gid[e] / static_cast<chipcae::Index>(nz);
                auto [it, inserted] = column_ranks.emplace(column, g.partitioning[e]);
                if (!inserted && it->second != g.partitioning[e]) {
                    std::ostringstream message;
                    message << (chipcae::par::ColumnExtrudePartitioner::uses_metis() ? "METIS"
                                                                                     : "SFC")
                            << " column " << column << " has conflicting ranks " << it->second
                            << " and " << g.partitioning[e] << " (requested " << ranks << " ranks)";
                    throw std::runtime_error(message.str());
                }
            }
        }
    // A non-power-of-two rectangular coarse shape, with a finest-only feature.
    auto nonuniform = s;
    nonuniform.outline.box = {0, 0, 3.5, 4};
    nonuniform.layers[0].regions[0].footprint.box = {1, 0, 2.5, 4};
    auto padded_options = k1::options();
    padded_options.nx = 3;
    padded_options.ny = 8;
    padded_options.levels = 2;
    auto padded = build_tensor_grid(nonuniform, padded_options);
    expect(bool(padded), "nonuniform padded grid");
    const auto& pg = padded.value();
    const auto& report = pg.grid_report;
    expect(report.padding[0] == 1 && report.padding[1] == 0, "padding counts");
    expect(report.levels == 2 && report.max_aspect_ratio.size() == 2, "per-level aspect report");
    expect(std::find(pg.axes[0].begin(), pg.axes[0].end(), 2.5) != pg.axes[0].end(),
           "finest feature retained");
    expect(std::find(report.level_axes[0][0].begin(), report.level_axes[0][0].end(), 2.5) ==
               report.level_axes[0][0].end(),
           "feature absent from coarse subset");
    for (int a = 0; a < 2; ++a) {
        for (std::size_t i = 0; i < report.level_axes[0][a].size(); ++i)
            expect(report.level_axes[0][a][i] == pg.axes[a][2 * i], "even line subset");
        for (std::size_t i = 2; i < pg.axes[a].size(); ++i) {
            auto left = pg.axes[a][i - 1] - pg.axes[a][i - 2];
            auto right = pg.axes[a][i] - pg.axes[a][i - 1];
            expect(std::max(left / right, right / left) <= 2 + 1e-12, "padded max ratio");
        }
    }
    for (auto bounds : {std::pair{0., 1.}, std::pair{1., 2.5}, std::pair{2.5, 3.5}}) {
        double width = 0;
        for (std::size_t i = 1; i < pg.axes[0].size(); ++i)
            if (pg.axes[0][i - 1] >= bounds.first && pg.axes[0][i] <= bounds.second) {
                auto next_width = pg.axes[0][i] - pg.axes[0][i - 1];
                if (width)
                    expect(std::abs(next_width - width) < 1e-12, "uniform feature segment");
                width = next_width;
            }
    }
    for (std::size_t d = 0; d < report.column_ranks[1].size(); ++d)
        expect(report.column_ranks[1][d] == report.column_ranks[0][d >> 2],
               "child same rank as parent");
    const auto px = pg.axes[0].size() - 1, py = pg.axes[1].size() - 1;
    std::vector<std::size_t> morton_columns(px * py);
    std::iota(morton_columns.begin(), morton_columns.end(), 0);
    std::sort(morton_columns.begin(), morton_columns.end(), [&](auto a, auto b) {
        return morton(a % px, a / px) < morton(b % px, b / px);
    });
    for (std::size_t d = 0; d < morton_columns.size(); ++d) {
        auto c = morton_columns[d];
        expect(dense_index(px / 2, py / 2, 1, c % px, c / px) == d,
               "finest dense equals tensor Morton index");
        for (std::size_t k = 0; k < nz; ++k)
            expect(pg.elem_gid[d * nz + k] == static_cast<chipcae::Index>(d * nz + k),
                   "padded GID rule");
    }
    std::cout << "padding x=" << report.padding[0] << " y=" << report.padding[1] << " aspect=";
    for (auto ar : report.max_aspect_ratio)
        std::cout << ar << ',';
    std::cout << '\n';
    auto aspect_options = padded_options;
    aspect_options.ar_warn = aspect_options.ar_max = report.max_aspect_ratio.back();
    auto aspect_grid = build_tensor_grid(nonuniform, aspect_options);
    expect(bool(aspect_grid) && aspect_grid.value().grid_report.warnings.empty(),
           "coarse aspect ratio does not warn");
    aspect_options.ar_warn = 1;
    auto aspect_warning = build_tensor_grid(nonuniform, aspect_options);
    expect(bool(aspect_warning) && !aspect_warning.value().grid_report.warnings.empty(),
           "finest aspect warning");
    auto invalid = padded_options;
    invalid.levels = 4;
    auto invalid_grid = build_tensor_grid(nonuniform, invalid);
    expect(!invalid_grid && invalid_grid.error().path == "HIERARCHY_INVALID",
           "explicit insufficient coarse columns");
    // Exercise normal, soft-limit, relaxed and direct selection, and inherited loads.
    for (int ranks : {1, 2, 4}) {
        auto options = k1::options();
        options.nx = options.ny = 8;
        options.ranks = ranks;
        auto grid = build_tensor_grid(s, options);
        expect(bool(grid) && grid.value().grid_report.levels > 1, "automatic multilevel grid");
        const auto& g = grid.value();
        std::vector<std::size_t> loads(ranks);
        for (auto rank : g.partitioning)
            ++loads[rank];
        expect(g.grid_report.partition_imbalance <= k1::threshold("partition_imbalance"),
               "fine-weight imbalance <= 0.25");
        for (std::size_t l = 1; l < g.grid_report.column_ranks.size(); ++l)
            for (std::size_t d = 0; d < g.grid_report.column_ranks[l].size(); ++d)
                expect(g.grid_report.column_ranks[l][d] ==
                           g.grid_report.column_ranks[l - 1][d >> 2],
                       "multirank child same rank as parent");
        options.max_coarse_columns = 1;
        auto soft = build_tensor_grid(s, options);
        expect(bool(soft) && soft.value().grid_report.levels == g.grid_report.levels &&
                   !soft.value().grid_report.warnings.empty(),
               "soft cap preserves lower bound");
    }
    StackModel plain = s;
    plain.layers[0].regions.clear();
    plain.activation_groups.clear();
    auto selection = k1::options();
    selection.nx = selection.ny = 4;
    auto relaxed = build_tensor_grid(plain, selection);
    expect(bool(relaxed) && relaxed.value().grid_report.levels == 1 &&
               !relaxed.value().grid_report.warnings.empty(),
           "relaxed minimum");
    selection.nx = selection.ny = 2;
    auto direct = build_tensor_grid(plain, selection);
    expect(bool(direct) && direct.value().grid_report.levels == 1 &&
               !direct.value().grid_report.warnings.empty(),
           "direct fallback");
    o.cell_budget = 1;
    expect(!build_tensor_grid(s, o), "budget failure");
    o.cell_budget = 50000;
    auto shifted = s;
    shifted.layers[0].regions[0].footprint.box[0] = o.snap_tol / 2;
    auto snap = build_tensor_grid(shifted, o);
    expect(bool(snap) && !snap.value().snapped.empty(), "snap report");
    for (auto ref : {"layer:base",
                     "region:die",
                     "body:die",
                     "body:*",
                     "group:both",
                     "group:overlap",
                     "faceset:1",
                     "interface:base|top"})
        expect(bool(resolve_activation(s, m, nullptr, {ref})), "resolve prefix");
    auto side = resolve_interface_side(m, "layer:base");
    auto volume_ref = resolve_volume_ref(s, m, nullptr, {"layer:base"});
    expect(bool(side) && bool(volume_ref) && side.value().size() == 1 &&
               volume_ref.value().cells.bodies.size() == 2,
           "split interface and volume layer semantics");
    expect(bool(resolve_activation(s, m, nullptr, {"interface:body:cap|layer:base"})),
           "mixed interface sides");
    expect(!resolve_activation(s, m, nullptr, {"interface:layer:base|body:base"}),
           "overlap interface rejected");
    expect(!resolve_activation(s, m, nullptr, {"interface:void|void"}), "void overlap rejected");
    auto ab = resolve_activation(s, m, nullptr, {"interface:base|top"});
    auto ba = resolve_activation(s, m, nullptr, {"interface:top|base"});
    expect(bool(ab) && bool(ba) &&
               ab.value().faces.bdr_attributes == ba.value().faces.bdr_attributes,
           "A|B equals B|A");
    bool report_ok = false;
    for (const auto& r : m.preparation_report)
        if (r.name == "interface:body:base|body:cap" || r.name == "interface:body:cap|body:base") {
            expect(r.aligned_faces > 0 && r.aligned_area > 0, "aligned interface report");
            expect(!r.side_a_bodies.empty() && !r.side_b_bodies.empty(), "interface sides listed");
            report_ok = true;
        }
    expect(report_ok, "preparation report has mixed body sides");
    auto mixed_ref = resolve_activation(s, m, nullptr, {"interface:layer:base|body:cap"});
    expect(bool(mixed_ref), "mixed layer/body interface");
    bool mixed_listed = false;
    for (const auto& r : m.preparation_report)
        if (r.name == "interface:layer:base|body:cap") {
            expect(r.side_a_bodies == std::vector<std::string>{"base"},
                   "mixed ref side A body names");
            expect(r.side_b_bodies == std::vector<std::string>{"cap"},
                   "mixed ref side B body names");
            expect(r.aligned_faces > 0 && r.aligned_area > 0 && r.cut_faces == 0,
                   "mixed ref aligned vs cut counts");
            mixed_listed = true;
        }
    expect(mixed_listed, "user-written mixed interface recorded");

    StackModel soldered;
    soldered.id = "soldered";
    soldered.outline.box = {0, 0, 1, 1};
    Layer substrate;
    substrate.name = "substrate";
    substrate.kind = "Explicit";
    substrate.material = "organic";
    substrate.thickness = .5;
    substrate.n_elem_min = 2;
    substrate.regions = {{"solder_a", "solder", "solder_a", {{.1, .1, .4, .4}, {}}},
                         {"solder_b", "solder", "solder_b", {{.6, .6, .9, .9}, {}}}};
    soldered.layers = {substrate};
    auto solder_opts = o;
    solder_opts.nx = 8;
    solder_opts.ny = 8;
    solder_opts.ranks = 1;
    auto solder_grid = build_tensor_grid(soldered, solder_opts);
    expect(bool(solder_grid), "substrate/solder grid");
    auto& sm = solder_grid.value();
    auto wildcard =
        resolve_activation(soldered, sm, nullptr, {"interface:layer:substrate|body:solder_*"});
    expect(bool(wildcard), "wildcard mixed interface");
    bool wildcard_listed = false;
    for (const auto& r : sm.preparation_report)
        if (r.name == "interface:layer:substrate|body:solder_*") {
            expect(r.side_a_bodies == std::vector<std::string>{"substrate"},
                   "wildcard side A is substrate matrix");
            expect(r.side_b_bodies == std::vector<std::string>{"solder_a", "solder_b"},
                   "wildcard side B solder bodies");
            expect(r.aligned_faces > 0 && r.aligned_area > 0, "wildcard shared aligned faces");
            wildcard_listed = true;
        }
    expect(wildcard_listed, "user-written wildcard interface recorded");
    auto overlap = resolve_activation(s, m, nullptr, {"group:overlap"});
    auto layer = resolve_activation(s, m, nullptr, {"layer:base"});
    expect(overlap.value().cells.attributes == layer.value().cells.attributes, "overlap union");
    for (auto ref :
         {"layer:missing", "body:none*", "group:missing", "faceset:missing", "interface:bad|top"})
        expect(!resolve_activation(s, m, nullptr, {ref}), "bad reference");
    expect(bool(check_no_reactivation(s, m, {{{"layer:base"}, {}}, {{}, {"region:die"}}})),
           "valid steps");
    expect(!check_no_reactivation(s, m, {{{}, {"layer:base"}}, {{"body:die"}, {}}}),
           "alias reactivation");
    expect(mask_hash({1, 2, 1}) == mask_hash({2, 1}), "canonical mask");
    expect(!resolve_activation(s, m, nullptr, {"*"}), "prefix required");
    expect(!check_no_reactivation(s, m, {{{"body:die"}, {"region:die"}}}), "same-step conflict");
    auto metis_options = o;
    metis_options.use_metis = true;
    auto metis_grid = build_tensor_grid(s, metis_options);
    expect(bool(metis_grid), "METIS or SFC fallback");
    for (std::size_t e = 0; e < metis_grid.value().partitioning.size(); ++e)
        expect(metis_grid.value().partitioning[e] == metis_grid.value().partitioning[(e / nz) * nz],
               "METIS column ownership");
    auto huge = o;
    huge.nx = std::numeric_limits<Local>::max();
    expect(!build_tensor_grid(s, huge), "budget checked before allocation");
    auto duplicate = s;
    duplicate.layers[1].regions = duplicate.layers[0].regions;
    expect(!build_tensor_grid(duplicate, o), "cross-layer duplicate");
    auto graded = s;
    graded.layers[0].grading = o.max_ratio;
    auto g = build_tensor_grid(graded, o);
    expect(bool(g), "geometric grading");
    expect(g.value().axes[2][2] - g.value().axes[2][1] > g.value().axes[2][1], "increasing sizes");
    graded.layers[0].grading = o.max_ratio + 1;
    expect(!build_tensor_grid(graded, o), "grading limit");
    std::set<std::uint64_t> codes, hcodes;
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x) {
            codes.insert(morton(x, y));
            hcodes.insert(hilbert(x, y, 4));
        }
    expect(codes.size() == 256 && hcodes.size() == 256, "SFC bijection");
    std::cout << "cells=" << m.attribute.size() << " volume=" << volume << '\n';
}
