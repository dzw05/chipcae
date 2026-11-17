#include "chipcae/material/shift.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

int main() {
    // Acceptance budget confirmed against the measured fallback residual below.
    constexpr double tolerance = 1e-12;
    int failures = 0;
    double worst = 0;
    bool finite_residuals = true;
    // Include the negative arguments from the existing physical piecewise regression:
    // u = T > 0 for Arrhenius, u = C2 + T - T_ref >= 80 for WLF.
    // With positive B, Ei(-B/u) = -E1(B/u); these arguments never reach zero.
    const double B = std::log(10.) * 4 * 100;
    // Fixed references let this test compile on libraries without special functions.
    // On libstdc++ the live standard-library oracle remains authoritative, including
    // in the forced-fallback target.
    const double inputs[] = {1e-4,
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
                             -B / 120,
                             51.,
                             100.,
                             700.,
                             -1.0001,
                             -50.};
#if !defined(__cpp_lib_math_special_functions) || __cpp_lib_math_special_functions < 201603L
    const double references[] = {-8.6330247045745931,
                                 -4.0179294654266693,
                                 -1.6228128139692766,
                                 1.895117816355937,
                                 2492.2289762418764,
                                 1.058563689713169e20,
                                 -4.0379295765381134,
                                 -1.8229239584193904,
                                 -0.21938393439551984,
                                 -4.1569689296853263e-6,
                                 -1.8321270047962866e-7,
                                 -4.6136682743215371e-7,
                                 -8.0367672914473252e-7,
                                 -5.4106639291623281e-5,
                                 2.8198752548955184e20,
                                 2.7155527448538798e41,
                                 1.4509787360525605e301,
                                 -0.21934715012989106,
                                 -3.7832640295504591e-24};
#endif
    for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        const double x = inputs[i];
#if defined(__cpp_lib_math_special_functions) && __cpp_lib_math_special_functions >= 201603L
        const double expected = std::expint(x);
#else
        const double expected = references[i];
#endif
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
