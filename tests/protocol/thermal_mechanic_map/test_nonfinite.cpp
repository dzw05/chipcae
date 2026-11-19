#include "support.hpp"

#include <array>
#include <limits>

int main() {
    using namespace ctmmap_test;
    Suite suite;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const double maximum = std::numeric_limits<double>::max();
    const std::array nonfinite{nan, inf, -inf};
    const std::array<std::string_view, 3> names{"NaN", "+Inf", "-Inf"};
    const std::array volumes{1.0, 3.0};
    const std::array temperatures{300.0, 350.0};
    for (std::size_t i = 0; i < nonfinite.size(); ++i) {
        suite.run("nonfinite T_cell " + std::string(names[i]), [&] {
            const std::array invalid_temperatures{300.0, nonfinite[i]};
            ThermalToMechanicInput input;
            input.T_cell = invalid_temperatures;
            input.volume = volumes;
            input.T_ref = 300.0;
            expect_failure(input, ErrorCode::INPUT_INVALID, "nonfinite T_cell");
        });
        suite.run("nonfinite T_ref " + std::string(names[i]), [&] {
            ThermalToMechanicInput input;
            input.T_cell = temperatures;
            input.volume = volumes;
            input.T_ref = nonfinite[i];
            expect_failure(input, ErrorCode::INPUT_INVALID, "invalid T_ref");
        });
        suite.run("nonfinite T_uniform " + std::string(names[i]), [&] {
            ThermalToMechanicInput input;
            input.volume = volumes;
            input.T_ref = 300.0;
            input.T_uniform = nonfinite[i];
            expect_failure(input, ErrorCode::INPUT_INVALID, "invalid T_uniform");
        });
    }
    suite.run("finite volumes overflow their ordered sum", [&] {
        const std::array overflowing_volumes{maximum, maximum};
        ThermalToMechanicInput input;
        input.T_cell = temperatures;
        input.volume = overflowing_volumes;
        input.T_ref = 300.0;
        expect_failure(input, ErrorCode::INPUT_INVALID, "invalid volume");
    });
    for (const bool uniform : {false, true}) {
        suite.run(
            uniform ? "uniform dT subtraction overflow" : "thermal dT subtraction overflow", [&] {
                const std::array temperature{maximum};
                const std::array volume{1.0};
                ThermalToMechanicInput input;
                input.volume = volume;
                input.T_ref = -maximum;
                if (uniform) {
                    input.T_uniform = maximum;
                } else {
                    input.T_cell = temperature;
                }
                expect_failure(input, ErrorCode::INPUT_INVALID, "invalid temperature arithmetic");
            });
    }
    suite.run("finite temperature times volume overflows", [&] {
        const std::array temperature{maximum};
        const std::array volume{2.0};
        ThermalToMechanicInput input;
        input.T_cell = temperature;
        input.volume = volume;
        expect_failure(input, ErrorCode::INPUT_INVALID, "invalid temperature arithmetic");
    });
    suite.run("finite TV products overflow their numerator sum", [&] {
        const std::array temperature{maximum, maximum};
        const std::array volume{1.0, 1.0};
        ThermalToMechanicInput input;
        input.T_cell = temperature;
        input.volume = volume;
        expect_failure(input, ErrorCode::INPUT_INVALID, "invalid temperature arithmetic");
    });
    suite.run("dT times volume overflows while TV stays zero", [&] {
        const std::array temperature{0.0};
        const std::array volume{2.0};
        ThermalToMechanicInput input;
        input.T_cell = temperature;
        input.volume = volume;
        input.T_ref = -maximum;
        expect_failure(input, ErrorCode::INPUT_INVALID, "invalid temperature arithmetic");
    });
    suite.run("dTV numerator sum overflows while TV sum stays zero", [&] {
        const std::array temperature{0.0, 0.0};
        const std::array volume{1.0, 1.0};
        ThermalToMechanicInput input;
        input.T_cell = temperature;
        input.volume = volume;
        input.T_ref = -maximum;
        expect_failure(input, ErrorCode::INPUT_INVALID, "invalid temperature arithmetic");
    });
    suite.run("finite rounding fails weighted consistency", [] {
        const std::array temperature{1e16, 1e16 + 2.0};
        const std::array volume{1.0, 1.0};
        ThermalToMechanicInput input;
        input.T_cell = temperature;
        input.volume = volume;
        input.T_ref = 1e16;
        expect_failure(input, ErrorCode::INPUT_INVALID, "invalid temperature arithmetic");
    });
    return suite.finish();
}
