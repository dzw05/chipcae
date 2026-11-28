#include "k1_stop.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace k1 = chipcae::solve::k1;
constexpr double u = 0x1p-53;

void require(bool ok, const std::string& message) {
    if (!ok)
        throw std::runtime_error(message);
}
void bits(double got, double want, const std::string& message) {
    require(std::bit_cast<std::uint64_t>(got) == std::bit_cast<std::uint64_t>(want), message);
}
void near(double got, double want, const std::string& message) {
    require(std::isfinite(got) && std::abs(got - want) <= 8 * u * std::abs(want), message);
}
struct Matrix {
    int n;
    std::vector<int> row{0}, col;
    std::vector<double> val;
    explicit Matrix(int size, bool tridiagonal = false) : n(size) {
        for (int i = 0; i < n; ++i) {
            if (tridiagonal && i > 0) {
                col.push_back(i - 1);
                val.push_back(-1);
            }
            col.push_back(i);
            val.push_back(2);
            if (tridiagonal && i + 1 < n) {
                col.push_back(i + 1);
                val.push_back(-1);
            }
            row.push_back(static_cast<int>(col.size()));
        }
    }
    k1::CsrView view() const { return {n, row.data(), col.data(), val.data()}; }
};

// Independent binary64 Thomas solve of tridiag(-1,2,-1), including the initial solve.
void thomas(int n, const double* rhs, double* x) {
    std::vector<double> diagonal(static_cast<std::size_t>(n), 2);
    std::copy_n(rhs, n, x);
    for (int i = 1; i < n; ++i) {
        const auto j = static_cast<std::size_t>(i);
        const double multiplier = -1 / diagonal[j - 1];
        diagonal[j] += multiplier;
        x[i] -= multiplier * x[i - 1];
    }
    x[n - 1] /= diagonal.back();
    for (int i = n - 2; i >= 0; --i)
        x[i] = (x[i] + x[i + 1]) / diagonal[static_cast<std::size_t>(i)];
}

void transforms() {
    const auto p = k1::two_prod(1 + 0x1p-30, 1 + 0x1p-30);
    bits(p.hi, 1 + 0x1p-29, "U1 TwoProduct hi");
    bits(p.lo, 0x1p-60, "U1 TwoProduct FMA error");
    for (const auto a : {1.0, 0x1p53}) {
        const double b = a < 2 ? 0x1p-60 : 1;
        const auto s = k1::two_sum(a, b);
        bits(s.hi, a, "U1 TwoSum hi");
        bits(s.lo, b, "U1 TwoSum error");
    }
}
constexpr std::array<int, 4> cancel_row{0, 3, 3, 3};
constexpr std::array<int, 3> cancel_col{0, 1, 2};
constexpr std::array<double, 3> cancel_val{1, 1, 1}, cancel_x{1, 0x1p-60, -1}, zero{};
const k1::CsrView cancellation{3, cancel_row.data(), cancel_col.data(), cancel_val.data()};
double dot2_bound() {
    const double gamma4 = 4 * u / (1 - 4 * u);
    return u * 0x1p-60 + gamma4 * gamma4 * (2 + 0x1p-60);
}
void cancellation_test() {
    std::array<double, 3> rd{}, re{};
    bits(k1::residual_binary64(cancellation, zero.data(), cancel_x.data(), rd.data()),
         0,
         "U2 binary64 residual norm");
    for (double r : rd)
        bits(r, 0, "U2 binary64 cancellation");
    k1::residual_dot2(cancellation, zero.data(), cancel_x.data(), re.data());
    require(std::abs(re[0] + 0x1p-60) <= dot2_bound(), "U2 Dot2 Proposition 5.5 bound");
    bits(re[1], 0, "U2 empty CSR row");
    bits(re[2], 0, "U2 empty CSR row");
}
void zero_dot2_still_checks_gate() {
    // Exact dot is one, but CSR left association loses the middle one.
    // Dot2 zero must stop immediately, while binary64 rel_d=1 rejects the result.
    const std::array<double, 3> x0{0x1p53, 1, -0x1p53}, b{1, 0, 0};
    auto x = x0;
    std::array<double, 3> r{};
    bits(k1::residual_binary64(cancellation, b.data(), x.data(), r.data()),
         1,
         "CSR left association residual norm");
    bits(r[0], 1, "CSR left association residual");
    k1::residual_dot2(cancellation, b.data(), x.data(), r.data());
    for (double value : r)
        bits(value, 0, "exact Dot2 zero");
    int calls = 0;
    const auto stats = k1::refine_mixed(
        cancellation, b.data(), x.data(), [&](const double*, double*) { ++calls; });
    require(stats.valid && !stats.accepted && stats.corrections == 0 && calls == 0,
            "Dot2 zero stops but does not override binary64 gate");
    bits(stats.rel_d, 1, "Dot2-zero rel_d");
    bits(stats.rel_e, 0, "Dot2-zero rel_e");
}
void invalid_correction() {
    const Matrix a(1);
    double x = 0;
    const double b = 2;
    int calls = 0;
    const auto stats = k1::refine_mixed(a.view(), &b, &x, [&](const double*, double* d) {
        ++calls;
        d[0] = std::numeric_limits<double>::infinity();
    });
    require(!stats.valid && !stats.accepted && calls == 1 && stats.corrections == 1,
            "nonfinite correction stops without another callback");
}
void long_double_test() {
    std::cout << "ld_crosscheck=" << (LDBL_MANT_DIG >= 64 ? "available" : "unavailable") << '\n';
    if constexpr (LDBL_MANT_DIG >= 64) {
        long double sum = 0;
        for (int j = 0; j < 3; ++j)
            sum += static_cast<long double>(cancel_val[j]) * cancel_x[j];
        const long double residual = -sum;
        require(residual == -0x1p-60L, "U7 exact long double residual");
        std::array<double, 3> re{};
        k1::residual_dot2(cancellation, zero.data(), cancel_x.data(), re.data());
        require(std::abs(static_cast<long double>(re[0]) - residual) <= dot2_bound(),
                "U7 Dot2/long double bound");
    } else {
        require(LDBL_MANT_DIG == 53, "U7 unsupported long double precision");
    }
}
void manufactured(int n) {
    const Matrix a(n, true);
    std::vector<double> b(static_cast<std::size_t>(n), 2), x(b.size());
    thomas(n, b.data(), x.data());
    int calls = 0;
    const auto s = k1::refine_mixed(a.view(), b.data(), x.data(), [&](const double* r, double* d) {
        ++calls;
        thomas(n, r, d);
    });
    require(s.valid && s.accepted, "U3/U8a manufactured solution must be accepted");
    require(calls == s.corrections && calls <= 9, "U3/U8a correction budget");
    for (int i = 0; i < n; ++i)
        bits(x[static_cast<std::size_t>(i)],
             static_cast<double>((i + 1) * (n - i)),
             "U3/U8a manufactured integer solution n=" + std::to_string(n) +
                 " i=" + std::to_string(i));
    if (n == 1000) {
        bits(s.rel_d, 0, "U3 rel_d");
        bits(s.rel_e, 0, "U3 rel_e");
    }
}
void stagnation() {
    const Matrix a(5);
    std::array<double, 5> b{1, 0, 0, 0, 0}, x{};
    int calls = 0;
    const auto s = k1::refine_mixed(a.view(), b.data(), x.data(), [&](const double* r, double* d) {
        bits(r[0], 1, "U4 callback receives residual");
        ++calls;
        std::fill_n(d, 5, 0);
    });
    require(s.valid && !s.accepted && s.corrections == 2 && calls == 2, "U4 stagnation at k=2");
    bits(s.rel_d, 1, "U4 rel_d");
    bits(s.rel_e, 1, "U4 rel_e");
    bits(s.omega_inf, 1, "U4 omega");
    bits(s.eta_inf, 1, "U4 eta");
}
void nonfinite() {
    for (double value :
         {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        for (int input = 0; input < 3; ++input) {
            Matrix a(5);
            std::array<double, 5> b{1, 1, 1, 1, 1}, x{};
            if (input == 0)
                x[2] = value;
            else if (input == 1)
                b[2] = value;
            else
                a.val[2] = value;
            int calls = 0;
            const auto s = k1::refine_mixed(
                a.view(), b.data(), x.data(), [&](const double*, double*) { ++calls; });
            require(!s.valid && !s.accepted && calls == 0, "U5 reject nonfinite without callback");
        }
    }
}
void zero_rhs() {
    const Matrix a(3);
    std::array<double, 3> b{}, x{};
    int calls = 0;
    const auto s =
        k1::refine_mixed(a.view(), b.data(), x.data(), [&](const double*, double*) { ++calls; });
    require(s.valid && s.accepted && s.corrections == 0 && calls == 0, "U6 zero RHS early stop");
    bits(s.rel_d, 0, "U6 rel_d");
    bits(s.rel_e, 0, "U6 rel_e");
}
void contraction(double c, int corrections, double residual) {
    const Matrix a(3);
    std::array<double, 3> b{2, 4, 4}, x{};
    int calls = 0;
    const auto s = k1::refine_mixed(a.view(), b.data(), x.data(), [&](const double* r, double* d) {
        ++calls;
        for (int i = 0; i < 3; ++i)
            d[i] = c * r[i] / 2;
    });
    require(s.valid && !s.accepted && s.corrections == corrections && calls == corrections,
            "U8b strict stagnation comparison and nine-correction cap");
    bits(s.rel_d, residual, "U8b rel_d");
    bits(s.rel_e, residual, "U8b rel_e");
    bits(x[0], 1 - residual, "U8b restored x[0]");
    bits(x[1], 2 * (1 - residual), "U8b restored x[1]");
    bits(x[2], x[1], "U8b restored x[2]");
    near(s.omega_inf, residual / (2 - residual), "U8b omega");
    near(s.eta_inf, residual / (2 - residual), "U8b eta");
    near(s.est_inf, u * (1 - residual), "U8b est_inf");
    near(s.est_abs, u * (2 - residual), "U8b est_abs");
}
void empty() {
    const int row = 0;
    int calls = 0;
    const auto s = k1::refine_mixed(
        {0, &row, nullptr, nullptr}, nullptr, nullptr, [&](const double*, double*) { ++calls; });
    require(s.valid && s.accepted && s.corrections == 0 && calls == 0, "empty system accepted");
    bits(s.rel_d, 0, "empty rel_d");
    bits(s.rel_e, 0, "empty rel_e");
}
void best_and_tie() {
    const Matrix a(1);
    const double b = 2;
    // Best at k=1; equal residuals at x=.75 and x=1.25 choose the later x.
    for (bool tie : {false, true}) {
        double x = 0;
        int calls = 0;
        const auto s = k1::refine_mixed(a.view(), &b, &x, [&](const double*, double* d) {
            d[0] = calls++ == 0 ? .75 : (tie ? .5 : -.25);
        });
        require(s.valid && !s.accepted && s.corrections == 2 && calls == 2, "best/tie stagnation");
        bits(x, tie ? 1.25 : .75, "restore best x; ties choose later");
        bits(s.rel_d, .25, "best rel_d agrees with restored x");
        bits(s.rel_e, .25, "best rel_e agrees with restored x");
        near(s.omega_inf, tie ? 1.0 / 9 : 1.0 / 7, "best omega");
        near(s.eta_inf, tie ? 1.0 / 9 : 1.0 / 7, "best eta");
        near(s.est_inf, u * (tie ? 1.25 : .75), "best est_inf");
        near(s.est_abs, u * (tie ? 2.25 : 1.75), "best est_abs");
    }
}
void continue_after_gate() {
    const Matrix a(1);
    const double b = 2;
    double x = 1 - 0x1p-40; // Already inside 1e-12, but not exact.
    int calls = 0;
    const auto s = k1::refine_mixed(a.view(), &b, &x, [&](const double*, double* d) {
        ++calls;
        d[0] = 0;
    });
    require(s.valid && s.accepted && calls == 2 && s.corrections == 2,
            "gate acceptance must not bypass minimum stagnation corrections");
    bits(s.rel_d, 0x1p-40, "gate residual");
}
void callback_exception() {
    struct Failure {
        int tag;
    };
    const Matrix a(1);
    const double b = 2;
    double x = 0;
    int calls = 0;
    try {
        (void)k1::refine_mixed(a.view(), &b, &x, [&](const double*, double* d) {
            if (++calls == 2)
                throw Failure{42};
            d[0] = .5;
        });
    } catch (const Failure& f) {
        require(f.tag == 42 && calls == 2, "callback exception identity");
        bits(x, .5, "exception leaves latest iterate");
        return;
    }
    require(false, "callback exception must propagate unchanged");
}
} // namespace

int main() {
    int failures = 0;
    const auto run = [&](const std::string& name, const auto& test) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& e) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
        } catch (...) {
            ++failures;
            std::cerr << "FAIL " << name << ": unexpected exception\n";
        }
    };
    run("K1-U1", transforms);
    run("K1-U2", cancellation_test);
    for (int n : {10, 100, 1000, 10000})
        run("K1-U3/U8a n=" + std::to_string(n), [=] { manufactured(n); });
    run("K1-U4", stagnation);
    run("K1-U5", nonfinite);
    run("K1-U6", zero_rhs);
    run("K1-U7", long_double_test);
    run("K1-U8b rho=3/4", [] { contraction(.25, 2, 9.0 / 16); });
    run("K1-U8b rho=1/2", [] { contraction(.5, 9, 0x1p-9); });
    run("K1-U8b rho=1/4", [] { contraction(.75, 9, 0x1p-18); });
    run("Dot2 zero versus gate", zero_dot2_still_checks_gate);
    run("invalid correction", invalid_correction);
    run("empty", empty);
    run("best/tie", best_and_tie);
    run("continue after gate", continue_after_gate);
    run("callback exception", callback_exception);
    return failures == 0 ? 0 : 1;
}
