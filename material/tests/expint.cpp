#include "chipcae/material/shift.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

int main() {
    // Acceptance budget; a finite fallback residual must be measured before implementation
    // acceptance.
    constexpr double tolerance = 1e-12;
    int failures = 0;
    double worst = 0;
    bool finite_residuals = true;
    // Include the negative arguments from the existing physical piecewise regression:
    // u = T > 0 for Arrhenius, u = C2 + T - T_ref >= 80 for WLF.
    // With positive B, Ei(-B/u) = -E1(B/u); these arguments never reach zero.
    const double B = std::log(10.) * 4 * 100;
    for (double x : {1e-4,
                     0.01,
                     0.1,
                     1.,
                     10.,
                     50.,
                     -0.01,
                     -0.1,
                     -1.,
                     -10.,
                     -30000. / (chipcae::material::kGasConstant * 280),
                     -30000. / (chipcae::material::kGasConstant * 300),
                     -B / 80,
                     -B / 120}) {
        const double expected = std::expint(x);
        const double actual = chipcae::material::detail::expint(x);
        const double scale = std::max(std::abs(expected), 1e-30);
        const double residual = std::abs(actual - expected) / scale;
        finite_residuals = finite_residuals && std::isfinite(residual);
        if (std::isfinite(residual))
            worst = std::max(worst, residual);
        if (!(std::abs(actual - expected) <= tolerance * scale)) {
            std::fprintf(stderr,
                         "FAIL expint x=%.17g actual=%.17g std=%.17g relative=%.17g budget=%.1e\n",
                         x,
                         actual,
                         expected,
                         residual,
                         tolerance);
            ++failures;
        }
        if (x < 0 && !(actual < 0)) {
            std::fprintf(stderr, "FAIL Ei(-z) = -E1(z) must be negative: x=%.17g\n", x);
            ++failures;
        }
    }
    // Helper contract: the real Ei has a negative-infinite logarithmic pole at zero.
    const double zero = chipcae::material::detail::expint(0.);
    if (!(std::isinf(zero) && std::signbit(zero))) {
        std::fprintf(stderr, "FAIL expint(0) must be -infinity, actual=%.17g\n", zero);
        ++failures;
    }
    if (finite_residuals)
        std::printf("Measured worst relative residual: %.17g\n", worst);
    else
        std::puts("Measured residuals are non-finite; no finite accuracy bound established.");
    return failures == 0 ? 0 : 1;
}
