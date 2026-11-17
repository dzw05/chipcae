#include "chipcae/geometry/ecad/ipc2581.hpp"
#include "chipcae/mesh/background/tensor_grid.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 4)
        return 1;
    namespace ecad = chipcae::geometry::ecad;
    namespace mesh = chipcae::mesh;
    const std::filesystem::path root(argv[1]), dir(argv[2]);
    if (std::string(argv[3]) == "export") {
        auto imported = ecad::read_ipc2581(root / "tests/integration/ecad/data/board_2layer.xml");
        std::string error;
        if (!imported ||
            !ecad::apply_material_map(imported.model, dir / "materials.map.json", &error) ||
            !ecad::to_stack(
                imported.model, dir / "board.stack.json", dir / "board.polyset", &error)) {
            std::cerr << imported.error << error << '\n';
            return 1;
        }
    } else {
        auto stack =
            mesh::load_stack(dir / "board.stack.json", root / "schema/stack-0.2.schema.json");
        auto polygons = mesh::read_polyset(dir / "board.polyset");
        if (!stack || !polygons) {
            std::cerr << (!stack ? stack.error().reason : polygons.error().reason) << '\n';
            return 2;
        }
        mesh::GridOptions options;
        options.nx = options.ny = 4;
        options.max_ratio = 100;
        auto candidates = mesh::hierarchy_candidates(stack.value(), options);
        if (!candidates || !std::any_of(candidates.value().begin(),
                                        candidates.value().end(),
                                        [](const auto& c) { return c.feasible; }))
            return 3;
        auto grid = mesh::build_tensor_grid(stack.value(), options, polygons.value());
        if (!grid) {
            std::cerr << grid.error().reason << '\n';
            return 4;
        }
    }
}
