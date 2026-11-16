#include "chipcae/mesh/background/activation.hpp"
#include "chipcae/mesh/background/sfc.hpp"
#include "support.hpp"

#include <algorithm>
#include <iostream>
#include <set>
using namespace chipcae::mesh;
using k1::expect;
int main() {
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
    std::size_t columns = (m.axes[0].size() - 1) * (m.axes[1].size() - 1);
    std::vector<int> load(4);
    for (std::size_t e = 0; e < m.attribute.size(); ++e) {
        expect(m.partitioning[e] == m.partitioning[e % columns], "column ownership");
        ++load[m.partitioning[e]];
    }
    for (auto n : load)
        expect(std::abs(double(n) * 4 / m.attribute.size() - 1) <=
                   k1::threshold("partition_imbalance"),
               "rank balance");
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
