#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace chipcae::geometry {
struct AcceptEntry {
    std::string defect_id, note;
};
struct AcceptList {
    std::filesystem::path source;
    std::vector<AcceptEntry> entries;
};
std::filesystem::path default_accept_file(const std::filesystem::path& input);
std::optional<AcceptList> read_accept_file(const std::filesystem::path& path,
                                           std::string* error,
                                           std::vector<std::string>* warnings = nullptr);
std::vector<std::string> accepted_ids(const AcceptList& list);
} // namespace chipcae::geometry
