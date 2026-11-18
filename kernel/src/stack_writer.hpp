#pragma once
#include "chipcae/schema/stack.hpp"

#include <filesystem>
namespace chipcae::importer {
mesh::Expected<bool> write_stack(const mesh::StackModel&, const std::filesystem::path&);
}
