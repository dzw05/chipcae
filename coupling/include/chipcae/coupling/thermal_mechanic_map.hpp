#pragma once

#include <chipcae/core/error_code.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace chipcae::coupling {
struct ThermalToMechanicInput {
    std::span<const double> T_cell;
    std::span<const double> volume;
    double T_ref = 0;
    std::string_view temperature_unit = "K";
    std::optional<double> T_uniform;
};

struct ThermalToMechanicResult {
    std::vector<double> dT;
    double volume_weighted_T = 0;
    double volume_weighted_dT = 0;
};

struct ThermalToMechanicStatus {
    ErrorCode code{};
    std::string message;
    ThermalToMechanicResult value;
};

ThermalToMechanicStatus map_thermal_to_mechanic(const ThermalToMechanicInput& in);
} // namespace chipcae::coupling
