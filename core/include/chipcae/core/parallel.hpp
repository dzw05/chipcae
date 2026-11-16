#pragma once

namespace chipcae {

/// Process communicator. Serial (size = 1) until MPI is enabled.
class Communicator {
public:
    static Communicator world();

    int rank() const noexcept { return rank_; }
    int size() const noexcept { return size_; }
    bool is_serial() const noexcept { return size_ == 1; }

private:
    Communicator(int rank, int size) noexcept : rank_(rank), size_(size) {}

    int rank_{0};
    int size_{1};
};

} // namespace chipcae
