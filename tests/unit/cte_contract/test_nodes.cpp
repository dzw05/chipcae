#include "chipcae/material/cte_convert.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace chipcae::material;

namespace {
void check(bool pass, const char* name) {
    if (!pass) {
        std::fprintf(stderr, "FAIL ctecontract: %s\n", name);
        std::exit(EXIT_FAILURE);
    }
}
bool near(Real actual, Real expected) {
    return std::isfinite(actual) &&
           std::abs(actual - expected) <= (expected == 0 ? 1e-18 : 1e-12 * std::abs(expected));
}
} // namespace

int main(int argc, char** argv) {
    Kokkos::ScopeGuard guard(argc, argv);
    const CteTable sec{{250, 300, 350}, {1e-5, 2e-5, 3e-5}, 250};
    const CteTable tan{{250, 300, 350}, {1e-5, 3e-5, 5e-5}, 250};
    // alpha_sec + (T - T_ref) times the prescribed adjacent segment slope.
    // The final node is 5e-5 /K; 4e-5 /K belongs to the 325 K off-knot case.
    const Real last_segment_slope =
        (sec.alpha[2] - sec.alpha[1]) / (sec.temperatures[2] - sec.temperatures[1]);
    const std::vector<Real> tangent_expected{
        sec.alpha[0],
        sec.alpha[1] + (sec.temperatures[1] - sec.reference_temperature) * last_segment_slope,
        sec.alpha[2] + (sec.temperatures[2] - sec.reference_temperature) * last_segment_slope};
    // Exact trapezoid areas divided by distance from T_ref; limit at T_ref.
    const std::vector<Real> secant_expected{
        1e-5,
        (300 - 250) * (1e-5 + 3e-5) / 2 / (300 - 250),
        ((300 - 250) * (1e-5 + 3e-5) / 2 + (350 - 300) * (3e-5 + 5e-5) / 2) / (350 - 250)};
    std::vector<Real> tangent, secant, repeated;
    check(secant_to_tangent(sec, tangent) == StatusCode::Ok, "secant vector status");
    check(tangent_to_secant(tan, secant) == StatusCode::Ok, "tangent vector status");
    check(tangent.size() == 3 && secant.size() == 3, "exact vector dimensions");
    for (std::size_t i = 0; i < 3; ++i) {
        check(near(tangent[i], tangent_expected[i]), "ordered tangent node");
        check(near(secant[i], secant_expected[i]), "ordered secant node");
        Real value;
        check(secant_to_tangent(sec, sec.temperatures[i], value) == StatusCode::Ok,
              "secant scalar node status");
        check(near(value, tangent_expected[i]), "tangent scalar node definition");
        check(tangent_to_secant(tan, tan.temperatures[i], value) == StatusCode::Ok,
              "tangent scalar node status");
        check(near(value, secant_expected[i]), "secant scalar node definition");
    }
    check(secant_to_tangent(sec, repeated) == StatusCode::Ok && repeated == tangent,
          "repeat secant vector exactly");
    check(tangent_to_secant(tan, repeated) == StatusCode::Ok && repeated == secant,
          "repeat tangent vector exactly");
}
