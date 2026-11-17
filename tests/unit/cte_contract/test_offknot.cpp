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

// Primitive anchored at the first knot, using analytic linear-segment areas.
// Subtracting two primitives gives the signed integral for any reference.
Real primitive(const CteTable& table, Real temperature) {
    const auto& knots = table.temperatures;
    if (temperature < knots.front())
        return (temperature - knots.front()) * table.alpha.front();
    Real area = 0;
    for (std::size_t i = 0; i + 1 < knots.size(); ++i) {
        const Real width = knots[i + 1] - knots[i];
        const Real distance = std::clamp(temperature - knots[i], Real{0}, width);
        area += table.alpha[i] * distance +
                (table.alpha[i + 1] - table.alpha[i]) * distance * distance / (2 * width);
    }
    if (temperature > knots.back())
        area += (temperature - knots.back()) * table.alpha.back();
    return area;
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
            // These reordered fixtures have unequal slopes. In the second segment,
            // accumulated area from the first makes the secant nonlinear in T;
            // unlike the original globally linear fixtures, its midpoint differs
            // from the mean of the converted endpoints.
            if (left == 0)
                check(!near(expected_derivative, (derivative[left] + derivative[left + 1]) / 2),
                      "derivative probe distinguishes node resampling");
            if (left == 1)
                check(!near(expected_integral, (integral[left] + integral[left + 1]) / 2),
                      "integral probe distinguishes node resampling");
        }
    }
    // Move Tref independently of the first knot. Include references in both
    // segments and beyond either endpoint, with queries on both sides of Tref.
    // Each numeric table is interpreted independently in each conversion direction;
    // CteTable has no directional kind: for the supplied alpha(T), the oracles
    // differentiate alpha(T)*(T-Tref) or integrate alpha(T) from Tref to T.
    // Neither direction assumes the two fixtures are companion functions.
    for (auto probe : {sec, tan}) {
        std::swap(probe.alpha[1], probe.alpha[2]);
        const auto& knots = probe.temperatures;
        const Real below = knots.front() - (knots[1] - knots.front()) / 2;
        const Real above = knots.back() + (knots.back() - knots[1]) / 2;
        for (Real reference : {below,
                               knots.front(),
                               (knots[0] + knots[1]) / 2,
                               inverse_sample,
                               knots.back(),
                               above}) {
            probe.reference_temperature = reference;
            std::vector<Real> converted;
            check(tangent_to_secant(probe, converted) == StatusCode::Ok &&
                      converted.size() == knots.size(),
                  "arbitrary reference vector status and size");
            std::vector<Real> derivatives;
            check(secant_to_tangent(probe, derivatives) == StatusCode::Ok &&
                      derivatives.size() == knots.size(),
                  "arbitrary reference derivative vector status and size");
            auto verify_derivative = [&](Real temperature, Real actual) {
                Real expected =
                    temperature < knots.front() ? probe.alpha.front() : probe.alpha.back();
                if (temperature >= knots.front() && temperature <= knots.back()) {
                    // Differentiate the linear segment times (T - Tref).
                    // Interior knots select the right segment; the last selects the left.
                    for (std::size_t i = 0; i + 1 < knots.size(); ++i) {
                        if (temperature < knots[i] ||
                            (temperature >= knots[i + 1] && i + 2 < knots.size()))
                            continue;
                        const Real slope =
                            (probe.alpha[i + 1] - probe.alpha[i]) / (knots[i + 1] - knots[i]);
                        const Real alpha = probe.alpha[i] + (temperature - knots[i]) * slope;
                        expected = alpha + (temperature - reference) * slope;
                        break;
                    }
                }
                check(near(actual, expected), "arbitrary reference derivative definition");
            };
            for (std::size_t i = 0; i < knots.size(); ++i) {
                verify_derivative(knots[i], derivatives[i]);
                check(secant_to_tangent(probe, knots[i], value) == StatusCode::Ok,
                      "arbitrary reference derivative scalar node status");
                verify_derivative(knots[i], value);
            }
            for (Real temperature : {below, (knots[0] + knots[1]) / 2, sample, above, reference}) {
                check(secant_to_tangent(probe, temperature, value) == StatusCode::Ok,
                      "arbitrary reference derivative scalar status");
                verify_derivative(temperature, value);
            }
            auto verify = [&](Real temperature, Real actual) {
                // These fixture values stay positive under interpolation and extension.
                // For T != Tref, integral and (T-Tref) have the same sign, so
                // their quotient is positive; at Tref the limit is alpha(Tref).
                check(std::isfinite(actual) && actual > 0,
                      "arbitrary reference secant is finite and positive");
                if (temperature == reference) {
                    Real limit = probe.alpha.front();
                    if (reference >= knots.back())
                        limit = probe.alpha.back();
                    else if (reference > knots.front()) {
                        const std::size_t i = reference < knots[1] ? 0 : 1;
                        limit = probe.alpha[i] + (reference - knots[i]) *
                                                     (probe.alpha[i + 1] - probe.alpha[i]) /
                                                     (knots[i + 1] - knots[i]);
                    }
                    check(near(actual, limit), "arbitrary reference limit");
                    return;
                }
                const Real signed_area =
                    primitive(probe, temperature) - primitive(probe, reference);
                check(near(actual, signed_area / (temperature - reference)),
                      "arbitrary reference signed integral");
                check(near((temperature - reference) * actual, signed_area),
                      "arbitrary reference reconstructed area");
                check((temperature < reference) == (signed_area < 0),
                      "arbitrary reference area orientation");
            };
            for (std::size_t i = 0; i < knots.size(); ++i)
                verify(knots[i], converted[i]);
            for (Real temperature :
                 {below, (knots[0] + knots[1]) / 2, inverse_sample, above, reference}) {
                check(tangent_to_secant(probe, temperature, value) == StatusCode::Ok,
                      "arbitrary reference scalar status");
                verify(temperature, value);
            }
        }
    }
    // Extrapolation probe; expectations use endpoint constant extension, not a new CTE datum.
    const Real below = 225;
    const Real exterior_tangent = sec.alpha.front(); // Constant extension: slope zero.
    check(secant_to_tangent(sec, below, value) == StatusCode::Ok, "below secant status");
    check(std::isfinite(value), "below tangent is finite");
    check(near(value, exterior_tangent), "below tangent constant extension");
    for (const Real reference : {tan.reference_temperature, inverse_sample}) {
        auto probe = tan;
        probe.reference_temperature = reference;
        // Integrate from the reference down to the first knot in reverse segment
        // order, then continue to below with constant endpoint extension.
        Real oriented_integral = 0;
        for (std::size_t right = probe.temperatures.size() - 1; right > 0; --right) {
            const Real lower = probe.temperatures[right - 1];
            const Real upper = std::min(reference, probe.temperatures[right]);
            if (upper <= lower)
                continue;
            const Real upper_alpha =
                probe.alpha[right - 1] + (upper - lower) *
                                             (probe.alpha[right] - probe.alpha[right - 1]) /
                                             (probe.temperatures[right] - lower);
            oriented_integral += (lower - upper) * (upper_alpha + probe.alpha[right - 1]) / 2;
        }
        oriented_integral += (below - probe.temperatures.front()) * probe.alpha.front();
        check(oriented_integral < 0, "below reference integral has negative orientation");
        const Real exterior_secant = oriented_integral / (below - reference);
        check(tangent_to_secant(probe, below, value) == StatusCode::Ok, "below tangent status");
        check(std::isfinite(value) && value > 0, "below secant is finite and positive");
        check(near((below - reference) * value, oriented_integral),
              "below reference output reconstructs oriented integral");
        check(near(value, exterior_secant), "below secant equals oriented integral");
    }
} catch (const std::exception&) {
    return 1;
}
