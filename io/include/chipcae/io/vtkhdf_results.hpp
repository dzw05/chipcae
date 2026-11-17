#pragma once
#include "chipcae/core/types.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace chipcae::io {
// Coordinates and U are interleaved xyz; connectivity uses rank-local point IDs.
// The caller samples Q2 at the eight exact vertex nodes, without interpolation.
struct ResultsPartition {
    std::vector<double> points, T, U;
    std::vector<Index> hex, elem_gid; // elem_gid optional: dense Morton column * global nz + k
    // Cut block only: polyhedron-only connectivity and ncells+1 offsets.
    // Faces are independently owned, outward loops; face IDs are nonnegative.
    std::vector<Index> connectivity, offsets{0}, face_connectivity, face_offsets{0},
        polyhedron_to_faces, polyhedron_offsets{0};
    std::vector<std::int32_t> layer_id, material_id, cell_kind;
};
struct ResultsStep {
    ResultsPartition hex;
    // Surface and particle geometry remains reserved (empty only).
    ResultsPartition cut_poly, embedded_boundary, particles;
};
class ResultsWriter {
public:
    // Collective on MPI_COMM_WORLD when MPI is initialized. Partition count is
    // fixed for the lifetime of the writer. stackup is a chipcae.stack 0.2 JSON.
    ResultsWriter(std::filesystem::path run,
                  const std::filesystem::path& stackup,
                  std::vector<std::string> layer_names = {},
                  std::vector<std::string> material_names = {});
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
};
} // namespace chipcae::io
