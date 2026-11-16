#include "chipcae/core/solver.hpp"

#include <stdexcept>

namespace chipcae {
void Solver::checkpoint(io::Writer&) const {
    throw std::logic_error("checkpoint unsupported");
}

void Solver::restart(io::Reader&) {
    throw std::logic_error("restart unsupported");
}
} // namespace chipcae
