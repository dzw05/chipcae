#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace chipcae::fem::detail {

// Column-major blocks are consecutive, with block c at sum_{j<c} m[j]^2.
// Each block acts on x[first[c] .. first[c] + m[c]).
class BatchedDenseLU {
public:
    // Reject invalid or overlapping nonempty ranges with std::invalid_argument.
    // Preserve input block numbers, including zero-sized blocks, and zero storage.
    void resize(std::vector<std::int64_t> first, std::vector<int> m);
    std::size_t blocks() const;
    int size(std::size_t c) const;
    std::int64_t first(std::size_t c) const;
    double* block(std::size_t c);
    std::size_t dense_size() const;

    // Return -1 on success, otherwise the smallest failing block index.
    std::int64_t factor();
    // Requires successful factorization and x storage through max(first[c] + m[c]).
    // Empty batches and zero-sized blocks do not access x.
    bool solve(double* x) const;
};

} // namespace chipcae::fem::detail
