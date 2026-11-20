#pragma once

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

inline std::string number(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    return buffer;
}

inline int xfail_check(const std::string& name,
                       double observed,
                       double target,
                       double current_prediction,
                       double tol_target,
                       double tol_pred = 1e-9) {
    const bool finite = std::isfinite(observed) && std::isfinite(target) &&
                        std::isfinite(current_prediction) && std::isfinite(tol_target) &&
                        std::isfinite(tol_pred) && tol_target >= 0 && tol_pred >= 0;
    if (finite && std::abs(observed - target) <= tol_target * std::abs(target)) {
        std::cout << "XPASS " << name << " observed=" << number(observed)
                  << " target=" << number(target) << '\n';
        return 1;
    }
    const bool predicted = finite && std::abs(observed - current_prediction) <=
                                         tol_pred * std::abs(current_prediction);
    std::cout << (predicted ? "XFAIL " : "XFAIL-WRONG-REASON ") << name
              << " observed=" << number(observed) << " target=" << number(target)
              << " current_prediction=" << number(current_prediction)
              << " ratio=" << number(observed / target) << '\n';
    return predicted ? 0 : 2;
}
