#include <cfloat>
#include <limits>

#if defined(__FAST_MATH__) || FLT_EVAL_METHOD != 0
#error "K1 requires sequential binary64 evaluation without fast math"
#endif
static_assert(std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::digits == 53 &&
              std::numeric_limits<double>::max_exponent == 1024);

#include "k1_stop.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace chipcae::solve::k1 {
namespace {
constexpr double inf = std::numeric_limits<double>::infinity();
constexpr double nan = std::numeric_limits<double>::quiet_NaN();
constexpr double u = 0x1p-53;

// The zero-denominator convention also covers the two diagnostic estimates.
double ratio(double numerator, double denominator) {
    if (!std::isfinite(numerator) || !std::isfinite(denominator))
        return inf;
    if (denominator == 0)
        return numerator == 0 ? 0 : inf;
    return numerator / denominator;
}
StopStats invalid_stats() {
    return {inf, inf, inf, inf, inf, inf, 0, false, false};
}

StopStats evaluate(const CsrView& a, const double* b, const double* x, double* rd, double* re) {
    StopStats s = invalid_stats();
    for (int i = 0; i < a.n; ++i)
        if (!std::isfinite(b[i]) || !std::isfinite(x[i]))
            return s;
    const double norm_d = residual_binary64(a, b, x, rd);
    residual_dot2(a, b, x, re);
    if (!std::isfinite(norm_d))
        return s;
    double b2 = 0, e2 = 0, abs2 = 0;
    double omega = 0, r_inf = 0, a_inf = 0, x_inf = 0, b_inf = 0;
    for (int i = 0; i < a.n; ++i) {
        if (!std::isfinite(rd[i]) || !std::isfinite(re[i]))
            return s;
        double denominator = std::abs(b[i]), row_norm = 0;
        for (int j = a.row[i]; j < a.row[i + 1]; ++j) {
            const double value = std::abs(a.val[j]);
            const double product = value * std::abs(x[a.col[j]]);
            denominator += product;
            row_norm += value;
            if (!std::isfinite(value) || !std::isfinite(product) || !std::isfinite(denominator) ||
                !std::isfinite(row_norm))
                return s;
        }
        const double component = ratio(std::abs(rd[i]), denominator);
        if (!std::isfinite(component))
            return s;
        omega = std::max(omega, component);
        r_inf = std::max(r_inf, std::abs(rd[i]));
        a_inf = std::max(a_inf, row_norm);
        x_inf = std::max(x_inf, std::abs(x[i]));
        b_inf = std::max(b_inf, std::abs(b[i]));
        b2 += b[i] * b[i];
        e2 += re[i] * re[i];
        abs2 += denominator * denominator;
        if (!std::isfinite(b2) || !std::isfinite(e2) || !std::isfinite(abs2))
            return s;
    }
    const double b_norm = std::sqrt(b2);
    const double ax = a_inf * x_inf;
    s.rel_d = ratio(norm_d, b_norm);
    s.rel_e = ratio(std::sqrt(e2), b_norm);
    s.omega_inf = omega;
    s.eta_inf = ratio(r_inf, ax + b_inf);
    s.est_inf = ratio(u * ax, b_inf);
    s.est_abs = ratio(u * std::sqrt(abs2), b_norm);
    s.valid = std::isfinite(ax) && std::isfinite(s.rel_d) && std::isfinite(s.rel_e) &&
              std::isfinite(s.omega_inf) && std::isfinite(s.eta_inf) && std::isfinite(s.est_inf) &&
              std::isfinite(s.est_abs);
    return s;
}
} // namespace

TwoTerm two_sum(double a, double b) {
    const double hi = a + b;
    const double z = hi - a;
    const double lo = (a - (hi - z)) + (b - z);
    return {hi, lo};
}
TwoTerm two_prod(double a, double b) {
    const double hi = a * b;
    return {hi, std::fma(a, b, -hi)};
}
double residual_binary64(const CsrView& a, const double* b, const double* x, double* r) {
    double square_sum = 0;
    for (int i = 0; i < a.n; ++i) {
        double sum = 0;
        for (int j = a.row[i]; j < a.row[i + 1]; ++j) {
            const double product = a.val[j] * x[a.col[j]];
            sum += product;
            if (!std::isfinite(product) || !std::isfinite(sum))
                sum = nan;
        }
        r[i] = b[i] - sum;
        square_sum += r[i] * r[i];
    }
    return std::sqrt(square_sum);
}
void residual_dot2(const CsrView& a, const double* b, const double* x, double* r) {
    for (int i = 0; i < a.n; ++i) {
        // ORO Algorithm 5.3, starting with the exact product b_i * 1.
        double p = b[i], s = 0;
        for (int j = a.row[i]; j < a.row[i + 1]; ++j) {
            const auto product = two_prod(-a.val[j], x[a.col[j]]);
            const auto sum = two_sum(p, product.hi);
            const double error = sum.lo + product.lo;
            s += error;
            p = sum.hi;
            if (!std::isfinite(product.hi) || !std::isfinite(product.lo) ||
                !std::isfinite(sum.hi) || !std::isfinite(sum.lo) || !std::isfinite(error) ||
                !std::isfinite(s))
                p = nan;
        }
        r[i] = p + s;
    }
}
StopStats refine_mixed(const CsrView& a,
                       const double* b,
                       double* x,
                       const std::function<void(const double*, double*)>& correct) {
    if (a.n == 0)
        return {0, 0, 0, 0, 0, 0, 0, true, true};
    const auto n = static_cast<std::size_t>(a.n);
    std::vector<double> rd(n), re(n), d(n), best_x(x, x + a.n);
    StopStats best = invalid_stats();
    int corrections = 0;
    double previous = inf;
    bool valid = true;
    for (;;) {
        const auto current = evaluate(a, b, x, rd.data(), re.data());
        if (!current.valid) {
            valid = false;
            break;
        }
        if (current.rel_d <= best.rel_d) {
            best = current;
            std::copy_n(x, a.n, best_x.data());
        }
        const bool zero = std::all_of(re.begin(), re.end(), [](double v) { return v == 0; });
        if (zero || (corrections >= kMinCorrections && current.rel_e > previous / 2) ||
            corrections == kMaxCorrections)
            break;
        previous = current.rel_e;
        correct(re.data(), d.data()); // Exceptions intentionally leave the latest x in place.
        ++corrections;
        for (int i = 0; i < a.n; ++i) {
            if (!std::isfinite(d[i]))
                valid = false;
            x[i] += d[i];
            if (!std::isfinite(x[i]))
                valid = false;
        }
        if (!valid)
            break;
    }
    std::copy(best_x.begin(), best_x.end(), x);
    best.corrections = corrections;
    best.valid = valid && best.valid;
    best.accepted = best.valid && best.rel_d <= kGate;
    return best;
}
} // namespace chipcae::solve::k1
