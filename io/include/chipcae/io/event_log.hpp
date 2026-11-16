#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

namespace chipcae::io {

struct KernelHello { std::string run_id; std::uint32_t attempt{}; std::string kernel_version; };
struct Progress { std::uint64_t window{}; double time{}; double dt{}; double fraction{}; std::string message; };
struct Residual { std::string solver; std::string field; std::uint64_t window{}; std::uint32_t iteration{}; double norm{}; std::uint32_t flags{}; std::string code; std::uint32_t error_code{}; };
struct LogLine { std::int32_t level{}; std::int32_t rank{}; std::string message; std::string code; std::uint32_t error_code{}; };
struct State { enum class Value : std::uint8_t { unspecified=0, starting=1, running=2, paused=3, completed=4, failed=5 }; Value value{Value::starting}; std::uint64_t window{}; double time{}; };
struct OutputAvailable { std::uint64_t window{}; double time{}; std::string path; std::string checksum; };

using EventPayload = std::variant<KernelHello, Progress, Residual, LogLine, State, OutputAvailable>;
struct Event { std::uint64_t seq{}; std::int64_t wall_time_unix_ns{}; EventPayload payload; };

// File format v1 is append-only: each record is "CAELOG1\0", a little-endian
// uint32 payload length, then a little-endian record body (seq, wall-time ns,
// kind byte, and kind-specific fixed-width values/uint32-length-prefixed UTF-8
// strings). Reader::read_from is inclusive and ignores an incomplete final record.
class EventLogWriter {
public:
    explicit EventLogWriter(const std::filesystem::path& path);
    ~EventLogWriter();
    EventLogWriter(const EventLogWriter&) = delete;
    EventLogWriter& operator=(const EventLogWriter&) = delete;
    void append(const Event& event);
    void close();
private:
    class Impl;
    Impl* impl_;
};

class EventLogReader {
public:
    explicit EventLogReader(std::filesystem::path path);
    std::vector<Event> read_from(std::uint64_t seq) const;
private:
    std::filesystem::path path_;
};

} // namespace chipcae::io
