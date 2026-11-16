#include "chipcae/solvers/particles/particle_solver.hpp"

#include "chipcae/core/log.hpp"

namespace chipcae::solvers::particles {

ParticleSolver::ParticleSolver(Method method, std::size_t particle_count)
    : method_(method), particle_count_(particle_count) {}

SolveStatus ParticleSolver::solve() {
    log(LogLevel::info, "Particle methods are reserved and not implemented in the MVP");
    return SolveStatus::not_implemented;
}

} // namespace chipcae::solvers::particles
