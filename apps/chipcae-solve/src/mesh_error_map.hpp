#pragma once
#include "chipcae/solve/case_loader.hpp"

#include <unordered_map>

namespace chipcae::solve {
inline ErrorCode mesh_error_code(const mesh::Error& error) {
    if (error.path == "COLUMN_SPLIT_ACROSS_RANKS")
        return ErrorCode::COLUMN_SPLIT_ACROSS_RANKS;
    if (error.path == "HIERARCHY_INVALID" || error.path == "hierarchy/levels")
        return ErrorCode::HIERARCHY_INVALID;
    if (error.path == "CUT_UNSUPPORTED" || error.path == "outline.unsupported")
        return ErrorCode::CUT_UNSUPPORTED;
    return ErrorCode::INPUT_INVALID;
}
// Precondition: nz > 0; elem_gid and partitioning have equal lengths.
// Generated element IDs enumerate each column's nz cells consecutively.
inline void validate_column_partition(const mesh::HexMeshArrays& mesh, std::size_t nz) {
    std::unordered_map<Index, mesh::Local> column_ranks;
    for (std::size_t e = 0; e < mesh.elem_gid.size(); ++e) {
        const auto column = mesh.elem_gid[e] / static_cast<Index>(nz);
        const auto rank = mesh.partitioning[e];
        const auto [first, inserted] = column_ranks.emplace(column, rank);
        if (!inserted && first->second != rank)
            throw InputError(ErrorCode::COLUMN_SPLIT_ACROSS_RANKS,
                             "column " + std::to_string(column) + " spans ranks " +
                                 std::to_string(first->second) + " and " + std::to_string(rank));
    }
}
} // namespace chipcae::solve
