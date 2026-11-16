#pragma once
#include "chipcae/core/communicator.hpp"
#include "chipcae/core/memory_space.hpp"

#include <stdexcept>

namespace chipcae::par {
/// Uses the same runtime capability query as Communicator.
bool gpu_aware() noexcept;
/// GPU backends stage through page-locked host memory.
#if defined(KOKKOS_ENABLE_CUDA) || defined(KOKKOS_ENABLE_HIP)
using StagingSpace = Kokkos::SharedHostPinnedSpace;
#else
using StagingSpace = HostSpace;
#endif
template <class T>
using StagingBuffer = View1<T, StagingSpace>;
template <class T, class M>
StagingBuffer<T> stage_to_host(const View1<T, M>& source) {
    StagingBuffer<T> host("halo staging", source.extent(0));
#ifdef CHIPCAE_ENABLE_KOKKOS
    Kokkos::deep_copy(host, source);
#else
    for (std::size_t i = 0; i < source.extent(0); ++i)
        host(i) = source(i);
#endif
    return host;
}
template <class T, class M>
void stage_from_host(const StagingBuffer<T>& host, const View1<T, M>& target) {
    if (host.extent(0) != target.extent(0))
        throw std::invalid_argument("staging extent mismatch");
#ifdef CHIPCAE_ENABLE_KOKKOS
    Kokkos::deep_copy(target, host);
#else
    for (std::size_t i = 0; i < host.extent(0); ++i)
        target(i) = host(i);
#endif
}
} // namespace chipcae::par
