#pragma once

#include "grid.hpp"

#include <mpi.h>
#include <optional>
#include <string>

namespace chipcae::solvers::fvm {
struct ThermalBc {
    enum class Kind { Dirichlet, Robin, Flux } kind;
    std::string target;
    double T = 0, h = 0, T_inf = 0, q = 0;
    // Proposed plan extension for MMS: when present, select just this public
    // face index within target. All original target-wide members are preserved.
    std::optional<std::size_t> face;
};
struct ThermalProblem {
    std::vector<std::array<double, 3>> k;
    std::vector<double> q_vol;
    std::vector<int> fixed_cv;
    std::vector<double> fixed_T;
    std::vector<ThermalBc> bcs;
    std::optional<double> T_ref;
    std::string preconditioner = "jacobi", stabilization = "diag";
    double rtol = 1e-8;
    int max_iter = 500;
};
struct ThermalResult {
    std::vector<double> T_cell, T_face, face_flux, T_vertex;
    int iterations = 0;
    double residual = 0;
    double heat_in = 0, heat_out = 0, imbalance = 0;
    double cell_balance_max = 0, face_jump_max = 0;
    double max_local_condition = 0;
    std::int64_t max_condition_gid = -1;
    std::int64_t gamma_floor_count = 0;
    std::vector<std::array<double, 3>> k_used;
};
Expected<ThermalResult>
solve_thermal(const FvGrid&, const mesh::HexMeshArrays&, const ThermalProblem&, MPI_Comm comm);
} // namespace chipcae::solvers::fvm
