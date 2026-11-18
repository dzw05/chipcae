#include "chipcae/coupling/run_control.hpp"

namespace chipcae::coupling {
// Minimum link stubs for the TEST-FIRST acceptance pass. No commands are consumed.
RunControl::RunControl(ControlChannel&,
                       ControlCapabilities,
                       std::function<void(io::EventPayload)>,
                       RunControlOptions) {}

BoundaryDecision RunControl::at_boundary(const WindowMark&) {
    return {};
}

bool RunControl::paused() const noexcept {
    return false;
}
} // namespace chipcae::coupling
