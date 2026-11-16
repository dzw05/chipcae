#pragma once

#include "chipcae/core/types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace chipcae {

enum class ParameterType { boolean, integer, real, string };

struct ParameterDecl {
    std::string name;
    ParameterType type{ParameterType::string};
    std::string default_value;
    std::string description;
};

class ParameterSet {
public:
    void declare(ParameterDecl decl);
    void set(std::string_view name, std::string value);

    const ParameterDecl* find(std::string_view name) const;
    std::optional<std::string> get(std::string_view name) const;
    const std::vector<ParameterDecl>& declarations() const noexcept { return declarations_; }

private:
    std::vector<ParameterDecl> declarations_;
    std::unordered_map<std::string, std::string> values_;
};

} // namespace chipcae
