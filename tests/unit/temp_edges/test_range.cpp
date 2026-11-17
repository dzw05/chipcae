#include "chipcae/material/temperature_table.hpp"

#include <Kokkos_Core.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace chipcae::material;

namespace {
void check(bool pass, const char* name) {
    if (!pass) {
        std::fprintf(stderr, "FAIL tempedge_range: %s\n", name);
        std::exit(EXIT_FAILURE);
    }
}
void check_near(Real got, Real ref, const char* name) {
    if (!(std::abs(got - ref) <= 1e-12 * std::max(std::abs(ref), Real{1e-30}))) {
        std::fprintf(
            stderr, "FAIL tempedge_range: %s: got %.17g, expected %.17g\n", name, got, ref);
        std::exit(EXIT_FAILURE);
    }
}
} // namespace

int main(int argc, char** argv) {
    Kokkos::ScopeGuard guard(argc, argv);
    TemperatureTable error({250, 300, 300, 400},
                           {{1e-5, 2e-5, 4e-5, 6e-5}},
                           Interpolation::Linear,
                           Extrapolation::Error);
    Real value = 0;
    check(error.evaluate(0, 200, value) == StatusCode::OutOfRange, "below range error");
    check(error.evaluate(0, 450, value) == StatusCode::OutOfRange, "above range error");
    // Separate tables ensure each clamp independently sets its warning.
    for (Real temperature : {Real{200}, Real{450}}) {
        TemperatureTable table({250, 300, 300, 400},
                               {{1e-5, 2e-5, 4e-5, 6e-5}},
                               Interpolation::Linear,
                               Extrapolation::Clamp);
        check(table.clamped_warning() == false, "warning initially clear");
        check(table.evaluate(0, temperature, value) == StatusCode::Ok, "clamp status");
        check_near(value, temperature == 200 ? 1e-5 : 6e-5, "clamped endpoint");
        check(table.clamped_warning() == true, "clamp sets warning");
    }
    check(error.evaluate(0, std::numeric_limits<Real>::quiet_NaN(), value) == StatusCode::Failed,
          "NaN temperature fails");
    check(error.evaluate(1, 300, value) == StatusCode::Failed, "invalid column fails");
}
