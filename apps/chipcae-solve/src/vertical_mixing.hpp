#pragma once

#include "chipcae/solve/mixture.hpp"

namespace chipcae::solve {
// Test-first placeholder: no vertical constitutive law or validation is implemented yet.
inline Constitutive mixture_vertical(const Material&, const Material&, double) {
    Constitutive result;
    result.C = 0.0;
    result.alpha = 0.0;
    return result;
}
inline void copper_vertical_conductivity(double, double, double, double* k) {
    k[0] = k[1] = k[2] = 0.0;
}
} // namespace chipcae::solve
