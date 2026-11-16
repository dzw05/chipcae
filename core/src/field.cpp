#include "chipcae/core/field.hpp"

#include <stdexcept>

namespace chipcae {

Field::Field(std::string name, std::size_t count, int components)
    : name_(std::move(name)), count_(count), components_(components) {
    if (components_ <= 0) {
        throw std::invalid_argument("Field components must be positive");
    }
    values_.assign(count_ * static_cast<std::size_t>(components_), Real{0});
}

} // namespace chipcae
