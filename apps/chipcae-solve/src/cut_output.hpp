#pragma once
#include "chipcae/io/vtkhdf_results.hpp"
#include "chipcae/mesh/cut/cut_layer.hpp"

#include <map>
#include <mfem.hpp>
#include <stdexcept>
#include <string>
#include <unordered_map>
namespace chipcae::solve {
// Geometry and reference coordinates belong to the mesh; only Q2 field
// evaluation belongs to this adapter. Every face ring is copied per cell.
inline void append_cut_output(io::ResultsStep& step,
                              const mesh::HexMeshArrays& m,
                              const mesh::cut::CutLayerArrays& cut,
                              mfem::GridFunction& T,
                              mfem::GridFunction& U,
                              int rank) {
    const auto& p = *cut.polyhedra;
    auto sample = [&](io::ResultsPartition& out, int parent, Index v) {
        mfem::IntegrationPoint ip;
        ip.Set3(p.point_xi[3 * v], p.point_xi[3 * v + 1], p.point_xi[3 * v + 2]);
        out.points.insert(out.points.end(), p.points.begin() + 3 * v, p.points.begin() + 3 * v + 3);
        out.T.push_back(T.GetValue(parent, ip));
        mfem::Vector value(3);
        U.GetVectorValue(parent, ip, value);
        for (int c = 0; c < 3; ++c)
            out.U.push_back(value[c]);
    };
    for (std::size_t cell = 0; cell < p.volume.size(); ++cell) {
        int parent = p.point_parent_elem[p.face_conn[p.face_off[p.poly_faces[p.poly_off[cell]]]]];
        if (m.partitioning[parent] != rank)
            continue;
        auto& out = step.cut;
        std::map<Index, Index> remap;
        for (Index j = p.poly_off[cell]; j < p.poly_off[cell + 1]; ++j) {
            Index f = p.poly_faces[j];
            out.poly_faces.push_back(out.face_off.size() - 1);
            for (Index k = p.face_off[f]; k < p.face_off[f + 1]; ++k) {
                Index v = p.face_conn[k];
                auto [it, added] = remap.emplace(v, out.points.size() / 3);
                if (added) {
                    sample(out, parent, v);
                    out.connectivity.push_back(it->second);
                }
                out.face_conn.push_back(it->second);
            }
            out.face_off.push_back(out.face_conn.size());
        }
        out.offsets.push_back(out.connectivity.size());
        out.poly_off.push_back(out.poly_faces.size());
        out.elem_gid.push_back(p.parent_elem_gid[cell]);
        out.layer_id.push_back(p.layer_id[cell]);
        out.material_id.push_back(p.material_id[cell]);
        out.cell_kind.push_back(1);
        out.volume.push_back(p.volume[cell]);
    }
    if (cut.eb_parent_elem.empty())
        return;

    std::unordered_map<Index, mesh::Local> poly_layer;
    poly_layer.reserve(p.parent_elem_gid.size());
    for (std::size_t cell = 0; cell < p.parent_elem_gid.size(); ++cell) {
        const auto gid = p.parent_elem_gid[cell];
        const auto layer = p.layer_id[cell];
        const auto [it, inserted] = poly_layer.emplace(gid, layer);
        if (!inserted && it->second != layer)
            throw std::runtime_error("cut_output: parent gid " + std::to_string(gid) +
                                     " has polyhedra in layers " + std::to_string(it->second) +
                                     " and " + std::to_string(layer));
    }
    for (std::size_t v = 0; v < cut.eb_parent_elem.size(); ++v) {
        int parent = cut.eb_parent_elem[v];
        if (m.partitioning[parent] != rank)
            continue;
        auto& out = step.embedded_boundary;
        mfem::IntegrationPoint ip;
        ip.Set3(cut.eb_xi[3 * v], cut.eb_xi[3 * v + 1], cut.eb_xi[3 * v + 2]);
        out.connectivity.push_back(out.points.size() / 3);
        out.points.insert(
            out.points.end(), cut.eb_points.begin() + 3 * v, cut.eb_points.begin() + 3 * v + 3);
        out.T.push_back(T.GetValue(parent, ip));
        mfem::Vector value(3);
        U.GetVectorValue(parent, ip, value);
        for (int c = 0; c < 3; ++c)
            out.U.push_back(value[c]);
        if (v % 3 == 0) {
            const auto gid = m.elem_gid[parent];
            const auto layer = poly_layer.find(gid);
            if (layer == poly_layer.end())
                throw std::runtime_error("cut_output: EB triangle parent gid " +
                                         std::to_string(gid) + " has no polyhedron");
            const auto& record = m.attr_table.entries[m.attribute[parent] - 1];
            const auto expected = m.attr_table.layers.find(record.layer);
            if (expected != m.attr_table.layers.end() && layer->second != expected->second)
                throw std::runtime_error("cut_output: EB triangle parent gid " +
                                         std::to_string(gid) + " layer " +
                                         std::to_string(layer->second) + " != attribute layer " +
                                         std::to_string(expected->second));
            out.elem_gid.push_back(gid);
            out.material_id.push_back(record.material);
            out.cell_kind.push_back(1);
            out.layer_id.push_back(layer->second);
        }
    }
}
} // namespace chipcae::solve
