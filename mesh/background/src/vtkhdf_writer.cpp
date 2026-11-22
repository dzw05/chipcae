#include "chipcae/mesh/background/vtkhdf_writer.hpp"

#include "chipcae/geometry/import_report.hpp"
#include "chipcae/io/detail/hdf5.hpp"
#include "chipcae/mesh/cut/cut_layer.hpp"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>
#ifdef CHIPCAE_ENABLE_MPI
#include <mpi.h>
#endif
namespace chipcae::mesh {
namespace {
using namespace chipcae::hdf_detail;
// Fixed-width UTF-8 strings also work with parallel HDF5 (no variable-length heap).
void strings(hid_t parent,
             const char* name,
             const std::vector<std::string>& values,
             const WriterContext& ctx) {
    std::size_t width = 1;
    for (const auto& value : values)
        width = std::max(width, value.size() + 1);
    H5 type(H5Tcopy(H5T_C_S1), H5Tclose);
    check(H5Tset_size(type, width));
    check(H5Tset_cset(type, H5T_CSET_UTF8));
    check(H5Tset_strpad(type, H5T_STR_NULLPAD));
    hsize_t count = values.size();
    H5 space(H5Screate_simple(1, &count, nullptr), H5Sclose);
    H5 set(H5Dcreate2(parent, name, type, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), H5Dclose);
    if (!ctx.collective || ctx.rank == 0) {
        std::vector<char> bytes(values.size() * width, 0);
        for (std::size_t i = 0; i < values.size(); ++i)
            std::copy(values[i].begin(), values[i].end(), bytes.begin() + i * width);
        if (!bytes.empty())
            check(H5Dwrite(set, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, bytes.data()));
    }
}

void topology(hid_t root,
              const char* name,
              const std::vector<Index>& connectivity,
              Index stride,
              const WriterContext& ctx) {
    H5 g(H5Gcreate2(root, name, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), H5Gclose);
    Index cells = static_cast<Index>(connectivity.size()) / stride;
    dataset(g, "NumberOfCells", std::vector<Index>{cells}, ctx);
    dataset(g,
            "NumberOfConnectivityIds",
            std::vector<Index>{static_cast<Index>(connectivity.size())},
            ctx);
    dataset(g, "Connectivity", connectivity, ctx);
    std::vector<Index> offsets(static_cast<std::size_t>(cells + 1));
    for (Index i = 0; i <= cells; ++i)
        offsets[static_cast<std::size_t>(i)] = i * stride;
    dataset(g, "Offsets", offsets, ctx);
}
} // namespace
Expected<std::filesystem::path>
write_defect_points_vtkhdf(const std::filesystem::path& path,
                           const std::vector<geometry::Defect>& defects,
                           const std::vector<std::array<Real, 3>>& points) {
    if (defects.size() != points.size())
        return Error{"vtkhdf", "defect and point counts differ"};
    try {
        ErrorSilencer hush;
        H5 file(H5Fcreate(path.string().c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT),
                H5Fclose);
        header(file, "UnstructuredGrid");
        H5 piece(H5Gcreate2(file, "VTKHDF", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), H5Gclose);
        std::vector<Real> coords;
        std::vector<Index> hashes, kinds;
        std::vector<unsigned char> tiers;
        coords.reserve(points.size() * 3);
        hashes.reserve(points.size());
        kinds.reserve(points.size());
        tiers.reserve(points.size());
        for (std::size_t i = 0; i < points.size(); ++i) {
            coords.insert(coords.end(), points[i].begin(), points[i].end());
            const auto id = geometry::defect_id(defects[i]);
            std::uint64_t hash = 1469598103934665603ULL;
            for (unsigned char c : id) {
                hash ^= c;
                hash *= 1099511628211ULL;
            }
            hashes.push_back(static_cast<Index>(hash));
            std::uint64_t kind_hash = 1469598103934665603ULL;
            for (unsigned char c : defects[i].kind) {
                kind_hash ^= c;
                kind_hash *= 1099511628211ULL;
            }
            kinds.push_back(static_cast<Index>(kind_hash));
            tiers.push_back(static_cast<unsigned char>(defects[i].tier));
        }
        WriterContext ctx;
        dataset(piece, "Points", coords, ctx, 3);
        dataset(piece, "defect_id_hash", hashes, ctx);
        dataset(piece, "tier", tiers, ctx);
        dataset(piece, "kind", kinds, ctx);
        return path;
    } catch (const std::exception& e) {
        return Error{"vtkhdf", e.what()};
    }
}
Expected<VtkhdfFiles> write_vtkhdf(const std::filesystem::path& stem,
                                   const HexMeshArrays& m,
                                   const cut::CutLayerArrays* cut,
                                   const Communicator& comm,
                                   bool debug_per_rank) {
    ErrorSilencer hush;
    WriterContext ctx;
    ctx.rank = comm.rank();
    ctx.size = comm.size();
#if defined(H5_HAVE_PARALLEL) && defined(CHIPCAE_ENABLE_MPI)
    ctx.collective = !debug_per_rank;
#endif
#ifdef CHIPCAE_ENABLE_MPI
    // Communicator is a world snapshot and has no native handle, so MPI-IO and
    // the agreement reduction use MPI_COMM_WORLD after checking the snapshot.
    // All ranks must call this with the same world communicator.
    auto vtkhdf_comm = MPI_COMM_WORLD;
    if (!comm.is_serial()) {
        int wr = 0, ws = 1, ready = 0;
        MPI_Initialized(&ready);
        if (ready) {
            MPI_Comm_rank(MPI_COMM_WORLD, &wr);
            MPI_Comm_size(MPI_COMM_WORLD, &ws);
        }
        if (wr != comm.rank() || ws != comm.size())
            return Error{"vtkhdf", "Communicator snapshot does not match MPI_COMM_WORLD"};
    }
#endif
    auto suffix = debug_per_rank ? ".rank" + std::to_string(comm.rank()) : std::string{};
    VtkhdfFiles paths{stem.string() + suffix + ".vtkhdf", stem.string() + suffix + ".eb.vtkhdf"};
    // Global mesh/cut arrays already gather all ranks' source data in preparation.
    auto agree = [&](bool ok) -> Expected<VtkhdfFiles> {
#ifdef CHIPCAE_ENABLE_MPI
        if (!debug_per_rank) {
            int flag = ok ? 1 : 0, all = flag;
            MPI_Allreduce(&flag, &all, 1, MPI_INT, MPI_MIN, vtkhdf_comm);
            if (!all)
                return Error{"vtkhdf", "collective write failed"};
        }
#endif
        if (!ok)
            return Error{"vtkhdf", "write failed"};
        return paths;
    };
    bool ok = true;
    std::filesystem::path grid_tmp = paths.grid.string() + ".tmp",
                          eb_tmp = paths.embedded_boundary.string() + ".tmp";
    if (ctx.collective || debug_per_rank || comm.rank() == 0)
        try {
            H5 access(H5Pcreate(H5P_FILE_ACCESS), H5Pclose);
#if defined(H5_HAVE_PARALLEL) && defined(CHIPCAE_ENABLE_MPI)
            if (ctx.collective)
                check(H5Pset_fapl_mpio(access, vtkhdf_comm, MPI_INFO_NULL));
#endif
            if (m.hex.size() != 8 * m.attribute.size() ||
                m.partitioning.size() != m.attribute.size())
                throw std::runtime_error("inconsistent arrays");
            std::vector<Local> cells;
            std::map<Local, Index> nodes;
            std::vector<Real> points;
            std::vector<Index> connectivity, offsets{0};
            for (std::size_t e = 0; e < m.attribute.size(); ++e)
                if (!debug_per_rank || m.partitioning[e] == comm.rank()) {
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
                H5 file(H5Fcreate(grid_tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, access), H5Fclose);
                H5 root(H5Gcreate2(file, "VTKHDF", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
                        H5Gclose);
                header(root, "UnstructuredGrid");
                dataset(root,
                        "NumberOfPoints",
                        std::vector<Index>{static_cast<Index>(nodes.size())},
                        ctx);
                dataset(root,
                        "NumberOfCells",
                        std::vector<Index>{static_cast<Index>(cells.size())},
                        ctx);
                dataset(root,
                        "NumberOfConnectivityIds",
                        std::vector<Index>{static_cast<Index>(connectivity.size())},
                        ctx);
                dataset(root, "Points", points, ctx, 3);
                dataset(root, "Connectivity", connectivity, ctx);
                dataset(root, "Offsets", offsets, ctx);
                dataset(root, "Types", std::vector<unsigned char>(cells.size(), 12), ctx);
                H5 cd(H5Gcreate2(root, "CellData", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
                      H5Gclose);
                std::map<std::string, std::vector<Local>> fields;
                std::vector<Real> vf, phi;
                phi.reserve(cells.size());
                const auto ncopper =
                    static_cast<std::size_t>(std::count_if(m.copper_layer_of_layer.begin(),
                                                           m.copper_layer_of_layer.end(),
                                                           [](auto cl) { return cl >= 0; }));
                std::map<std::string, Local> layers, regions;
                for (const auto& a : m.attr_table.entries) {
                    if (!layers.contains(a.layer))
                        layers[a.layer] = static_cast<Local>(layers.size() + 1);
                    if (!a.region.empty() && !regions.contains(a.region))
                        regions[a.region] = static_cast<RegionId>(regions.size() + 1);
                }
                std::vector<std::string> layer_names(layers.size()),
                    material_names(m.attr_table.materials.size());
                for (const auto& [name, id] : layers)
                    layer_names.at(id - 1) = name;
                for (const auto& [name, id] : m.attr_table.materials)
                    material_names.at(id - 1) = name;
                H5 fd(H5Gcreate2(root, "FieldData", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
                      H5Gclose);
                strings(fd, "layer_names", layer_names, ctx);
                strings(fd, "material_names", material_names, ctx);
                for (auto e : cells) {
                    const auto& a =
                        m.attr_table.entries.at(static_cast<std::size_t>(m.attribute[e] - 1));
                    fields["attribute"].push_back(m.attribute[e]);
                    fields["layer_id"].push_back(layers[a.layer]);
                    fields["region"].push_back(a.region.empty() ? 0 : regions[a.region]);
                    fields["material_id"].push_back(a.material);
                    fields["body"].push_back(a.body);
                    fields["rank"].push_back(m.partitioning[e]);
                    fields["cell_kind"].push_back(voids.contains(e) ? 2 : cuts.contains(e) ? 1 : 0);
                    vf.push_back(voids.contains(e)  ? 0
                                 : cuts.contains(e) ? cut->sub_volfrac[cuts[e]]
                                                    : 1);
                    Real copper = 0;
                    const auto layer = m.attr_table.layers.find(a.layer);
                    if (layer != m.attr_table.layers.end() && !m.copper_layer_of_layer.empty()) {
                        if (layer->second <= 0 || static_cast<std::size_t>(layer->second - 1) >=
                                                      m.copper_layer_of_layer.size())
                            throw std::runtime_error("inconsistent copper arrays");
                        const auto cl =
                            m.copper_layer_of_layer[static_cast<std::size_t>(layer->second - 1)];
                        if (cl >= 0) {
                            if (m.axes[0].size() < 2 || m.axes[1].size() < 2 ||
                                m.axes[2].size() < 2 ||
                                static_cast<std::size_t>(e) >= m.elem_gid.size() ||
                                m.elem_gid[e] < 0)
                                throw std::runtime_error("inconsistent copper arrays");
                            const auto nx = m.axes[0].size() - 1, ny = m.axes[1].size() - 1;
                            if (nx > std::numeric_limits<std::size_t>::max() / ny)
                                throw std::runtime_error("inconsistent copper arrays");
                            const auto ncol = nx * ny, nz = m.axes[2].size() - 1;
                            const auto column = static_cast<std::size_t>(m.elem_gid[e]) / nz;
                            if (ncopper > std::numeric_limits<std::size_t>::max() / ncol ||
                                m.copper_phi.size() != ncopper * ncol ||
                                static_cast<std::size_t>(cl) >= ncopper || column >= ncol)
                                throw std::runtime_error("inconsistent copper arrays");
                            copper = m.copper_phi[static_cast<std::size_t>(cl) * ncol + column];
                        }
                    }
                    phi.push_back(copper);
                }
                for (auto name : {"attribute",
                                  "layer_id",
                                  "region",
                                  "material_id",
                                  "body",
                                  "rank",
                                  "cell_kind"})
                    dataset(cd, name, fields[name], ctx);
                dataset(cd, "vol_frac", vf, ctx);
                dataset(cd, "copper_phi", phi, ctx);
                check(H5Fflush(file, H5F_SCOPE_GLOBAL));
            }
            {
                H5 file(H5Fcreate(eb_tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, access), H5Fclose);
                H5 root(H5Gcreate2(file, "VTKHDF", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
                        H5Gclose);
                header(root, "PolyData");
                std::vector<Real> eb, normals, weights;
                std::vector<Index> verts, triangles;
                if (cut)
                    for (const auto& [e, sub] : cuts)
                        if (!debug_per_rank || m.partitioning[e] == comm.rank()) {
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
                            // Explicit mesh-owned triangles are independent of bq degree.
                            for (std::size_t v = 0; v < cut->eb_parent_elem.size(); ++v)
                                if (cut->eb_parent_elem[v] == e) {
                                    triangles.push_back(static_cast<Index>(eb.size() / 3));
                                    for (int k = 0; k < 3; ++k) {
                                        eb.push_back(cut->eb_points[3 * v + k]);
                                        normals.push_back(cut->eb_normals[3 * v + k]);
                                    }
                                    weights.push_back(0);
                                }
                        }
                dataset(root,
                        "NumberOfPoints",
                        std::vector<Index>{static_cast<Index>(eb.size() / 3)},
                        ctx);
                dataset(root, "Points", eb, ctx, 3);
                topology(root, "Vertices", verts, 1, ctx);
                topology(root, "Lines", {}, 2, ctx);
                topology(root, "Polygons", triangles, 3, ctx);
                topology(root, "Strips", {}, 3, ctx);
                H5 pd(H5Gcreate2(root, "PointData", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
                      H5Gclose);
                dataset(pd, "normal", normals, ctx, 3);
                dataset(pd, "area_weight", weights, ctx);
                check(H5Fflush(file, H5F_SCOPE_GLOBAL));
            }
        } catch (const std::exception& e) {
            std::error_code ec;
            std::filesystem::remove(grid_tmp, ec);
            std::filesystem::remove(eb_tmp, ec);
            ok = false;
        }
    auto result = agree(ok);
    if (!result) {
        if (debug_per_rank || comm.rank() == 0) {
            std::error_code ec;
            std::filesystem::remove(grid_tmp, ec);
            std::filesystem::remove(eb_tmp, ec);
            std::filesystem::remove(paths.grid, ec);
            std::filesystem::remove(paths.embedded_boundary, ec);
        }
#ifdef CHIPCAE_ENABLE_MPI
        if (!debug_per_rank)
            MPI_Barrier(vtkhdf_comm);
#endif
        return result;
    }
    bool renamed = true;
    if (debug_per_rank || comm.rank() == 0) {
        std::error_code ec;
        std::filesystem::rename(grid_tmp, paths.grid, ec);
        renamed = !ec;
        if (renamed) {
            std::filesystem::rename(eb_tmp, paths.embedded_boundary, ec);
            renamed = !ec;
        }
    }
    result = agree(renamed);
    if (!result) {
        if (debug_per_rank || comm.rank() == 0) {
            std::error_code ec;
            std::filesystem::remove(grid_tmp, ec);
            std::filesystem::remove(eb_tmp, ec);
            std::filesystem::remove(paths.grid, ec);
            std::filesystem::remove(paths.embedded_boundary, ec);
        }
#ifdef CHIPCAE_ENABLE_MPI
        if (!debug_per_rank)
            MPI_Barrier(vtkhdf_comm);
#endif
        return result;
    }
#ifdef CHIPCAE_ENABLE_MPI
    if (!debug_per_rank)
        MPI_Barrier(vtkhdf_comm);
#endif
    return result;
}
} // namespace chipcae::mesh
