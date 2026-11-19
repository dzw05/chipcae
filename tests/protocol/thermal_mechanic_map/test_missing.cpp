#include "support.hpp"

#include <array>
#include <limits>

int main() {
    using namespace ctmmap_test;
    Suite suite;
    const std::array temperatures{300.0, 350.0};
    const std::array volumes{1.0, 3.0};
    suite.run("missing thermal cell temperatures", [&] {
        ThermalToMechanicInput input;
        input.volume = volumes;
        input.T_ref = 300.0;
        expect_failure(input, ErrorCode::INPUT_REFERENCE_MISSING, "missing T_cell");
    });
    suite.run("temperature array shorter than volumes", [&] {
        ThermalToMechanicInput input;
        input.T_cell = std::span<const double>(temperatures).first(1);
        input.volume = volumes;
        input.T_ref = 300.0;
        expect_failure(input, ErrorCode::INPUT_INVALID, "length mismatch");
    });
    suite.run("temperature array longer than volumes", [&] {
        ThermalToMechanicInput input;
        input.T_cell = temperatures;
        input.volume = std::span<const double>(volumes).first(1);
        input.T_ref = 300.0;
        expect_failure(input, ErrorCode::INPUT_INVALID, "length mismatch");
    });
    suite.run("empty volume array", [&] {
        ThermalToMechanicInput input;
        input.T_cell = temperatures;
        input.T_ref = 300.0;
        expect_failure(input, ErrorCode::INPUT_INVALID, "invalid volume");
    });
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const std::array invalid_volumes{0.0, -1.0, nan, inf, -inf};
    const std::array<std::string_view, 5> names{"zero", "negative", "NaN", "+Inf", "-Inf"};
    for (std::size_t i = 0; i < invalid_volumes.size(); ++i) {
        suite.run("invalid volume " + std::string(names[i]), [&] {
            const std::array volume{1.0, invalid_volumes[i]};
            ThermalToMechanicInput input;
            input.T_cell = temperatures;
            input.volume = volume;
            input.T_ref = 300.0;
            expect_failure(input, ErrorCode::INPUT_INVALID, "invalid volume");
        });
    }
    return suite.finish();
}
