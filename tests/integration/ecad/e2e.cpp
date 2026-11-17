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
        out << R"({"FR4":"mat.fr4"})";
    }
    auto result = read_ipc2581(xml);
    std::string error;
    if (!result || !apply_material_map(result.model, map, &error) ||
        !to_stack(result.model, dir / "board.stack.json", dir / "board.polyset", &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::filesystem::remove_all(dir);
    return 0;
}
