#include "chipcae/geometry/ecad/ipc2581.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    using namespace chipcae::geometry::ecad;
    EcadModel model;
    model.layers = {{"core", "FR4", "", 0.0, false, {}}, {"top", "", "SIGNAL", 0.0, true, {}}};
    const auto path = std::filesystem::temp_directory_path() / "chipcae-material-map-test.json";
    {
        std::ofstream out(path);
        out << R"({"FR4":"mat.fr4","copper":"mat.cu"})";
    }
    std::string error;
    if (!apply_material_map(model, path, &error) || model.layers[0].material != "mat.fr4" ||
        model.layers[1].material != "mat.cu") {
        std::cerr << "valid map was not applied: " << error << '\n';
        return 1;
    }
    {
        std::ofstream out(path);
        out << R"({"FR4":"mat.fr4"})";
    }
    model.layers[1].material.clear();
    if (apply_material_map(model, path, &error) ||
        error.find("MATERIAL_MAPPING_MISSING") == std::string::npos) {
        std::cerr << "missing copper mapping was not rejected: " << error << '\n';
        return 1;
    }
    std::filesystem::remove(path);
    return 0;
}
