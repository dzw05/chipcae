#include "batched_dense_lu.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <petscblaslapack.h>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace {
using chipcae::fem::detail::BatchedDenseLU;

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct Fixture {
    std::vector<std::int64_t> first;
    std::vector<int> sizes;
    std::vector<std::vector<double>> matrices;
    std::vector<double> rhs;
};

std::vector<std::int64_t> prefix_starts(const std::vector<int>& sizes) {
    std::vector<std::int64_t> first;
    std::int64_t next = 0;
    for (int n : sizes) {
        first.push_back(next);
        next += n;
    }
    return first;
}

Fixture make_fixture(const std::vector<std::int64_t>& first, const std::vector<int>& sizes) {
    Fixture fixture{first, sizes, {}, {}};
    std::mt19937_64 random(20261119);
    std::uniform_real_distribution<double> entries(-1., 1.);
    std::uniform_real_distribution<double> loads(1., 2.);
    std::size_t extent = 0;
    for (std::size_t c = 0; c < sizes.size(); ++c)
        extent = std::max(extent, static_cast<std::size_t>(first[c] + sizes[c]));
    // Include trailing sentinels as well as any gaps between the block ranges.
    fixture.rhs.assign(extent + 3, -1234.5);
    for (std::size_t c = 0; c < sizes.size(); ++c) {
        const auto n = static_cast<std::size_t>(sizes[c]);
        std::vector<double> matrix(n * n);
        for (std::size_t row = 0; row < n; ++row) {
            double off_diagonal_sum = 0.;
            for (std::size_t col = 0; col < n; ++col) {
                if (row == col)
                    continue;
                const double value = entries(random);
                require(std::isfinite(value), "fixture matrix must be finite");
                matrix[col * n + row] = value;
                off_diagonal_sum += std::abs(value);
            }
            matrix[row * n + row] = off_diagonal_sum + 2.;
            require(std::isfinite(matrix[row * n + row]) &&
                        matrix[row * n + row] > off_diagonal_sum,
                    "fixture must be strictly row diagonally dominant");
            fixture.rhs[static_cast<std::size_t>(first[c]) + row] = loads(random);
        }
        fixture.matrices.push_back(std::move(matrix));
    }
    return fixture;
}

// This reference owns and factors each matrix separately; it never calls BatchedDenseLU.
std::vector<double> serial_reference(const Fixture& fixture, const std::vector<double>& rhs) {
    auto solution = rhs;
    for (std::size_t c = 0; c < fixture.sizes.size(); ++c) {
        if (fixture.sizes[c] == 0)
            continue;
        const auto n = static_cast<PetscBLASInt>(fixture.sizes[c]);
        const PetscBLASInt nrhs = 1;
        PetscBLASInt info = 0;
        auto matrix = fixture.matrices[c];
        std::vector<PetscBLASInt> pivots(static_cast<std::size_t>(n));
        LAPACKgetrf_(&n, &n, matrix.data(), &n, pivots.data(), &info);
        require(info == 0, "serial reference factorization must succeed");
        LAPACKgetrs_("N",
                     &n,
                     &nrhs,
                     matrix.data(),
                     &n,
                     pivots.data(),
                     solution.data() + fixture.first[c],
                     &n,
                     &info);
        require(info == 0, "serial reference solve must succeed");
    }
    return solution;
}

void load(BatchedDenseLU& batch, const Fixture& fixture) {
    batch.resize(fixture.first, fixture.sizes);
    require(batch.blocks() == fixture.sizes.size(), "resize must preserve block count");
    std::size_t dense_size = 0;
    for (std::size_t c = 0; c < fixture.sizes.size(); ++c) {
        require(batch.size(c) == fixture.sizes[c], "size must preserve input dimensions");
        require(batch.first(c) == fixture.first[c], "first must preserve input block order");
        const auto& matrix = fixture.matrices[c];
        dense_size += matrix.size();
        if (matrix.empty())
            continue;
        auto* block = batch.block(c);
        require(block != nullptr, "nonempty block must have storage");
        for (std::size_t i = 0; i < matrix.size(); ++i) {
            require(block[i] == 0., "resize must zero-initialize dense storage");
            block[i] = matrix[i];
        }
    }
    require(batch.dense_size() == dense_size,
            "dense_size must equal the sum of squared dimensions");
}

void check_solution(const Fixture& fixture,
                    const std::vector<double>& rhs,
                    const std::vector<double>& solution,
                    const std::vector<double>& reference) {
    require(solution.size() == reference.size(), "solution extent must be unchanged");
    for (std::size_t i = 0; i < solution.size(); ++i)
        require(std::isfinite(solution[i]) && solution[i] == reference[i],
                "solution, including RHS gaps, must equal serial LAPACK componentwise at " +
                    std::to_string(i));
    for (std::size_t c = 0; c < fixture.sizes.size(); ++c) {
        const auto n = static_cast<std::size_t>(fixture.sizes[c]);
        if (n == 0)
            continue;
        const auto first = static_cast<std::size_t>(fixture.first[c]);
        double residual_norm = 0.;
        double rhs_norm = 0.;
        for (std::size_t row = 0; row < n; ++row) {
            double ax = 0.;
            for (std::size_t col = 0; col < n; ++col)
                ax += fixture.matrices[c][col * n + row] * solution[first + col];
            residual_norm = std::max(residual_norm, std::abs(ax - rhs[first + row]));
            rhs_norm = std::max(rhs_norm, std::abs(rhs[first + row]));
        }
        require(rhs_norm > 0., "residual fixture must have a nonzero RHS norm");
        require(std::isfinite(residual_norm) && residual_norm / rhs_norm <= 1e-12,
                "relative infinity-norm residual must be <= 1e-12 for block " + std::to_string(c));
    }
}

std::vector<double> solve_fixture(const Fixture& fixture) {
    const auto reference = serial_reference(fixture, fixture.rhs);
    BatchedDenseLU batch;
    load(batch, fixture);
    require(batch.factor() == -1, "nonsingular batch factor must return -1");
    auto solution = fixture.rhs;
    const BatchedDenseLU& factors = batch;
    require(factors.solve(solution.data()), "nonsingular batch solve must succeed");
    check_solution(fixture, fixture.rhs, solution, reference);
    return solution;
}

void check_equal_sizes() {
    const std::vector<int> sizes(7, 21);
    solve_fixture(make_fixture(prefix_starts(sizes), sizes));
}

void check_mixed_sizes() {
    const std::vector<int> sizes{0, 1, 21, 75, 0, 39};
    const auto fixture = make_fixture(prefix_starts(sizes), sizes);
    solve_fixture(fixture);
    BatchedDenseLU batch;
    load(batch, fixture);
    require(batch.dense_size() == 7588, "mixed_sizes dense storage must contain 7588 doubles");
}

void check_layout() {
    BatchedDenseLU batch;
    batch.resize({0, 0, 1, 22, 97, 97}, {0, 1, 21, 75, 0, 39});
    require(batch.blocks() == 6 && batch.dense_size() == 7588, "mixed layout shape");
    const auto* base = batch.block(0);
    require(base != nullptr, "zero-sized first block must point into nonempty batch storage");
    const std::array<std::ptrdiff_t, 6> offsets{0, 0, 1, 442, 6067, 6067};
    for (std::size_t c = 0; c < offsets.size(); ++c)
        require(batch.block(c) - base == offsets[c], "blocks must be stored consecutively");
    for (std::size_t i = 0; i < batch.dense_size(); ++i)
        require(base[i] == 0., "all initial dense values must be zero");
}

void check_empty() {
    BatchedDenseLU batch;
    std::array<double, 3> rhs{17., -23., 42.};
    const auto original = rhs;
    for (int pass = 0; pass < 2; ++pass) {
        require(batch.blocks() == 0 && batch.dense_size() == 0, "empty batch has no storage");
        require(batch.factor() == -1, "empty factor must succeed");
        require(batch.solve(rhs.data()), "empty solve must succeed");
        require(rhs == original, "empty solve must leave sentinel RHS unchanged");
        require(batch.solve(nullptr), "empty solve must not touch x");
        batch.resize({}, {});
    }
}

void check_all_zero_sizes() {
    BatchedDenseLU batch;
    const std::vector<std::int64_t> first{7, 0, 7, 100, 4};
    batch.resize(first, {0, 0, 0, 0, 0});
    require(batch.blocks() == 5 && batch.dense_size() == 0,
            "zero sizes preserve all block numbers");
    for (std::size_t c = 0; c < first.size(); ++c)
        require(batch.first(c) == first[c] && batch.size(c) == 0, "zero-sized range metadata");
    require(batch.factor() == -1, "all-zero factor must succeed without LAPACK");
    std::array<double, 2> rhs{13., -19.};
    const auto original = rhs;
    require(batch.solve(rhs.data()) && rhs == original, "all-zero solve must preserve RHS");
    require(batch.solve(nullptr), "all-zero solve must not touch x");
}

void check_single() {
    Fixture scalar{{0}, {1}, {{4.}}, {8., -17.}};
    require(solve_fixture(scalar) == std::vector<double>({2., -17.}), "m=1 analytic solution");
    // Column-major [[0, 2], [1, 3]] requires a first-column pivot swap.
    Fixture pivot{{0}, {2}, {{0., 1., 2., 3.}}, {4., 7., -17.}};
    require(solve_fixture(pivot) == std::vector<double>({1., 2., -17.}),
            "pivoted single-block analytic solution");
}

void check_singular() {
    const std::vector<int> sizes(7, 21);
    auto fixture = make_fixture(prefix_starts(sizes), sizes);
    for (std::size_t c : {2u, 4u})
        std::fill_n(fixture.matrices[c].begin(), sizes[c], 0.);
    for (int threads : {1, 4}) {
#ifdef _OPENMP
        omp_set_num_threads(threads);
#else
        (void)threads;
#endif
        BatchedDenseLU batch;
        load(batch, fixture);
        require(batch.factor() == 2, "factor must return the smallest singular block index, 2");
        // Failed factors must never be passed to solve().
    }
}

void check_threads() {
    const std::vector<int> sizes{0, 1, 21, 75, 0, 39};
    const auto fixture = make_fixture(prefix_starts(sizes), sizes);
#ifdef _OPENMP
    omp_set_dynamic(0);
    omp_set_num_threads(1);
    const auto one = solve_fixture(fixture);
    omp_set_num_threads(4);
    int observed_threads = 0;
#pragma omp parallel reduction(max : observed_threads)
    {
        observed_threads = omp_get_num_threads();
    }
    require(observed_threads == 4, "OpenMP test must exercise a four-thread team");
    const auto four = solve_fixture(fixture);
    require(one == four, "one-thread and four-thread solutions must be componentwise identical");
    std::cout << "openmp=on threads=1,4\n";
#else
    std::cout << "openmp=off\n";
    solve_fixture(fixture);
#endif
}

void expect_invalid_ranges(const std::vector<std::int64_t>& first, const std::vector<int>& sizes) {
    BatchedDenseLU batch;
    try {
        batch.resize(first, sizes);
    } catch (const std::invalid_argument&) {
        return;
    } catch (const std::exception& error) {
        require(false,
                "bad ranges must throw std::invalid_argument; got: " + std::string(error.what()));
    }
    require(false, "bad ranges must throw std::invalid_argument");
}

void check_gaps_and_reuse() {
    // Nonempty ranges are disjoint and deliberately out of order. Empty ranges
    // may share a start or lie inside a nonempty range without overlapping it.
    const auto fixture = make_fixture({12, 2, 7, 12, 3}, {3, 2, 1, 0, 0});
    BatchedDenseLU batch;
    load(batch, fixture);
    require(batch.factor() == -1, "out-of-order disjoint ranges must factor");
    const BatchedDenseLU& factors = batch;
    auto rhs = fixture.rhs;
    for (int pass = 0; pass < 2; ++pass) {
        const auto reference = serial_reference(fixture, rhs);
        auto solution = rhs;
        require(factors.solve(solution.data()), "factors must support repeated solves");
        check_solution(fixture, rhs, solution, reference);
        for (std::size_t c = 0; c < fixture.sizes.size(); ++c)
            for (int row = 0; row < fixture.sizes[c]; ++row)
                rhs[static_cast<std::size_t>(fixture.first[c] + row)] += 3.25;
    }
}
} // namespace

int main() {
    int failures = 0;
    const auto run = [&](const std::string& name, const std::function<void()>& test) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    };
#if defined(CHIPCAE_TEST_EXPECT_OPENMP) && !defined(_OPENMP)
    run("openmp_configuration", [] {
        require(false, "CHIPCAE_TEST_EXPECT_OPENMP requires compiler OpenMP support (_OPENMP)");
    });
#endif
    run("equal_sizes", check_equal_sizes);
    run("mixed_sizes", check_mixed_sizes);
    run("layout", check_layout);
    run("empty", check_empty);
    run("all_zero_sizes", check_all_zero_sizes);
    run("single", check_single);
    run("singular", check_singular);
    run("threads", check_threads);
    run("bad_ranges overlap", [] { expect_invalid_ranges({0, 1}, {2, 2}); });
    run("bad_ranges unsorted overlap", [] { expect_invalid_ranges({5, 0}, {2, 6}); });
    run("bad_ranges negative dimension", [] { expect_invalid_ranges({0}, {-1}); });
    run("bad_ranges negative start", [] { expect_invalid_ranges({-1}, {1}); });
    run("bad_ranges length mismatch", [] { expect_invalid_ranges({0, 1}, {1}); });
    run("bad_ranges endpoint overflow",
        [] { expect_invalid_ranges({std::numeric_limits<std::int64_t>::max()}, {1}); });
    run("gaps_and_reuse", check_gaps_and_reuse);
    return failures == 0 ? 0 : 1;
}
