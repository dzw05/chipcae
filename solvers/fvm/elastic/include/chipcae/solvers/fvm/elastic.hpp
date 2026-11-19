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
// SI units: lengths/displacements m, areas m^2, volumes m^3, C Pa, alpha 1/K,
// dT K. Face and cell indices are positions in the caller's arrays, never gids.
struct ElasticFace {
    // hi=-1 denotes a boundary; the unit normal points outward from lo.
    std::int64_t lo = -1, hi = -1;
    int side = -1;
    double area = 0;
    std::array<double, 3> normal{}, centroid{};
};
// Globally replicated on every communicator rank. gids must be unique; rank[c]
// owns the three cell equations. Caller order and numbering are preserved.
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
    // Row-major SPD stiffness acting on engineering strains
    // (xx,yy,zz,yz,xz,xy); shear strains are twice tensor shear.
    std::vector<std::array<double, 36>> C;
    // Thermal eigenstrain: (alpha_x,alpha_y,alpha_z,0,0,0)*dT.
    std::vector<std::array<double, 3>> alpha;
    std::vector<double> dT;
    enum class Support { Rigid321, Fixed } support = Support::Rigid321;
    // Fixed targets expand to zero displacement on nonembedded exterior faces.
    // Every target must match. Rigid321 prohibits all displacement targets.
    std::vector<int> fixed_side;
    std::vector<int> fixed_layer;
    struct PrescribedFace {
        std::int64_t face = -1;
        std::array<double, 3> U{};
    };
    // Full-vector values at exterior face centroids. Equal duplicates merge;
    // conflicting duplicates (including expanded zero targets) are errors.
    std::vector<PrescribedFace> prescribed;
    std::string preconditioner = "jacobi";
    // Relative force tolerance, subject to the documented double-precision
    // backward-error floor for ill-conditioned bodies.
    double rtol = 1e-8;
    int max_iter = 2000;
};
// Full, identical output on every rank. Cell/face arrays retain input order;
// top_samples retain qualifying face order. Empty top sets have zero warpage.
struct ElasticResult {
    std::vector<std::array<double, 3>> U_cell;
    std::vector<std::array<double, 3>> U_face;
    std::vector<char> face_prescribed;
    std::vector<std::array<double, 3>> top_samples;
    double warpage_pv = 0, U_mag_max = 0;
    // Total Krylov iterations and the recomputed unpreconditioned force residual.
    int iterations = 0;
    double residual = 0;
    std::string warpage_method;
    std::string U_mag_method;
};
// Caller initializes/finalizes MPI and PETSc and retains ownership of all inputs.
// Rigid321 solves without pins and removes arithmetic translation and rotation
// separately for each connected body (rank-reduced modes for degenerate centers).
// Invalid inputs return the existing Error carrier with INPUT_INVALID in reason,
// before KSPSolve; solver failures include reason, iterations and force residual.
Expected<ElasticResult> solve_elastic(const ElasticMesh&, const ElasticProblem&, MPI_Comm comm);
} // namespace chipcae::solvers::fvm
