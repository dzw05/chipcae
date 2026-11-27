#include "chipcae/io/vtkhdf_results.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <stdexcept>
#include <sys/resource.h>
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
           p.material_id.empty() && p.cell_kind.empty() && p.elem_gid.empty() && p.volume.empty() &&
           p.copper_fraction.empty() && p.conductivity_diag.empty() && p.connectivity.empty() &&
           p.face_conn.empty() && p.poly_faces.empty() && p.offsets == std::vector<Index>{0} &&
           p.face_off == std::vector<Index>{0} && p.poly_off == std::vector<Index>{0};
}
#ifdef CHIPCAE_ENABLE_HDF5
using namespace chipcae::hdf_detail;
// VTK 9.7 reads variable-length ASCII string datasets as vtkStringArray.
// Write these in a serial metadata pass after all MPI file handles close.
void strings(hid_t parent, const char* name, const std::vector<std::string>& values, hid_t dcpl) {
    H5 type(H5Tcopy(H5T_C_S1), H5Tclose);
    check(H5Tset_size(type, H5T_VARIABLE));
    hsize_t count = values.size();
    H5 space(H5Screate_simple(1, &count, nullptr), H5Sclose);
    H5 set(H5Dcreate2(parent, name, type, space, H5P_DEFAULT, dcpl, H5P_DEFAULT), H5Dclose);
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
                 hid_t dcpl,
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
                dataset(group, name, gathered, WriterContext{}, components, dcpl);
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
                         group, name, native<T>(), space, H5P_DEFAULT, dcpl, H5P_DEFAULT);
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
bool ResultsWriter::selected(const std::string& name) const {
    return std::find(fields_.begin(), fields_.end(), name) != fields_.end();
}
void ResultsWriter::set_mesh(MeshSummary mesh) {
    mesh_ = std::move(mesh);
    finalize(); // Publish before any physical solve, with no steps yet.
}
ResultsWriter::ResultsWriter(std::filesystem::path run,
                             const std::filesystem::path& stackup,
                             std::vector<std::string> layer_names,
                             std::vector<std::string> material_names,
                             std::vector<std::string> fields)
    : run_(std::move(run)), layer_names_(std::move(layer_names)),
      material_names_(std::move(material_names)), fields_(std::move(fields)) {
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
    [[maybe_unused]] const auto start = std::chrono::steady_clock::now();
#ifndef CHIPCAE_ENABLE_HDF5
    (void)n;
    (void)time;
    (void)local;
    throw std::runtime_error("ResultsWriter requires CHIPCAE_ENABLE_HDF5");
#else
    agree(io_stats_.write_step_count < std::numeric_limits<int>::max());
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
    ResultsPartition p;
    phase([&] { p = local.hex; });
    const auto input_cells = p.hex.size() / 8;
    agree(p.hex.size() % 8 == 0 && p.points.size() % 3 == 0 && p.T.size() == p.points.size() / 3 &&
          p.U.size() == p.points.size() && p.layer_id.size() == input_cells &&
          p.material_id.size() == input_cells && p.cell_kind.size() == input_cells &&
          (p.elem_gid.empty() || p.elem_gid.size() == input_cells) &&
          (p.volume.empty() || p.volume.size() == input_cells));
    auto diagnostics_valid = [](const ResultsPartition& q, std::size_t cells) {
        bool valid = (q.copper_fraction.empty() || q.copper_fraction.size() == cells) &&
                     (q.conductivity_diag.empty() || q.conductivity_diag.size() == 3 * cells);
        for (double value : q.copper_fraction)
            valid &= std::isfinite(value);
        for (double value : q.conductivity_diag)
            valid &= std::isfinite(value);
        return valid;
    };
    // Validate before filtering, including values on cut parents discarded below.
    agree(diagnostics_valid(p, input_cells) &&
          diagnostics_valid(local.cut,
                            local.cut.poly_off.empty() ? 0 : local.cut.poly_off.size() - 1) &&
          diagnostics_valid(local.embedded_boundary,
                            local.embedded_boundary.connectivity.size() / 3));
    const std::array<const ResultsPartition*, 3> inputs{&p, &local.cut, &local.embedded_boundary};
    const std::array<std::size_t, 3> input_counts{
        input_cells,
        local.cut.poly_off.empty() ? 0 : local.cut.poly_off.size() - 1,
        local.embedded_boundary.connectivity.size() / 3};
    std::array<int, 3> input_schemas{};
    for (std::size_t b = 0; b < inputs.size(); ++b) {
        const auto& q = *inputs[b];
        int present = (!q.copper_fraction.empty() ? 1 : 0) | (!q.conductivity_diag.empty() ? 2 : 0);
        input_schemas[b] = present;
#ifdef CHIPCAE_ENABLE_MPI
        if (size_ > 1)
            MPI_Allreduce(&present, &input_schemas[b], 1, MPI_INT, MPI_BOR, MPI_COMM_WORLD);
#endif
        agree(input_counts[b] == 0 || present == input_schemas[b]);
    }
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
    phase([&] {
        if (!local.cut_as_hex) {
            ResultsPartition filtered = p;
            filtered.hex.clear();
            filtered.elem_gid.clear();
            filtered.layer_id.clear();
            filtered.material_id.clear();
            filtered.cell_kind.clear();
            filtered.volume.clear();
            filtered.copper_fraction.clear();
            filtered.conductivity_diag.clear();
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
                if (!p.volume.empty())
                    filtered.volume.push_back(p.volume[i]);
                if (!p.copper_fraction.empty())
                    filtered.copper_fraction.push_back(p.copper_fraction[i]);
                if (!p.conductivity_diag.empty())
                    filtered.conductivity_diag.insert(filtered.conductivity_diag.end(),
                                                      p.conductivity_diag.begin() + 3 * i,
                                                      p.conductivity_diag.begin() + 3 * i + 3);
            }
            p = std::move(filtered);
        }
    });
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
    phase([&] {
        if (cv) {
            for (std::size_t i = 0; i < cn; ++i) {
                std::set<Index> vertices(cut.connectivity.begin() + cut.offsets[i],
                                         cut.connectivity.begin() + cut.offsets[i + 1]);
                cv &= vertices.size() ==
                      static_cast<std::size_t>(cut.offsets[i + 1] - cut.offsets[i]);
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
    });
    for (auto v : cut.volume)
        cv &= std::isfinite(v) && v > 0;
    for (auto kind : cut.cell_kind)
        cv &= kind == 1 || kind == 2;
    int fields = (!cut.T.empty() ? 1 : 0) | (!cut.U.empty() ? 2 : 0);
    int schema = fields;
    int cut_present = cn != 0, cut_any = cut_present;
    int cut_points_any = !cut.points.empty();
    int fallback = local.cut_as_hex, fallback_max = fallback, fallback_min = fallback;
#ifdef CHIPCAE_ENABLE_MPI
    if (size_ > 1) {
        MPI_Allreduce(&cut_present, &cut_any, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
        MPI_Allreduce(&fields, &schema, 1, MPI_INT, MPI_BOR, MPI_COMM_WORLD);
        int cut_points = cut_points_any;
        MPI_Allreduce(&cut_points, &cut_points_any, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
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
                 empty(local.particles);
    for (auto id : p.hex)
        valid = valid && id >= 0 && static_cast<std::size_t>(id) < np;
    for (auto kind : p.cell_kind)
        valid = valid && kind >= 0 && kind <= 2;
    valid = valid && p.connectivity.empty() && p.offsets == std::vector<Index>{0} &&
            p.face_conn.empty() && p.face_off == std::vector<Index>{0} && p.poly_faces.empty() &&
            p.poly_off == std::vector<Index>{0};
    valid = valid && cut.hex.empty();
    std::vector<int> owners;
    phase([&] { owners.resize(cut.face_off.size() - 1, 0); });
    for (auto id : cut.poly_faces)
        valid = valid && ++owners[static_cast<std::size_t>(id)] == 1;
    for (auto count : owners)
        valid = valid && count == 1;
    for (const auto* q : {static_cast<const ResultsPartition*>(&p), &cut}) {
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
    int hex_volume_present = !p.volume.empty(), hex_volume_any = hex_volume_present;
#ifdef CHIPCAE_ENABLE_MPI
    if (size_ > 1) {
        MPI_Allreduce(&gid_present, &gid_any, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
        MPI_Allreduce(&hex_volume_present, &hex_volume_any, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
    }
#endif
    agree(!hex_volume_any || nc == 0 || !p.volume.empty());
    agree(!gid_any || ((nc == 0 || !p.elem_gid.empty()) && (cn == 0 || !cut.elem_gid.empty())));
    agree(steps_.empty() || gids_ == (gid_any != 0));
    const auto& eb = local.embedded_boundary;
    bool eb_valid =
        eb.face_conn.empty() && eb.hex.empty() && eb.poly_faces.empty() &&
        eb.face_off == std::vector<Index>{0} && eb.poly_off == std::vector<Index>{0} &&
        eb.offsets == std::vector<Index>{0} && eb.points.size() % 3 == 0 &&
        eb.connectivity.size() % 3 == 0 && eb.T.size() == eb.points.size() / 3 &&
        eb.U.size() == eb.points.size() && eb.elem_gid.size() == eb.connectivity.size() / 3 &&
        eb.layer_id.size() == eb.elem_gid.size() && eb.material_id.size() == eb.elem_gid.size() &&
        eb.cell_kind.size() == eb.elem_gid.size();
    for (auto v : eb.connectivity)
        eb_valid &= v >= 0 && static_cast<std::size_t>(v) < eb.points.size() / 3;
    agree(eb_valid);
    // All ranks must enter the same dataset collectives, including empty ranks.
    // A legacy block with no diagnostics anywhere emits neither diagnostic.
    const std::array<const ResultsPartition*, 4> blocks{&p, &cut, &eb, &local.particles};
    std::array<int, 4> diagnostic_schema{};
    std::array<int, 4> block_cells_any{};
    int emitted_diagnostics = 0;
    for (std::size_t b = 0; b < blocks.size(); ++b) {
        const auto& q = *blocks[b];
        const int any = b == 0   ? input_schemas[0]
                        : b == 1 ? (local.cut_as_hex ? 0 : input_schemas[1])
                        : b == 2 ? input_schemas[2]
                                 : 0;
        const auto cells = b == 0 ? nc : b == 1 ? cn : q.connectivity.size() / 3;
        int has_cells = cells != 0;
        block_cells_any[b] = has_cells;
#ifdef CHIPCAE_ENABLE_MPI
        if (size_ > 1)
            MPI_Allreduce(&has_cells, &block_cells_any[b], 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
#endif
        diagnostic_schema[b] = any;
        emitted_diagnostics |= any;
    }
    // Preserve empty block templates (including --cut-output hex), but never
    // create a conductivity dataset for a legacy run with no supplied values.
    for (std::size_t b = 0; b < blocks.size(); ++b)
        if (!block_cells_any[b])
            diagnostic_schema[b] = emitted_diagnostics;
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
        check(hcall(true, rank_, "H5Pset_obj_track_times", [&] {
            return H5Pset_obj_track_times(gcpl, 0);
        }));
        // Keep ordinary groups' default link-order properties.
        H5 untimed_gcpl(
            hcall(true, rank_, "H5Pcreate", [&] { return H5Pcreate(H5P_GROUP_CREATE); }), H5Pclose);
        check(hcall(true, rank_, "H5Pset_obj_track_times", [&] {
            return H5Pset_obj_track_times(untimed_gcpl, 0);
        }));
        H5 fcpl(hcall(true, rank_, "H5Pcreate", [&] { return H5Pcreate(H5P_FILE_CREATE); }),
                H5Pclose);
        check(hcall(true, rank_, "H5Pset_obj_track_times", [&] {
            return H5Pset_obj_track_times(fcpl, 0);
        }));
        H5 dcpl(hcall(true, rank_, "H5Pcreate", [&] { return H5Pcreate(H5P_DATASET_CREATE); }),
                H5Pclose);
        check(hcall(true, rank_, "H5Pset_obj_track_times", [&] {
            return H5Pset_obj_track_times(dcpl, 0);
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
                return H5Fcreate(tmp.c_str(), H5F_ACC_TRUNC, fcpl, access);
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
        for (int block = 0; block < 4; ++block) {
            hid_t g = -1;
            metadata([&] {
                g = hcall(parallel, rank_, "H5Gcreate2", [&] {
                    return H5Gcreate2(root, names[block], H5P_DEFAULT, untimed_gcpl, H5P_DEFAULT);
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

                const char* category = block < 2    ? "volume"
                                       : block == 2 ? "boundary"
                                                    : "lagrangian";
                if (hcall(parallel, rank_, "H5Lexists", [&] {
                        return H5Lexists(assembly, category, H5P_DEFAULT);
                    }) == 0) {
                    H5 subgroup(hcall(parallel,
                                      rank_,
                                      "H5Gcreate2",
                                      [&] {
                                          return H5Gcreate2(
                                              assembly, category, H5P_DEFAULT, gcpl, H5P_DEFAULT);
                                      }),
                                H5Gclose);
                }
                H5 subgroup(hcall(parallel,
                                  rank_,
                                  "H5Gopen2",
                                  [&] { return H5Gopen2(assembly, category, H5P_DEFAULT); }),
                            H5Gclose);
                check(hcall(parallel, rank_, "H5Lcreate_soft", [&] {
                    return H5Lcreate_soft((std::string("/VTKHDF/") + names[block]).c_str(),
                                          subgroup,
                                          names[block],
                                          H5P_DEFAULT,
                                          H5P_DEFAULT);
                }));
            });
            ResultsPartition none;
            const auto& q = block == 0   ? p
                            : block == 1 ? cut
                            : block == 2 ? local.embedded_boundary
                                         : none;
            auto emit = [&]<class T>(hid_t target,
                                     const char* name,
                                     const std::vector<T>& v,
                                     hsize_t c = 1) {
                distributed(target, name, v, rank_, size_, parallel, dcpl, c);
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
            auto emit_prepared = [&](hid_t target, const char* name, auto prepare) {
                decltype(prepare()) values;
                phase([&] { values = prepare(); });
                emit(target, name, values);
            };
            if (block < 2) {
                if (block == 0) {
                    topology(g, q.hex, 8);
                    emit_prepared(g, "Types", [&] {
                        return std::vector<unsigned char>(q.hex.size() / 8, 12);
                    });
                } else {
                    emit_prepared(g, "NumberOfCells", [&] {
                        return std::vector<Index>{static_cast<Index>(cn)};
                    });
                    emit_prepared(g, "NumberOfConnectivityIds", [&] {
                        return std::vector<Index>{static_cast<Index>(q.connectivity.size())};
                    });
                    emit(g, "Connectivity", q.connectivity);
                    emit(g, "Offsets", q.offsets);
                    emit_prepared(g, "Types", [&] { return std::vector<unsigned char>(cn, 42); });
                    emit(g, "FaceConnectivity", q.face_conn);
                    emit(g, "FaceOffsets", q.face_off);
                    emit(g, "PolyhedronToFaces", q.poly_faces);
                    emit(g, "PolyhedronOffsets", q.poly_off);
                    emit_prepared(g, "NumberOfFaces", [&] {
                        return std::vector<Index>{static_cast<Index>(q.face_off.size() - 1)};
                    });
                    emit_prepared(g, "NumberOfFaceConnectivityIds", [&] {
                        return std::vector<Index>{static_cast<Index>(q.face_conn.size())};
                    });
                    emit_prepared(g, "NumberOfPolyhedronToFaceIds", [&] {
                        return std::vector<Index>{static_cast<Index>(q.poly_faces.size())};
                    });
                }
            } else
                for (auto name : {"Vertices", "Lines", "Polygons", "Strips"}) {
                    hid_t t = -1;
                    metadata([&] {
                        t = hcall(parallel, rank_, "H5Gcreate2", [&] {
                            return H5Gcreate2(g, name, H5P_DEFAULT, untimed_gcpl, H5P_DEFAULT);
                        });
                        if (t < 0)
                            throw std::runtime_error("create topology");
                    });
                    if (block == 2 && std::string(name) == "Polygons")
                        topology(t, q.connectivity, 3);
                    else
                        topology(t, {}, 1);
                    if (t >= 0)
                        check(hcall(parallel, rank_, "H5Gclose", [&] { return H5Gclose(t); }));
                }
            {
                hid_t pd = -1, cd = -1;
                metadata([&] {
                    pd = hcall(parallel, rank_, "H5Gcreate2", [&] {
                        return H5Gcreate2(g, "PointData", H5P_DEFAULT, untimed_gcpl, H5P_DEFAULT);
                    });
                    cd = hcall(parallel, rank_, "H5Gcreate2", [&] {
                        return H5Gcreate2(g, "CellData", H5P_DEFAULT, untimed_gcpl, H5P_DEFAULT);
                    });
                    if (pd < 0 || cd < 0)
                        throw std::runtime_error("create fields");
                });
                if (selected("T") && (block != 1 || cut_points_any == 0 || (schema & 1)))
                    emit(pd, "T", q.T);
                if (selected("U") && (block != 1 || cut_points_any == 0 || (schema & 2)))
                    emit(pd, "U", q.U, 3);
                if (selected("layer_id") || block == 1)
                    emit(cd, "layer_id", q.layer_id);
                if (selected("material_id") || block == 1)
                    emit(cd, "material_id", q.material_id);
                if (selected("cell_kind") || block == 1)
                    emit(cd, "cell_kind", q.cell_kind);
                if ((gid_any && selected("elem_gid")) || block == 1)
                    emit(cd, "elem_gid", q.elem_gid);
                if (block == 0 && hex_volume_any)
                    emit(cd, "volume", q.volume);
                if (block == 1 && (selected("volume") || cut_any))
                    emit(cd, "volume", q.volume);
                if (selected("copper_fraction") && (diagnostic_schema[block] & 1))
                    emit(cd, "copper_fraction", q.copper_fraction);
                if (selected("conductivity_diag") && (diagnostic_schema[block] & 2))
                    emit(cd, "conductivity_diag", q.conductivity_diag, 3);
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
    std::uint64_t step_bytes = 0;
    if (rank_ == 0)
        try {
            {
                H5 metadata_file(H5Fopen(tmp.c_str(), H5F_ACC_RDWR, H5P_DEFAULT), H5Fclose);
                H5 gcpl(H5Pcreate(H5P_GROUP_CREATE), H5Pclose);
                check(H5Pset_obj_track_times(gcpl, 0));
                H5 dcpl(H5Pcreate(H5P_DATASET_CREATE), H5Pclose);
                check(H5Pset_obj_track_times(dcpl, 0));
                for (auto name : {"hex", "cut", "eb_surface", "particles"}) {
                    std::string path = std::string("/VTKHDF/") + name + "/FieldData";
                    H5 fd(H5Gcreate2(metadata_file, path.c_str(), H5P_DEFAULT, gcpl, H5P_DEFAULT),
                          H5Gclose);
                    strings(fd, "layer_names", layer_names_, dcpl);
                    strings(fd, "material_names", material_names_, dcpl);
                }
                check(H5Fflush(metadata_file, H5F_SCOPE_GLOBAL));
                check(H5Fclose(metadata_file));
                metadata_file.id = -1;
            }
            std::filesystem::rename(tmp, run_ / filename);
            const auto bytes = std::filesystem::file_size(run_ / filename);
            if (bytes > std::numeric_limits<std::uint64_t>::max() - step_bytes_ - manifest_bytes_)
                throw std::overflow_error("results step byte count overflow");
            step_bytes = static_cast<std::uint64_t>(bytes);
        } catch (...) {
            ok = false;
        }
    agree(ok);
#ifdef CHIPCAE_ENABLE_MPI
    if (size_ > 1)
        MPI_Bcast(&step_bytes, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);
#endif
    phase([&] { steps_.push_back({n, time, std::move(filename)}); });
    diagnostic_fields_ |= emitted_diagnostics;
    gids_ = gid_any != 0;
    cut_fields_ = cut_fields_ || cut_any != 0;
    step_bytes_ += step_bytes;
    io_stats_.bytes_written = step_bytes_ + manifest_bytes_;
    const double duration =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    io_stats_.last_write_step_seconds = duration;
    io_stats_.write_step_seconds += duration;
    ++io_stats_.write_step_count;
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
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t manifest_bytes = 0;
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
            out << "],\"solver_fixed_per_run\":true},\n\"fields\":[";
            bool first_field = true;
            auto manifest_fields = fields_;
            // Required cut metadata survives physical-field selection.
            if (cut_fields_)
                for (const auto* name :
                     {"layer_id", "material_id", "cell_kind", "elem_gid", "volume"})
                    if (!selected(name))
                        manifest_fields.emplace_back(name);
            for (const auto& name : manifest_fields) {
                if ((name == "copper_fraction" && !(diagnostic_fields_ & 1)) ||
                    (name == "conductivity_diag" && !(diagnostic_fields_ & 2)))
                    continue;
                const bool diagnostic = name == "copper_fraction" || name == "conductivity_diag";
                // The default volume schema includes the empty cut template and its GIDs.
                if (name == "elem_gid" && !gids_ && !cut_fields_ && !selected("volume"))
                    continue;
                if (!first_field)
                    out << ',';
                first_field = false;
                out << "{\"name\":" << quoted(name)
                    << ",\"location\":" << quoted(name == "T" || name == "U" ? "point" : "cell")
                    << ",\"components\":" << (name == "U" || name == "conductivity_diag" ? 3 : 1)
                    << ",\"unit\":"
                    << quoted(diagnostic         ? ""
                              : name == "T"      ? "K"
                              : name == "U"      ? "m"
                              : name == "volume" ? "m3"
                                                 : "1")
                    << ",\"dtype\":"
                    << quoted(name == "T" || name == "U" || name == "volume" || diagnostic
                                  ? "float64"
                              : name == "elem_gid" ? "int64"
                                                   : "int32")
                    << '}';
            }
            if (!first_field)
                out << ',';
            out << "{\"name\":\"sigma_vm\",\"location\":\"cell\",\"components\":1,\"unit\":\"Pa\","
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
            out << "]";
            if (mesh_) {
                const auto& m = *mesh_;
                out << ",\n\"mesh\":{\"nx0\":" << m.nx0 << ",\"ny0\":" << m.ny0 << ",\"L\":" << m.L
                    << ",\"padded_cells\":" << m.padded_cells << ",\"void_cells\":" << m.void_cells
                    << ",\"nz\":" << m.nz << ",\"mesh_sha256\":" << quoted(m.mesh_sha256) << '}';
            }
            out << "\n}\n";
            out.close();
            std::filesystem::rename(tmp, run_ / "manifest.json");
            const auto bytes = std::filesystem::file_size(run_ / "manifest.json");
            if (bytes > std::numeric_limits<std::uint64_t>::max() - step_bytes_)
                throw std::overflow_error("results manifest byte count overflow");
            manifest_bytes = static_cast<std::uint64_t>(bytes);
        } catch (...) {
            ok = false;
        }
    agree(ok);
#ifdef CHIPCAE_ENABLE_MPI
    if (size_ > 1)
        MPI_Bcast(&manifest_bytes, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);
#endif
    std::uint64_t peak_rss_bytes = 0;
    phase([&] {
        struct rusage usage{};
        if (getrusage(RUSAGE_SELF, &usage) != 0)
            throw std::runtime_error("results getrusage failed");
#if defined(__APPLE__)
        constexpr std::uint64_t rss_scale = 1;
#else
        constexpr std::uint64_t rss_scale = 1024;
#endif
        if (usage.ru_maxrss < 0 || static_cast<std::uintmax_t>(usage.ru_maxrss) >
                                       std::numeric_limits<std::uint64_t>::max() / rss_scale)
            throw std::overflow_error("results peak RSS byte count overflow");
        peak_rss_bytes = static_cast<std::uint64_t>(usage.ru_maxrss) * rss_scale;
    });
    // Publish statistics only after every rank has completed successfully.
    manifest_bytes_ = manifest_bytes;
    io_stats_.bytes_written = step_bytes_ + manifest_bytes_;
    io_stats_.peak_rss_bytes = peak_rss_bytes;
    io_stats_.finalize_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}
} // namespace chipcae::io
