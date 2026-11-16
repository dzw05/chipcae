#pragma once
#include "hex_mesh_arrays.hpp"
namespace chipcae::mesh {
class ColumnExtrudePartitioner {
public:
    static Expected<std::vector<Local>>
    partition(Local nx, Local ny, Local nz, Local ranks, bool use_metis = false);
};
} // namespace chipcae::mesh
