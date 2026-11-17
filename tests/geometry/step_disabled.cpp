#include "chipcae/geometry/step_import.hpp"

#include <stdexcept>
#include <string>

int main() {
    if (chipcae::geometry::step_import_available())
        return 1;
    try {
        (void)chipcae::geometry::import_step("unavailable.step");
    } catch (const std::runtime_error& error) {
        return std::string(error.what()).find("CHIPCAE_ENABLE_OCCT=ON") == std::string::npos;
    }
    return 1;
}
