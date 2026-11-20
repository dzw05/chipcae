#include "chipcae/geometry/ecad/ipc2581.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    using namespace chipcae::geometry::ecad;
    const auto dir = std::filesystem::temp_directory_path() / "chipcae-ecad-e2e";
    std::filesystem::create_directories(dir);
    const auto xml = dir / "board.xml", map = dir / "material_map.json";
    {
        std::ofstream out(xml);
        out << "<IPC-2581><Ecad><CadData><Layer name='Core' layerFunction='DIELECTRIC'/>"
               "<Stackup><StackupGroup name='S'><StackupLayer layerOrGroupRef='Core' "
               "thickness='1.0' matDes='FR4'/></StackupGroup></Stackup>"
               "<Step name='board'><Profile><Polygon><PolyBegin x='0' y='0'/>"
               "<PolyStepSegment x='10' y='0'/><PolyStepSegment x='10' y='10'/>"
               "<PolyStepSegment x='0' y='10'/></Polygon></Profile></Step>"
               "</CadData></Ecad></IPC-2581>";
    }
    {
        std::ofstream out(map);
        out << R"({"FR4":"fr4_eq"})";
    }
    auto result = read_ipc2581(xml);
    std::string error;
    if (!result || !apply_material_map(result.model, map, &error) ||
        !to_stack(result.model, dir / "board.stack.json", dir / "board.polyset", &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    {
        std::ofstream out(map);
        out << R"({"FR4":"mat.bt"})";
    }
    auto invalid = read_ipc2581(xml);
    error.clear();
    if (!invalid || !apply_material_map(invalid.model, map, &error)) {
        std::cerr << "ecad-map-targets: illegal target must still map: " << error << '\n';
        std::filesystem::remove_all(dir);
        return 1;
    }
    const auto invalid_stack = dir / "invalid.stack.json",
               invalid_polyset = dir / "invalid.polyset";
    error.clear();
    const bool exported = to_stack(invalid.model, invalid_stack, invalid_polyset, &error);
    const std::string expected =
        "layer 'Core' material 'mat.bt': must match ^[a-z0-9_-]+$ (lowercase letters, digits, "
        "'_' or '-'; no dots, e.g. cu, fr4_eq)";
    if (exported || error != expected || std::filesystem::exists(invalid_stack) ||
        std::filesystem::exists(invalid_polyset)) {
        std::cerr << "ecad-map-targets: mat.bt export must reject before publication; exported="
                  << exported << "; expected [" << expected << "], got [" << error << "]\n";
        std::filesystem::remove_all(dir);
        return 1;
    }
    std::filesystem::remove_all(dir);
    return 0;
}
