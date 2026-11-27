#pragma once
#include "chipcae/solvers/fvm/elastic.hpp"

#include <map>

namespace chipcae::solvers::fvm::detail {
// Private assembly seam for the planned solver and its numerical invariants.
// Indices refer to caller cell order. Blocks include the full implicit stencil.
struct ElasticBlock {
    std::int64_t row = -1, column = -1;
    std::array<double, 9> value{};
};
struct ElasticFaceBlock {
    std::int64_t column = -1;
    std::array<double, 9> value{};
};
struct ElasticFaceStencil {
    std::vector<ElasticFaceBlock> blocks;
    std::array<double, 3> constant{};
};
struct ElasticAssembly {
    std::vector<ElasticBlock> blocks;
    std::vector<std::array<double, 3>> rhs;
    // Integrated energy-conjugate numerical traction outward from lo, including thermal stress.
    // Internal faces enter lo and hi equations with opposite signs.
    std::vector<ElasticFaceStencil> faces;
};
// G(i,j) is dU_i/dx_j. Each block maps three cell unknowns to nine gradients.
using ElasticVector = std::array<double, 3>;
using ElasticMatrix = std::array<double, 9>;
struct ElasticGradient {
    std::map<std::int64_t, std::array<double, 27>> blocks;
    std::map<std::int64_t, std::array<double, 27>> boundary;
    std::array<double, 9> constant{};
};
struct ElasticBoundary {
    std::vector<char> prescribed;
    std::vector<ElasticVector> value;
};
Expected<ElasticAssembly> assemble_elastic(const ElasticMesh&, const ElasticProblem&, MPI_Comm);
struct ElasticGeometry {
    double min_dn_ratio = 1;
    std::int64_t min_dn_gid = -1;
    double max_identity_defect = 0;
};
// Evaluate assembly's gradient maps for any supplied cell field; no solve is required.
// Uncut cells use same-region LS; cut gradients satisfy the implicit face-value GG equations.
Expected<std::vector<ElasticMatrix>> evaluate_cut_gradients(const ElasticMesh&,
                                                            const ElasticProblem&,
                                                            const std::vector<ElasticVector>&,
                                                            MPI_Comm);
// Face-local diagnostic, evaluated from the same eliminated maps as assembly.
// Gradients use the common K-weighted tangent and signed half-face normal derivative.
struct ElasticInterfaceSample {
    std::size_t face = 0;
    ElasticVector displacement{};
    std::array<ElasticMatrix, 2> gradient{};
    std::array<ElasticVector, 2> traction{}; // Both use the lo-to-hi normal.
};
Expected<std::vector<ElasticInterfaceSample>> evaluate_elastic_interfaces(
    const ElasticMesh&, const ElasticProblem&, const std::vector<ElasticVector>&, MPI_Comm);
// Boundary value is supplied explicitly. Internal faces ignore this argument.
Expected<ElasticMatrix> evaluate_cut_face_gradient(const ElasticMesh&,
                                                   std::size_t,
                                                   const std::vector<ElasticVector>&,
                                                   const std::vector<ElasticMatrix>&,
                                                   const ElasticVector&);
} // namespace chipcae::solvers::fvm::detail
