#include "chipcae/solve/case_loader.hpp"

#include <iostream>
int main() {
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
    for (std::size_t e = 0; e < mesh.partitioning.size(); ++e) {
        if (mesh.partitioning[e] != mesh.partitioning[(e / nz) * nz])
            return 2;
        if (mesh.elem_gid[e] != static_cast<chipcae::Index>(e))
            return 3;
    }
    std::cout << "uncut elements=" << mesh.elem_gid.size() << " nz=" << nz << '\n';
}
