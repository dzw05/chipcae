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
    std::vector<std::int32_t> layer_id, material_id, cell_kind;
};
struct ResultsStep {
    ResultsPartition hex;
    // Reserved interfaces: nonempty blocks are rejected until geometry is agreed.
    ResultsPartition cut_poly, embedded_boundary, particles;
    // TODO: VTK_POLYHEDRON FaceConnectivity/FaceOffsets and
    // PolyhedronToFaces/PolyhedronOffsets; shared face -(n+1) convention
    // awaits mesh/geometry confirmation. Do not emit invented face arrays.
};
class ResultsWriter {
public:
    // Collective on MPI_COMM_WORLD when MPI is initialized. Partition count is
    // fixed for the lifetime of the writer. stackup is a chipcae.stack 0.2 JSON.
    ResultsWriter(std::filesystem::path run,
                  const std::filesystem::path& stackup,
                  std::vector<std::string> fields = {
                      "T", "U", "layer_id", "material_id", "cell_kind", "elem_gid"});
    void write_step(int n, double time, const ResultsStep& local);
    void add_summary(std::string name, double value, std::string unit, int step);
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
    };
    std::filesystem::path run_;
    std::vector<Step> steps_;
    std::vector<Summary> summary_;
    int rank_ = 0, size_ = 1;
    bool gids_ = false;
    std::vector<std::string> fields_;
    bool selected(const std::string& name) const;
};
} // namespace chipcae::io
