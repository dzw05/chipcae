#pragma once
#include "archive.hpp"
#include "chipcae/geometry/ecad/ipc2581.hpp"
#include "odb_job.hpp"

namespace chipcae::geometry::ecad::detail {
std::string lower(std::string);
ImportResult read_odbpp_job(const std::filesystem::path&, OdbJob&);
bool parse_matrix(const ArchiveReader&, OdbJob&, std::string&);
bool parse_misc(const ArchiveReader&, OdbJob&, std::string&);
bool parse_features(const ArchiveReader&, OdbJob&, ImportResult&);
std::string layer_directory(const ArchiveReader&, const OdbJob&, const std::string&);
std::string trim(std::string_view);
} // namespace chipcae::geometry::ecad::detail
