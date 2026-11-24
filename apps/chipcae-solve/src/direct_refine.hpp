#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <mfem.hpp>

namespace chipcae::solve {

struct RefineResult {
    double omega = std::numeric_limits<double>::infinity();
    double eta = std::numeric_limits<double>::infinity();
    double rel = std::numeric_limits<double>::infinity();
    int iterations = 0;
    bool accepted = false;
};

inline double backward_error_ratio(double numerator, double denominator) {
    if (!std::isfinite(numerator) || !std::isfinite(denominator) || numerator < 0 ||
        denominator < 0)
        return std::numeric_limits<double>::infinity();
    if (denominator == 0)
        return numerator == 0 ? 0 : std::numeric_limits<double>::infinity();
    return numerator / denominator;
}

// max_steps counts evaluated iterates, including the initial LU solution.
// The callback maps a physical residual to a physical correction using the existing factors.
inline RefineResult refine(const mfem::SparseMatrix& a,
                           const mfem::Vector& b,
                           mfem::Vector& x,
                           const std::function<void(const mfem::Vector&, mfem::Vector&)>& correct,
                           double rtol,
                           int max_steps) {
    RefineResult best;
    const int n = b.Size();
    mfem::Vector residual(n), correction(n), best_x(x);
    double previous = std::numeric_limits<double>::infinity();
    bool valid = true;
    for (int step = 0; step < std::min(max_steps, 8); ++step) {
        double omega = 0, r_inf = 0, a_inf = 0, x_inf = 0, b_inf = 0;
        double r_norm = 0, b_norm = 0;
        for (int i = 0; i < n; ++i) {
            double product = 0, denominator = std::abs(b[i]), row_norm = 0;
            for (int j = a.GetI()[i]; j < a.GetI()[i + 1]; ++j) {
                const double value = a.GetData()[j], xj = x[a.GetJ()[j]];
                product += value * xj;
                denominator += std::abs(value) * std::abs(xj);
                row_norm += std::abs(value);
            }
            residual[i] = b[i] - product;
            const double ratio = backward_error_ratio(std::abs(residual[i]), denominator);
            valid = valid && std::isfinite(ratio) && std::isfinite(row_norm) &&
                    std::isfinite(x[i]) && std::isfinite(b[i]);
            omega = std::max(omega, ratio);
            a_inf = std::max(a_inf, row_norm);
            x_inf = std::max(x_inf, std::abs(x[i]));
            b_inf = std::max(b_inf, std::abs(b[i]));
            r_inf = std::max(r_inf, std::abs(residual[i]));
            r_norm = std::hypot(r_norm, residual[i]);
            b_norm = std::hypot(b_norm, b[i]);
        }
        const double eta = backward_error_ratio(r_inf, a_inf * x_inf + b_inf);
        const double rel = b_norm == 0 ? std::numeric_limits<double>::infinity() : r_norm / b_norm;
        valid = valid && std::isfinite(eta) && std::isfinite(r_norm) && std::isfinite(b_norm) &&
                (b_norm == 0 || std::isfinite(rel));
        best.iterations = step + 1;
        if (!valid)
            break;
        if (omega < best.omega) {
            best.omega = omega;
            best.eta = eta;
            best.rel = rel;
            best_x = x;
        }
        if (omega <= std::numeric_limits<double>::epsilon() / 2 || omega > previous / 2 ||
            step + 1 == std::min(max_steps, 8))
            break;
        previous = omega;
        correct(residual, correction);
        x += correction;
    }
    x = best_x;
    best.accepted = valid && std::isfinite(best.omega) && best.omega <= rtol;
    return best;
}

} // namespace chipcae::solve
