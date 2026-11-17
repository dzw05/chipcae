#include "chipcae/solve/case_loader.hpp"

#include <iostream>
#include <set>
int main() {
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
    inputs.grid.levels = 0;
    auto automatic = chipcae::solve::build_mesh(inputs);
    if (!automatic.grid_report.auto_selected || automatic.grid_report.levels < 1 ||
        automatic.grid_report.level_axes.size() !=
            static_cast<std::size_t>(automatic.grid_report.levels))
        return 5;
    std::cout << "uncut elements=" << mesh.elem_gid.size() << " nz=" << nz << '\n';
}
