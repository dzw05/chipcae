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
    // Interpolated secant plus distance times slope at 325 K.
    const Real tangent_expected =
        (2e-5 + (325 - 300) * (3e-5 - 2e-5) / 50) + (325 - 250) * (3e-5 - 2e-5) / 50;
    // Two trapezoids, with interpolated tangent 4e-5 at 325 K.
    const Real secant_expected = (50 * (1e-5 + 3e-5) / 2 + 25 * (3e-5 + 4e-5) / 2) / (325 - 250);
    Real value;
    check(secant_to_tangent(sec, 325, value) == StatusCode::Ok, "325 secant status");
    check(near(value, tangent_expected), "325 tangent definition (4e-5 /K)");
    check(tangent_to_secant(tan, 325, value) == StatusCode::Ok, "325 tangent status");
    check(near(value, secant_expected), "325 secant definition (2.5e-5 /K)");
    const Real below = 225;
    const Real exterior_tangent = sec.alpha.front(); // Constant extension: slope zero.
    // Reverse integration orientation gives a negative area and denominator.
    const Real oriented_integral = -(tan.reference_temperature - below) * tan.alpha.front();
    const Real exterior_secant = oriented_integral / (below - tan.reference_temperature);
    check(secant_to_tangent(sec, below, value) == StatusCode::Ok, "below secant status");
    check(near(value, exterior_tangent), "below tangent constant extension");
    check(tangent_to_secant(tan, below, value) == StatusCode::Ok, "below tangent status");
    check(near(value, exterior_secant), "below secant oriented integral is finite");
}
