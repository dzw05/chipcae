#include "chipcae/mesh/cut/outline_cut.hpp"
#include "chipcae/mesh/cut/region_cut.hpp"
#include "chipcae/solve/case_loader.hpp"
#include "mesh_error_map.hpp"

#include <map>
#include <set>
namespace chipcae::solve {
mesh::HexMeshArrays build_mesh(const RunInputs& inputs,
                               std::optional<mesh::cut::CutLayerArrays>* cut) {
    // Build the complete bounding grid first: clipping must not discard cells
    // whose centers lie outside the physical outline.
    auto stack = inputs.stack;
    stack.outline.polygon.clear();
    // Match preview's stable sidecar merge, relative to the stack file.
    std::set<std::filesystem::path> sidecars;
    for (const auto& layer : stack.layers)
        if (layer.copper && layer.copper->polygons)
            sidecars.insert(inputs.stack_path.parent_path() / layer.copper->polygons->file);
    mesh::Polyset polygons;
    std::set<std::string> names;
    for (const auto& path : sidecars) {
        auto part = mesh::read_polyset(path);
        if (!part)
            throw InputError(ErrorCode::INPUT_INVALID, part.error().reason);
        if (polygons.dbu_m != 0 && polygons.dbu_m != part.value().dbu_m)
            throw InputError(ErrorCode::INPUT_INVALID, "polysets must use the same database unit");
        polygons.dbu_m = part.value().dbu_m;
        for (auto& layer : part.value().layers) {
            if (!names.insert(layer.name).second)
                throw InputError(ErrorCode::INPUT_INVALID,
                                 "duplicate polyset layer name: " + layer.name);
            polygons.layers.push_back(std::move(layer));
        }
    }
    auto result = sidecars.empty() ? mesh::build_tensor_grid(stack, inputs.grid)
                                   : mesh::build_tensor_grid(stack, inputs.grid, polygons);
    if (!result) {
        const auto& err = result.error();
        const auto code = mesh_error_code(err);
        if (code.code() == ErrorCode::CUT_UNSUPPORTED &&
            err.reason.ends_with(": polygon in material layer"))
            throw InputError(code.code(),
                             "internal material cuts are not supported: " + err.path + ": " +
                                 err.reason);
        throw InputError(code.code(), err.path + ": " + err.reason);
    }
    auto mesh = std::move(result.value());
    if (mesh.elem_gid.empty())
        throw InputError(ErrorCode::INPUT_INVALID, "empty mesh");
    auto region_planes = mesh::cut::region_cut_planes(inputs.stack, mesh);
    if (!region_planes) {
        const auto& err = region_planes.error();
        throw InputError(mesh_error_code(err).code(), err.path + ": " + err.reason);
    }
    std::set<BodyId> covered;
    for (const auto& plane : region_planes.value().planes)
        covered.insert(plane.body);
    std::map<BodyId, std::string> body_names;
    for (const auto& [name, id] : mesh.attr_table.bodies)
        body_names.emplace(id, name);
    std::set<std::string> missing;
    for (auto e : mesh.pending_cut) {
        const auto body = mesh.attr_table.entries.at(mesh.attribute.at(e) - 1).body;
        if (!covered.contains(body))
            missing.insert(body_names.at(body));
    }
    if (!missing.empty())
        throw InputError(ErrorCode::CUT_UNSUPPORTED,
                         "pending cut cell without cut planes: " + *missing.begin());
    const auto nz = mesh.axes[2].size() - 1;
    validate_column_partition(mesh, nz);
    if (cut)
        cut->reset();
    if (inputs.stack.outline.polygon.empty() && region_planes.value().planes.empty())
        return mesh;
    if (!cut)
        throw InputError(ErrorCode::CUT_UNSUPPORTED, "cut data receiver is required");
    auto clipped = mesh::cut::apply_outline_cut(
        mesh, inputs.stack.outline, inputs.cut_options, region_planes.value());
    if (!clipped) {
        const auto& err = clipped.error();
        throw InputError(mesh_error_code(err).code(), err.path + ": " + err.reason);
    }
    *cut = std::move(clipped.value());
    return mesh;
}
} // namespace chipcae::solve
