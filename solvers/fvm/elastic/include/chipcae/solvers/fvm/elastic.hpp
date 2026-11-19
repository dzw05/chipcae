#pragma once
#include "chipcae/schema/stack.hpp"

#include <array>
#include <cstdint>
#include <mpi.h>
#include <string>
#include <vector>

namespace chipcae::solvers::fvm {
// Reuse the repository's existing error carrier; no new error code is introduced.
using chipcae::mesh::Expected;
struct ElasticFace {
    std::int64_t lo = -1, hi = -1;
    int side = -1;
    double area = 0;
    std::array<double, 3> normal{}, centroid{};
};
struct ElasticMesh {
    std::vector<double> volume;
    std::vector<std::array<double, 3>> centroid;
    std::vector<std::int64_t> gid;
    std::vector<int> layer, rank;
    std::vector<ElasticFace> faces;
    std::vector<char> is_embedded;
    double z_top = 0;
};
struct ElasticProblem {
    std::vector<std::array<double, 36>> C;
    std::vector<std::array<double, 3>> alpha;
    std::vector<double> dT;
    enum class Support { Rigid321, Fixed } support = Support::Rigid321;
    std::vector<int> fixed_side;
    std::vector<int> fixed_layer;
    struct PrescribedFace {
        std::int64_t face = -1;
        std::array<double, 3> U{};
    };
    std::vector<PrescribedFace> prescribed;
    std::string preconditioner = "jacobi";
    double rtol = 1e-8;
    int max_iter = 2000;
};
struct ElasticResult {
    std::vector<std::array<double, 3>> U_cell;
    std::vector<std::array<double, 3>> U_face;
    std::vector<char> face_prescribed;
    std::vector<std::array<double, 3>> top_samples;
    double warpage_pv = 0, U_mag_max = 0;
    int iterations = 0;
    double residual = 0;
    std::string warpage_method;
    std::string U_mag_method;
};
Expected<ElasticResult> solve_elastic(const ElasticMesh&, const ElasticProblem&, MPI_Comm comm);
} // namespace chipcae::solvers::fvm
