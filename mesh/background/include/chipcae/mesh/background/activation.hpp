#pragma once
#include "hex_mesh_arrays.hpp"
namespace chipcae::mesh::cut {
struct CutLayerArrays;
}
namespace chipcae::mesh {
using ActivationMask = std::vector<Local>;
struct ActivationSet {
    std::string ref;
    std::vector<Local> attributes;
    std::vector<BodyId> bodies;
    std::vector<Local> elems, cut_subs;
    Index n_elem_global = 0;
    Real volume = 0;
};
struct FaceSet {
    std::string ref;
    std::vector<Local> bdr_attributes, eb_subs;
};
struct ActivationResolution {
    ActivationSet cells;
    FaceSet faces;
    std::vector<HexMeshArrays::InterfaceReport> interface_reports;
};
// Hash the sorted unique global element IDs, never attribute numbers.
std::uint64_t mask_hash(std::vector<Index> gids);
std::vector<Index> activated_gids(const HexMeshArrays&, const ActivationMask&);
std::string canonical_interface(std::string, std::string);
void fill_interface_sides(HexMeshArrays::InterfaceReport&, const HexMeshArrays&);
Expected<std::vector<BodyId>> resolve_interface_side(const HexMeshArrays&, const std::string&);
Expected<ActivationResolution> resolve_volume_ref(const StackModel&,
                                                  const HexMeshArrays&,
                                                  const cut::CutLayerArrays*,
                                                  const std::vector<std::string>&);
Expected<ActivationResolution> resolve_activation(const StackModel&,
                                                  const HexMeshArrays&,
                                                  const cut::CutLayerArrays*,
                                                  const std::vector<std::string>&);
// Non-const overloads also append interface: reports to preparation_report. Pass the completed
// cut layer so cut-face counts and areas are included in those reports.
Expected<ActivationResolution> resolve_volume_ref(const StackModel&,
                                                  HexMeshArrays&,
                                                  const cut::CutLayerArrays*,
                                                  const std::vector<std::string>&);
Expected<ActivationResolution> resolve_activation(const StackModel&,
                                                  HexMeshArrays&,
                                                  const cut::CutLayerArrays*,
                                                  const std::vector<std::string>&);
struct ActivationStep {
    std::vector<std::string> activate, deactivate;
};
Expected<bool>
check_no_reactivation(const StackModel&, const HexMeshArrays&, const std::vector<ActivationStep>&);
} // namespace chipcae::mesh
