#include "chipcae/io/vtkhdf_results.hpp"
#include "chipcae/mesh/background/tensor_grid.hpp"
#include "chipcae/mesh/cut/plane_cut.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
#ifdef CHIPCAE_ENABLE_MPI
#include <mpi.h>
#endif
int main(int argc, char** argv) {
#ifdef CHIPCAE_ENABLE_MPI
    MPI_Init(&argc, &argv);
#endif
    int rank = 0, ranks = 1, status = 0;
#ifdef CHIPCAE_ENABLE_MPI
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &ranks);
#endif
    try {
        using namespace chipcae;
        using namespace chipcae::mesh;
        if (argc < 3)
            throw std::runtime_error("usage: s2_cut_minimal RUN STACKUP [--cut-as-hex]");
        StackModel stack;
        stack.id = "s2";
        stack.outline.box = {0, 0, 1, 1};
        Layer layer;
        layer.name = "base";
        layer.material = "solid";
        layer.kind = "Explicit";
        layer.thickness = 1;
        layer.n_elem_min = 1;
        stack.layers.push_back(layer);
        layer.name = "top";
        layer.body = "cap";
        stack.layers.push_back(layer);
        GridOptions options;
        options.cell_budget = 100;
        auto mesh = build_tensor_grid(stack, options);
        if (!mesh)
            throw std::runtime_error("grid failed");
        const auto& m = mesh.value();
        cut::CutOptions co;
        co.export_polyhedra = true;
        auto clipped =
            cut::build_cut_layer(m, {{{m.attr_table.bodies.at("cap"), {1, 1, 0}, 1}}}, co);
        if (!clipped)
            throw std::runtime_error("cut failed");
        const auto& c = *clipped.value().polyhedra;
        io::ResultsStep step;
        step.cut_as_hex = argc > 3 && std::string(argv[3]) == "--cut-as-hex";
        auto& h = step.hex;
        for (std::size_t e = 0; e < m.attribute.size(); ++e) {
            if (static_cast<int>(e % ranks) != rank)
                continue;
            Index start = h.points.size() / 3;
            for (int j = 0; j < 8; ++j) {
                auto v = m.hex[8 * e + j];
                h.points.insert(
                    h.points.end(), m.vertices.begin() + 3 * v, m.vertices.begin() + 3 * v + 3);
                h.hex.push_back(start + j);
            }
            const auto& attr = m.attr_table.entries[m.attribute[e] - 1];
            h.layer_id.push_back(attr.layer == "base" ? 1 : 2);
            h.material_id.push_back(attr.material);
            h.elem_gid.push_back(m.elem_gid[e]);
            h.cell_kind.push_back(std::find(c.parent_elem_gid.begin(),
                                            c.parent_elem_gid.end(),
                                            m.elem_gid[e]) != c.parent_elem_gid.end()
                                      ? 1
                                      : 0);
        }
        auto& q = step.cut;
        for (std::size_t i = 0; i < c.volume.size(); ++i) {
            auto first_face = c.poly_faces[c.poly_off[i]];
            auto parent = c.point_parent_elem[c.face_conn[c.face_off[first_face]]];
            if (parent % ranks != rank)
                continue;
            std::vector<Index> remap(c.points.size() / 3, -1);
            for (Index f = c.poly_off[i]; f < c.poly_off[i + 1]; ++f) {
                auto face = c.poly_faces[f];
                q.poly_faces.push_back(q.face_off.size() - 1);
                for (Index j = c.face_off[face]; j < c.face_off[face + 1]; ++j) {
                    auto v = c.face_conn[j];
                    if (remap[v] < 0) {
                        remap[v] = q.points.size() / 3;
                        // Solver boundary: point_parent_elem and point_xi identify
                        // the parent basis location. This analytic field is exact there.
                        q.points.insert(
                            q.points.end(), c.points.begin() + 3 * v, c.points.begin() + 3 * v + 3);
                        q.connectivity.push_back(remap[v]);
                    }
                    q.face_conn.push_back(remap[v]);
                }
                q.face_off.push_back(q.face_conn.size());
            }
            q.offsets.push_back(q.connectivity.size());
            q.poly_off.push_back(q.poly_faces.size());
            q.elem_gid.push_back(c.parent_elem_gid[i]);
            q.layer_id.push_back(c.layer_id[i]);
            q.material_id.push_back(c.material_id[i]);
            q.cell_kind.push_back(1);
            q.volume.push_back(c.volume[i]);
        }
        for (auto* p : {&h, &q})
            for (std::size_t v = 0; v < p->points.size() / 3; ++v) {
                p->T.push_back(300 + p->points[3 * v] + 2 * p->points[3 * v + 1] +
                               3 * p->points[3 * v + 2]);
                p->U.insert(p->U.end(), {0.001, 0.002, 0.003});
            }
        io::ResultsWriter writer(argv[1], argv[2]);
        writer.write_step(0, 0, step);
        writer.add_summary("warpage_pv", 0, "m", 0);
        writer.add_summary("T_max", step.cut_as_hex ? 309 : 308, "K", 0);
        writer.add_summary("U_mag_max", std::sqrt(14.) * .001, "m", 0);
        writer.finalize();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        status = 1;
    }
#ifdef CHIPCAE_ENABLE_MPI
    MPI_Finalize();
#endif
    return status;
}
