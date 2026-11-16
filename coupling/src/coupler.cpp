#include "chipcae/coupling/coupler.hpp"

#include "chipcae/core/log.hpp"

namespace chipcae::coupling {

void Coupler::add_field(const Field* field) {
    if (field != nullptr) {
        fields_.push_back(field);
    }
}

void Coupler::exchange() {
    log(LogLevel::debug, "Coupler::exchange placeholder");
}

} // namespace chipcae::coupling
