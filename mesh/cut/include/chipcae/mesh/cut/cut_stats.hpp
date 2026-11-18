#pragma once
#include "chipcae/mesh/background/hex_mesh_arrays.hpp"
#include "chipcae/mesh/cut/cut_layer.hpp"
#include "chipcae/schema/stack.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace chipcae::mesh::cut {
inline constexpr std::array<Real, 4> kSmallVfBins{1e-8, 1e-6, 1e-4, 1e-2};

struct CutStats {
    std::size_t total_cells = 0;
    std::size_t full_cells = 0;
    std::size_t cut_cells = 0;
    std::size_t sub_cells = 0;
    std::size_t void_cells = 0;
    std::size_t small_subs_recorded = 0;
    // Entry i counts fractions strictly below kSmallVfBins[i].
    std::array<std::size_t, 4> small_vf_cumulative{};
    std::optional<Real> min_sub_volfrac;
    Real solid_volume = 0;
    std::map<BodyId, Real> body_volume;
    // Present when quadrature offsets are available and a fraction is nonzero.
    std::optional<Real> max_vq_relative_error;
};

// Pair the cut arrays with the mesh as it was left by the same build_cut_layer call.
inline Expected<CutStats> cut_stats(const HexMeshArrays& m, const CutLayerArrays& c) {
    const auto total = m.attribute.size();
    const auto cut_count = c.cut_elem.size();
    const auto sub_count = c.sub_volfrac.size();
    // Values below cut_count identify the cut parent's CSR row.
    const auto full_cell = cut_count;
    const auto void_cell = cut_count + 1;
    std::vector<std::size_t> cut_row(total, full_cell);
    for (std::size_t row = 0; row < cut_count; ++row) {
        const auto cell = c.cut_elem[row];
        if (cell < 0 || static_cast<std::size_t>(cell) >= total)
            return Error{"INPUT_INVALID", "cut_stats: index out of range in cut_elem"};
        auto& membership = cut_row[static_cast<std::size_t>(cell)];
        if (membership != full_cell)
            return Error{"INPUT_INVALID", "cut_stats: duplicate element in cut_elem"};
        membership = row;
    }
    for (const auto cell : c.void_elem) {
        if (cell < 0 || static_cast<std::size_t>(cell) >= total)
            return Error{"INPUT_INVALID", "cut_stats: index out of range in void_elem"};
        auto& membership = cut_row[static_cast<std::size_t>(cell)];
        if (membership == void_cell)
            return Error{"INPUT_INVALID", "cut_stats: duplicate element in void_elem"};
        if (membership != full_cell)
            return Error{"INPUT_INVALID", "cut_stats: cut and void overlap"};
        membership = void_cell;
    }

    const std::array<Local, 1> empty_offsets{0};
    const auto sub_off = c.sub_off.empty() ? std::span<const Local>{empty_offsets}
                                           : std::span<const Local>{c.sub_off};
    const auto valid_offsets = [](std::span<const Local> offsets, std::size_t value_count) {
        return offsets.front() == 0 && std::ranges::is_sorted(offsets) &&
               static_cast<std::size_t>(offsets.back()) == value_count;
    };
    if (sub_off.size() != cut_count + 1 || sub_count != c.sub_attribute.size() ||
        !valid_offsets(sub_off, sub_count))
        return Error{"INPUT_INVALID", "cut_stats: sub_off mismatch"};

    const auto& entries = m.attr_table.entries;
    const auto valid_attribute = [&](Local attr) {
        return attr > 0 && static_cast<std::size_t>(attr) <= entries.size();
    };
    for (std::size_t cell = 0; cell < total; ++cell) {
        if (cut_row[cell] != void_cell && !valid_attribute(m.attribute[cell]))
            return Error{"INPUT_INVALID", "cut_stats: attribute out of range in mesh"};
    }
    if (!std::ranges::all_of(c.sub_attribute, valid_attribute))
        return Error{"INPUT_INVALID", "cut_stats: attribute out of range in sub_attribute"};

    const bool has_vq = sub_count != 0 && c.vq_off.size() == sub_count + 1;
    if (std::ranges::any_of(c.vq_off, [](Local offset) { return offset < 0; }) ||
        (has_vq && !valid_offsets(c.vq_off, c.vq_w.size())))
        return Error{"INPUT_INVALID", "cut_stats: vq_off mismatch"};

    CutStats stats;
    stats.total_cells = total;
    stats.cut_cells = cut_count;
    stats.sub_cells = sub_count;
    stats.void_cells = c.void_elem.size();
    stats.full_cells = total - cut_count - stats.void_cells;
    stats.small_subs_recorded = c.small_subs.size();

    const auto add_volume = [&](Local attr, Real volume) {
        stats.solid_volume += volume;
        stats.body_volume[entries[static_cast<std::size_t>(attr - 1)].body] += volume;
    };
    for (std::size_t cell = 0; cell < total; ++cell) {
        const auto row = cut_row[cell];
        if (row == void_cell)
            continue;

        const auto lo = 3 * static_cast<std::size_t>(m.hex[8 * cell]);
        const auto hi = 3 * static_cast<std::size_t>(m.hex[8 * cell + 6]);
        const Real jac = (m.vertices[hi] - m.vertices[lo]) *
                         (m.vertices[hi + 1] - m.vertices[lo + 1]) *
                         (m.vertices[hi + 2] - m.vertices[lo + 2]);
        if (row == full_cell) {
            add_volume(m.attribute[cell], jac);
        } else {
            const auto begin = static_cast<std::size_t>(sub_off[row]);
            const auto end = static_cast<std::size_t>(sub_off[row + 1]);
            for (auto sub = begin; sub < end; ++sub)
                add_volume(c.sub_attribute[sub], jac * c.sub_volfrac[sub]);
        }
    }

    for (std::size_t sub = 0; sub < sub_count; ++sub) {
        const Real vf = c.sub_volfrac[sub];
        for (std::size_t bin = 0; bin < kSmallVfBins.size(); ++bin) {
            if (vf < kSmallVfBins[bin])
                ++stats.small_vf_cumulative[bin];
        }
        if (!stats.min_sub_volfrac || vf < *stats.min_sub_volfrac)
            stats.min_sub_volfrac = vf;

        if (!has_vq || vf == 0)
            continue;
        const auto begin = static_cast<std::size_t>(c.vq_off[sub]);
        const auto end = static_cast<std::size_t>(c.vq_off[sub + 1]);
        Real sum = 0;
        for (auto q = begin; q < end; ++q)
            sum += c.vq_w[q];
        const Real error = std::abs(sum - vf) / vf;
        if (!stats.max_vq_relative_error || error > *stats.max_vq_relative_error)
            stats.max_vq_relative_error = error;
    }
    return stats;
}
} // namespace chipcae::mesh::cut
