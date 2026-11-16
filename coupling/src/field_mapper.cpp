#include "chipcae/coupling/field_mapper.hpp"

#include <stdexcept>

namespace chipcae::coupling {

void FieldMapper::map(const Field& source, Field& target) const {
    if (source.components() != target.components() || source.size() != target.size()) {
        throw std::invalid_argument("FieldMapper placeholder requires matching layouts");
    }
    auto src = source.values();
    auto dst = target.values();
    for (std::size_t i = 0; i < dst.size(); ++i) {
        dst[i] = src[i];
    }
}

} // namespace chipcae::coupling
