#include "chipcae/solve/case_loader.hpp"
namespace chipcae::solve {
mesh::HexMeshArrays build_mesh(const RunInputs& inputs) {
    auto result = mesh::build_tensor_grid(inputs.stack, inputs.grid);
    if (!result)
        throw InputError(ErrorCode::INPUT_INVALID,
                         result.error().path + ": " + result.error().reason);
    auto mesh = std::move(result.value());
    if (!mesh.pending_cut.empty())
        throw InputError(ErrorCode::CUT_UNSUPPORTED, "cut not supported in O2");
    const auto nz = mesh.axes[2].size() - 1;
    for (std::size_t e = 0; e < mesh.partitioning.size(); ++e)
        if (mesh.partitioning[e] != mesh.partitioning[(e / nz) * nz])
            throw InputError(ErrorCode::COLUMN_SPLIT_ACROSS_RANKS, "Column spans multiple ranks");
    return mesh;
}
} // namespace chipcae::solve
