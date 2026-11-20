#pragma once

#include <algorithm>
#include <chipcae/coupling/thermal_mechanic_map.hpp>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ctmmap_test {
using chipcae::ErrorCode;
using chipcae::coupling::map_thermal_to_mechanic;
using chipcae::coupling::ThermalToMechanicInput;
using chipcae::coupling::ThermalToMechanicStatus;

class Failure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline void require(bool condition, std::string_view message) {
    if (!condition) {
        throw Failure(std::string(message));
    }
}

class Suite {
public:
    template <class Function>
    void run(std::string_view name, Function&& function) {
        ++cases_;
        try {
            std::forward<Function>(function)();
            std::cout << "PASS: " << name << '\n';
        } catch (const Failure& error) {
            ++failures_;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        } catch (const std::exception& error) {
            ++failures_;
            std::cerr << "FAIL: " << name << ": unexpected exception: " << error.what() << '\n';
        } catch (...) {
            ++failures_;
            std::cerr << "FAIL: " << name << ": unexpected non-standard exception\n";
        }
    }

    int finish() const {
        std::cout << cases_ << " cases, " << failures_ << " failures\n";
        return failures_ == 0 ? 0 : 1;
    }

private:
    std::size_t cases_ = 0;
    std::size_t failures_ = 0;
};

inline void require_code(const ThermalToMechanicStatus& status, ErrorCode::Value expected) {
    const ErrorCode code(expected);
    require(status.code.code() == code.code(),
            "status code: expected " + std::to_string(code.code()) + " (" +
                std::string(code.name()) + "), got " + std::to_string(status.code.code()) + " (" +
                std::string(status.code.name()) + ")");
    require(status.code.name() == code.name(), "status code name mismatch");
}

inline void expect_failure(const ThermalToMechanicInput& input,
                           ErrorCode::Value expected,
                           std::string_view message) {
    const auto status = map_thermal_to_mechanic(input);
    require_code(status, expected);
    require(!status.message.empty(), "failure message must not be empty");
    require(status.message.find(message) != std::string::npos,
            "failure message must contain: " + std::string(message));
    require(status.value.dT.empty(), "failure must not publish partial dT");
    require(status.value.volume_weighted_T == 0.0, "failure must clear weighted T");
    require(status.value.volume_weighted_dT == 0.0, "failure must clear weighted dT");
}

// Each contract operation rounds to double separately, including before an addition.
// Volatile storage prevents fused multiply-add and excess-precision reference results.
inline double rounded(double value) {
    volatile double stored = value;
    return stored;
}

inline double finite(double value) {
    require(std::isfinite(value), "reference arithmetic must remain finite for success fixture");
    return value;
}

inline ThermalToMechanicStatus expect_success(const ThermalToMechanicInput& input) {
    const auto status = map_thermal_to_mechanic(input);
    require_code(status, ErrorCode::NONE);
    require(status.message.empty(), "success message must be empty");
    require(status.value.dT.size() == input.volume.size(),
            "dT length: expected " + std::to_string(input.volume.size()) + ", got " +
                std::to_string(status.value.dT.size()));

    double sum_volume = 0.0;
    for (const double volume : input.volume) {
        require(std::isfinite(volume) && volume > 0.0, "success fixture needs positive volumes");
        sum_volume = finite(rounded(sum_volume + volume));
    }
    require(sum_volume > 0.0, "success fixture needs positive total volume");
    double sum_TV = 0.0;
    double sum_dTV = 0.0;
    for (std::size_t i = 0; i < input.volume.size(); ++i) {
        const double temperature = input.T_uniform ? *input.T_uniform : input.T_cell[i];
        const double dT = finite(rounded(temperature - input.T_ref));
        require(std::isfinite(status.value.dT[i]), "dT must be finite");
        require(status.value.dT[i] == dT,
                "dT must equal replayed double subtraction at each index");
        const double TV = finite(rounded(temperature * input.volume[i]));
        const double dTV = finite(rounded(dT * input.volume[i]));
        sum_TV = finite(rounded(sum_TV + TV));
        sum_dTV = finite(rounded(sum_dTV + dTV));
    }
    const double weighted_T = finite(rounded(sum_TV / sum_volume));
    const double weighted_dT = finite(rounded(sum_dTV / sum_volume));
    require(std::isfinite(status.value.volume_weighted_T), "weighted T must be finite");
    require(std::isfinite(status.value.volume_weighted_dT), "weighted dT must be finite");
    require(status.value.volume_weighted_T == weighted_T,
            "weighted T must match ordered double products, sum and quotient");
    require(status.value.volume_weighted_dT == weighted_dT,
            "weighted dT must match ordered double products, sum and quotient");
    const double delta = finite(rounded(status.value.volume_weighted_T - input.T_ref));
    const double residual = finite(rounded(status.value.volume_weighted_dT - delta));
    const double tolerance = finite(rounded(1e-14 * std::max(std::abs(delta), 1.0)));
    require(std::abs(residual) <= tolerance, "weighted temperature consistency threshold exceeded");

    const auto repeated = map_thermal_to_mechanic(input);
    require_code(repeated, ErrorCode::NONE);
    require(repeated.message.empty(), "repeated success message must be empty");
    require(repeated.value.dT == status.value.dT, "repeated dT must match element by element");
    require(repeated.value.volume_weighted_T == status.value.volume_weighted_T,
            "repeated weighted T must match exactly");
    require(repeated.value.volume_weighted_dT == status.value.volume_weighted_dT,
            "repeated weighted dT must match exactly");
    return status;
}
} // namespace ctmmap_test
