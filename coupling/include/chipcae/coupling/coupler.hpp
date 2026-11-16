#pragma once

#include "chipcae/core/field.hpp"
#include "chipcae/coupling/field_mapper.hpp"

#include <string>
#include <vector>

namespace chipcae::coupling {

/// In-house coupler for exchanging fields between solvers in this repository.
class Coupler {
public:
    void add_field(const Field* field);
    void exchange();
    std::size_t field_count() const noexcept { return fields_.size(); }

private:
    FieldMapper mapper_{};
    std::vector<const Field*> fields_{};
};

} // namespace chipcae::coupling
