#include "chipcae/solvers/particles/particle_solver.hpp"

#include "chipcae/core/log.hpp"

namespace chipcae::solvers::particles {

ParticleSolver::ParticleSolver(Method method, std::size_t particle_count)
    : method_(method), particle_count_(particle_count) {}

StepResult ParticleSolver::advance(const LoadStep&) {
    log(LogLevel::info, "Particle methods are reserved and not implemented in the MVP");
    StepResult result;
    result.status = StepResult::Status::Failed;
    result.code = ErrorCode::NOT_IMPLEMENTED;
    return result;
}

} // namespace chipcae::solvers::particles
