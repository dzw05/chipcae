#pragma once
#include "chipcae/geometry/ecad/ipc2581.hpp"
namespace chipcae::geometry::ecad {
ImportResult read_ipc2581_zip(const std::filesystem::path& file);
}
