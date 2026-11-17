#include "chipcae/solve/case_loader.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>

int main() {
    using namespace chipcae::solve;
    auto check = [](bool condition) {
        if (!condition)
            throw std::runtime_error("interpolation assertion failed");
    };
    PropertyTable table{{250, 400}, {10, 40}};
    for (auto [temperature, value, clamped] : {std::tuple{325., 25., false},
                                               {250., 10., false},
                                               {400., 40., false},
                                               {200., 10., true},
                                               {450., 40., true}}) {
        auto result = interpolate_property(table, temperature);
        check(std::abs(result.value - value) <= 1e-12 && result.clamped == clamped);
    }
    for (double t : {0., 300., 1000.}) {
        auto result = interpolate_property({{300}, {17}}, t);
        check(result.value == 17 && !result.clamped);
    }
    for (auto bad : {PropertyTable{{250, 250}, {10, 40}}, PropertyTable{{400, 250}, {10, 40}}}) {
        try {
            (void)interpolate_property(bad, 300);
            check(false);
        } catch (const InputError& error) {
            check(error.code == chipcae::ErrorCode::INPUT_INVALID);
            check(std::string(error.what()).find("strictly increasing") != std::string::npos);
        }
    }
    for (auto bad : {PropertyTable{{250}, {std::numeric_limits<double>::infinity()}},
                     PropertyTable{{std::numeric_limits<double>::quiet_NaN()}, {10}},
                     PropertyTable{{}, {}},
                     PropertyTable{{250, 400}, {10}}}) {
        try {
            (void)interpolate_property(bad, 300, "material demo/E");
            check(false);
        } catch (const InputError& error) {
            check(error.code == chipcae::ErrorCode::INPUT_INVALID);
            check(std::string(error.what()).find("material demo/E") != std::string::npos);
        }
    }
}
