#pragma once

#include "chipcae/core/types.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace chipcae {

/// Placeholder std::vector storage: replace after F4 freezes the Kokkos Layout.
/// Agents must not treat this storage or interface as final.
/// A named, densely stored field with one or more components per entry.
class Field {
public:
    Field(std::string name, std::size_t count, int components = 1);

    const std::string& name() const noexcept { return name_; }
    int components() const noexcept { return components_; }
    std::size_t size() const noexcept { return count_; }
    std::size_t value_count() const noexcept { return values_.size(); }

    std::span<Real> values() noexcept { return values_; }
    std::span<const Real> values() const noexcept { return values_; }

private:
    std::string name_;
    std::size_t count_{0};
    int components_{1};
    std::vector<Real> values_;
};

} // namespace chipcae
