#pragma once

#include <span>

namespace chipcae {
/// Snapshot of the world communicator. Construct after runtime initialization
/// for distributed use; reductions must finish before runtime finalization.
class Communicator {
public:
    static Communicator world();
    int rank() const noexcept { return rank_; }
    int size() const noexcept { return size_; }
    bool is_serial() const noexcept { return size_ == 1; }
    bool gpu_aware() const noexcept;
    void allreduce_sum(std::span<double> values) const;
    /// Sum in ascending rank order, then broadcast (fixed process count).
    void deterministic_allreduce_sum(std::span<double> values) const;
private:
    Communicator(int rank, int size) noexcept : rank_(rank), size_(size) {}
    int rank_;
    int size_;
};
} // namespace chipcae
