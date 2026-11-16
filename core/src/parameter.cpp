#include "chipcae/core/parameter.hpp"

#include <stdexcept>
#include <utility>

namespace chipcae {

void ParameterSet::declare(ParameterDecl decl) {
    if (decl.name.empty()) {
        throw std::invalid_argument("Parameter name must not be empty");
    }
    if (find(decl.name) != nullptr) {
        throw std::invalid_argument("Parameter already declared: " + decl.name);
    }
    if (values_.find(decl.name) == values_.end()) {
        values_.emplace(decl.name, decl.default_value);
    }
    declarations_.push_back(std::move(decl));
}

void ParameterSet::set(std::string_view name, std::string value) {
    const auto* decl = find(name);
    if (decl == nullptr) {
        throw std::invalid_argument("Unknown parameter: " + std::string(name));
    }
    values_[decl->name] = std::move(value);
}

const ParameterDecl* ParameterSet::find(std::string_view name) const {
    for (const auto& decl : declarations_) {
        if (decl.name == name) {
            return &decl;
        }
    }
    return nullptr;
}

std::optional<std::string> ParameterSet::get(std::string_view name) const {
    const auto* decl = find(name);
    if (decl == nullptr) {
        return std::nullopt;
    }
    const auto it = values_.find(decl->name);
    if (it == values_.end()) {
        return decl->default_value;
    }
    return it->second;
}

} // namespace chipcae
