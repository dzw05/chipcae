#pragma once
#include "chipcae/io/vtkhdf_results.hpp"
#include "chipcae/mesh/cut/cut_layer.hpp"

#include <mfem.hpp>
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
            const auto& record = m.attr_table.entries[m.attribute[parent] - 1];
            out.elem_gid.push_back(m.elem_gid[parent]);
            out.material_id.push_back(record.material);
            out.cell_kind.push_back(1);
            auto it =
                std::find(p.parent_elem_gid.begin(), p.parent_elem_gid.end(), m.elem_gid[parent]);
            out.layer_id.push_back(p.layer_id.at(it - p.parent_elem_gid.begin()));
        }
    }
}
} // namespace chipcae::solve
