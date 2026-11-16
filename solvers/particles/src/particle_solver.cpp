#include "chipcae/solvers/particles/particle_solver.hpp"

#include "chipcae/core/log.hpp"

namespace chipcae::solvers::particles {

ParticleSolver::ParticleSolver(Method method, std::size_t particle_count)
    : method_(method), particle_count_(particle_count) {}

StepResult ParticleSolver::solve() {
    log(LogLevel::info, "Particle methods are reserved and not implemented in the MVP");
    return {false, FailureReason::not_implemented, 0, std::nullopt};
}

} // namespace chipcae::solvers::particles
