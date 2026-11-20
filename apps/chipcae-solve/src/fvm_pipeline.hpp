#pragma once
#include "chipcae/solve/case_loader.hpp"

#include <filesystem>

namespace chipcae::solve {
int run_fvm(const RunInputs&, const std::filesystem::path&);
} // namespace chipcae::solve
