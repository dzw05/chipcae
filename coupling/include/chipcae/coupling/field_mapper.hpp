#pragma once

#include "chipcae/core/field.hpp"

namespace chipcae::coupling {

class FieldMapper {
public:
    /// Copy values when the source and target layouts currently match.
    /// Conservative and interpolating maps will be added later.
    void map(const Field& source, Field& target) const;
};

} // namespace chipcae::coupling
