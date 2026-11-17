#include "chipcae/material/cte_convert.hpp"
#include "cte_fixtures.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

using namespace chipcae::material;
using chipcae::material::test::near;

namespace {
void check(bool pass, const char* name) {
    if (!pass) {
        std::fprintf(stderr, "FAIL ctecontract: %s\n", name);
        throw std::runtime_error(name);
    }
}

} // namespace

int main(int argc, char** argv) try {
    Kokkos::ScopeGuard guard(argc, argv);
    const auto& sec = test::sec;
    const auto& tan = test::tan;
    // Expectations and sample temperatures are the existing core_math checks.
    // Equal adjacent fixture slopes also prevent discriminating off-knot segment selection.
    const Real sample = test::secant_to_tangent_temperature;
    const Real slope = (sec.alpha[2] - sec.alpha[1]) / (sec.temperatures[2] - sec.temperatures[1]);
    const Real tangent_expected = sec.alpha[1] + (sample - sec.temperatures[1]) * slope +
                                  (sample - sec.reference_temperature) * slope;
    const Real inverse_sample = test::tangent_to_secant_temperature;
    const Real interpolated = tan.alpha[1] + (inverse_sample - tan.temperatures[1]) *
                                                 (tan.alpha[2] - tan.alpha[1]) /
                                                 (tan.temperatures[2] - tan.temperatures[1]);
    const Real secant_expected =
        ((tan.temperatures[1] - tan.temperatures[0]) * (tan.alpha[0] + tan.alpha[1]) / 2 +
         (inverse_sample - tan.temperatures[1]) * (tan.alpha[1] + interpolated) / 2) /
        (inverse_sample - tan.reference_temperature);
    check(near(tangent_expected, test::secant_to_tangent_expected), "existing tangent sample");
    check(near(secant_expected, test::tangent_to_secant_expected), "existing secant sample");
    Real value = std::numeric_limits<Real>::quiet_NaN();
    check(secant_to_tangent(sec, test::secant_to_tangent_temperature, value) == StatusCode::Ok,
          "325 secant status");
    check(near(value, tangent_expected), "canonical off-knot tangent definition");
    value = std::numeric_limits<Real>::quiet_NaN();
    check(tangent_to_secant(tan, test::tangent_to_secant_temperature, value) == StatusCode::Ok,
          "325 tangent status");
    check(near(value, secant_expected), "canonical off-knot secant definition");
    // Extrapolation probe; expectations use endpoint constant extension, not a new CTE datum.
    const Real below = 225;
    const Real exterior_tangent = sec.alpha.front(); // Constant extension: slope zero.
    // Reverse integration orientation gives a negative area and denominator.
    const Real oriented_integral = -(tan.reference_temperature - below) * tan.alpha.front();
    const Real exterior_secant = oriented_integral / (below - tan.reference_temperature);
    check(secant_to_tangent(sec, below, value) == StatusCode::Ok, "below secant status");
    check(std::isfinite(value), "below tangent is finite");
    check(near(value, exterior_tangent), "below tangent constant extension");
    check(tangent_to_secant(tan, below, value) == StatusCode::Ok, "below tangent status");
    check(value * (below - tan.reference_temperature) < 0,
          "below reference output implies a negative oriented integral");
    check(std::isfinite(value) && value > 0, "below secant is finite and positive");
    check(near(value, exterior_secant), "below secant equals oriented integral");
} catch (const std::exception&) {
    return 1;
}
