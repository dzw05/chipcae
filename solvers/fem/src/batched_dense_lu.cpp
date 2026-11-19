#include "batched_dense_lu.hpp"

#include <algorithm>
#include <limits>
#include <petscblaslapack.h>
#include <stdexcept>
#include <type_traits>
#include <utility>

static_assert(std::is_same_v<PetscScalar, double>);

namespace chipcae::fem::detail {

void BatchedDenseLU::resize(std::vector<std::int64_t> first, std::vector<int> m) {
    if (first.size() != m.size())
        throw std::invalid_argument("batched_dense_lu: mismatched block counts");
    if (!std::in_range<std::int64_t>(m.size()))
        throw std::invalid_argument("batched_dense_lu: block count overflow");

    std::vector<std::pair<std::int64_t, std::int64_t>> ranges;
    std::vector<std::size_t> dense_offset, pivot_offset;
    if (m.size() > ranges.max_size() || m.size() > dense_offset.max_size())
        throw std::invalid_argument("batched_dense_lu: offset storage overflow");
    ranges.reserve(m.size());
    dense_offset.reserve(m.size());
    pivot_offset.reserve(m.size());
    std::vector<double> dense;
    std::vector<PetscBLASInt> pivots;
    std::size_t dense_count = 0, pivot_count = 0;
    for (std::size_t c = 0; c < m.size(); ++c) {
        if (m[c] < 0 || first[c] < 0 || first[c] > std::numeric_limits<std::int64_t>::max() - m[c])
            throw std::invalid_argument("batched_dense_lu: invalid block range");
        const auto end = first[c] + m[c];
        if (m[c] != 0) {
            if (!std::in_range<std::ptrdiff_t>(end))
                throw std::invalid_argument("batched_dense_lu: RHS index overflow");
            ranges.emplace_back(first[c], end);
        }
        // Validate all LAPACK dimensions and index arithmetic before parallel work.
        if (!std::in_range<PetscBLASInt>(m[c]) || !std::in_range<std::size_t>(m[c]))
            throw std::invalid_argument("batched_dense_lu: dimension overflow");
        const auto n = static_cast<std::size_t>(m[c]);
        if (n != 0 && n > dense.max_size() / n)
            throw std::invalid_argument("batched_dense_lu: dense size overflow");
        const auto square = n * n;
        if (square > dense.max_size() - dense_count || n > pivots.max_size() - pivot_count)
            throw std::invalid_argument("batched_dense_lu: storage size overflow");
        dense_offset.push_back(dense_count);
        pivot_offset.push_back(pivot_count);
        dense_count += square;
        pivot_count += n;
    }
    // Sort only this temporary list, so public block numbers retain the input order.
    std::sort(ranges.begin(), ranges.end());
    for (std::size_t c = 1; c < ranges.size(); ++c)
        if (ranges[c].first < ranges[c - 1].second)
            throw std::invalid_argument("batched_dense_lu: overlapping block ranges");

    dense.assign(dense_count, 0.);
    pivots.resize(pivot_count);
    first_ = std::move(first);
    sizes_ = std::move(m);
    dense_offset_ = std::move(dense_offset);
    pivot_offset_ = std::move(pivot_offset);
    dense_ = std::move(dense);
    pivots_ = std::move(pivots);
}

std::size_t BatchedDenseLU::blocks() const {
    return sizes_.size();
}

int BatchedDenseLU::size(std::size_t c) const {
    return sizes_[c];
}

std::int64_t BatchedDenseLU::first(std::size_t c) const {
    return first_[c];
}

double* BatchedDenseLU::block(std::size_t c) {
    // Avoid even zero-offset arithmetic on a null data pointer for an all-zero batch.
    return dense_.empty() ? nullptr : dense_.data() + dense_offset_[c];
}

std::size_t BatchedDenseLU::dense_size() const {
    return dense_.size();
}

std::int64_t BatchedDenseLU::factor() {
    const auto count = static_cast<std::int64_t>(blocks()); // Checked by resize().
    if (count == 0)
        return -1;
    std::int64_t failed = count;
#ifdef _OPENMP
#pragma omp parallel for schedule(static) reduction(min : failed)
#endif
    for (std::int64_t c = 0; c < count; ++c) {
        const auto n = static_cast<PetscBLASInt>(sizes_[c]); // Checked by resize().
        if (n == 0)
            continue;
        PetscBLASInt info = 0;
        LAPACKgetrf_(
            &n, &n, dense_.data() + dense_offset_[c], &n, pivots_.data() + pivot_offset_[c], &info);
        if (info != 0)
            failed = std::min(failed, c);
    }
    return failed == count ? -1 : failed;
}

bool BatchedDenseLU::solve(double* x) const {
    const auto count = static_cast<std::int64_t>(blocks()); // Checked by resize().
    if (count == 0)
        return true;
    int failed = 0;
#ifdef _OPENMP
#pragma omp parallel for schedule(static) reduction(| : failed)
#endif
    for (std::int64_t c = 0; c < count; ++c) {
        const auto n = static_cast<PetscBLASInt>(sizes_[c]); // Checked by resize().
        if (n == 0)
            continue;
        const PetscBLASInt nrhs = 1;
        PetscBLASInt info = 0;
        LAPACKgetrs_("N",
                     &n,
                     &nrhs,
                     dense_.data() + dense_offset_[c],
                     &n,
                     pivots_.data() + pivot_offset_[c],
                     x + first_[c],
                     &n,
                     &info);
        failed |= info != 0;
    }
    return failed == 0;
}

} // namespace chipcae::fem::detail
