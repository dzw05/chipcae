#include "chipcae/io/vtkhdf_results.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#ifdef CHIPCAE_ENABLE_HDF5
#include "chipcae/io/detail/hdf5.hpp"
#endif
#ifdef CHIPCAE_ENABLE_MPI
#include <mpi.h>
#endif
namespace chipcae::io {
namespace {
void agree(bool ok) {
#ifdef CHIPCAE_ENABLE_MPI
    int initialized = 0;
    MPI_Initialized(&initialized);
    if (initialized) {
        int local = ok, global;
        MPI_Allreduce(&local, &global, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD);
        ok = global != 0;
    }
#endif
    if (!ok)
        throw std::runtime_error("results collective operation failed");
}
std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 32)
            throw std::invalid_argument("control character in metadata");
        else
            out += static_cast<char>(c);
    }
    return out + '"';
}
[[maybe_unused]] bool empty(const ResultsPartition& p) {
    return p.points.empty() && p.hex.empty() && p.T.empty() && p.U.empty() && p.layer_id.empty() &&
           p.material_id.empty() && p.cell_kind.empty() && p.elem_gid.empty() && p.volume.empty() &&
           p.connectivity.empty() && p.face_conn.empty() && p.poly_faces.empty() &&
           p.offsets == std::vector<Index>{0} && p.face_off == std::vector<Index>{0} &&
           p.poly_off == std::vector<Index>{0};
}
#ifdef CHIPCAE_ENABLE_HDF5
using namespace chipcae::hdf_detail;
// Same checked HDF5 handles, types and headers as the background mesh writer.
// Each rank owns one contiguous partition. Empty ranks select none, but still
// participate in every collective H5Dwrite.
template <class T>
void distributed(hid_t group,
                 const char* name,
                 const std::vector<T>& local,
                 int rank,
                 int size,
                 bool parallel,
                 hsize_t components = 1) {
    std::vector<int> counts(static_cast<std::size_t>(size)),
        offsets(static_cast<std::size_t>(size));
    agree(local.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max() / sizeof(T) /
                                                   static_cast<unsigned>(size)));
    int count = static_cast<int>(local.size());
    counts[0] = count;
#ifdef CHIPCAE_ENABLE_MPI
    if (size > 1)
        MPI_Allgather(&count, 1, MPI_INT, counts.data(), 1, MPI_INT, MPI_COMM_WORLD);
#endif
    int total = 0;
    for (int i = 0; i < size; ++i) {
        offsets[i] = total;
        total += counts[i];
    }
    if (!parallel) {
        std::vector<T> gathered(rank == 0 ? static_cast<std::size_t>(total) : 0);
#ifdef CHIPCAE_ENABLE_MPI
        if (size > 1)
            MPI_Gatherv(
                local.data(),
                count * static_cast<int>(sizeof(T)),
                MPI_BYTE,
                gathered.data(),
                [&] {
                    for (auto& c : counts)
                        c *= static_cast<int>(sizeof(T));
                    return counts.data();
                }(),
                [&] {
                    for (auto& o : offsets)
                        o *= static_cast<int>(sizeof(T));
                    return offsets.data();
                }(),
                MPI_BYTE,
                0,
                MPI_COMM_WORLD);
        else
#endif
            gathered = local;
        bool ok = true;
        if (rank == 0)
            try {
                dataset(group, name, gathered, WriterContext{}, components);
            } catch (...) {
                ok = false;
            }
        agree(ok);
        return;
    }
    hsize_t dims[2] = {static_cast<hsize_t>(total) / components, components};
    H5 space(H5Screate_simple(components == 1 ? 1 : 2, dims, nullptr), H5Sclose);
    H5 set(H5Dcreate2(group, name, native<T>(), space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
           H5Dclose);
    hsize_t md[2] = {std::max(hsize_t(1), static_cast<hsize_t>(count) / components), components};
    H5 memory(H5Screate_simple(components == 1 ? 1 : 2, md, nullptr), H5Sclose);
    if (!count) {
        check(H5Sselect_none(space));
        check(H5Sselect_none(memory));
    } else {
        hsize_t start[2] = {static_cast<hsize_t>(offsets[rank]) / components, 0},
                extent[2] = {static_cast<hsize_t>(count) / components, components};
        check(H5Sselect_hyperslab(space, H5S_SELECT_SET, start, nullptr, extent, nullptr));
    }
    H5 transfer(H5Pcreate(H5P_DATASET_XFER), H5Pclose);
#if defined(H5_HAVE_PARALLEL) && defined(CHIPCAE_ENABLE_MPI)
    check(H5Pset_dxpl_mpio(transfer, H5FD_MPIO_COLLECTIVE));
#endif
    check(H5Dwrite(set, native<T>(), memory, space, transfer, local.data()));
}
#endif
} // namespace
ResultsWriter::ResultsWriter(std::filesystem::path run, const std::filesystem::path& stackup)
    : run_(std::move(run)) {
#ifdef CHIPCAE_ENABLE_MPI
    int initialized = 0;
    MPI_Initialized(&initialized);
    if (initialized) {
        MPI_Comm_rank(MPI_COMM_WORLD, &rank_);
        MPI_Comm_size(MPI_COMM_WORLD, &size_);
    }
#endif
    bool ok = true;
    if (rank_ == 0)
        try {
            std::filesystem::create_directories(run_ / "out");
            if (std::filesystem::exists(run_ / "manifest.json"))
                throw std::runtime_error("run already finalized");
            std::filesystem::copy_file(
                stackup, run_ / "stackup.json", std::filesystem::copy_options::overwrite_existing);
        } catch (...) {
            ok = false;
        }
    agree(ok);
}
void ResultsWriter::write_step(int n, double time, const ResultsStep& local) {
#ifndef CHIPCAE_ENABLE_HDF5
    (void)n;
    (void)time;
    (void)local;
    throw std::runtime_error("ResultsWriter requires CHIPCAE_ENABLE_HDF5");
#else
#ifdef CHIPCAE_ENABLE_MPI
    if (size_ > 1) {
        int lo, hi;
        double tlo, thi;
        MPI_Allreduce(&n, &lo, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD);
        MPI_Allreduce(&n, &hi, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
        MPI_Allreduce(&time, &tlo, 1, MPI_DOUBLE, MPI_MIN, MPI_COMM_WORLD);
        MPI_Allreduce(&time, &thi, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
        agree(lo == hi && tlo == thi);
    }
#endif
    auto p = local.hex;
    const auto input_cells = p.hex.size() / 8;
    agree(p.hex.size() % 8 == 0 && p.points.size() % 3 == 0 && p.T.size() == p.points.size() / 3 &&
          p.U.size() == p.points.size() && p.layer_id.size() == input_cells &&
          p.material_id.size() == input_cells && p.cell_kind.size() == input_cells &&
          (p.elem_gid.empty() || p.elem_gid.size() == input_cells));
    bool input_ids = true;
    for (auto id : p.hex)
        input_ids &= id >= 0 && static_cast<std::size_t>(id) < p.points.size() / 3;
    agree(input_ids);
    bool matched = true;
    if (!local.cut_as_hex) {
        // Never silently lose a cut parent when geometric export was skipped.
        for (std::size_t i = 0; i < input_cells; ++i)
            if (p.cell_kind[i] == 1)
                matched &= p.elem_gid.size() == input_cells &&
                           std::find(local.cut.elem_gid.begin(),
                                     local.cut.elem_gid.end(),
                                     p.elem_gid[i]) != local.cut.elem_gid.end();
    }
    agree(matched);
    if (!local.cut_as_hex) {
        ResultsPartition filtered = p;
        filtered.hex.clear();
        filtered.elem_gid.clear();
        filtered.layer_id.clear();
        filtered.material_id.clear();
        filtered.cell_kind.clear();
        for (std::size_t i = 0; i < p.hex.size() / 8; ++i) {
            if (p.cell_kind[i] == 1)
                continue;
            filtered.hex.insert(
                filtered.hex.end(), p.hex.begin() + 8 * i, p.hex.begin() + 8 * i + 8);
            if (i < p.elem_gid.size())
                filtered.elem_gid.push_back(p.elem_gid[i]);
            if (i < p.layer_id.size())
                filtered.layer_id.push_back(p.layer_id[i]);
            if (i < p.material_id.size())
                filtered.material_id.push_back(p.material_id[i]);
            if (i < p.cell_kind.size())
                filtered.cell_kind.push_back(p.cell_kind[i]);
        }
        p = std::move(filtered);
    }
    ResultsPartition no_cut;
    const auto& cut = local.cut_as_hex ? no_cut : local.cut;
    auto cn = cut.poly_off.size() ? cut.poly_off.size() - 1 : 0;
    auto offsets_valid = [](const auto& off, std::size_t count) {
        return !off.empty() && off.front() == 0 && off.back() == static_cast<Index>(count) &&
               std::is_sorted(off.begin(), off.end());
    };
    bool cv = cut.points.size() % 3 == 0 && offsets_valid(cut.offsets, cut.connectivity.size()) &&
              cut.offsets.size() == cn + 1 && offsets_valid(cut.face_off, cut.face_conn.size()) &&
              offsets_valid(cut.poly_off, cut.poly_faces.size()) && cut.elem_gid.size() == cn &&
              cut.layer_id.size() == cn && cut.material_id.size() == cn &&
              cut.cell_kind.size() == cn && cut.volume.size() == cn &&
              (cut.T.empty() || cut.T.size() == cut.points.size() / 3) &&
              (cut.U.empty() || cut.U.size() == cut.points.size());
    if (cv) {
        for (std::size_t i = 0; i < cn; ++i)
            cv &= cut.poly_off[i + 1] - cut.poly_off[i] >= 4 &&
                  cut.offsets[i + 1] - cut.offsets[i] >= 4;
        for (std::size_t i = 1; i < cut.face_off.size(); ++i)
            cv &= cut.face_off[i] - cut.face_off[i - 1] >= 3;
    }
    for (auto id : cut.connectivity)
        cv &= id >= 0 && static_cast<std::size_t>(id) < cut.points.size() / 3;
    for (auto id : cut.face_conn)
        cv &= id >= 0 && static_cast<std::size_t>(id) < cut.points.size() / 3;
    for (auto id : cut.poly_faces)
        cv &= id >= 0 && static_cast<std::size_t>(id) + 1 < cut.face_off.size();
    if (cv) {
        for (std::size_t i = 0; i < cn; ++i) {
            std::set<Index> vertices(cut.connectivity.begin() + cut.offsets[i],
                                     cut.connectivity.begin() + cut.offsets[i + 1]);
            cv &= vertices.size() == static_cast<std::size_t>(cut.offsets[i + 1] - cut.offsets[i]);
            std::set<std::set<Index>> faces;
            for (auto j = cut.poly_off[i]; j < cut.poly_off[i + 1]; ++j) {
                auto f = cut.poly_faces[j];
                std::set<Index> ring;
                for (auto k = cut.face_off[f]; k < cut.face_off[f + 1]; ++k) {
                    cv &= vertices.contains(cut.face_conn[k]);
                    cv &= ring.insert(cut.face_conn[k]).second;
                }
                cv &= faces.insert(std::move(ring)).second;
            }
        }
    }
    for (auto v : cut.volume)
        cv &= std::isfinite(v) && v > 0;
    for (auto kind : cut.cell_kind)
        cv &= kind == 1 || kind == 2;
    int fields = (!cut.T.empty() ? 1 : 0) | (!cut.U.empty() ? 2 : 0);
    int schema = fields;
    int fallback = local.cut_as_hex, fallback_max = fallback, fallback_min = fallback;
#ifdef CHIPCAE_ENABLE_MPI
    if (size_ > 1) {
        MPI_Allreduce(&fields, &schema, 1, MPI_INT, MPI_BOR, MPI_COMM_WORLD);
        MPI_Allreduce(&fallback, &fallback_min, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD);
        MPI_Allreduce(&fallback, &fallback_max, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
    }
#endif
    cv &= cut.points.empty() || fields == schema;
    agree(cv && fallback_min == fallback_max);
    auto np = p.points.size() / 3, nc = p.hex.size() / 8;
    bool valid = n >= 0 && std::isfinite(time) && p.points.size() % 3 == 0 &&
                 p.hex.size() % 8 == 0 && p.T.size() == np && p.U.size() == 3 * np &&
                 p.layer_id.size() == nc && p.material_id.size() == nc &&
                 p.cell_kind.size() == nc && (p.elem_gid.empty() || p.elem_gid.size() == nc) &&
                 empty(local.embedded_boundary) && empty(local.particles);
    for (auto id : p.hex)
        valid = valid && id >= 0 && static_cast<std::size_t>(id) < np;
    for (auto kind : p.cell_kind)
        valid = valid && kind >= 0 && kind <= 2;
    for (auto& s : steps_)
        valid = valid && s.n != n;
    agree(valid);
    int gid_present = !p.elem_gid.empty(), gid_any = gid_present;
#ifdef CHIPCAE_ENABLE_MPI
    if (size_ > 1)
        MPI_Allreduce(&gid_present, &gid_any, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
#endif
    agree(!gid_any || nc == 0 || gid_present);
    agree(steps_.empty() || gids_ == (gid_any != 0));
    bool parallel = false;
#if defined(H5_HAVE_PARALLEL) && defined(CHIPCAE_ENABLE_MPI)
    parallel = size_ > 1;
#endif
    auto filename = "out/step-" + std::to_string(n) + ".vtkhdf";
    auto tmp = run_ / (filename + ".tmp");
    bool ok = true;
    try {
        H5 access(H5Pcreate(H5P_FILE_ACCESS), H5Pclose);
        check(H5Pset_fclose_degree(access, H5F_CLOSE_STRONG));
        std::unique_ptr<H5> file_guard;
#if defined(H5_HAVE_PARALLEL) && defined(CHIPCAE_ENABLE_MPI)
        if (parallel)
            check(H5Pset_fapl_mpio(access, MPI_COMM_WORLD, MPI_INFO_NULL));
#endif
        H5 gcpl(H5Pcreate(H5P_GROUP_CREATE), H5Pclose);
        check(H5Pset_link_creation_order(gcpl, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED));
        hid_t file = -1, root = -1, assembly = -1;
        auto metadata = [&](auto action) {
            bool success = true;
            if (parallel || rank_ == 0)
                try {
                    action();
                } catch (...) {
                    success = false;
                }
            agree(success);
        };
        metadata([&] {
            file = H5Fcreate(tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, access);
            if (file < 0)
                throw std::runtime_error("create results file");
            file_guard = std::make_unique<H5>(file, H5Fclose);
            root = H5Gcreate2(file, "VTKHDF", H5P_DEFAULT, gcpl, H5P_DEFAULT);
            header(root, "PartitionedDataSetCollection", 2, 8);
            assembly = H5Gcreate2(root, "Assembly", H5P_DEFAULT, gcpl, H5P_DEFAULT);
            if (root < 0 || assembly < 0)
                throw std::runtime_error("create root groups");
        });
        const char* names[] = {"hex", "cut", "eb_surface", "particles"};
        for (int block = 0; block < 4; ++block) {
            hid_t g = -1;
            metadata([&] {
                g = H5Gcreate2(root, names[block], H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
                header(g, block >= 2 ? "PolyData" : "UnstructuredGrid", 2, 8);
                H5 scalar(H5Screate(H5S_SCALAR), H5Sclose);
                H5 index(H5Acreate2(g, "Index", H5T_NATIVE_INT64, scalar, H5P_DEFAULT, H5P_DEFAULT),
                         H5Aclose);
                Index block_index = block;
                check(H5Awrite(index, H5T_NATIVE_INT64, &block_index));

                const char* category = block < 2    ? "volume"
                                       : block == 2 ? "boundary"
                                                    : "lagrangian";
                if (H5Lexists(assembly, category, H5P_DEFAULT) == 0) {
                    H5 subgroup(H5Gcreate2(assembly, category, H5P_DEFAULT, gcpl, H5P_DEFAULT),
                                H5Gclose);
                }
                H5 subgroup(H5Gopen2(assembly, category, H5P_DEFAULT), H5Gclose);
                check(H5Lcreate_soft((std::string("/VTKHDF/") + names[block]).c_str(),
                                     subgroup,
                                     names[block],
                                     H5P_DEFAULT,
                                     H5P_DEFAULT));
            });
            ResultsPartition none;
            const auto& q = block == 0 ? p : block == 1 ? cut : none;
            auto emit = [&]<class T>(hid_t target,
                                     const char* name,
                                     const std::vector<T>& v,
                                     hsize_t c = 1) {
                distributed(target, name, v, rank_, size_, parallel, c);
            };
            emit(g, "NumberOfPoints", std::vector<Index>{static_cast<Index>(q.points.size() / 3)});
            emit(g, "Points", q.points, 3);
            auto topology = [&](hid_t target, const std::vector<Index>& conn, int stride) {
                Index cells = static_cast<Index>(conn.size()) / stride;
                emit(target, "NumberOfCells", std::vector<Index>{cells});
                emit(target,
                     "NumberOfConnectivityIds",
                     std::vector<Index>{static_cast<Index>(conn.size())});
                emit(target, "Connectivity", conn);
                std::vector<Index> off(static_cast<std::size_t>(cells + 1));
                for (Index i = 0; i <= cells; ++i)
                    off[static_cast<std::size_t>(i)] = i * stride;
                emit(target, "Offsets", off);
            };
            if (block < 2) {
                if (block == 0) {
                    topology(g, q.hex, 8);
                    emit(g, "Types", std::vector<unsigned char>(q.hex.size() / 8, 12));
                } else {
                    emit(g, "NumberOfCells", std::vector<Index>{static_cast<Index>(cn)});
                    emit(g,
                         "NumberOfConnectivityIds",
                         std::vector<Index>{static_cast<Index>(q.connectivity.size())});
                    emit(g, "Connectivity", q.connectivity);
                    emit(g, "Offsets", q.offsets);
                    emit(g, "Types", std::vector<unsigned char>(cn, 42));
                    emit(g, "FaceConnectivity", q.face_conn);
                    emit(g, "FaceOffsets", q.face_off);
                    emit(g, "PolyhedronToFaces", q.poly_faces);
                    emit(g, "PolyhedronOffsets", q.poly_off);
                    emit(g,
                         "NumberOfFaces",
                         std::vector<Index>{static_cast<Index>(q.face_off.size() - 1)});
                    emit(g,
                         "NumberOfFaceConnectivityIds",
                         std::vector<Index>{static_cast<Index>(q.face_conn.size())});
                    emit(g,
                         "NumberOfPolyhedronToFaceIds",
                         std::vector<Index>{static_cast<Index>(q.poly_faces.size())});
                }
            } else
                for (auto name : {"Vertices", "Lines", "Polygons", "Strips"}) {
                    hid_t t = -1;
                    metadata([&] {
                        t = H5Gcreate2(g, name, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
                        if (t < 0)
                            throw std::runtime_error("create topology");
                    });
                    topology(t, {}, 1);
                    if (t >= 0)
                        check(H5Gclose(t));
                }
            if (block < 2) {
                hid_t pd = -1, cd = -1;
                metadata([&] {
                    pd = H5Gcreate2(g, "PointData", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
                    cd = H5Gcreate2(g, "CellData", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
                    if (pd < 0 || cd < 0)
                        throw std::runtime_error("create fields");
                });
                if (block == 0 || (schema & 1))
                    emit(pd, "T", q.T);
                if (block == 0 || (schema & 2))
                    emit(pd, "U", q.U, 3);
                emit(cd, "layer_id", q.layer_id);
                emit(cd, "material_id", q.material_id);
                emit(cd, "cell_kind", q.cell_kind);
                if (gid_any || block == 1)
                    emit(cd, "elem_gid", q.elem_gid);
                if (block == 1)
                    emit(cd, "volume", q.volume);
                if (pd >= 0) {
                    check(H5Gclose(pd));
                    check(H5Gclose(cd));
                }
            }
            if (block >= 2) {
                metadata([&] {
                    H5 pd(H5Gcreate2(g, "PointData", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
                          H5Gclose);
                    H5 cd(H5Gcreate2(g, "CellData", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
                          H5Gclose);
                });
            }
            if (g >= 0)
                check(H5Gclose(g));
        }
        if (file >= 0) {
            check(H5Gclose(assembly));
            check(H5Gclose(root));
            check(H5Fflush(file, H5F_SCOPE_GLOBAL));
            check(H5Fclose(file));
            file_guard->id = -1;
            file_guard.reset();
        }
    } catch (...) {
        ok = false;
    }
    agree(ok);
    if (rank_ == 0)
        try {
            std::filesystem::rename(tmp, run_ / filename);
        } catch (...) {
            ok = false;
        }
    agree(ok);
    gids_ = gid_any != 0;
    steps_.push_back({n, time, filename});
#endif
}
void ResultsWriter::add_summary(std::string name, double value, std::string unit, int step) {
    if (!std::isfinite(value))
        throw std::invalid_argument("nonfinite summary");
    quoted(name);
    quoted(unit);
    summary_.push_back({std::move(name), value, std::move(unit), step});
}
void ResultsWriter::finalize() {
    bool ok = true;
    if (rank_ == 0)
        try {
            for (auto& step : steps_)
                for (auto name : {"warpage_pv", "T_max", "U_mag_max"}) {
                    bool found = false;
                    for (auto& s : summary_)
                        if (s.step == step.n && s.name == name)
                            found = true;
                    if (!found)
                        throw std::runtime_error("missing required summary");
                }
            auto tmp = run_ / "manifest.json.tmp";
            std::ofstream out(tmp);
            out.exceptions(std::ios::badbit | std::ios::failbit);
            out << std::setprecision(17);
            out << "{\n\"schema_version\":\"chipcae.results/"
                   "1.0\",\"units\":\"SI\",\"stackup\":\"stackup.json\",\n\"steps\":[";
            bool first = true;
            for (auto& s : steps_) {
                if (!first)
                    out << ',';
                first = false;
                out << "{\"n\":" << s.n << ",\"time\":" << s.time << ",\"file\":" << quoted(s.file)
                    << '}';
            }
            out << "],\n\"fields\":[{\"name\":\"T\",\"location\":\"point\",\"components\":1,"
                   "\"unit\":\"K\",\"dtype\":\"float64\"},{\"name\":\"U\",\"location\":\"point\","
                   "\"components\":3,"
                   "\"unit\":\"m\",\"dtype\":\"float64\"}";
            for (auto name : {"layer_id", "material_id", "cell_kind"})
                out << ",{\"name\":" << quoted(name)
                    << ",\"location\":\"cell\",\"components\":1,\"unit\":\"1\",\"dtype\":"
                       "\"int32\"}";
            // Cut always carries its parent GID, including empty templates.
            out << ",{\"name\":\"elem_gid\",\"location\":\"cell\",\"components\":1,\"unit\":"
                   "\"1\",\"dtype\":\"int64\"}";
            out << ",{\"name\":\"volume\",\"location\":\"cell\",\"components\":1,\"unit\":\"m3\","
                   "\"dtype\":\"float64\"}";
            out << ",{\"name\":\"sigma_vm\",\"location\":\"cell\",\"components\":1,\"unit\":\"Pa\","
                   "\"dtype\":\"float64\",\"reserved\":true}],\n\"blocks\":[{\"index\":0,\"name\":"
                   "\"hex\",\"kind\":"
                   "\"UnstructuredGrid\"},{\"index\":1,\"name\":\"cut\",\"kind\":\"VTK_"
                   "POLYHEDRON\"},{\"index\":2,\"name\":\"eb_surface\",\"kind\":"
                   "\"PolyData\"},{\"index\":3,\"name\":\"particles\",\"kind\":\"PolyData\"}],"
                   "\n\"summary\":[";
            first = true;
            for (auto& s : summary_) {
                if (!first)
                    out << ',';
                first = false;
                out << "{\"name\":" << quoted(s.name) << ",\"value\":" << s.value
                    << ",\"unit\":" << quoted(s.unit) << ",\"step\":" << s.step << '}';
            }
            out << "]\n}\n";
            out.close();
            std::filesystem::rename(tmp, run_ / "manifest.json");
        } catch (...) {
            ok = false;
        }
    agree(ok);
}
} // namespace chipcae::io
