#include "chipcae/material/cte_convert.hpp"
#include "cte_fixtures.hpp"

#include <Kokkos_Core.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

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
    // Reorder existing fixture samples to expose unequal adjacent slopes without
    // introducing physical data. References follow derivative/trapezoid definitions.
    for (auto probe : {sec, tan}) {
        std::swap(probe.alpha[1], probe.alpha[2]);
        std::vector<Real> derivative, integral;
        check(secant_to_tangent(probe, derivative) == StatusCode::Ok &&
                  derivative.size() == probe.temperatures.size(),
              "unequal-slope derivative vector");
        check(tangent_to_secant(probe, integral) == StatusCode::Ok &&
                  integral.size() == probe.temperatures.size(),
              "unequal-slope integral vector");
        Real area = 0;
        for (std::size_t i = 0; i < probe.temperatures.size(); ++i) {
            const std::size_t left = i + 1 < probe.temperatures.size() ? i : i - 1;
            const Real slope = (probe.alpha[left + 1] - probe.alpha[left]) /
                               (probe.temperatures[left + 1] - probe.temperatures[left]);
            const Real expected_derivative =
                probe.alpha[i] + (probe.temperatures[i] - probe.reference_temperature) * slope;
            if (i > 0)
                area += (probe.temperatures[i] - probe.temperatures[i - 1]) *
                        (probe.alpha[i - 1] + probe.alpha[i]) / 2;
            const Real expected_integral =
                i == 0 ? probe.alpha[0]
                       : area / (probe.temperatures[i] - probe.reference_temperature);
            check(near(derivative[i], expected_derivative), "unequal-slope ordered derivative");
            check(near(integral[i], expected_integral), "unequal-slope ordered integral");
            check(secant_to_tangent(probe, probe.temperatures[i], value) == StatusCode::Ok &&
                      near(value, expected_derivative),
                  "unequal-slope scalar node derivative");
            check(tangent_to_secant(probe, probe.temperatures[i], value) == StatusCode::Ok &&
                      near(value, expected_integral),
                  "unequal-slope scalar node integral");
        }
        for (std::size_t left = 0; left + 1 < probe.temperatures.size(); ++left) {
            const Real width = probe.temperatures[left + 1] - probe.temperatures[left];
            const Real temperature = (probe.temperatures[left] + probe.temperatures[left + 1]) / 2;
            const Real alpha = (probe.alpha[left] + probe.alpha[left + 1]) / 2;
            const Real expected_derivative =
                alpha + (temperature - probe.reference_temperature) *
                            (probe.alpha[left + 1] - probe.alpha[left]) / width;
            Real preceding_area = 0;
            for (std::size_t segment = 0; segment < left; ++segment)
                preceding_area += (probe.temperatures[segment + 1] - probe.temperatures[segment]) *
                                  (probe.alpha[segment] + probe.alpha[segment + 1]) / 2;
            const Real expected_integral =
                (preceding_area + width / 2 * (probe.alpha[left] + alpha) / 2) /
                (temperature - probe.reference_temperature);
            check(secant_to_tangent(probe, temperature, value) == StatusCode::Ok &&
                      near(value, expected_derivative),
                  "unequal-slope exact off-knot derivative");
            check(tangent_to_secant(probe, temperature, value) == StatusCode::Ok &&
                      near(value, expected_integral),
                  "unequal-slope exact off-knot integral");
            // Establish that these probes reject interpolation of converted nodes.
            if (left == 0)
                check(!near(expected_derivative, (derivative[left] + derivative[left + 1]) / 2),
                      "derivative probe distinguishes node resampling");
            if (left == 1)
                check(!near(expected_integral, (integral[left] + integral[left + 1]) / 2),
                      "integral probe distinguishes node resampling");
        }
    }
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
    check(std::isfinite(value) && value > 0, "below secant is finite and positive");
    check(near((below - tan.reference_temperature) * value, oriented_integral),
          "below reference output reconstructs oriented integral");
    check(near(value, exterior_secant), "below secant equals oriented integral");
} catch (const std::exception&) {
    return 1;
}
