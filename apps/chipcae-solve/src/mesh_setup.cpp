#include "chipcae/solve/case_loader.hpp"
#include "mesh_error_map.hpp"

#include <cmath>
namespace chipcae::solve {
mesh::HexMeshArrays build_mesh(const RunInputs& inputs,
                               std::optional<mesh::cut::CutLayerArrays>* cut) {
    // Build the complete bounding grid first: clipping must not discard cells
    // whose centers lie outside the physical outline.
    for (auto v : inputs.stack.outline.polygon) {
        const auto& box = inputs.stack.outline.box;
        if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || v[0] < box[0] || v[0] > box[2] ||
            v[1] < box[1] || v[1] > box[3])
            throw InputError(ErrorCode::INPUT_INVALID,
                             "outline polygon vertex outside outline.box");
    }
    auto stack = inputs.stack;
    stack.outline.polygon.clear();
    auto result = mesh::build_tensor_grid(stack, inputs.grid);
    if (!result) {
        const auto& err = result.error();
        const auto code = mesh_error_code(err);
        throw InputError(code.code(),
                         std::string(code.name()) + ": " + err.path + ": " + err.reason);
    }
    auto mesh = std::move(result.value());
    if (!mesh.pending_cut.empty())
        throw InputError(ErrorCode::CUT_UNSUPPORTED, "internal material cuts are not supported");
    const auto nz = mesh.axes[2].size() - 1;
    validate_column_partition(mesh, nz);
    if (cut)
        cut->reset();
    auto ring = inputs.stack.outline.polygon;
    if (ring.empty())
        return mesh;
    if (!cut)
        throw InputError(ErrorCode::CUT_UNSUPPORTED, "cut data receiver is required");
    if (ring.size() > 1 && ring.front() == ring.back())
        ring.pop_back();
    double area = 0;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        auto a = ring[i], b = ring[(i + 1) % ring.size()];
        area += a[0] * b[1] - a[1] * b[0];
    }
    if (ring.size() < 3 || area == 0)
        throw InputError(ErrorCode::CUT_UNSUPPORTED, "outline must be a convex polygon");
    mesh::cut::CutGeometry geometry;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        auto a = ring[i], b = ring[(i + 1) % ring.size()];
        double nx = (b[1] - a[1]) * (area > 0 ? 1 : -1), ny = (a[0] - b[0]) * (area > 0 ? 1 : -1);
        double length = std::hypot(nx, ny);
        if (length == 0)
            throw InputError(ErrorCode::CUT_UNSUPPORTED, "duplicate outline vertex");
        nx /= length;
        ny /= length;
        double d = nx * a[0] + ny * a[1];
        for (auto v : ring)
            if (nx * v[0] + ny * v[1] - d > 1e-12 * length)
                throw InputError(ErrorCode::CUT_UNSUPPORTED,
                                 "non-convex outlines are not supported");
        // Same external footprint applies to every layer body, retaining material attributes.
        for (auto [name, body] : mesh.attr_table.bodies)
            geometry.planes.push_back({body, {nx, ny, 0}, d});
    }
    auto options = inputs.cut_options;
    options.export_polyhedra = true;
    options.keep_void = false;
    auto clipped = mesh::cut::build_cut_layer(mesh, geometry, options);
    if (!clipped)
        throw InputError(ErrorCode::INPUT_INVALID, clipped.error().reason);
    if (mesh.elem_gid.empty())
        throw InputError(ErrorCode::INPUT_INVALID, "empty cut domain");
    *cut = std::move(clipped.value());
    return mesh;
}
} // namespace chipcae::solve
