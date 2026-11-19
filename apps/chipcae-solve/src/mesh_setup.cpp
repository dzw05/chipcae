#include "chipcae/mesh/cut/outline_cut.hpp"
#include "chipcae/solve/case_loader.hpp"
#include "mesh_error_map.hpp"
namespace chipcae::solve {
mesh::HexMeshArrays build_mesh(const RunInputs& inputs,
                               std::optional<mesh::cut::CutLayerArrays>* cut) {
    // Build the complete bounding grid first: clipping must not discard cells
    // whose centers lie outside the physical outline.
    auto stack = inputs.stack;
    stack.outline.polygon.clear();
    auto result = mesh::build_tensor_grid(stack, inputs.grid);
    if (!result) {
        const auto& err = result.error();
        const auto code = mesh_error_code(err);
        throw InputError(code.code(), err.path + ": " + err.reason);
    }
    auto mesh = std::move(result.value());
    if (!mesh.pending_cut.empty())
        throw InputError(ErrorCode::CUT_UNSUPPORTED, "internal material cuts are not supported");
    const auto nz = mesh.axes[2].size() - 1;
    validate_column_partition(mesh, nz);
    if (cut)
        cut->reset();
    if (inputs.stack.outline.polygon.empty())
        return mesh;
    if (!cut)
        throw InputError(ErrorCode::CUT_UNSUPPORTED, "cut data receiver is required");
    auto clipped = mesh::cut::apply_outline_cut(mesh, inputs.stack.outline, inputs.cut_options);
    if (!clipped) {
        const auto& err = clipped.error();
        throw InputError(mesh_error_code(err).code(), err.path + ": " + err.reason);
    }
    *cut = std::move(clipped.value());
    return mesh;
}
} // namespace chipcae::solve
