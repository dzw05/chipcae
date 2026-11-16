#include "chipcae/coupling/precice_adapter.hpp"

#include "chipcae/core/log.hpp"

#include <utility>

namespace chipcae::coupling {

PreciceAdapter::PreciceAdapter(std::string participant_name)
    : participant_name_(std::move(participant_name)) {}

void PreciceAdapter::initialize() {
    log(LogLevel::info, "preCICE adapter is not connected in this build");
}

void PreciceAdapter::finalize() {}

} // namespace chipcae::coupling
