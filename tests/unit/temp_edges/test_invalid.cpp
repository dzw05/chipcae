#include "chipcae/material/temperature_table.hpp"

#include <Kokkos_Core.hpp>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

using namespace chipcae::material;

namespace {
void check(bool pass, const char* name) {
    if (!pass) {
        std::fprintf(stderr, "FAIL tempedge_invalid: %s\n", name);
        std::exit(EXIT_FAILURE);
    }
}
template <class Construct>
void check_invalid(Construct construct, const char* name) {
    try {
        construct();
    } catch (const std::invalid_argument&) {
        return;
    } catch (...) {
        check(false, name);
    }
    check(false, name);
}
} // namespace

int main(int argc, char** argv) {
    Kokkos::ScopeGuard guard(argc, argv);
    check_invalid([] { TemperatureTable table({}, {{1e-5}}); }, "empty temperatures");
    check_invalid([] { TemperatureTable table({250}, {}); }, "empty columns");
    check_invalid([] { TemperatureTable table({250, 300}, {{1e-5}}); }, "column length mismatch");
    check_invalid([] { TemperatureTable table({300, 300, 300}, {{1e-5, 2e-5, 4e-5}}); },
                  "three consecutive equal temperatures");
    check_invalid([] { TemperatureTable table({300, 250}, {{2e-5, 1e-5}}); },
                  "descending temperatures");
    for (Real node : {std::numeric_limits<Real>::quiet_NaN(),
                      std::numeric_limits<Real>::infinity(),
                      -std::numeric_limits<Real>::infinity()}) {
        check_invalid([node] { TemperatureTable table({250, node}, {{1e-5, 2e-5}}); },
                      "non-finite temperature node");
    }
    check_invalid([] { TemperatureTable table({250, 300, 250}, {{1e-5, 2e-5, 4e-5}}); },
                  "non-adjacent repetition");
    TemperatureTable pair({300, 300}, {{2e-5, 4e-5}});
    check(pair.has_duplicate_temperatures() == true, "adjacent pair is valid");
    check(pair.strictly_increasing() == false, "adjacent pair is not strictly increasing");
    TemperatureTable table({250, 300, 300, 400},
                           {{1e-5, 2e-5, 4e-5, 6e-5}},
                           Interpolation::Linear,
                           Extrapolation::Clamp);
    check(table.has_duplicate_temperatures() == true, "longer table duplicate is valid");
    check(table.strictly_increasing() == false, "longer table is not strictly increasing");
}
