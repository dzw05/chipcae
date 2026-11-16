#include "chipcae/core/communicator.hpp"

#ifdef CHIPCAE_ENABLE_MPI
#include <mpi.h>
#if __has_include(<mpi-ext.h>)
#include <mpi-ext.h>
#endif
#include <algorithm>
#include <stdexcept>
#include <vector>
#endif

namespace chipcae {
#ifdef CHIPCAE_ENABLE_MPI
namespace {
bool active() noexcept {
    int initialized = 0, finalized = 0;
    MPI_Initialized(&initialized);
    if (initialized)
        MPI_Finalized(&finalized);
    return initialized && !finalized;
}
void check(int result) {
    if (result != MPI_SUCCESS)
        throw std::runtime_error("Communicator collective failed");
}
} // namespace
#endif

Communicator Communicator::world() {
    int rank = 0, size = 1;
#ifdef CHIPCAE_ENABLE_MPI
    if (active()) {
        check(MPI_Comm_rank(MPI_COMM_WORLD, &rank));
        check(MPI_Comm_size(MPI_COMM_WORLD, &size));
    }
#endif
    return Communicator(rank, size);
}

bool Communicator::gpu_aware() const noexcept {
#if defined(CHIPCAE_ENABLE_MPI) && defined(MPIX_CUDA_AWARE_SUPPORT) && MPIX_CUDA_AWARE_SUPPORT
    return active() && MPIX_Query_cuda_support() != 0;
#else
    return false;
#endif
}

void Communicator::allreduce_sum(std::span<double> values) const {
#ifdef CHIPCAE_ENABLE_MPI
    if (size_ == 1)
        return;
    if (!active())
        throw std::runtime_error("Inactive communication runtime");
    // Bounded chunks also avoid the legacy collective count limit.
    constexpr std::size_t chunk = 1024;
    for (std::size_t offset = 0; offset < values.size(); offset += chunk) {
        const auto count = static_cast<int>(std::min(chunk, values.size() - offset));
        check(MPI_Allreduce(
            MPI_IN_PLACE, values.data() + offset, count, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD));
    }
#else
    (void)values;
#endif
}

void Communicator::deterministic_allreduce_sum(std::span<double> values) const {
#ifdef CHIPCAE_ENABLE_MPI
    if (size_ == 1)
        return;
    if (!active())
        throw std::runtime_error("Inactive communication runtime");
    constexpr std::size_t chunk = 1024;
    std::vector<double> gathered(rank_ == 0 ? chunk * static_cast<std::size_t>(size_) : 0);
    for (std::size_t offset = 0; offset < values.size(); offset += chunk) {
        const auto count = static_cast<int>(std::min(chunk, values.size() - offset));
        check(MPI_Gather(values.data() + offset,
                         count,
                         MPI_DOUBLE,
                         gathered.data(),
                         count,
                         MPI_DOUBLE,
                         0,
                         MPI_COMM_WORLD));
        if (rank_ == 0) {
            for (int i = 0; i < count; ++i) {
                double sum = gathered[static_cast<std::size_t>(i)];
                for (int rank = 1; rank < size_; ++rank)
                    sum += gathered[static_cast<std::size_t>(rank) * count + i];
                values[offset + static_cast<std::size_t>(i)] = sum;
            }
        }
        check(MPI_Bcast(values.data() + offset, count, MPI_DOUBLE, 0, MPI_COMM_WORLD));
    }
#else
    (void)values;
#endif
}
} // namespace chipcae
