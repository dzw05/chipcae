#pragma once

#include <chipcae/core/error_code.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace chipcae::coupling {
struct ThermalToMechanicInput {
    std::span<const double> T_cell;          // Cell temperatures, K; required in thermal mode.
    std::span<const double> volume;          // Positive cell volumes, m^3, in the same cell order.
    double T_ref = 0;                        // Finite reference temperature, K.
    std::string_view temperature_unit = "K"; // Accepts exactly "K" or "" (also K).
    std::optional<double> T_uniform;         // Uniform mode; requires empty T_cell.
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

// Maps shared control volumes in input order; does not interpolate between meshes.
// Validation failures return an error with empty dT and zero weighted averages.
ThermalToMechanicStatus map_thermal_to_mechanic(const ThermalToMechanicInput& in);
} // namespace chipcae::coupling
