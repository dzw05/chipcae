#include "chipcae/material/temperature_table.hpp"

#include <Kokkos_Core.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace chipcae::material;

namespace {
void check(bool pass, const char* name) {
    if (!pass) {
        std::fprintf(stderr, "FAIL tempedge_jump: %s\n", name);
        std::exit(EXIT_FAILURE);
    }
}
void check_near(Real got, Real ref, const char* name) {
    if (!(std::abs(got - ref) <= 1e-12 * std::max(std::abs(ref), Real{1e-30}))) {
        std::fprintf(stderr, "FAIL tempedge_jump: %s: got %.17g, expected %.17g\n", name, got, ref);
        std::exit(EXIT_FAILURE);
    }
}
} // namespace

int main(int argc, char** argv) {
    Kokkos::ScopeGuard guard(argc, argv);
    TemperatureTable table({250, 300, 300, 400},
                           {{1e-5, 2e-5, 4e-5, 6e-5}},
                           Interpolation::Linear,
                           Extrapolation::Clamp);
    check(table.num_temperatures() == 4, "four temperature nodes");
    check(table.num_columns() == 1, "one column");
    check(table.has_duplicate_temperatures() == true, "duplicate temperatures");
    check(table.strictly_increasing() == false, "jump is not strictly increasing");
    Real value = 0;
    check(table.evaluate(0, 300, value) == StatusCode::Ok, "jump status");
    check_near(value, 4e-5, "jump takes right value");
    check(table.evaluate(0, 275, value) == StatusCode::Ok, "left segment status");
    // Midpoint of (250, 1e-5) and the left jump node (300, 2e-5).
    check_near(value, 1.5e-5, "left segment interpolation");
    check(table.evaluate(0, 350, value) == StatusCode::Ok, "right segment status");
    // Midpoint of the right jump node (300, 4e-5) and (400, 6e-5).
    check_near(value, 5e-5, "right segment interpolation");
    for (int repeat = 0; repeat < 100; ++repeat) {
        check(table.evaluate(0, 300, value) == StatusCode::Ok, "repeated jump status");
        check_near(value, 4e-5, "repeated jump has no drift");
    }
}
