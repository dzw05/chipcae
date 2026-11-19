#include <algorithm>
#include <chipcae/coupling/thermal_mechanic_map.hpp>
#include <cmath>
#include <utility>

namespace chipcae::coupling {
namespace {
// Store each operation separately as double, including products before sums.
// The volatile round trip prevents excess precision and fused multiply-add.
double rounded(double value) {
    volatile double stored = value;
    return stored;
}

ThermalToMechanicStatus failure(ErrorCode::Value code, const char* message) {
    return {code, message, {}};
}

ThermalToMechanicStatus invalid_arithmetic() {
    return failure(ErrorCode::INPUT_INVALID, "invalid temperature arithmetic");
}
} // namespace

ThermalToMechanicStatus map_thermal_to_mechanic(const ThermalToMechanicInput& in) {
    if (in.volume.empty()) {
        return failure(ErrorCode::INPUT_INVALID, "invalid volume");
    }
    double sum_volume = 0.0;
    for (const double volume : in.volume) {
        if (!std::isfinite(volume) || volume <= 0.0) {
            return failure(ErrorCode::INPUT_INVALID, "invalid volume");
        }
        sum_volume = rounded(sum_volume + volume);
        if (!std::isfinite(sum_volume)) {
            return failure(ErrorCode::INPUT_INVALID, "invalid volume");
        }
    }
    if (sum_volume <= 0.0) {
        return failure(ErrorCode::INPUT_INVALID, "invalid volume");
    }
    if (!std::isfinite(in.T_ref)) {
        return failure(ErrorCode::INPUT_INVALID, "invalid T_ref");
    }
    if (!in.temperature_unit.empty() && in.temperature_unit != "K") {
        return failure(ErrorCode::INPUT_INVALID, "expected unit K");
    }
    if (in.T_uniform) {
        if (!in.T_cell.empty()) {
            return failure(ErrorCode::INPUT_INVALID, "inconsistent temperature sources");
        }
        if (!std::isfinite(*in.T_uniform)) {
            return failure(ErrorCode::INPUT_INVALID, "invalid T_uniform");
        }
    } else {
        if (in.T_cell.empty()) {
            return failure(ErrorCode::INPUT_REFERENCE_MISSING, "missing T_cell");
        }
        if (in.T_cell.size() != in.volume.size()) {
            return failure(ErrorCode::INPUT_INVALID, "length mismatch");
        }
        for (const double temperature : in.T_cell) {
            if (!std::isfinite(temperature)) {
                return failure(ErrorCode::INPUT_INVALID, "nonfinite T_cell");
            }
        }
    }

    ThermalToMechanicResult result;
    result.dT.resize(in.volume.size());
    double sum_TV = 0.0;
    double sum_dTV = 0.0;
    for (std::size_t i = 0; i < in.volume.size(); ++i) {
        const double temperature = in.T_uniform ? *in.T_uniform : in.T_cell[i];
        result.dT[i] = rounded(temperature - in.T_ref);
        if (!std::isfinite(result.dT[i])) {
            return invalid_arithmetic();
        }
        const double TV = rounded(temperature * in.volume[i]);
        const double dTV = rounded(result.dT[i] * in.volume[i]);
        if (!std::isfinite(TV) || !std::isfinite(dTV)) {
            return invalid_arithmetic();
        }
        sum_TV = rounded(sum_TV + TV);
        sum_dTV = rounded(sum_dTV + dTV);
        if (!std::isfinite(sum_TV) || !std::isfinite(sum_dTV)) {
            return invalid_arithmetic();
        }
    }
    result.volume_weighted_T = rounded(sum_TV / sum_volume);
    result.volume_weighted_dT = rounded(sum_dTV / sum_volume);
    if (!std::isfinite(result.volume_weighted_T) || !std::isfinite(result.volume_weighted_dT)) {
        return invalid_arithmetic();
    }
    const double delta = rounded(result.volume_weighted_T - in.T_ref);
    if (!std::isfinite(delta)) {
        return invalid_arithmetic();
    }
    const double residual = rounded(result.volume_weighted_dT - delta);
    if (!std::isfinite(residual)) {
        return invalid_arithmetic();
    }
    const double tolerance = rounded(1e-14 * std::max(std::abs(delta), 1.0));
    if (!std::isfinite(tolerance) || std::abs(residual) > tolerance) {
        return invalid_arithmetic();
    }
    return {ErrorCode::NONE, {}, std::move(result)};
}
} // namespace chipcae::coupling
