#include "chipcae/mesh/cut/cut_stats.hpp"

#include "chipcae/mesh/cut/plane_cut.hpp"
#include "support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using chipcae::BodyId;
using chipcae::Real;
using namespace chipcae::mesh;
using namespace chipcae::mesh::cut;
using k1::expect;

template <class T>
T require_value(Expected<T> result, const char* message) {
    if (!result) {
        const auto& error = result.error();
        expect(false, (std::string(message) + " [" + error.path + "] " + error.reason).c_str());
    }
    return std::move(result.value());
}

CutGeometry plane(Real delta, BodyId body = 0) {
    return {{{body, {1, 0, 0}, 2.0 + delta}}};
}

void check_counts(const CutStats& stats, const std::array<std::size_t, 6>& expected) {
    const auto [total, full, cut, sub, empty, recorded] = expected;
    expect(stats.total_cells == total, "total cell count");
    expect(stats.full_cells == full, "full cell count");
    expect(stats.cut_cells == cut, "cut cell count");
    expect(stats.sub_cells == sub, "subcell count");
    expect(stats.void_cells == empty, "void cell count");
    expect(stats.small_subs_recorded == recorded, "recorded small subcell count");
}

void check_volume(Real actual, Real expected, const char* message) {
    static const Real tolerance = k1::threshold("cut_volume_relative");
    expect(std::abs(actual - expected) <= tolerance * expected, message);
}

void check_body_volume_sum(const CutStats& stats) {
    if (!stats.body_volume.empty()) {
        Real sum = 0;
        for (const auto& [body, volume] : stats.body_volume)
            sum += volume;
        expect(std::abs(sum - stats.solid_volume) <= 1e-12 * stats.solid_volume,
               "sum of body volumes agrees with solid volume");
    }
}

void check_solid_volume(const CutStats& stats, Real expected) {
    check_volume(stats.solid_volume, expected, "analytic solid volume");
    check_body_volume_sum(stats);
}

void check_minimum(const CutStats& stats, Real expected) {
    expect(stats.min_sub_volfrac && std::abs(*stats.min_sub_volfrac - expected) <= 1e-14,
           "minimum subcell volume fraction");
}

void stats_sliver(const HexMeshArrays& mesh,
                  const CutOptions& options,
                  Real delta,
                  std::size_t recorded,
                  const std::array<std::size_t, 4>& bins) {
    const auto cut =
        require_value(build_cut_layer(mesh, plane(delta), options), "build sliver cut");
    const auto stats = require_value(cut_stats(mesh, cut), "summarize sliver cut");
    check_counts(stats, {16, 8, 4, 4, 4, recorded});
    expect(stats.small_vf_cumulative == bins, "sliver volume fraction bins");
    check_minimum(stats, (2.0 + delta) - 2.0);
    check_solid_volume(stats, 8 + 4 * delta);
}

void stats_void_modes(const HexMeshArrays& original, CutOptions options) {
    const auto geometry = plane(0.5);
    const auto const_mesh = original;
    auto removed_mesh = original;
    auto retained_mesh = original;
    const auto const_cut =
        require_value(build_cut_layer(const_mesh, geometry, options), "const cut");
    options.keep_void = false;
    const auto removed_cut =
        require_value(build_cut_layer(removed_mesh, geometry, options), "cut removing voids");
    options.keep_void = true;
    const auto retained_cut =
        require_value(build_cut_layer(retained_mesh, geometry, options), "cut retaining voids");

    const auto const_stats = require_value(cut_stats(const_mesh, const_cut), "summarize const cut");
    const auto removed_stats =
        require_value(cut_stats(removed_mesh, removed_cut), "summarize cut removing voids");
    const auto retained_stats =
        require_value(cut_stats(retained_mesh, retained_cut), "summarize cut retaining voids");
    check_counts(const_stats, {16, 8, 4, 4, 4, 0});
    check_counts(removed_stats, {12, 8, 4, 4, 0, 0});
    check_counts(retained_stats, {16, 8, 4, 4, 4, 0});
    check_solid_volume(const_stats, 10);
    check_solid_volume(removed_stats, 10);
    check_solid_volume(retained_stats, 10);
}

void stats_two_bodies(StackModel stack,
                      const GridOptions& grid_options,
                      const CutOptions& options) {
    stack.layers.front().thickness = 2;
    stack.layers.front().body = "lower";
    auto upper = stack.layers.front();
    upper.name = "top";
    upper.body = "upper";
    upper.material = "silicon";
    stack.layers.push_back(upper);

    const auto mesh = require_value(build_tensor_grid(stack, grid_options), "build two-body grid");
    expect(mesh.attribute.size() == 32, "two-body grid has 32 cells");
    const auto lower_id = mesh.attr_table.bodies.at("lower");
    const auto upper_id = mesh.attr_table.bodies.at("upper");
    const auto cut =
        require_value(build_cut_layer(mesh, plane(0.5, upper_id), options), "cut upper body");
    for (auto cell : cut.cut_elem) {
        const auto attr = mesh.attribute.at(static_cast<std::size_t>(cell));
        expect(mesh.attr_table.entries.at(static_cast<std::size_t>(attr - 1)).body == upper_id,
               "every cut parent belongs to the upper body");
    }

    const auto stats = require_value(cut_stats(mesh, cut), "summarize two-body cut");
    check_counts(stats, {32, 24, 4, 4, 4, 0});
    expect(stats.body_volume.size() == 2 && stats.body_volume.contains(lower_id) &&
               stats.body_volume.contains(upper_id),
           "two-body volume keys");
    check_volume(stats.body_volume.at(lower_id), 32, "uncut lower-body volume");
    check_volume(stats.body_volume.at(upper_id), 20, "cut upper-body volume");
    check_solid_volume(stats, 52);
}

template <class Run>
void stats_invalid(const HexMeshArrays& mesh, const CutLayerArrays& cut, const Run& run) {
    expect(cut.cut_elem.size() == 4 && cut.void_elem.size() == 4 && cut.sub_volfrac.size() == 4 &&
               cut.sub_attribute.size() == 4 && cut.sub_off.size() == 5 && cut.vq_off.size() == 5,
           "half-cut fixture for invalid inputs");
    const auto cells = std::views::iota(Local{0}, static_cast<Local>(mesh.attribute.size()));
    const auto full = std::ranges::find_if(cells, [&](Local cell) {
        return std::ranges::find(cut.cut_elem, cell) == cut.cut_elem.end() &&
               std::ranges::find(cut.void_elem, cell) == cut.void_elem.end();
    });
    expect(full != cells.end(), "half-cut fixture has a full parent");
    const auto full_cell = static_cast<std::size_t>(*full);
    const auto cut_cell = static_cast<std::size_t>(cut.cut_elem.front());

    const auto invalid = [&](const std::string& name, std::string_view prefix, auto mutate) {
        run("stats_invalid / " + name, [&] {
            auto bad_mesh = mesh;
            auto bad_cut = cut;
            mutate(bad_mesh, bad_cut);
            const auto result = cut_stats(bad_mesh, bad_cut);
            expect(!result, "expected rejection with INPUT_INVALID");
            expect(result.error().path == "INPUT_INVALID", "error path must be INPUT_INVALID");
            expect(
                result.error().reason.starts_with(prefix),
                ("expected error prefix " + std::string(prefix) + ", got " + result.error().reason)
                    .c_str());
        });
    };

    constexpr std::string_view index_prefix = "cut_stats: index out of range";
    constexpr std::string_view attribute_prefix = "cut_stats: attribute out of range";
    for (Local index : {-1, static_cast<Local>(mesh.attribute.size())}) {
        invalid("cut index " + std::to_string(index), index_prefix, [=](auto&, auto& c) {
            c.cut_elem.front() = index;
        });
        invalid("void index " + std::to_string(index), index_prefix, [=](auto&, auto& c) {
            c.void_elem.front() = index;
        });
    }
    invalid("duplicate cut index", "cut_stats: duplicate element", [](auto&, auto& c) {
        c.cut_elem[1] = c.cut_elem.front();
    });
    invalid("duplicate void index", "cut_stats: duplicate element", [](auto&, auto& c) {
        c.void_elem[1] = c.void_elem.front();
    });
    invalid("cut and void overlap", "cut_stats: cut and void overlap", [](auto&, auto& c) {
        c.void_elem.front() = c.cut_elem.front();
    });
    invalid("truncated sub_off", "cut_stats: sub_off mismatch", [](auto&, auto& c) {
        c.sub_off.pop_back();
    });
    invalid("negative sub offset", "cut_stats: sub_off mismatch", [](auto&, auto& c) {
        c.sub_off[1] = -1;
    });
    invalid("negative VQ offset", "cut_stats: vq_off mismatch", [](auto&, auto& c) {
        c.vq_off[1] = -1;
    });
    invalid("negative VQ offset with wrong length",
            "cut_stats: vq_off mismatch",
            [](auto&, auto& c) { c.vq_off = {-1}; });
    invalid("VQ terminal offset beyond weights", "cut_stats: vq_off mismatch", [](auto&, auto& c) {
        c.vq_off.back() = static_cast<Local>(c.vq_w.size() + 1);
    });
    for (Local attr : {0, static_cast<Local>(mesh.attr_table.entries.size() + 1)}) {
        invalid("full parent attribute " + std::to_string(attr),
                attribute_prefix,
                [=](auto& m, auto&) { m.attribute[full_cell] = attr; });
        invalid("cut parent attribute " + std::to_string(attr),
                attribute_prefix,
                [=](auto& m, auto&) { m.attribute[cut_cell] = attr; });
        invalid("subcell attribute " + std::to_string(attr), attribute_prefix, [=](auto&, auto& c) {
            c.sub_attribute.front() = attr;
        });
    }
}
} // namespace

int main(int argc, char** argv) {
#ifdef CHIPCAE_ENABLE_KOKKOS
    Kokkos::ScopeGuard guard(argc, argv);
#else
    (void)argc;
    (void)argv;
#endif

    auto stack = k1::stack();
    stack.activation_groups.clear();
    stack.layers.resize(1);
    stack.layers.front().regions.clear();
    stack.layers.front().thickness = 1;
    stack.layers.front().n_elem_min = 1;
    stack.outline.box = {0, 0, 4, 4};
    auto grid_options = k1::options();
    grid_options.nx = 4;
    grid_options.ny = 4;
    grid_options.levels = 1;
    const auto mesh = require_value(build_tensor_grid(stack, grid_options), "build unit-cell grid");
    expect(mesh.attribute.size() == 16, "single-layer grid has 16 cells");
    CutOptions options;
    options.snap_phi = k1::threshold("snap_phi");
    options.small_sub_vf = k1::threshold("small_sub_vf");
    const auto half = require_value(build_cut_layer(mesh, plane(0.5), options), "build half cut");

    int failures = 0;
    const auto run = [&](std::string_view name, auto test) {
        try {
            test();
        } catch (const std::runtime_error& error) {
            std::cerr << name << ": " << error.what() << '\n';
            ++failures;
        }
    };

    run("stats_no_planes", [&] {
        const auto cut =
            require_value(build_cut_layer(mesh, {}, options), "build cut with no planes");
        const auto stats = require_value(cut_stats(mesh, cut), "summarize cut with no planes");
        check_counts(stats, {16, 16, 0, 0, 0, 0});
        expect(stats.small_vf_cumulative == std::array<std::size_t, 4>{}, "no-plane bins");
        expect(!stats.min_sub_volfrac && !stats.max_vq_relative_error,
               "no-plane diagnostics empty");
        check_solid_volume(stats, 16);
        const auto body = mesh.attr_table.bodies.at(stack.layers.front().name);
        expect(stats.body_volume.size() == 1 && stats.body_volume.contains(body),
               "no-plane body key");
        check_volume(stats.body_volume.at(body), 16, "no-plane body volume");
    });
    run("stats_sliver_1e-9", [&] { stats_sliver(mesh, options, 1e-9, 4, {4, 4, 4, 4}); });
    run("stats_sliver_5e-5", [&] { stats_sliver(mesh, options, 5e-5, 0, {0, 0, 4, 4}); });
    run("stats_half", [&] {
        const auto stats = require_value(cut_stats(mesh, half), "summarize half cut");
        check_counts(stats, {16, 8, 4, 4, 4, 0});
        expect(stats.small_vf_cumulative == std::array<std::size_t, 4>{}, "half-cut bins");
        check_minimum(stats, 0.5);
        check_solid_volume(stats, 10);
        expect(stats.max_vq_relative_error && *stats.max_vq_relative_error <= 1e-12,
               "half-cut quadrature relative error");
    });
    run("stats_void_modes", [&] { stats_void_modes(mesh, options); });
    run("stats_two_bodies", [&] { stats_two_bodies(stack, grid_options, options); });
    run("stats_empty_mesh", [&] {
        const HexMeshArrays empty_mesh;
        CutLayerArrays cut;
        const auto check_empty = [&] {
            const auto stats = require_value(cut_stats(empty_mesh, cut), "summarize empty mesh");
            check_counts(stats, {});
            expect(stats.small_vf_cumulative == std::array<std::size_t, 4>{}, "empty mesh bins");
            expect(stats.solid_volume == 0 && stats.body_volume.empty(), "empty mesh volumes");
            expect(!stats.min_sub_volfrac && !stats.max_vq_relative_error,
                   "empty mesh diagnostics");
        };
        check_empty();
        cut.sub_off.clear();
        check_empty();
    });
    run("stats_invalid", [&] { stats_invalid(mesh, half, run); });
    run("stats_bin_edges", [&] {
        constexpr std::array<Real, 4> bin_edges{1e-8, 1e-6, 1e-4, 1e-2};
        static_assert(kSmallVfBins == bin_edges);
        auto cut = half;
        cut.sub_volfrac.assign(bin_edges.begin(), bin_edges.end());
        cut.vq_off = {0};
        const auto stats = require_value(cut_stats(mesh, cut), "summarize bin-edge fractions");
        expect(stats.small_vf_cumulative == std::array<std::size_t, 4>{0, 1, 2, 3},
               "bin edges use strict less-than");
        expect(stats.small_subs_recorded == 0, "recorded small subcells come from small_subs");
        expect(!stats.max_vq_relative_error, "no quadrature diagnostic for short vq_off");
        check_body_volume_sum(stats);
    });
    run("stats_vq_error", [&] {
        auto cut = half;
        cut.vq_off = {0, 1, 2, 3, 4};
        cut.vq_w = {0.5, 0.25, 0.5, 0.5};
        const auto stats = require_value(cut_stats(mesh, cut), "summarize perturbed quadrature");
        expect(stats.max_vq_relative_error && std::abs(*stats.max_vq_relative_error - 0.5) <= 1e-12,
               "maximum quadrature relative error is 0.5");
        check_solid_volume(stats, 10);
    });
    return failures == 0 ? 0 : 1;
}
