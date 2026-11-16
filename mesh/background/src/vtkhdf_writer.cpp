#include "chipcae/mesh/background/vtkhdf_writer.hpp"

#include "chipcae/mesh/cut/cut_layer.hpp"

#include <algorithm>
#include <hdf5.h>
#include <numeric>
#include <set>
#include <stdexcept>
namespace chipcae::mesh {
namespace {
void check(herr_t status) {
    if (status < 0)
        throw std::runtime_error("HDF5 operation failed");
}
struct H5 {
    hid_t id;
    herr_t (*close)(hid_t);
    H5(hid_t value, herr_t (*closer)(hid_t)) : id(value), close(closer) {
        if (id < 0)
            throw std::runtime_error("HDF5 object creation failed");
    }
    ~H5() { close(id); }
    H5(const H5&) = delete;
    H5& operator=(const H5&) = delete;
    operator hid_t() const { return id; }
};
template <class T>
hid_t native();
template <>
hid_t native<Real>() {
    return H5T_NATIVE_DOUBLE;
}
template <>
hid_t native<Local>() {
    return H5T_NATIVE_INT32;
}
template <>
hid_t native<Index>() {
    return H5T_NATIVE_INT64;
}
template <>
hid_t native<unsigned char>() {
    return H5T_NATIVE_UCHAR;
}
template <class T>
void dataset(hid_t parent, const char* name, const std::vector<T>& data, hsize_t components = 1) {
    hsize_t dims[2] = {data.size() / components, components};
    H5 space(H5Screate_simple(components == 1 ? 1 : 2, dims, nullptr), H5Sclose);
    H5 set(H5Dcreate2(parent, name, native<T>(), space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
           H5Dclose);
    if (!data.empty())
        check(H5Dwrite(set, native<T>(), H5S_ALL, H5S_ALL, H5P_DEFAULT, data.data()));
}
void header(hid_t root, const std::string& type) {
    hsize_t two = 2;
    H5 vs(H5Screate_simple(1, &two, nullptr), H5Sclose);
    H5 v(H5Acreate2(root, "Version", H5T_NATIVE_INT, vs, H5P_DEFAULT, H5P_DEFAULT), H5Aclose);
    int version[2] = {2, 0};
    check(H5Awrite(v, H5T_NATIVE_INT, version));
    H5 ts(H5Tcopy(H5T_C_S1), H5Tclose);
    check(H5Tset_size(ts, type.size() + 1));
    check(H5Tset_strpad(ts, H5T_STR_NULLTERM));
    H5 scalar(H5Screate(H5S_SCALAR), H5Sclose);
    H5 a(H5Acreate2(root, "Type", ts, scalar, H5P_DEFAULT, H5P_DEFAULT), H5Aclose);
    check(H5Awrite(a, ts, type.c_str()));
}
void topology(hid_t root, const char* name, const std::vector<Index>& connectivity, Index stride) {
    H5 g(H5Gcreate2(root, name, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), H5Gclose);
    Index cells = static_cast<Index>(connectivity.size()) / stride;
    dataset(g, "NumberOfCells", std::vector<Index>{cells});
    dataset(
        g, "NumberOfConnectivityIds", std::vector<Index>{static_cast<Index>(connectivity.size())});
    dataset(g, "Connectivity", connectivity);
    std::vector<Index> offsets(static_cast<std::size_t>(cells + 1));
    for (Index i = 0; i <= cells; ++i)
        offsets[static_cast<std::size_t>(i)] = i * stride;
    dataset(g, "Offsets", offsets);
}
} // namespace
Expected<VtkhdfFiles> write_vtkhdf(const std::filesystem::path& stem,
                                   const HexMeshArrays& m,
                                   const cut::CutLayerArrays* cut,
                                   const Communicator& comm) {
    VtkhdfFiles paths{stem.string() + ".rank" + std::to_string(comm.rank()) + ".vtkhdf",
                      stem.string() + ".rank" + std::to_string(comm.rank()) + ".eb.vtkhdf"};
    std::filesystem::path grid_tmp = paths.grid.string() + ".tmp",
                          eb_tmp = paths.embedded_boundary.string() + ".tmp";
    try {
        if (m.hex.size() != 8 * m.attribute.size() || m.partitioning.size() != m.attribute.size())
            return Error{"vtkhdf", "inconsistent arrays"};
        std::vector<Local> cells;
        std::map<Local, Index> nodes;
        std::vector<Real> points;
        std::vector<Index> connectivity, offsets{0};
        for (std::size_t e = 0; e < m.attribute.size(); ++e)
            if (m.partitioning[e] == comm.rank()) {
                cells.push_back(static_cast<Local>(e));
                for (int i = 0; i < 8; ++i) {
                    auto n = m.hex[8 * e + i];
                    if (!nodes.contains(n)) {
                        nodes[n] = static_cast<Index>(nodes.size());
                        for (int k = 0; k < 3; ++k)
                            points.push_back(m.vertices[3 * n + k]);
                    }
                    connectivity.push_back(nodes[n]);
                }
                offsets.push_back(static_cast<Index>(connectivity.size()));
            }
        std::map<Local, std::size_t> cuts;
        std::set<Local> voids;
        if (cut) {
            for (std::size_t i = 0; i < cut->cut_elem.size(); ++i)
                cuts[cut->cut_elem[i]] = i;
            voids.insert(cut->void_elem.begin(), cut->void_elem.end());
        }
        {
            H5 file(H5Fcreate(grid_tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT), H5Fclose);
            H5 root(H5Gcreate2(file, "VTKHDF", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), H5Gclose);
            header(root, "UnstructuredGrid");
            dataset(root, "NumberOfPoints", std::vector<Index>{static_cast<Index>(nodes.size())});
            dataset(root, "NumberOfCells", std::vector<Index>{static_cast<Index>(cells.size())});
            dataset(root,
                    "NumberOfConnectivityIds",
                    std::vector<Index>{static_cast<Index>(connectivity.size())});
            dataset(root, "Points", points, 3);
            dataset(root, "Connectivity", connectivity);
            dataset(root, "Offsets", offsets);
            dataset(root, "Types", std::vector<unsigned char>(cells.size(), 12));
            H5 cd(H5Gcreate2(root, "CellData", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), H5Gclose);
            std::map<std::string, std::vector<Local>> fields;
            std::vector<Real> vf;
            std::map<std::string, Local> layers, regions;
            for (const auto& a : m.attr_table.entries) {
                if (!layers.contains(a.layer))
                    layers[a.layer] = static_cast<Local>(layers.size() + 1);
                if (!a.region.empty() && !regions.contains(a.region))
                    regions[a.region] = static_cast<RegionId>(regions.size() + 1);
            }
            for (auto e : cells) {
                const auto& a =
                    m.attr_table.entries.at(static_cast<std::size_t>(m.attribute[e] - 1));
                fields["attribute"].push_back(m.attribute[e]);
                fields["layer"].push_back(layers[a.layer]);
                fields["region"].push_back(a.region.empty() ? 0 : regions[a.region]);
                fields["material"].push_back(a.material);
                fields["body"].push_back(a.body);
                fields["rank"].push_back(comm.rank());
                fields["cell_kind"].push_back(voids.contains(e) ? 2 : cuts.contains(e) ? 1 : 0);
                vf.push_back(voids.contains(e)  ? 0
                             : cuts.contains(e) ? cut->sub_volfrac[cuts[e]]
                                                : 1);
            }
            for (auto name :
                 {"attribute", "layer", "region", "material", "body", "rank", "cell_kind"})
                dataset(cd, name, fields[name]);
            dataset(cd, "vol_frac", vf);
            check(H5Fflush(file, H5F_SCOPE_GLOBAL));
        }
        {
            H5 file(H5Fcreate(eb_tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT), H5Fclose);
            H5 root(H5Gcreate2(file, "VTKHDF", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), H5Gclose);
            header(root, "PolyData");
            std::vector<Real> eb, normals, weights;
            std::vector<Index> verts, triangles;
            if (cut)
                for (const auto& [e, sub] : cuts)
                    if (m.partitioning[e] == comm.rank()) {
                        auto a = m.hex[8 * e], b = m.hex[8 * e + 6];
                        auto physical = [&](std::size_t q, int k) {
                            return m.vertices[3 * a + k] +
                                   cut->bq_xi[k][q] *
                                       (m.vertices[3 * b + k] - m.vertices[3 * a + k]);
                        };
                        for (Local q = cut->bq_off[sub]; q < cut->bq_off[sub + 1]; ++q) {
                            verts.push_back(static_cast<Index>(eb.size() / 3));
                            for (int k = 0; k < 3; ++k) {
                                eb.push_back(physical(q, k));
                                normals.push_back(cut->bq_n[k][q]);
                            }
                            weights.push_back(cut->bq_w[q]);
                        }
                        // Three triangle-rule points determine each planar facet exactly.
                        for (Local q = cut->bq_off[sub]; q + 2 < cut->bq_off[sub + 1]; q += 3)
                            for (int v = 0; v < 3; ++v) {
                                triangles.push_back(static_cast<Index>(eb.size() / 3));
                                for (int k = 0; k < 3; ++k) {
                                    eb.push_back(
                                        2 * physical(q + v, k) -
                                        (physical(q, k) + physical(q + 1, k) + physical(q + 2, k)) /
                                            3);
                                    normals.push_back(cut->bq_n[k][q]);
                                }
                                weights.push_back(0);
                            }
                    }
            dataset(root, "NumberOfPoints", std::vector<Index>{static_cast<Index>(eb.size() / 3)});
            dataset(root, "Points", eb, 3);
            topology(root, "Vertices", verts, 1);
            topology(root, "Lines", {}, 2);
            topology(root, "Polygons", triangles, 3);
            topology(root, "Strips", {}, 3);
            H5 pd(H5Gcreate2(root, "PointData", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), H5Gclose);
            dataset(pd, "normal", normals, 3);
            dataset(pd, "area_weight", weights);
            check(H5Fflush(file, H5F_SCOPE_GLOBAL));
        }
        std::filesystem::rename(grid_tmp, paths.grid);
        std::filesystem::rename(eb_tmp, paths.embedded_boundary);
        return paths;
    } catch (const std::exception& e) {
        std::error_code ec;
        std::filesystem::remove(grid_tmp, ec);
        std::filesystem::remove(eb_tmp, ec);
        return Error{stem.string(), e.what()};
    }
}
} // namespace chipcae::mesh
