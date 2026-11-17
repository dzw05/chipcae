#pragma once
#include "chipcae/core/error_code.hpp"
#include "chipcae/mesh/background/tensor_grid.hpp"
#include "chipcae/mesh/cut/plane_cut.hpp"

#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
namespace chipcae::solve {
// Numeric values are the append-only protocol/errors.yaml contract.
struct InputError : std::runtime_error {
    int code;
    InputError(int value, const std::string& message) : std::runtime_error(message), code(value) {}
};
struct LinearOptions {
    double rtol = 1e-8;
    int max_iter = 500;
    std::string preconditioner = "column_gmg";
};
struct Load {
    std::string type, target, id;
    double T_ref = 0, T = 0, h = 0, T_inf = 0, source = 0;
    bool total_power = false;
};
struct Material {
    std::string id;
    double E = 0, nu = 0, alpha = 0, k = 0;
};
struct RunInputs {
    std::map<std::string, Material> materials;
    std::filesystem::path case_path, profile_path, stack_path, material_path;
    mesh::StackModel stack;
    mesh::GridOptions grid;
    LinearOptions linear;
    std::vector<Load> loads;
    std::vector<std::string> fixed, fields;
    mesh::cut::CutOptions cut_options;
    double ghost_penalty = 0.1;
    bool cut_as_hex = false;
    bool thermal = false;
    double T_ref = 0;
};
RunInputs load_inputs(const std::filesystem::path& case_path,
                      const std::filesystem::path& profile_path,
                      int ranks);
int run(const RunInputs&, const std::filesystem::path&);
mesh::HexMeshArrays build_mesh(const RunInputs& inputs,
                               std::optional<mesh::cut::CutLayerArrays>* cut = nullptr);
} // namespace chipcae::solve
