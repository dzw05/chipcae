#pragma once
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
};
struct SnapReport {
    char axis;
    Real from, to, displacement;
};
struct HexMeshArrays {
    std::vector<Real> vertices;
    std::vector<Local> hex, attribute, bdr_quad, bdr_attribute;
    std::vector<Index> elem_gid, node_gid;
    std::vector<Local> partitioning, node_layer, node_column;
    AttributeTable attr_table;
    std::map<std::string, std::vector<Local>> face_sets;
    std::vector<Local> pending_cut;
    std::vector<SnapReport> snapped;
    std::array<std::vector<Real>, 3> axes;
};
} // namespace chipcae::mesh
