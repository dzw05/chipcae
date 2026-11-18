#include "../../apps/chipcae-solve/src/mesh_error_map.hpp"
#include "chipcae/solve/case_loader.hpp"

#ifdef CHIPCAE_ENABLE_KOKKOS
#include <Kokkos_Core.hpp>
#endif

#include <iostream>
#include <set>
namespace {
bool mesh_error_acceptance() {
    using chipcae::ErrorCode;
    using chipcae::solve::InputError;
    bool passed = true;
    const auto check = [&](bool ok, const std::string& name) {
        if (!ok) {
            std::cerr << "FAIL: " << name << '\n';
            passed = false;
        }
    };
    // The contract enumerates five distinct recognized paths (not six).
    const std::pair<const char*, ErrorCode::Value> mappings[] = {
        {"COLUMN_SPLIT_ACROSS_RANKS", ErrorCode::COLUMN_SPLIT_ACROSS_RANKS},
        {"HIERARCHY_INVALID", ErrorCode::HIERARCHY_INVALID},
        {"hierarchy/levels", ErrorCode::HIERARCHY_INVALID},
        {"CUT_UNSUPPORTED", ErrorCode::CUT_UNSUPPORTED},
        {"outline.unsupported", ErrorCode::CUT_UNSUPPORTED},
        {"unknown", ErrorCode::INPUT_INVALID},
        {"", ErrorCode::INPUT_INVALID},
        {"column_split_across_ranks", ErrorCode::INPUT_INVALID},
        {"hierarchy_invalid", ErrorCode::INPUT_INVALID},
        {"Hierarchy/levels", ErrorCode::INPUT_INVALID},
        {"cut_unsupported", ErrorCode::INPUT_INVALID},
        {"Outline.unsupported", ErrorCode::INPUT_INVALID},
    };
    for (const auto& [path, expected] : mappings)
        check(chipcae::solve::mesh_error_code({path, "test reason"}).code() == expected,
              std::string("mesh-error-map: ") + path);
    chipcae::mesh::HexMeshArrays mesh;
    mesh.elem_gid = {0, 2, 1, 3};
    mesh.partitioning = {0, 1, 0, 1};
    try {
        chipcae::solve::validate_column_partition(mesh, 2);
    } catch (const InputError& error) {
        check(false, std::string("column-split-elem-gid valid: ") + error.what());
    }
    mesh.partitioning = {0, 0, 1, 1};
    try {
        chipcae::solve::validate_column_partition(mesh, 2);
        check(false, "column-split-elem-gid: expected InputError for column 0");
    } catch (const InputError& error) {
        check(error.code == ErrorCode::COLUMN_SPLIT_ACROSS_RANKS, "column split error code");
        const std::string message = error.what();
        for (const auto* text : {"column 0", "spans ranks 0 and 1"})
            check(message.find(text) != std::string::npos,
                  std::string("column split message: ") + text);
    }
    mesh.elem_gid = {0, 1, 0, 1};
    mesh.partitioning = {1, 1, 1, 1};
    try {
        chipcae::solve::validate_column_partition(mesh, 2);
        mesh.elem_gid.clear();
        mesh.partitioning.clear();
        chipcae::solve::validate_column_partition(mesh, 2);
    } catch (const InputError& error) {
        check(false, std::string("repeated/empty columns: ") + error.what());
    }
    return passed;
}
} // namespace
int main() {
#ifdef CHIPCAE_ENABLE_KOKKOS
    // Initialize every enabled execution space before building meshes, and keep
    // it alive until all mesh-owned Views have been destroyed.
    Kokkos::ScopeGuard guard;
#endif
    const bool acceptance_passed = mesh_error_acceptance();
    chipcae::solve::Load power;
    power.target = "layer:empty";
    power.total_power = true;
    power.source = 10;
    try {
        chipcae::solve::heat_source_density(power, 0);
        return 6;
    } catch (const chipcae::solve::InputError& error) {
        if (error.code != chipcae::ErrorCode::INPUT_INVALID ||
            std::string(error.what()).find("positive target layer volume") == std::string::npos)
            return 7;
    }
    if (chipcae::solve::heat_source_density(power, 2) != 5)
        return 8;
    chipcae::solve::RunInputs inputs;
    inputs.stack.outline.box = {0, 0, 0.02, 0.02};
    chipcae::mesh::Layer layer;
    layer.name = "base";
    layer.kind = "Homogenized";
    layer.material = "base";
    layer.thickness = 0.001;
    layer.n_elem_min = 2;
    inputs.stack.layers.push_back(layer);
    inputs.grid.nx = 8;
    inputs.grid.ny = 8;
    inputs.grid.levels = 1;
    inputs.grid.ranks = 2;
    auto mesh = chipcae::solve::build_mesh(inputs);
    if (!mesh.pending_cut.empty() || mesh.elem_gid.empty())
        return 1;
    auto nz = mesh.axes[2].size() - 1;
    if (mesh.elem_gid.size() != 128 || nz != 2 ||
        std::set<int>(mesh.partitioning.begin(), mesh.partitioning.end()) != std::set<int>{0, 1})
        return 4;
    for (std::size_t e = 0; e < mesh.partitioning.size(); ++e) {
        if (mesh.partitioning[e] != mesh.partitioning[(e / nz) * nz])
            return 2;
        if (mesh.elem_gid[e] != static_cast<chipcae::Index>(e))
            return 3;
    }
    inputs.stack.outline.polygon = {{{0, 0}}, {{0.03, 0}}, {{0, 0.02}}};
    try {
        std::optional<chipcae::mesh::cut::CutLayerArrays> cut;
        chipcae::solve::build_mesh(inputs, &cut);
        return 9;
    } catch (const chipcae::solve::InputError& error) {
        if (std::string(error.what()).find("outside outline.box") == std::string::npos)
            return 10;
    }
    inputs.stack.outline.polygon.clear();
    inputs.grid.levels = 0;
    auto automatic = chipcae::solve::build_mesh(inputs);
    if (!automatic.grid_report.auto_selected || automatic.grid_report.levels < 1 ||
        automatic.grid_report.level_axes.size() !=
            static_cast<std::size_t>(automatic.grid_report.levels))
        return 5;
    if (!acceptance_passed)
        return 11;
    std::cout << "uncut elements=" << mesh.elem_gid.size() << " nz=" << nz << '\n';
}
