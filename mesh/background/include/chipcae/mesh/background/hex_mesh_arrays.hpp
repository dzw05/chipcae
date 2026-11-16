#pragma once
#include "chipcae/core/memory_space.hpp"
#include "chipcae/schema/stack.hpp"
namespace chipcae::mesh {
struct AttributeRecord {
    std::string layer, region;
    MaterialId material{};
    BodyId body{};
};
struct AttributeTable {
    std::vector<AttributeRecord> entries;
    std::map<std::string, std::vector<Local>> references;
    std::map<std::string, MaterialId> materials;
    std::map<std::string, BodyId> bodies;
    std::map<Local, std::string> boundary_names;
};
struct SnapReport {
    char axis;
    Real from, to, displacement;
};
struct HexMeshArrays {
    std::vector<Real> vertices;
    std::vector<Local> hex, attribute, bdr_quad, bdr_attribute;
    std::vector<Index> elem_gid, node_gid;
    std::vector<Local> partitioning, node_layer;
    std::vector<Index> node_column;
    std::vector<Real> attr_frame, nodes;
    bool internal_interfaces = false;
    int order = 1;
    struct InterfaceReport {
        std::string name; // user-written reference, or a generated body-pair name
        std::vector<std::string> side_a_bodies, side_b_bodies; // body names, not BodyIds
        Index aligned_faces = 0, cut_faces = 0;
        Real aligned_area = 0, cut_area = 0;
    };
    std::vector<InterfaceReport> preparation_report;
    AttributeTable attr_table;
    // Named face groups. Cut-face attributes also appear here (and therefore in
    // MFEM bdr_attribute_sets) but are not attached to MFEM boundary elements;
    // solvers must take cut faces from CutLayerArrays.
    std::map<std::string, std::vector<Local>> face_sets;
    std::vector<Local> pending_cut;
    std::vector<SnapReport> snapped;
    std::array<std::vector<Real>, 3> axes;
};
template <class T, class MemSpace>
View1<T, MemSpace> upload_vector(const std::vector<T>& src, const std::string& name) {
    View1<T, MemSpace> v(name, src.size());
#ifdef CHIPCAE_ENABLE_KOKKOS
    auto h = Kokkos::create_mirror_view(v);
    for (std::size_t i = 0; i < src.size(); ++i)
        h(i) = src[i];
    Kokkos::deep_copy(v, h);
#else
    for (std::size_t i = 0; i < src.size(); ++i)
        v(i) = src[i];
#endif
    return v;
}
template <class MemSpace>
struct HexMeshDevice {
    View1<const Local, MemSpace> node_layer, elem_attribute, elem_material;
    View1<const Index, MemSpace> node_column;
};
template <class MemSpace>
HexMeshDevice<MemSpace> upload(const HexMeshArrays& m) {
    std::vector<Local> materials;
    for (auto a : m.attribute)
        materials.push_back(m.attr_table.entries[a - 1].material);
    return {upload_vector<Local, MemSpace>(m.node_layer, "node_layer"),
            upload_vector<Local, MemSpace>(m.attribute, "elem_attribute"),
            upload_vector<Local, MemSpace>(materials, "elem_material"),
            upload_vector<Index, MemSpace>(m.node_column, "node_column")};
}
} // namespace chipcae::mesh
