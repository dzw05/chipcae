#include "direct_refine.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace mfem;
using chipcae::solve::refine;

namespace {
void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct Fixture {
    static constexpr int n = 30;
    SparseMatrix A{n};
    DenseMatrix dense{n}, eigenvectors{n};
    Vector eigenvalues = Vector(n), exact = Vector(n), b = Vector(n);
    Fixture() {
        dense = 0.0;
        // Dirichlet endpoints are eliminated; all 31 edges alternate Cu/FR4.
        auto k = [](int edge) { return edge % 2 == 0 ? 390.0 : 0.3; };
        for (int i = 0; i < n; ++i) {
            dense(i, i) = k(i) + k(i + 1);
            A.Add(i, i, dense(i, i));
            if (i + 1 < n) {
                dense(i, i + 1) = dense(i + 1, i) = -k(i + 1);
                A.Add(i, i + 1, -k(i + 1));
                A.Add(i + 1, i, -k(i + 1));
            }
        }
        A.Finalize();
        DenseMatrix copy(dense);
        copy.Eigenvalues(eigenvalues, eigenvectors);
        for (int i = 0; i < n; ++i)
            exact(i) = eigenvectors(i, 0) + 0.2 * std::sin(0.37 * (i + 1));
        A.Mult(exact, b);
    }
    double rho(double delta) const {
        // diag(A)=390.3 I, so A and A+delta*diag(A) have the same eigenvectors.
        // These are the eigenvalues of I - (A+E)^-1 A, not an iteration estimate.
        double result = 0;
        for (int i = 0; i < n; ++i)
            result = std::max(result, std::abs(delta * 390.3 / (eigenvalues(i) + delta * 390.3)));
        return result;
    }
};

double omega(const SparseMatrix& A, const Vector& b, const Vector& x) {
    Vector r(b.Size());
    A.Mult(x, r);
    double result = 0;
    for (int i = 0; i < b.Size(); ++i) {
        double denominator = std::abs(b(i));
        for (int j = A.GetI()[i]; j < A.GetI()[i + 1]; ++j)
            denominator += std::abs(A.GetData()[j]) * std::abs(x(A.GetJ()[j]));
        result = std::max(result, std::abs(b(i) - r(i)) / denominator);
    }
    return result;
}

void factor_case(double delta) {
    Fixture f;
    DenseMatrix perturbed(f.dense);
    for (int i = 0; i < Fixture::n; ++i)
        perturbed(i, i) *= 1 + delta;
    DenseMatrixInverse inverse(perturbed);
    Vector x(Fixture::n);
    inverse.Mult(f.b, x);
    const double initial = omega(f.A, f.b, x);
    const double rho = f.rho(delta);
    if (delta < 0) {
        require(rho > 1, "large perturbation must excite an unstable iteration");
        double unstable_projection = 0;
        for (int mode = 0; mode < Fixture::n; ++mode) {
            if (std::abs(delta * 390.3 / (f.eigenvalues(mode) + delta * 390.3)) <= 1)
                continue;
            double projection = 0;
            for (int i = 0; i < Fixture::n; ++i)
                projection += f.eigenvectors(i, mode) * (x(i) - f.exact(i));
            unstable_projection = std::max(unstable_projection, std::abs(projection));
        }
        require(unstable_projection > 1e-6, "initial error must excite an unstable eigenmode");
    }
    if (delta > 0) {
        require(rho <= 1e-3, "small perturbation must contract by at least 1000");
        require(initial > 1e-12, "small perturbation must require refinement");
    }
    int calls = 0;
    auto result = refine(
        f.A,
        f.b,
        x,
        [&](const Vector& r, Vector& d) {
            ++calls;
            inverse.Mult(r, d);
        },
        1e-12,
        8);
    std::cout << "delta=" << delta << " rho=" << rho << " initial omega=" << initial
              << " measured omega=" << result.omega << " iterations=" << result.iterations << '\n';
    require(result.iterations >= 1 && result.iterations <= 8, "iterations must be in [1,8]");
    if (delta < 0) {
        require(!result.accepted && result.omega > 1e-12,
                "unstable factorization must be rejected");
    } else {
        require(result.accepted, "accurate/contracting factorization must be accepted");
        const double u = std::numeric_limits<double>::epsilon() / 2;
        require(result.omega <= (delta == 0 ? 8 * u / (1 - 4 * u) : 1e-12),
                "backward error exceeds the derived bound");
        if (delta > 0)
            require(calls > 0, "small perturbation must actually use correction solves");
    }
    require(std::abs(omega(f.A, f.b, x) - result.omega) <= 1e-18,
            "reported omega must describe the returned solution");
}

void zero_case() {
    SparseMatrix A(1);
    A.Add(0, 0, 1.0);
    A.Finalize();
    Vector b(1), x(1);
    b = x = 0.0;
    auto result = refine(
        A,
        b,
        x,
        [](const Vector&, Vector&) {
            throw std::runtime_error("zero RHS must not need correction");
        },
        1e-12,
        8);
    require(result.accepted && std::abs(result.omega) <= 0 && std::abs(result.eta) <= 0,
            "zero RHS must have zero backward errors and be accepted");
    require(std::isinf(result.rel) && result.rel > 0, "zero RHS relative residual must be +inf");
}

void nonfinite_case(double invalid) {
    SparseMatrix A(1);
    A.Add(0, 0, 1.0);
    A.Finalize();
    Vector b(1), x(1);
    b = 1.0;
    x = 0.0;
    int calls = 0;
    auto result = refine(
        A,
        b,
        x,
        [&](const Vector&, Vector& d) {
            ++calls;
            d = invalid;
        },
        2.0,
        8);
    require(!result.accepted, "nonfinite correction must reject even an acceptable best iterate");
    require(result.iterations == 2 && calls == 1, "must stop at the first nonfinite evaluation");
    require(std::abs(x[0]) <= 0 && std::abs(result.omega - 1) <= 1e-15,
            "nonfinite correction must preserve the best finite solution and metrics");
    b = invalid;
    result = refine(A, b, x, [](const Vector&, Vector&) {}, 2.0, 8);
    require(!result.accepted && std::isinf(result.omega), "nonfinite RHS must reject");
    b = 1.0;
    x = invalid;
    result = refine(A, b, x, [](const Vector&, Vector&) {}, 2.0, 8);
    require(!result.accepted && std::isinf(result.omega), "nonfinite solution must reject");
    x = 0.0;
    A.GetData()[0] = invalid;
    result = refine(A, b, x, [](const Vector&, Vector&) {}, 2.0, 8);
    require(!result.accepted && std::isinf(result.omega), "nonfinite matrix must reject");
}

void best_case(int max_steps) {
    SparseMatrix A(1);
    A.Add(0, 0, 1.0);
    A.Finalize();
    Vector b(1), x(1);
    b = 1.0;
    x = 0.0;
    int calls = 0;
    auto result = refine(
        A,
        b,
        x,
        [&](const Vector&, Vector& d) {
            d.SetSize(1);
            d = ++calls == 1 ? 0.5 : 10.0;
        },
        0.4,
        max_steps);
    const double expected_x = max_steps == 1 ? 0.0 : 0.5;
    const double expected_error = max_steps == 1 ? 1.0 : 1.0 / 3;
    require(std::abs(x(0) - expected_x) <= 1e-15, "must restore the best evaluated solution");
    require(std::abs(result.omega - expected_error) <= 1e-15, "best omega mismatch");
    require(std::abs(result.eta - expected_error) <= 1e-15, "best eta mismatch");
    require(std::abs(result.rel - (1 - expected_x)) <= 1e-15, "best rel mismatch");
    require(result.accepted == (max_steps > 1), "acceptance must use best omega");
    require(result.iterations == (max_steps == 1 ? 1 : 3), "evaluation limit/stagnation mismatch");
    require(calls == (max_steps == 1 ? 0 : 2), "must not generate an unevaluated correction");
}
} // namespace

int main() {
    int failures = 0;
    auto test = [&](const char* name, auto body) {
        try {
            body();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& e) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
        }
    };
    test("exact LU", [] { factor_case(0); });
    test("unstable LU", [] { factor_case(-1e-3); });
    test("contracting LU", [] { factor_case(1e-9); });
    test("zero RHS", zero_case);
    test("zero denominator conventions", [] {
        using chipcae::solve::backward_error_ratio;
        require(std::abs(backward_error_ratio(0, 0)) <= 0, "0/0 must contribute zero");
        const double value = backward_error_ratio(1, 0);
        require(std::isinf(value) && value > 0, "positive/0 must contribute +inf");
    });
    test("NaN rejection", [] { nonfinite_case(std::numeric_limits<double>::quiet_NaN()); });
    test("infinity rejection", [] { nonfinite_case(std::numeric_limits<double>::infinity()); });
    test("best iterate", [] { best_case(8); });
    test("one evaluation", [] { best_case(1); });
    return failures ? 1 : 0;
}
