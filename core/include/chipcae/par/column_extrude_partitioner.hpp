#pragma once
#include "chipcae/core/memory_space.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
namespace chipcae::par {
/// Structured aligned hexes: element = (k * ny + j) * nx + i.
/// Graph vertices are (i,j), with four-neighbor edges and weight nz.
/// This interface returns only a column-to-rank map, replicated along z, so
/// every element in a column belongs to the same rank. The structured order
/// above is internal: it must not appear in public interfaces, files, or halo data.
class ColumnExtrudePartitioner {
public:
    static bool uses_metis() noexcept;
    static View1<std::int32_t, HostSpace>
    partition(std::size_t nx,
              std::size_t ny,
              std::size_t nz,
              int parts,
              std::span<const std::int32_t> column_weights = {});
};
} // namespace chipcae::par
