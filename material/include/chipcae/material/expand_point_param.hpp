#pragma once

#include "chipcae/material/material_batch.hpp"
#include "chipcae/mesh/background/hex_mesh_arrays.hpp"

#include <stdexcept>

namespace chipcae::material {
// copper_phi is indexed by global Morton column; the result follows local element order.
template <class MS, class PhiView>
View1<Real, MS> expand_point_param(const mesh::HexMeshDevice<MS>& mesh, const PhiView& copper_phi) {
    if (mesh.nz == 0)
        throw std::invalid_argument("expand_point_param requires nz > 0");
    const auto gids = mesh.elem_gid;
    const auto nz = mesh.nz;
    int invalid = 0;
    Kokkos::parallel_reduce(
        "validate copper columns",
        Kokkos::RangePolicy<ExecSpaceFor<MS>>(0, gids.extent(0)),
        KOKKOS_LAMBDA(int e, int& count) {
            if (gids(e) < 0 || static_cast<std::size_t>(gids(e)) / nz >= copper_phi.extent(0))
                ++count;
        },
        invalid);
    if (invalid)
        throw std::invalid_argument("copper_phi does not cover element columns");
    View1<Real, MS> result("point_param", gids.extent(0));
    Kokkos::parallel_for(
        "expand copper columns",
        Kokkos::RangePolicy<ExecSpaceFor<MS>>(0, gids.extent(0)),
        KOKKOS_LAMBDA(int e) { result(e) = copper_phi(gids(e) / nz); });
    return result;
}
} // namespace chipcae::material
