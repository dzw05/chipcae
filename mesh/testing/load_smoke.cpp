#include "chipcae/mesh/testing/mesh_fixture.hpp"

#include <iostream>
int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    auto mesh = chipcae::mesh::testing::load_mesh_fixture(argv[1]);
    if (!mesh) {
        std::cerr << mesh.error().reason << '\n';
        return 1;
    }
    return 0;
}
