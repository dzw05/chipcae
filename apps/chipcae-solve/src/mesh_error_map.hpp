#pragma once
#include "chipcae/solve/case_loader.hpp"

namespace chipcae::solve {
// TEST-FIRST compile stubs: replace with the contract implementation.
inline ErrorCode mesh_error_code(const mesh::Error&) {
    return ErrorCode::INPUT_INVALID;
}
// Precondition: nz > 0; elem_gid and partitioning have equal lengths.
inline void validate_column_partition(const mesh::HexMeshArrays&, std::size_t) {}
} // namespace chipcae::solve
