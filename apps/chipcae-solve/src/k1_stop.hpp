#pragma once

#include <functional>

namespace chipcae::solve::k1 {
struct TwoTerm {
    double hi, lo;
};
TwoTerm two_sum(double a, double b);
TwoTerm two_prod(double a, double b);
struct CsrView {
    int n;
    const int* row;
    const int* col;
    const double* val;
};
double residual_binary64(const CsrView& a, const double* b, const double* x, double* r);
void residual_dot2(const CsrView& a, const double* b, const double* x, double* r);
struct StopStats {
    double rel_d, rel_e, omega_inf, eta_inf, est_inf, est_abs;
    int corrections;
    bool valid, accepted;
};
inline constexpr double kGate = 1e-12;
inline constexpr int kMinCorrections = 2;
inline constexpr int kMaxCorrections = 9;
inline constexpr const char* kResidualPrecision = "double-double";
inline constexpr const char* kGatePrecision = "binary64";
StopStats refine_mixed(const CsrView& a,
                       const double* b,
                       double* x,
                       const std::function<void(const double* r, double* d)>& correct);
} // namespace chipcae::solve::k1
