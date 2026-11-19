#include "chipcae/solvers/fvm/elastic.hpp"

#include "elastic_stencil.hpp"

namespace chipcae::solvers::fvm {
// TEST-FIRST compile/link stubs only. No validation, assembly or solve is implemented.
Expected<ElasticResult> solve_elastic(const ElasticMesh&, const ElasticProblem&, MPI_Comm) {
    return chipcae::mesh::Error{"fvm.elastic",
                                "FVM elastic solver not implemented (test-first stub)"};
}
namespace detail {
Expected<ElasticAssembly> assemble_elastic(const ElasticMesh&, const ElasticProblem&, MPI_Comm) {
    return chipcae::mesh::Error{"fvm.elastic",
                                "FVM elastic assembly not implemented (test-first stub)"};
}
} // namespace detail
} // namespace chipcae::solvers::fvm
