#pragma once

#include "../../schema/src/sha256.hpp"
#include "chipcae/mesh/background/hex_mesh_arrays.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace chipcae::preview::detail {
struct MeshDigest {
    std::size_t nx0, ny0, levels, nz, nodes, cells, padded_cells;
    std::string sha256;
    std::size_t void_cells = 0;
};
inline void append_i64(std::string& bytes, std::int64_t value) {
    auto bits = static_cast<std::uint64_t>(value);
    for (int i = 0; i < 8; ++i)
        bytes.push_back(static_cast<char>(bits >> (8 * i)));
}
inline void append_f64(std::string& bytes, double value) {
    auto bits = std::bit_cast<std::uint64_t>(value);
    for (int i = 0; i < 8; ++i)
        bytes.push_back(static_cast<char>(bits >> (8 * i)));
}
inline MeshDigest digest(const chipcae::mesh::HexMeshArrays& mesh) {
    const auto& report = mesh.grid_report;
    const auto& coarse = report.level_axes.front();
    const auto nx0 = coarse[0].size() - 1, ny0 = coarse[1].size() - 1;
    const auto nz = mesh.axes[2].size() - 1;
    const auto nodes = mesh.node_gid.size(), cells = mesh.elem_gid.size();
    std::string bytes;
    bytes.reserve(48 + nodes * 24 + cells * 64);
    for (auto v : {nx0, ny0, static_cast<std::size_t>(report.levels - 1), nz, nodes, cells})
        append_i64(bytes, static_cast<std::int64_t>(v));
    std::vector<std::size_t> order(nodes);
    for (std::size_t i = 0; i < nodes; ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](auto a, auto b) {
        return mesh.node_gid[a] < mesh.node_gid[b];
    });
    for (auto i : order)
        for (int d = 0; d < 3; ++d)
            append_f64(bytes, mesh.vertices[3 * i + d]);
    order.resize(cells);
    for (std::size_t i = 0; i < cells; ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](auto a, auto b) {
        return mesh.elem_gid[a] < mesh.elem_gid[b];
    });
    for (auto i : order)
        for (int n = 0; n < 8; ++n)
            append_i64(bytes, mesh.node_gid[mesh.hex[8 * i + n]]);
    const auto nx = mesh.axes[0].size() - 1, ny = mesh.axes[1].size() - 1;
    if (report.padding[0] > nx || report.padding[1] > ny)
        throw std::invalid_argument("mesh padding exceeds axis cell count");
    const auto padded_cells = (nx * ny - (nx - report.padding[0]) * (ny - report.padding[1])) * nz;
    return {nx0,
            ny0,
            static_cast<std::size_t>(report.levels - 1),
            nz,
            nodes,
            cells,
            padded_cells,
            chipcae::mesh::sha256(bytes),
            static_cast<std::size_t>(report.void_cells)};
}
} // namespace chipcae::preview::detail
