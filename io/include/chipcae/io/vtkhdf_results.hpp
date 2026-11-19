#pragma once
#include "chipcae/core/types.hpp"

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
    // Test-first interface stubs: population, validation and writing are not implemented yet.
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
                  std::vector<std::string> fields = {
                      "T", "U", "layer_id", "material_id", "cell_kind", "elem_gid", "volume"});
    void set_mesh(MeshSummary mesh);
    void write_step(int n, double time, const ResultsStep& local);
    void add_summary(
        std::string name, double value, std::string unit, int step, std::string method = {});
    void finalize();

private:
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
    std::vector<std::string> fields_;
    bool selected(const std::string& name) const;
    std::optional<MeshSummary> mesh_;
};
} // namespace chipcae::io
