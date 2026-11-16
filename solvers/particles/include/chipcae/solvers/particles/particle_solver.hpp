#pragma once

#include "chipcae/core/types.hpp"

#include <cstddef>

namespace chipcae::solvers::particles {

enum class Method { mpm, sph };
enum class SolveStatus { ok, not_implemented };

/// Reserved interface for MPM/SPH. The implementation is intentionally deferred.
class ParticleSolver {
public:
    ParticleSolver(Method method, std::size_t particle_count);

    Method method() const noexcept { return method_; }
    std::size_t particle_count() const noexcept { return particle_count_; }
    SolveStatus solve();

private:
    Method method_{Method::mpm};
    std::size_t particle_count_{0};
};

} // namespace chipcae::solvers::particles
