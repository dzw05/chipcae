#pragma once
#include "chipcae/geometry/ecad/ipc2581.hpp"

namespace chipcae::geometry::ecad {
ImportResult read_odbpp(const std::filesystem::path& path);
}
