#include "chipcae/material/cte_convert.hpp"
#include "cte_fixtures.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace chipcae::material;

namespace {
void check(bool pass, const char* name) {
    if (!pass) {
        std::fprintf(stderr, "FAIL ctecontract: %s\n", name);
        throw std::runtime_error(name);
    }
}
bool near(Real actual, Real expected) {
    return std::isfinite(actual) &&
           std::abs(actual - expected) <= (expected == 0 ? 1e-18 : 1e-12 * std::abs(expected));
}
} // namespace

int main(int argc, char** argv) try {
    Kokkos::ScopeGuard guard(argc, argv);
    const auto& sec = test::sec;
    const auto& tan = test::tan;
    // Differentiate alpha_sec(T) * (T - T_ref). Interior knots use the
    // right-hand segment; the final node uses the left-hand segment.
    // At T_ref the derivative limit is alpha_sec(T_ref).
    std::vector<Real> tangent_expected;
    for (std::size_t i = 0; i < sec.temperatures.size(); ++i) {
        const std::size_t left = i + 1 < sec.temperatures.size() ? i : i - 1;
        const Real slope = (sec.alpha[left + 1] - sec.alpha[left]) /
                           (sec.temperatures[left + 1] - sec.temperatures[left]);
        tangent_expected.push_back(sec.alpha[i] +
                                   (sec.temperatures[i] - sec.reference_temperature) * slope);
    }
    // Exact trapezoid areas divided by distance from T_ref; limit at T_ref.
    const Real first_area =
        (tan.temperatures[1] - tan.temperatures[0]) * (tan.alpha[0] + tan.alpha[1]) / 2;
    const std::vector<Real> secant_expected{
        tan.alpha[0],
        first_area / (tan.temperatures[1] - tan.reference_temperature),
        (first_area +
         (tan.temperatures[2] - tan.temperatures[1]) * (tan.alpha[1] + tan.alpha[2]) / 2) /
            (tan.temperatures[2] - tan.reference_temperature)};
    std::vector<Real> tangent, secant;
    check(secant_to_tangent(sec, tangent) == StatusCode::Ok, "secant vector status");
    check(tangent_to_secant(tan, secant) == StatusCode::Ok, "tangent vector status");
    check(tangent.size() == sec.temperatures.size() && secant.size() == tan.temperatures.size(),
          "exact vector dimensions");
    for (std::size_t i = 0; i < sec.temperatures.size(); ++i) {
        check(near(tangent[i], tangent_expected[i]), "ordered tangent node");
        check(near(secant[i], secant_expected[i]), "ordered secant node");
        Real value = std::numeric_limits<Real>::quiet_NaN();
        check(secant_to_tangent(sec, sec.temperatures[i], value) == StatusCode::Ok,
              "secant scalar node status");
        const Real first_tangent = value;
        check(secant_to_tangent(sec, sec.temperatures[i], value) == StatusCode::Ok &&
                  value == first_tangent,
              "repeat secant scalar exactly");
        check(near(value, tangent_expected[i]), "tangent scalar node definition");
        value = std::numeric_limits<Real>::quiet_NaN();
        check(tangent_to_secant(tan, tan.temperatures[i], value) == StatusCode::Ok,
              "tangent scalar node status");
        const Real first_secant = value;
        check(tangent_to_secant(tan, tan.temperatures[i], value) == StatusCode::Ok &&
                  value == first_secant,
              "repeat tangent scalar exactly");
        check(near(value, secant_expected[i]), "secant scalar node definition");
    }
    // Keep the canonical knots/alpha values, moving only Tref to an existing
    // interior knot. Iterate the input sequence to check vector output order.
    for (auto fixture : {sec, tan}) {
        const std::size_t reference = fixture.temperatures.size() / 2;
        fixture.reference_temperature = fixture.temperatures[reference];
        std::vector<Real> derivative, integral;
        check(secant_to_tangent(fixture, derivative) == StatusCode::Ok,
              "interior reference tangent vector status");
        check(tangent_to_secant(fixture, integral) == StatusCode::Ok,
              "interior reference secant vector status");
        check(derivative.size() == fixture.temperatures.size() &&
                  integral.size() == fixture.temperatures.size(),
              "interior reference exact dimensions");
        std::size_t index = 0;
        for (Real temperature : fixture.temperatures) {
            const std::size_t left = index + 1 < fixture.temperatures.size() ? index : index - 1;
            const Real slope = (fixture.alpha[left + 1] - fixture.alpha[left]) /
                               (fixture.temperatures[left + 1] - fixture.temperatures[left]);
            const Real expected_derivative =
                fixture.alpha[index] + (temperature - fixture.reference_temperature) * slope;
            Real area = 0;
            for (std::size_t segment = std::min(index, reference);
                 segment < std::max(index, reference);
                 ++segment)
                area += (fixture.temperatures[segment + 1] - fixture.temperatures[segment]) *
                        (fixture.alpha[segment] + fixture.alpha[segment + 1]) / 2;
            if (index < reference)
                area = -area;
            const Real expected_integral =
                index == reference ? fixture.alpha[index]
                                   : area / (temperature - fixture.reference_temperature);
            check(near(derivative[index], expected_derivative), "input knot order derivative");
            check(near(integral[index], expected_integral), "input knot order integral");
            Real scalar = std::numeric_limits<Real>::quiet_NaN();
            check(secant_to_tangent(fixture, temperature, scalar) == StatusCode::Ok &&
                      near(scalar, expected_derivative),
                  "interior reference scalar derivative");
            check(tangent_to_secant(fixture, temperature, scalar) == StatusCode::Ok &&
                      near(scalar, expected_integral),
                  "interior reference scalar integral");
            ++index;
        }
        check(near(derivative[reference], fixture.alpha[reference]) &&
                  near(integral[reference], fixture.alpha[reference]),
              "interior reference node limits");
    }
    auto repeated = tangent;
    check(secant_to_tangent(sec, repeated) == StatusCode::Ok && repeated == tangent,
          "repeat secant vector exactly");
    repeated = secant;
    check(tangent_to_secant(tan, repeated) == StatusCode::Ok && repeated == secant,
          "repeat tangent vector exactly");
} catch (const std::exception&) {
    return 1;
}
