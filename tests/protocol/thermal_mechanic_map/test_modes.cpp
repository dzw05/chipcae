#include "support.hpp"

#include <array>

int main() {
    using namespace ctmmap_test;
    Suite suite;
    suite.run("uniform and thermal sources are mutually exclusive", [] {
        const std::array temperatures{300.0, 350.0};
        const std::array volumes{1.0, 3.0};
        ThermalToMechanicInput input;
        input.T_cell = temperatures;
        input.volume = volumes;
        input.T_ref = 300.0;
        input.T_uniform = 400.0;
        expect_failure(input, ErrorCode::INPUT_INVALID, "inconsistent temperature sources");
    });
    return suite.finish();
}
