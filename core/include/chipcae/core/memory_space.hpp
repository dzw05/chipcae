#pragma once

#ifdef CHIPCAE_ENABLE_KOKKOS
#include <Kokkos_Core.hpp>
#else
#include <cstddef>
#include <memory>
#include <string>
#include <type_traits>
#endif

namespace chipcae {
#ifdef CHIPCAE_ENABLE_KOKKOS
using HostSpace = Kokkos::HostSpace;
using DeviceSpace = Kokkos::DefaultExecutionSpace::memory_space;
using ExecSpace = Kokkos::DefaultExecutionSpace;
template <class T, class M = DeviceSpace>
using View1 = Kokkos::View<T*, Kokkos::LayoutLeft, M>;
#else
struct HostSpace {};
using DeviceSpace = HostSpace;
using ExecSpace = HostSpace;

/// Contiguous 1D view. Copies share ownership; pointer construction borrows storage.
/// A borrowed buffer must outlive every view of it. Indexing is unchecked.
template <class T, class M = DeviceSpace>
class View1 {
    template <class, class>
    friend class View1;

public:
    using memory_space = M;
    using value_type = T;
    View1() = default;
    View1(const std::string&, std::size_t count)
        : owner_(new std::remove_const_t<T>[count] {}), data_(owner_.get()), count_(count) {}
    View1(T* data, std::size_t count) : data_(data), count_(count) {}
    template <class U>
        requires std::is_convertible_v<U*, T*>
    View1(const View1<U, M>& other)
        : owner_(other.owner_), data_(other.data_), count_(other.count_) {}
    T& operator()(std::size_t i) const noexcept { return data_[i]; }
    T* data() const noexcept { return data_; }
    std::size_t extent(std::size_t dim) const noexcept { return dim == 0 ? count_ : 1; }
    std::size_t size() const noexcept { return count_; }

private:
    std::shared_ptr<std::remove_const_t<T>[]> owner_;
    T* data_ = nullptr;
    std::size_t count_ = 0;
};
#endif
} // namespace chipcae
