#include "chipcae/material/cte_convert.hpp"
#include "cte_fixtures.hpp"

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
    // Interior knots use the right-hand segment (both fixture slopes coincide).
    // alpha_sec + (T - T_ref) times the prescribed adjacent segment slope.
    // The final node is 5e-5 /K; 4e-5 /K belongs to the 325 K off-knot case.
    const Real last_segment_slope =
        (sec.alpha[2] - sec.alpha[1]) / (sec.temperatures[2] - sec.temperatures[1]);
    const std::vector<Real> tangent_expected{
        sec.alpha[0],
        sec.alpha[1] + (sec.temperatures[1] - sec.reference_temperature) * last_segment_slope,
        sec.alpha[2] + (sec.temperatures[2] - sec.reference_temperature) * last_segment_slope};
    // Exact trapezoid areas divided by distance from T_ref; limit at T_ref.
    const Real first_area =
        (tan.temperatures[1] - tan.temperatures[0]) * (tan.alpha[0] + tan.alpha[1]) / 2;
    const std::vector<Real> secant_expected{
        tan.alpha[0],
        first_area / (tan.temperatures[1] - tan.reference_temperature),
        (first_area +
         (tan.temperatures[2] - tan.temperatures[1]) * (tan.alpha[1] + tan.alpha[2]) / 2) /
            (tan.temperatures[2] - tan.reference_temperature)};
    std::vector<Real> tangent, secant, repeated;
    check(secant_to_tangent(sec, tangent) == StatusCode::Ok, "secant vector status");
    check(tangent_to_secant(tan, secant) == StatusCode::Ok, "tangent vector status");
    check(tangent.size() == 3 && secant.size() == 3, "exact vector dimensions");
    for (std::size_t i = 0; i < 3; ++i) {
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
    repeated = secant;
    check(secant_to_tangent(sec, repeated) == StatusCode::Ok && repeated == tangent,
          "repeat secant vector exactly");
    check(tangent_to_secant(tan, repeated) == StatusCode::Ok && repeated == secant,
          "repeat tangent vector exactly");
} catch (const std::exception&) {
    return 1;
}
