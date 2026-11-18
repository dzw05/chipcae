#include "chipcae/coupling/residual_decimator.hpp"

namespace chipcae::coupling {
// Inert test-first stubs: supply symbols only, leaving acceptance assertions red.
ResidualDecimator::ResidualDecimator(ResidualBatchSink, ResidualDecimatorOptions) {}
void ResidualDecimator::on_iteration(const ResidualRecord&) {}
void ResidualDecimator::on_special(SpecialPoint) {}
void ResidualDecimator::on_window_end(const WindowSummary&) {}
std::uint64_t ResidualDecimator::batches_emitted() const noexcept {
    return 0;
}
} // namespace chipcae::coupling
