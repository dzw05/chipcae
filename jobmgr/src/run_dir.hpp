#pragma once

#include "chipcae/jobs/v1/jobs.pb.h"

#include <filesystem>
#include <jsoncons/json.hpp>
#include <set>
#include <string>

namespace chipcae::jobmgr::detail {
using Json = jsoncons::json;
std::string read_file(const std::filesystem::path& path);
Json read_object(const std::filesystem::path& path);
void snapshot(const std::filesystem::path& source,
              const std::filesystem::path& target,
              Json& hashes,
              std::set<std::filesystem::path>& visited,
              const std::string& expected = {});
void validate_case(const std::filesystem::path& case_path);
void stamp(google::protobuf::Timestamp* timestamp);
std::string issued_at();
void persist(const chipcae::jobs::v1::Job& job);
void append_control(const std::filesystem::path& path, const std::string& line);
} // namespace chipcae::jobmgr::detail
