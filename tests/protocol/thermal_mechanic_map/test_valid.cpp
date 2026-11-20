#include "support.hpp"

#include <array>
#include <limits>

int main() {
    using namespace ctmmap_test;
    Suite suite;
    const std::array temperatures{300.0, 350.0};
    const std::array volumes{1.0, 3.0};
    for (const std::string_view unit : {std::string_view("K"), std::string_view("")}) {
        suite.run("thermal unit '" + std::string(unit) + "' and unequal volumes", [&] {
            ThermalToMechanicInput input;
            input.T_cell = temperatures;
            input.volume = volumes;
            input.T_ref = 300.0;
            input.temperature_unit = unit;
            const auto status = expect_success(input);
            require(status.value.dT == std::vector<double>{0.0, 50.0}, "expected dT = {0, 50}");
            require(status.value.volume_weighted_T == 337.5, "expected weighted T = 337.5 K");
            require(status.value.volume_weighted_dT == 37.5, "expected weighted dT = 37.5 K");
        });
    }
    suite.run("thermal default unit", [&] {
        ThermalToMechanicInput input;
        input.T_cell = temperatures;
        input.volume = volumes;
        input.T_ref = 300.0;
        const auto status = expect_success(input);
        require(status.value.dT == std::vector<double>{0.0, 50.0}, "expected dT = {0, 50}");
        require(status.value.volume_weighted_T == 337.5, "expected weighted T = 337.5 K");
        require(status.value.volume_weighted_dT == 37.5, "expected weighted dT = 37.5 K");
    });
    suite.run("uniform temperature and unequal volumes", [] {
        const std::array uniform_volumes{1.0, 2.0};
        ThermalToMechanicInput input;
        input.volume = uniform_volumes;
        input.T_ref = 293.15;
        input.T_uniform = 400.0;
        const auto status = expect_success(input);
        const double dT = rounded(400.0 - 293.15);
        require(status.value.dT == std::vector<double>{dT, dT}, "uniform dT = 400 - 293.15");
        require(status.value.volume_weighted_T == 400.0, "expected uniform weighted T = 400 K");
    });
    suite.run("cooling preserves negative temperature differences", [&] {
        const std::array cooling{280.0, 290.0};
        ThermalToMechanicInput input;
        input.T_cell = cooling;
        input.volume = volumes;
        input.T_ref = 300.0;
        const auto status = expect_success(input);
        require(status.value.dT == std::vector<double>{-20.0, -10.0}, "expected negative dT");
        require(status.value.volume_weighted_T == 287.5, "expected cooling weighted T = 287.5 K");
        require(status.value.volume_weighted_dT == -12.5, "expected cooling weighted dT = -12.5 K");
    });
    suite.run("single control volume", [] {
        const std::array temperature{305.0};
        const std::array volume{2.0};
        ThermalToMechanicInput input;
        input.T_cell = temperature;
        input.volume = volume;
        input.T_ref = 300.0;
        const auto status = expect_success(input);
        require(status.value.dT == std::vector<double>{5.0}, "expected single-cell dT = 5");
        require(status.value.volume_weighted_T == 305.0, "expected single-cell weighted T = 305 K");
        require(status.value.volume_weighted_dT == 5.0, "expected single-cell weighted dT = 5 K");
    });
    for (const bool uniform : {false, true}) {
        suite.run(uniform ? "uniform finite underflow" : "thermal finite underflow", [&] {
            const double tiny = std::numeric_limits<double>::min();
            const std::array temperature{tiny};
            const std::array volume{tiny};
            ThermalToMechanicInput input;
            input.volume = volume;
            if (uniform) {
                input.T_uniform = tiny;
            } else {
                input.T_cell = temperature;
            }
            const auto status = expect_success(input);
            require(status.value.dT == std::vector<double>{tiny},
                    "underflow must preserve cell dT");
            require(status.value.volume_weighted_T == 0.0, "underflowed TV gives weighted T = 0");
            require(status.value.volume_weighted_dT == 0.0,
                    "underflowed dTV gives weighted dT = 0");
        });
    }
    return suite.finish();
}
