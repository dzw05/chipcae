#include "support.hpp"

#include <array>

int main() {
    using namespace ctmmap_test;
    Suite suite;
    const std::array temperature{300.0};
    const std::array volume{1.0};
    for (const std::string_view unit : {"k", "C", "K "}) {
        suite.run("reject unit '" + std::string(unit) + "'", [&] {
            ThermalToMechanicInput input;
            input.T_cell = temperature;
            input.volume = volume;
            input.T_ref = 300.0;
            input.temperature_unit = unit;
            expect_failure(input, ErrorCode::INPUT_INVALID, "expected unit K");
        });
    }
    return suite.finish();
}
