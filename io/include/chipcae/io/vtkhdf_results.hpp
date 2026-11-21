#pragma once
#include "chipcae/core/types.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace chipcae::io {
// Coordinates and U are interleaved xyz; connectivity uses rank-local point IDs.
// The caller samples Q2 at the eight exact vertex nodes, without interpolation.
struct ResultsPartition {
    std::vector<double> points, T, U, volume;
    // Cut topology, all rank-local. Offsets include initial zero and terminal size.
    // Each cell lists its unique points and references its own outward face rings.
    std::vector<Index> connectivity, offsets{0}, face_conn, face_off{0}, poly_faces, poly_off{0};
    std::vector<Index> hex, elem_gid; // elem_gid optional: dense Morton column * global nz + k
    std::vector<std::int32_t> layer_id, material_id, cell_kind;
    // Optional cell diagnostics, scalar and interleaved xyz respectively.
    std::vector<double> copper_fraction;
    std::vector<double> conductivity_diag;
};
struct ResultsStep {
    ResultsPartition hex;
    // T/U optional on cut, schema must match across ranks with cut points.
    // elem_gid, metadata and volume are required per cut cell.
    ResultsPartition cut, embedded_boundary, particles;
    // Caller supplies the parent hex representation in hex for fallback.
    bool cut_as_hex = false;
};
class ResultsWriter {
public:
    // Successful-call wall times in seconds; statistics stay in memory.
    // Includes finalize() calls made internally by set_mesh().
    struct IoStats {
        double write_step_seconds = 0;
        double finalize_seconds = 0;
        double last_write_step_seconds = 0;
        // Whole-process peak RSS, not an incremental I/O allocation measurement.
        std::uint64_t peak_rss_bytes = 0;
        std::uint64_t bytes_written = 0;
        int write_step_count = 0;
    };
    // The reference observes subsequent calls and is valid only for this writer's lifetime.
    const IoStats& io_stats() const noexcept { return io_stats_; }

    // Collective on MPI_COMM_WORLD when MPI is initialized. Partition count is
    // fixed for the lifetime of the writer. stackup is a chipcae.stack 0.2 JSON.
    struct MeshSummary {
        std::int64_t nx0, ny0, L, padded_cells, nz;
        std::string mesh_sha256;
        std::int64_t void_cells = 0;
    };
    ResultsWriter(std::filesystem::path run,
                  const std::filesystem::path& stackup,
                  std::vector<std::string> layer_names = {},
                  std::vector<std::string> material_names = {},
                  std::vector<std::string> fields = {"T",
                                                     "U",
                                                     "layer_id",
                                                     "material_id",
                                                     "cell_kind",
                                                     "elem_gid",
                                                     "volume",
                                                     "copper_fraction",
                                                     "conductivity_diag"});
    void set_mesh(MeshSummary mesh);
    void write_step(int n, double time, const ResultsStep& local);
    void add_summary(
        std::string name, double value, std::string unit, int step, std::string method = {});
    void finalize();

private:
    IoStats io_stats_;
    std::uint64_t step_bytes_ = 0, manifest_bytes_ = 0;
    struct Step {
        int n;
        double time;
        std::string file;
    };
    struct Summary {
        std::string name;
        double value;
        std::string unit;
        int step;
        std::string method;
    };
    std::filesystem::path run_;
    std::vector<std::string> layer_names_, material_names_;
    std::vector<Step> steps_;
    std::vector<Summary> summary_;
    int rank_ = 0, size_ = 1;
    bool gids_ = false;
    bool cut_fields_ = false;
    int diagnostic_fields_ = 0;
    std::vector<std::string> fields_;
    bool selected(const std::string& name) const;
    std::optional<MeshSummary> mesh_;
};
} // namespace chipcae::io
