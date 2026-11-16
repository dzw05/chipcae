#pragma once
#include "assembly.hpp"
namespace chipcae::proto_cutfem {
struct ElementVolumes {
    std::vector<double> matrix, rhs;
};
// Host callbacks are sampled before launching the volume kernel. Boundary and
// ghost-penalty terms remain in assemble() on the host.
ElementVolumes kokkos_volumes(const HexMeshArrays&, const CutLayer&, const Problem&, System&);
} // namespace chipcae::proto_cutfem
