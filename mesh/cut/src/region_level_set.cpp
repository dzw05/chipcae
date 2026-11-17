#include "chipcae/mesh/cut/region_level_set.hpp"

#include <stdexcept>

// SP0 contract stub; SP5 supplies the implementation.
namespace chipcae::mesh {
std::vector<BodyLevelSet>
region_level_sets(const StackModel&, const HexMeshArrays&, const RegionLSOptions&) {
    throw std::logic_error("not implemented: region_level_sets");
}
} // namespace chipcae::mesh
