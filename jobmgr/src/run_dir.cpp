#include "run_dir.hpp"

#include "chipcae/jobmgr/local_paths.hpp"
#include "sha256_stream.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace chipcae::jobmgr::detail {
std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot read " + path.string());
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    if (input.bad())
        throw std::runtime_error("Cannot read " + path.string());
    return bytes;
}
Json read_object(const std::filesystem::path& path) {
    auto value = Json::parse(read_file(path));
    if (!value.is_object())
        throw std::runtime_error("Invalid JSON object");
    return value;
}
void snapshot(const std::filesystem::path& source,
              const std::filesystem::path& target,
              Json& hashes,
              std::set<std::filesystem::path>& visited,
              const std::string& expected) {
    auto lowercase = expected;
    std::transform(lowercase.begin(), lowercase.end(), lowercase.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const auto destination = target.lexically_normal();
    if (visited.contains(destination)) {
        stream_copy_hash(source, {}, lowercase);
        return;
    }
    const auto streamed = stream_copy_hash(source, destination, lowercase);
    visited.insert(destination);
    hashes[destination.string()] = streamed.sha256;
    if (!streamed.json_candidate)
        return;
    // Non-JSON assets are copied verbatim. Parse failures are not case-validation failures here.
    Json value;
    try {
        value = Json::parse(read_file(destination));
    } catch (const jsoncons::ser_error&) {
        return;
    }
    const auto visit = [&](auto&& self, const Json& node) -> void {
        if (node.is_array()) {
            for (const auto& child : node.array_range())
                self(self, child);
        } else if (node.is_object()) {
            const bool official_ref = node.contains("ref");
            const bool hashed_path = node.contains("path") && node.contains("sha256");
            if (official_ref || hashed_path) {
                const auto& raw_ref = node.at(official_ref ? "ref" : "path");
                if (raw_ref.is_string()) {
                    const auto ref = raw_ref.as<std::string>();
                    if (!ref.empty()) {
                        check_snapshot_ref(ref);
                        const auto expected_hash =
                            node.contains("sha256") && node.at("sha256").is_string()
                                ? node.at("sha256").as<std::string>()
                                : "";
                        snapshot(source.parent_path() / ref,
                                 destination.parent_path() / ref,
                                 hashes,
                                 visited,
                                 expected_hash);
                    }
                }
            }
            for (const auto& member : node.object_range())
                if (!member.key().starts_with("x-"))
                    self(self, member.value());
        }
    };
    visit(visit, value);
}
void validate_case(const std::filesystem::path& case_path) {
    try {
        const auto value = read_object(case_path);
        const auto resolver = [](const jsoncons::uri& uri) -> Json {
            // The case schema's only external reference is resolved locally, never over HTTP.
            if (uri.path().ends_with("parameters.schema.json"))
                return read_object(std::filesystem::path(CHIPCAE_SCHEMA_DIR) /
                                   "parameters.schema.json");
            throw std::runtime_error("Unknown external schema reference");
        };
        const auto validator = jsoncons::jsonschema::make_json_schema(
            read_object(std::filesystem::path(CHIPCAE_SCHEMA_DIR) / "case-1.0.schema.json"),
            resolver);
        if (!validator.is_valid(value))
            throw std::runtime_error("case schema");
        const auto root = case_path.parent_path();
        const auto materials = root / value.at("materials").at("ref").as<std::string>();
        const auto stack = root / value.at("stack").at("ref").as<std::string>();
        if (!std::filesystem::is_regular_file(materials) ||
            !std::filesystem::is_regular_file(stack))
            throw std::runtime_error("missing materials/stack reference");
        const auto stack_validator = jsoncons::jsonschema::make_json_schema(
            read_object(std::filesystem::path(CHIPCAE_SCHEMA_DIR) / "stack-0.2.schema.json"));
        if (!stack_validator.is_valid(read_object(stack)))
            throw std::runtime_error("stack schema");
        std::set<std::string> ids;
        const auto check_ids = [&](auto&& self, const Json& node) -> void {
            if (node.is_object()) {
                if (node.contains("id") && node.at("id").is_string() &&
                    !ids.insert(node.at("id").as<std::string>()).second)
                    throw std::runtime_error("Duplicate case item id");
                for (const auto& member : node.object_range())
                    if (!member.key().starts_with("x-"))
                        self(self, member.value());
            } else if (node.is_array())
                for (const auto& child : node.array_range())
                    self(self, child);
        };
        check_ids(check_ids, value);
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string("CASE_INVALID: snapshot validation failed: ") +
                                 error.what());
    }
}
void stamp(google::protobuf::Timestamp* timestamp) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now);
    timestamp->set_seconds(seconds.count());
    timestamp->set_nanos(static_cast<int>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds).count()));
}
std::string issued_at() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds);
    const auto time = std::chrono::system_clock::to_time_t(seconds);
    std::tm utc{};
    if (!::gmtime_r(&time, &utc))
        throw std::runtime_error("Cannot format UTC timestamp");
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
           << milliseconds.count() << 'Z';
    return output.str();
}
void persist(const chipcae::jobs::v1::Job& job) {
    write_file_atomic(std::filesystem::path(job.result_dir()) / "job.pb", job.SerializeAsString());
}
void append_control(const std::filesystem::path& path, const std::string& line) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd < 0)
        throw std::runtime_error("Control append failed");
    // Restore the prior end on any failed append; never expose a partial command to recovery.
    const auto end = ::lseek(fd, 0, SEEK_END);
    bool ok = end >= 0;
    std::size_t offset = 0;
    while (ok && offset < line.size()) {
        const auto written = ::write(fd, line.data() + offset, line.size() - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            ok = false;
        else
            offset += static_cast<std::size_t>(written);
    }
    if (ok)
        ok = ::fsync(fd) == 0;
    if (!ok && end >= 0)
        ::ftruncate(fd, end);
    const auto closed = ::close(fd);
    if (!ok || closed != 0)
        throw std::runtime_error("Control append failed");
}
} // namespace chipcae::jobmgr::detail
