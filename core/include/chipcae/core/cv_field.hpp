#pragma once

#include "chipcae/core/memory_space.hpp"
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace chipcae {
/// Dense CV values: cv is the contiguous dimension; components are slowest.
struct FieldDesc {
    std::string name;
    std::size_t cv_count = 0;
    std::size_t components = 1;
};

#ifndef CHIPCAE_ENABLE_KOKKOS
/// Host equivalent of a rank-two LayoutLeft view, with shared ownership.
template<class M>
class CvValues {
public:
    using memory_space = M;
    CvValues(const std::string& name, std::size_t rows, std::size_t cols)
        : values_(name, rows * cols), rows_(rows), cols_(cols) {}
    double& operator()(std::size_t cv, std::size_t comp) const noexcept {
        return values_(cv + rows_ * comp);
    }
    double* data() const noexcept { return values_.data(); }
    std::size_t extent(std::size_t dim) const noexcept { return dim == 0 ? rows_ : cols_; }
private:
    View1<double, M> values_;
    std::size_t rows_, cols_;
};
#endif

template<class M = DeviceSpace>
class CvField {
public:
#ifdef CHIPCAE_ENABLE_KOKKOS
    using value_view = Kokkos::View<double**, Kokkos::LayoutLeft, M>;
#else
    using value_view = CvValues<M>;
#endif
    /// Offsets are host input: [0, ..., cv_count], one interval per block.
    /// Omitted offsets create a single block, including for an empty field.
    explicit CvField(FieldDesc desc, std::vector<std::int32_t> offsets = {})
        : desc_(validate(std::move(desc), offsets)),
          values_(desc_.name, desc_.cv_count, desc_.components),
          offsets_(desc_.name + "_block_offset", offsets.size()) {
#ifdef CHIPCAE_ENABLE_KOKKOS
        auto host = Kokkos::create_mirror_view(offsets_);
        for (std::size_t i = 0; i < offsets.size(); ++i) host(i) = offsets[i];
        Kokkos::deep_copy(offsets_, host);
#else
        for (std::size_t i = 0; i < offsets.size(); ++i) offsets_(i) = offsets[i];
#endif
    }
    const FieldDesc& desc() const noexcept { return desc_; }
    /// Copy this view into kernels; the descriptor and field object are host metadata.
    value_view raw() { return values_; }
    View1<const std::int32_t, M> block_offset() const { return offsets_; }
private:
    static FieldDesc validate(FieldDesc desc, std::vector<std::int32_t>& offsets) {
        if (desc.components == 0 ||
            desc.cv_count > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
            desc.cv_count > std::numeric_limits<std::size_t>::max() / desc.components / sizeof(double))
            throw std::invalid_argument("Invalid field dimensions");
        if (offsets.empty()) offsets = {0, static_cast<std::int32_t>(desc.cv_count)};
        if (offsets.size() < 2 || offsets.front() != 0 ||
            offsets.back() != static_cast<std::int32_t>(desc.cv_count))
            throw std::invalid_argument("Block offsets must span the field");
        for (std::size_t i = 1; i < offsets.size(); ++i)
            if (offsets[i] < offsets[i - 1])
                throw std::invalid_argument("Block offsets must be nondecreasing");
        return desc;
    }
    FieldDesc desc_;
    value_view values_;
    View1<std::int32_t, M> offsets_;
};
} // namespace chipcae
