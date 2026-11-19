#pragma once
#include "chipcae/solvers/fvm/elastic.hpp"

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
    // Integrated physical traction outward from lo, including thermal stress.
    // Internal faces enter lo and hi equations with opposite signs.
    std::vector<ElasticFaceStencil> faces;
};
Expected<ElasticAssembly> assemble_elastic(const ElasticMesh&, const ElasticProblem&, MPI_Comm);
} // namespace chipcae::solvers::fvm::detail
