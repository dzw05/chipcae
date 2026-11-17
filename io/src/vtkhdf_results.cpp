#include "chipcae/io/vtkhdf_results.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
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
// Every rank reaches the same boundary before advancing to the next collective.
template <class F>
void phase(F action) {
    bool ok = true;
    try {
        action();
    } catch (...) {
        ok = false;
    }
    agree(ok);
}
[[maybe_unused]] bool injected(const char* point, int rank) {
#ifdef CHIPCAE_RESULTS_FAULT_INJECTION
    const char* requested = std::getenv("CHIPCAE_RESULTS_FAIL");
    const char* target = std::getenv("CHIPCAE_RESULTS_FAIL_RANK");
    return requested && target && std::strcmp(requested, point) == 0 && std::atoi(target) == rank;
#else
    (void)point;
    (void)rank;
    return false;
#endif
}
#ifdef CHIPCAE_ENABLE_HDF5
// Inject a reported failure only after all participants have entered the HDF5
// call. Local preparation failures are caught at phase boundaries instead.
template <class F>
auto hcall(bool parallel, int rank, const char* name, F action) {
    auto result = action();
    bool ok = result >= 0 && !injected(name, rank);
    if (parallel)
        agree(ok);
    else if (!ok)
        throw std::runtime_error("results HDF5 operation failed");
    return result;
}
#endif
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
           p.material_id.empty() && p.cell_kind.empty() && p.elem_gid.empty() &&
           p.connectivity.empty() && p.offsets == std::vector<Index>{0} &&
           p.face_connectivity.empty() && p.face_offsets == std::vector<Index>{0} &&
           p.polyhedron_to_faces.empty() && p.polyhedron_offsets == std::vector<Index>{0};
}
#ifdef CHIPCAE_ENABLE_HDF5
using namespace chipcae::hdf_detail;
// VTK 9.7 reads variable-length ASCII string datasets as vtkStringArray.
// Write these in a serial metadata pass after all MPI file handles close.
void strings(hid_t parent, const char* name, const std::vector<std::string>& values) {
    H5 type(H5Tcopy(H5T_C_S1), H5Tclose);
    check(H5Tset_size(type, H5T_VARIABLE));
    hsize_t count = values.size();
    H5 space(H5Screate_simple(1, &count, nullptr), H5Sclose);
    H5 set(H5Dcreate2(parent, name, type, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), H5Dclose);
    std::vector<const char*> pointers;
    for (const auto& value : values)
        pointers.push_back(value.c_str());
    if (!pointers.empty())
        check(H5Dwrite(set, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, pointers.data()));
}
void results_header(hid_t root, const char* type, int major, int minor, bool parallel, int rank) {
    hsize_t two = 2;
    H5 vs(
        hcall(
            parallel, rank, "H5Screate_simple", [&] { return H5Screate_simple(1, &two, nullptr); }),
        H5Sclose);
    H5 v(hcall(parallel,
               rank,
               "H5Acreate2",
               [&] {
                   return H5Acreate2(root, "Version", H5T_NATIVE_INT, vs, H5P_DEFAULT, H5P_DEFAULT);
               }),
         H5Aclose);
    int version[2] = {major, minor};
    check(hcall(parallel, rank, "H5Awrite", [&] { return H5Awrite(v, H5T_NATIVE_INT, version); }));
    H5 ts(hcall(parallel, rank, "H5Tcopy", [&] { return H5Tcopy(H5T_C_S1); }), H5Tclose);
    check(hcall(
        parallel, rank, "H5Tset_size", [&] { return H5Tset_size(ts, std::strlen(type) + 1); }));
    check(
        hcall(parallel, rank, "H5Tset_strpad", [&] { return H5Tset_strpad(ts, H5T_STR_NULLPAD); }));
    H5 scalar(hcall(parallel, rank, "H5Screate", [&] { return H5Screate(H5S_SCALAR); }), H5Sclose);
    H5 a(hcall(parallel,
               rank,
               "H5Acreate2",
               [&] { return H5Acreate2(root, "Type", ts, scalar, H5P_DEFAULT, H5P_DEFAULT); }),
         H5Aclose);
    check(hcall(parallel, rank, "H5Awrite", [&] { return H5Awrite(a, ts, type); }));
}
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
    std::vector<int> counts, offsets;
    phase([&] {
        counts.resize(static_cast<std::size_t>(size));
        offsets.resize(static_cast<std::size_t>(size));
    });
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
        std::vector<T> gathered;
        phase([&] {
            if (injected("gather_alloc", rank))
                throw std::bad_alloc();
            gathered.resize(rank == 0 ? static_cast<std::size_t>(total) : 0);
            if (size == 1)
                gathered = local;
        });
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

#endif
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
    H5 space(hcall(parallel,
                   rank,
                   "H5Screate_simple",
                   [&] { return H5Screate_simple(components == 1 ? 1 : 2, dims, nullptr); }),
             H5Sclose);
    H5 set(hcall(parallel,
                 rank,
                 "H5Dcreate2",
                 [&] {
                     return H5Dcreate2(
                         group, name, native<T>(), space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
                 }),
           H5Dclose);
    hsize_t md[2] = {std::max(hsize_t(1), static_cast<hsize_t>(count) / components), components};
    H5 memory(hcall(parallel,
                    rank,
                    "H5Screate_simple",
                    [&] { return H5Screate_simple(components == 1 ? 1 : 2, md, nullptr); }),
              H5Sclose);
    phase([&] {
        if (injected("dataset_setup", rank))
            throw std::bad_alloc();
        if (!count) {
            check(hcall(false, rank, "H5Sselect_none", [&] { return H5Sselect_none(space); }));
            check(hcall(false, rank, "H5Sselect_none", [&] { return H5Sselect_none(memory); }));
        } else {
            hsize_t start[2] = {static_cast<hsize_t>(offsets[rank]) / components, 0},
                    extent[2] = {static_cast<hsize_t>(count) / components, components};
            check(hcall(false, rank, "H5Sselect_hyperslab", [&] {
                return H5Sselect_hyperslab(space, H5S_SELECT_SET, start, nullptr, extent, nullptr);
            }));
        }
    });
    H5 transfer(hcall(parallel, rank, "H5Pcreate", [&] { return H5Pcreate(H5P_DATASET_XFER); }),
                H5Pclose);
#if defined(H5_HAVE_PARALLEL) && defined(CHIPCAE_ENABLE_MPI)
    check(hcall(parallel, rank, "H5Pset_dxpl_mpio", [&] {
        return H5Pset_dxpl_mpio(transfer, H5FD_MPIO_COLLECTIVE);
    }));
#endif
    check(hcall(parallel, rank, "H5Dwrite", [&] {
        return H5Dwrite(set, native<T>(), memory, space, transfer, local.data());
    }));
    check(hcall(parallel, rank, "H5Dclose", [&] {
        auto status = H5Dclose(set);
        if (status >= 0)
            set.id = -1;
        return status;
    }));
    set.id = -1;
}
#endif
} // namespace
ResultsWriter::ResultsWriter(std::filesystem::path run,
                             const std::filesystem::path& stackup,
                             std::vector<std::string> layer_names,
                             std::vector<std::string> material_names)
    : run_(std::move(run)), layer_names_(std::move(layer_names)),
      material_names_(std::move(material_names)) {
#ifdef CHIPCAE_ENABLE_MPI
    int initialized = 0;
    MPI_Initialized(&initialized);
    if (initialized) {
        MPI_Comm_rank(MPI_COMM_WORLD, &rank_);
        MPI_Comm_size(MPI_COMM_WORLD, &size_);
    }
#endif
    phase([&] {
        for (const auto* names : {&layer_names_, &material_names_})
            for (const auto& name : *names) {
                quoted(name);
                if (name.find('\0') != std::string::npos)
                    throw std::invalid_argument("NUL in name");
            }
    });
#ifdef CHIPCAE_ENABLE_MPI
    if (size_ > 1) {
        for (const auto* names : {&layer_names_, &material_names_}) {
            std::string encoded;
            for (const auto& name : *names)
                encoded += quoted(name) + ",";
            std::string reference = encoded;
            int length = static_cast<int>(reference.size());
            MPI_Bcast(&length, 1, MPI_INT, 0, MPI_COMM_WORLD);
            phase([&] { reference.resize(static_cast<std::size_t>(length)); });
            MPI_Bcast(reference.data(), length, MPI_CHAR, 0, MPI_COMM_WORLD);
            agree(reference == encoded);
        }
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
                 empty(local.embedded_boundary) && empty(local.particles);
    for (auto id : p.hex)
        valid = valid && id >= 0 && static_cast<std::size_t>(id) < np;
    for (auto kind : p.cell_kind)
        valid = valid && kind >= 0 && kind <= 2;
    valid = valid && p.connectivity.empty() && p.offsets == std::vector<Index>{0} &&
            p.face_connectivity.empty() && p.face_offsets == std::vector<Index>{0} &&
            p.polyhedron_to_faces.empty() && p.polyhedron_offsets == std::vector<Index>{0};
    const auto& cut = local.cut_poly;
    auto offsets_valid = [](const std::vector<Index>& off, std::size_t length) {
        return !off.empty() && off.front() == 0 && off.back() == static_cast<Index>(length) &&
               std::is_sorted(off.begin(), off.end());
    };
    valid = valid && cut.hex.empty() && cut.points.size() % 3 == 0 &&
            offsets_valid(cut.offsets, cut.connectivity.size()) &&
            offsets_valid(cut.face_offsets, cut.face_connectivity.size()) &&
            offsets_valid(cut.polyhedron_offsets, cut.polyhedron_to_faces.size()) &&
            cut.polyhedron_offsets.size() == cut.offsets.size();
    agree(valid);
    auto cnp = cut.points.size() / 3, cnc = cut.offsets.size() - 1;
    valid = valid && cut.T.size() == cnp && cut.U.size() == 3 * cnp && cut.layer_id.size() == cnc &&
            cut.material_id.size() == cnc && cut.cell_kind.size() == cnc &&
            (cut.elem_gid.empty() || cut.elem_gid.size() == cnc);
    for (auto id : cut.connectivity)
        valid = valid && id >= 0 && static_cast<std::size_t>(id) < cnp;
    for (auto id : cut.face_connectivity)
        valid = valid && id >= 0 && static_cast<std::size_t>(id) < cnp;
    std::vector<int> owners;
    phase([&] { owners.resize(cut.face_offsets.size() - 1, 0); });
    for (auto id : cut.polyhedron_to_faces) {
        bool in_range = id >= 0 && static_cast<std::size_t>(id) < owners.size();
        valid = valid && in_range;
        if (in_range)
            valid = valid && ++owners[static_cast<std::size_t>(id)] == 1;
    }
    for (auto count : owners)
        valid = valid && count == 1;
    for (std::size_t i = 1; i < cut.face_offsets.size(); ++i)
        valid = valid && cut.face_offsets[i] - cut.face_offsets[i - 1] >= 3;
    agree(valid);
    for (std::size_t cell = 0; cell < cnc; ++cell) {
        valid = valid && cut.offsets[cell + 1] - cut.offsets[cell] >= 4 &&
                cut.polyhedron_offsets[cell + 1] - cut.polyhedron_offsets[cell] >= 4;
        auto begin = cut.connectivity.begin() + cut.offsets[cell];
        auto end = cut.connectivity.begin() + cut.offsets[cell + 1];
        for (Index f = cut.polyhedron_offsets[cell]; f < cut.polyhedron_offsets[cell + 1]; ++f) {
            auto face =
                static_cast<std::size_t>(cut.polyhedron_to_faces[static_cast<std::size_t>(f)]);
            for (Index v = cut.face_offsets[face]; v < cut.face_offsets[face + 1]; ++v)
                valid = valid &&
                        std::find(begin, end, cut.face_connectivity[static_cast<std::size_t>(v)]) !=
                            end;
        }
    }
    for (const auto* q : {&p, &cut}) {
        for (auto id : q->layer_id)
            valid = valid && id >= 0 &&
                    (layer_names_.empty() ||
                     (id > 0 && static_cast<std::size_t>(id) <= layer_names_.size()));
        for (auto id : q->material_id)
            valid = valid && id >= 0 &&
                    (material_names_.empty() ||
                     (id > 0 && static_cast<std::size_t>(id) <= material_names_.size()));
        for (auto kind : q->cell_kind)
            valid = valid && kind >= 0 && kind <= 2;
    }
    for (auto& s : steps_)
        valid = valid && s.n != n;
    agree(valid);
    int gid_present = !p.elem_gid.empty() || !cut.elem_gid.empty(), gid_any = gid_present;
#ifdef CHIPCAE_ENABLE_MPI
    if (size_ > 1)
        MPI_Allreduce(&gid_present, &gid_any, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
#endif
    agree(!gid_any || ((nc == 0 || !p.elem_gid.empty()) && (cnc == 0 || !cut.elem_gid.empty())));
    agree(steps_.empty() || gids_ == (gid_any != 0));
    bool parallel = false;
#if defined(H5_HAVE_PARALLEL) && defined(CHIPCAE_ENABLE_MPI)
    parallel = size_ > 1 && !std::getenv("CHIPCAE_RESULTS_SERIAL");
#endif
    std::string filename;
    std::filesystem::path tmp;
    phase([&] {
        filename = "out/step-" + std::to_string(n) + ".vtkhdf";
        tmp = run_ / (filename + ".tmp");
        steps_.reserve(steps_.size() + 1);
    });
    bool ok = true;
    H5 file_guard(H5P_DEFAULT, H5Fclose);
    file_guard.id = -1;
    try {
        H5 access(hcall(true, rank_, "H5Pcreate", [&] { return H5Pcreate(H5P_FILE_ACCESS); }),
                  H5Pclose);
        check(hcall(true, rank_, "H5Pset_fclose_degree", [&] {
            return H5Pset_fclose_degree(access, H5F_CLOSE_STRONG);
        }));

#if defined(H5_HAVE_PARALLEL) && defined(CHIPCAE_ENABLE_MPI)
        if (parallel)
            check(hcall(true, rank_, "H5Pset_fapl_mpio", [&] {
                return H5Pset_fapl_mpio(access, MPI_COMM_WORLD, MPI_INFO_NULL);
            }));
#endif
        H5 gcpl(hcall(true, rank_, "H5Pcreate", [&] { return H5Pcreate(H5P_GROUP_CREATE); }),
                H5Pclose);
        check(hcall(true, rank_, "H5Pset_link_creation_order", [&] {
            return H5Pset_link_creation_order(gcpl, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED);
        }));
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
            file = hcall(parallel, rank_, "H5Fcreate", [&] {
                return H5Fcreate(tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, access);
            });
            if (file < 0)
                throw std::runtime_error("create results file");
            file_guard.id = file;
            root = hcall(parallel, rank_, "H5Gcreate2", [&] {
                return H5Gcreate2(file, "VTKHDF", H5P_DEFAULT, gcpl, H5P_DEFAULT);
            });
            results_header(root, "PartitionedDataSetCollection", 2, 8, parallel, rank_);
            assembly = hcall(parallel, rank_, "H5Gcreate2", [&] {
                return H5Gcreate2(root, "Assembly", H5P_DEFAULT, gcpl, H5P_DEFAULT);
            });
            if (root < 0 || assembly < 0)
                throw std::runtime_error("create root groups");
        });
        const char* names[] = {"hex", "cut", "eb_surface", "particles"};
        const char* paths[] = {
            "/VTKHDF/hex", "/VTKHDF/cut", "/VTKHDF/eb_surface", "/VTKHDF/particles"};
        for (int block = 0; block < 4; ++block) {
            hid_t g = -1;
            metadata([&] {
                g = hcall(parallel, rank_, "H5Gcreate2", [&] {
                    return H5Gcreate2(root, names[block], H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
                });
                results_header(
                    g, block >= 2 ? "PolyData" : "UnstructuredGrid", 2, 8, parallel, rank_);
                H5 scalar(
                    hcall(parallel, rank_, "H5Screate", [&] { return H5Screate(H5S_SCALAR); }),
                    H5Sclose);
                H5 index(
                    hcall(parallel,
                          rank_,
                          "H5Acreate2",
                          [&] {
                              return H5Acreate2(
                                  g, "Index", H5T_NATIVE_INT64, scalar, H5P_DEFAULT, H5P_DEFAULT);
                          }),
                    H5Aclose);
                Index block_index = block;
                check(hcall(parallel, rank_, "H5Awrite", [&] {
                    return H5Awrite(index, H5T_NATIVE_INT64, &block_index);
                }));

                check(hcall(parallel, rank_, "H5Lcreate_soft", [&] {
                    const char* parent = block < 2    ? "volume"
                                         : block == 2 ? "boundary"
                                                      : "lagrangian";
                    if (block != 1) {
                        H5 subgroup(H5Gcreate2(assembly, parent, H5P_DEFAULT, gcpl, H5P_DEFAULT),
                                    H5Gclose);
                    }
                    std::string link = std::string(parent) + "/" + names[block];
                    return H5Lcreate_soft(
                        paths[block], assembly, link.c_str(), H5P_DEFAULT, H5P_DEFAULT);
                }));
            });
            ResultsPartition none;
            const auto& q = block == 0 ? p : block == 1 ? cut : none;
            auto emit = [&]<class T>(hid_t target,
                                     const char* name,
                                     const std::vector<T>& v,
                                     hsize_t c = 1) {
                distributed(target, name, v, rank_, size_, parallel, c);
            };
            std::vector<Index> point_counts;
            phase([&] { point_counts = {static_cast<Index>(q.points.size() / 3)}; });
            emit(g, "NumberOfPoints", point_counts);
            emit(g, "Points", q.points, 3);
            auto topology = [&](hid_t target, const std::vector<Index>& conn, int stride) {
                Index cells = static_cast<Index>(conn.size()) / stride;
                std::vector<Index> cell_counts, connectivity_counts, off;
                phase([&] {
                    cell_counts = {cells};
                    connectivity_counts = {static_cast<Index>(conn.size())};
                    off.resize(static_cast<std::size_t>(cells + 1));
                    for (Index i = 0; i <= cells; ++i)
                        off[static_cast<std::size_t>(i)] = i * stride;
                });
                emit(target, "NumberOfCells", cell_counts);
                emit(target, "NumberOfConnectivityIds", connectivity_counts);
                emit(target, "Connectivity", conn);
                emit(target, "Offsets", off);
            };
            if (block < 2) {
                if (block == 0)
                    topology(g, q.hex, 8);
                else {
                    emit(g, "NumberOfCells", std::vector<Index>{static_cast<Index>(cnc)});
                    emit(g,
                         "NumberOfConnectivityIds",
                         std::vector<Index>{static_cast<Index>(q.connectivity.size())});
                    emit(g, "Connectivity", q.connectivity);
                    emit(g, "Offsets", q.offsets);
                    emit(g, "FaceConnectivity", q.face_connectivity);
                    emit(g, "FaceOffsets", q.face_offsets);
                    emit(g, "PolyhedronToFaces", q.polyhedron_to_faces);
                    emit(g, "PolyhedronOffsets", q.polyhedron_offsets);
                    emit(g,
                         "NumberOfFaces",
                         std::vector<Index>{static_cast<Index>(q.face_offsets.size() - 1)});
                    emit(g,
                         "NumberOfFaceConnectivityIds",
                         std::vector<Index>{static_cast<Index>(q.face_connectivity.size())});
                    emit(g,
                         "NumberOfPolyhedronToFaceIds",
                         std::vector<Index>{static_cast<Index>(q.polyhedron_to_faces.size())});
                }
                std::vector<unsigned char> types;
                phase([&] {
                    types.assign(block == 0 ? q.hex.size() / 8 : cnc, block == 0 ? 12 : 42);
                });
                emit(g, "Types", types);
            } else
                for (auto name : {"Vertices", "Lines", "Polygons", "Strips"}) {
                    hid_t t = -1;
                    metadata([&] {
                        t = hcall(parallel, rank_, "H5Gcreate2", [&] {
                            return H5Gcreate2(g, name, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
                        });
                        if (t < 0)
                            throw std::runtime_error("create topology");
                    });
                    topology(t, {}, 1);
                    if (t >= 0)
                        check(hcall(parallel, rank_, "H5Gclose", [&] { return H5Gclose(t); }));
                }
            {
                hid_t pd = -1, cd = -1;
                metadata([&] {
                    pd = hcall(parallel, rank_, "H5Gcreate2", [&] {
                        return H5Gcreate2(g, "PointData", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
                    });
                    cd = hcall(parallel, rank_, "H5Gcreate2", [&] {
                        return H5Gcreate2(g, "CellData", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
                    });
                    if (pd < 0 || cd < 0)
                        throw std::runtime_error("create fields");
                });
                emit(pd, "T", q.T);
                emit(pd, "U", q.U, 3);
                emit(cd, "layer_id", q.layer_id);
                emit(cd, "material_id", q.material_id);
                emit(cd, "cell_kind", q.cell_kind);
                if (gid_any)
                    emit(cd, "elem_gid", q.elem_gid);
                if (pd >= 0) {
                    check(hcall(parallel, rank_, "H5Gclose", [&] { return H5Gclose(pd); }));
                    check(hcall(parallel, rank_, "H5Gclose", [&] { return H5Gclose(cd); }));
                }
            }
            if (g >= 0)
                check(hcall(parallel, rank_, "H5Gclose", [&] { return H5Gclose(g); }));
        }
        if (file >= 0) {
            check(hcall(parallel, rank_, "H5Gclose", [&] { return H5Gclose(assembly); }));
            check(hcall(parallel, rank_, "H5Gclose", [&] { return H5Gclose(root); }));
            check(hcall(
                parallel, rank_, "H5Fflush", [&] { return H5Fflush(file, H5F_SCOPE_GLOBAL); }));
            check(hcall(parallel, rank_, "H5Fclose", [&] {
                auto status = H5Fclose(file);
                if (status >= 0)
                    file_guard.id = -1;
                return status;
            }));
            file_guard.id = -1;
        }
    } catch (...) {
        ok = false;
    }
    agree(ok);
    if (rank_ == 0)
        try {
            {
                H5 metadata_file(H5Fopen(tmp.c_str(), H5F_ACC_RDWR, H5P_DEFAULT), H5Fclose);
                for (auto name : {"hex", "cut", "eb_surface", "particles"}) {
                    std::string path = std::string("/VTKHDF/") + name + "/FieldData";
                    H5 fd(H5Gcreate2(
                              metadata_file, path.c_str(), H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
                          H5Gclose);
                    strings(fd, "layer_names", layer_names_);
                    strings(fd, "material_names", material_names_);
                }
                check(H5Fflush(metadata_file, H5F_SCOPE_GLOBAL));
                check(H5Fclose(metadata_file));
                metadata_file.id = -1;
            }
            std::filesystem::rename(tmp, run_ / filename);
        } catch (...) {
            ok = false;
        }
    agree(ok);
    gids_ = gid_any != 0;
    steps_.push_back({n, time, std::move(filename)});
#endif
}
void ResultsWriter::add_summary(
    std::string name, double value, std::string unit, int step, std::string method) {
    if (!std::isfinite(value))
        throw std::invalid_argument("nonfinite summary");
    quoted(name);
    quoted(unit);
    quoted(method);
    summary_.push_back({std::move(name), value, std::move(unit), step, std::move(method)});
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
            out << "],\n\"partitions\":{\"per_step\":[";
            for (std::size_t i = 0; i < steps_.size(); ++i)
                out << (i ? "," : "") << size_;
            out << "],\"solver_fixed_per_run\":true},\n\"fields\":[{\"name\":\"T\",\"location\":"
                   "\"point\",\"components\":1,"
                   "\"unit\":\"K\",\"dtype\":\"float64\"},{\"name\":\"U\",\"location\":\"point\","
                   "\"components\":3,"
                   "\"unit\":\"m\",\"dtype\":\"float64\"}";
            for (auto name : {"layer_id", "material_id", "cell_kind"})
                out << ",{\"name\":" << quoted(name)
                    << ",\"location\":\"cell\",\"components\":1,\"unit\":\"1\",\"dtype\":"
                       "\"int32\"}";
            if (gids_)
                out << ",{\"name\":\"elem_gid\",\"location\":\"cell\",\"components\":1,\"unit\":"
                       "\"1\",\"dtype\":\"int64\"}";
            out << ",{\"name\":\"sigma_vm\",\"location\":\"cell\",\"components\":1,\"unit\":\"Pa\","
                   "\"dtype\":\"float64\",\"reserved\":true}],\n\"blocks\":[{\"index\":0,\"name\":"
                   "\"hex\",\"kind\":"
                   "\"UnstructuredGrid\"},{\"index\":1,\"name\":\"cut\",\"kind\":"
                   "\"UnstructuredGrid\"},{\"index\":2,\"name\":\"eb_surface\",\"kind\":"
                   "\"PolyData\"},{\"index\":3,\"name\":\"particles\",\"kind\":\"PolyData\"}],"
                   "\n\"summary\":[";
            first = true;
            for (auto& s : summary_) {
                if (!first)
                    out << ',';
                first = false;
                out << "{\"name\":" << quoted(s.name) << ",\"value\":" << s.value
                    << ",\"unit\":" << quoted(s.unit) << ",\"step\":" << s.step;
                if (!s.method.empty())
                    out << ",\"method\":" << quoted(s.method);
                out << '}';
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
