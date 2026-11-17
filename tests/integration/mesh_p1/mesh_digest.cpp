#include "../../../preview/src/mesh_digest.hpp"

#include <array>
#include <iostream>

int main() {
    chipcae::mesh::HexMeshArrays mesh;
    mesh.grid_report.level_axes.push_back({{{0, 1}, {0, 1}}});
    mesh.axes[2] = {0, 1};
    mesh.elem_gid = {0};
    mesh.node_gid = {0, 1, 2, 3, 4, 5, 6, 7};
    mesh.vertices = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1};
    mesh.hex = {0, 1, 2, 3, 4, 5, 6, 7};
    const auto expected = chipcae::preview::detail::digest(mesh).sha256;
    auto permuted = mesh;
    // Map old local indices to new local indices, keeping global geometry intact.
    constexpr std::array<int, 8> permutation = {3, 6, 1, 7, 0, 4, 2, 5};
    for (std::size_t old = 0; old < permutation.size(); ++old) {
        const auto local = permutation[old];
        permuted.node_gid[local] = mesh.node_gid[old];
        for (int d = 0; d < 3; ++d)
            permuted.vertices[3 * local + d] = mesh.vertices[3 * old + d];
    }
    for (auto& local : permuted.hex)
        local = permutation[local];
    if (chipcae::preview::detail::digest(permuted).sha256 != expected) {
        std::cerr << "digest changed after local node permutation\n";
        return 1;
    }
    // A real connectivity change must still change the digest.
    std::swap(permuted.hex[0], permuted.hex[1]);
    if (chipcae::preview::detail::digest(permuted).sha256 == expected) {
        std::cerr << "digest ignored global connectivity change\n";
        return 1;
    }
}
