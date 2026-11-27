#include "k1_stop.hpp"

// Test-first link stubs only. No K1 arithmetic or Direct integration is implemented.
namespace chipcae::solve::k1 {
TwoTerm two_sum(double, double) {
    return {};
}
TwoTerm two_prod(double, double) {
    return {};
}
double residual_binary64(const CsrView& a, const double*, const double*, double* r) {
    for (int i = 0; i < a.n; ++i)
        r[i] = 0;
    return 0;
}
void residual_dot2(const CsrView& a, const double*, const double*, double* r) {
    for (int i = 0; i < a.n; ++i)
        r[i] = 0;
}
StopStats refine_mixed(const CsrView&,
                       const double*,
                       double*,
                       const std::function<void(const double*, double*)>&) {
    return {};
}
} // namespace chipcae::solve::k1
