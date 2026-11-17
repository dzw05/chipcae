#include "chipcae/io/vtkhdf_results.hpp"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
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
           p.material_id.empty() && p.cell_kind.empty() && p.elem_gid.empty();
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
    const auto& p = local.hex;
    auto np = p.points.size() / 3, nc = p.hex.size() / 8;
    bool valid = n >= 0 && std::isfinite(time) && p.points.size() % 3 == 0 &&
                 p.hex.size() % 8 == 0 && p.T.size() == np && p.U.size() == 3 * np &&
                 p.layer_id.size() == nc && p.material_id.size() == nc &&
                 p.cell_kind.size() == nc && (p.elem_gid.empty() || p.elem_gid.size() == nc) &&
                 empty(local.cut_poly) && empty(local.embedded_boundary) && empty(local.particles);
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
            header(root, "PartitionedDataSetCollection", 2, 9);
            assembly = H5Gcreate2(root, "Assembly", H5P_DEFAULT, gcpl, H5P_DEFAULT);
            if (root < 0 || assembly < 0)
                throw std::runtime_error("create root groups");
        });
        const char* names[] = {"hex", "cut_poly", "embedded_boundary", "particles"};
        for (int block = 0; block < 4; ++block) {
            hid_t g = -1;
            metadata([&] {
                g = H5Gcreate2(root, names[block], H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
                header(g, block >= 2 ? "PolyData" : "UnstructuredGrid", 2, 9);
                H5 scalar(H5Screate(H5S_SCALAR), H5Sclose);
                H5 index(H5Acreate2(g, "Index", H5T_NATIVE_INT64, scalar, H5P_DEFAULT, H5P_DEFAULT),
                         H5Aclose);
                Index block_index = block;
                check(H5Awrite(index, H5T_NATIVE_INT64, &block_index));

                check(H5Lcreate_soft((std::string("/VTKHDF/") + names[block]).c_str(),
                                     assembly,
                                     names[block],
                                     H5P_DEFAULT,
                                     H5P_DEFAULT));
            });
            ResultsPartition none;
            const auto& q = block == 0 ? p : none;
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
                topology(g, q.hex, 8);
                emit(g, "Types", std::vector<unsigned char>(q.hex.size() / 8, 12));
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
            if (block == 0) {
                hid_t pd = -1, cd = -1;
                metadata([&] {
                    pd = H5Gcreate2(g, "PointData", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
                    cd = H5Gcreate2(g, "CellData", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
                    if (pd < 0 || cd < 0)
                        throw std::runtime_error("create fields");
                });
                emit(pd, "T", p.T);
                emit(pd, "U", p.U, 3);
                emit(cd, "layer_id", p.layer_id);
                emit(cd, "material_id", p.material_id);
                emit(cd, "cell_kind", p.cell_kind);
                if (gid_any)
                    emit(cd, "elem_gid", p.elem_gid);
                if (pd >= 0) {
                    check(H5Gclose(pd));
                    check(H5Gclose(cd));
                }
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
                   "\"unit\":\"K\"},{\"name\":\"U\",\"location\":\"point\",\"components\":3,"
                   "\"unit\":\"m\"}";
            for (auto name : {"layer_id", "material_id", "cell_kind"})
                out << ",{\"name\":" << quoted(name)
                    << ",\"location\":\"cell\",\"components\":1,\"unit\":\"1\"}";
            if (gids_)
                out << ",{\"name\":\"elem_gid\",\"location\":\"cell\",\"components\":1,\"unit\":"
                       "\"1\"}";
            out << ",{\"name\":\"sigma_vm\",\"location\":\"cell\",\"components\":1,\"unit\":\"Pa\","
                   "\"reserved\":true}],\n\"blocks\":[{\"index\":0,\"name\":\"hex\",\"kind\":"
                   "\"UnstructuredGrid\"},{\"index\":1,\"name\":\"cut_poly\",\"kind\":\"VTK_"
                   "POLYHEDRON\"},{\"index\":2,\"name\":\"embedded_boundary\",\"kind\":"
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
