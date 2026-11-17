#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace chipcae::io {

struct Plugin {
    std::string key;
    std::string version;
    bool operator==(const Plugin&) const = default;
};
struct KernelHello {
    std::string run_id;
    std::uint32_t attempt{};
    std::vector<std::string> protocols;
    std::string kernel_version;
    std::string schema_bundle_hash;
    std::vector<Plugin> plugins;
    std::uint32_t ranks{};
    std::uint32_t event_log_format{1};
    std::vector<std::uint32_t> vtkhdf_version;
    std::string case_sha256;
    std::string git_sha;
    std::string protocol_version;
    std::vector<std::string> features;
    bool operator==(const KernelHello&) const = default;
};
struct Progress {
    std::uint64_t window{};
    double time{};
    double dt{};
    double fraction{};
    std::string message;
};
struct Residual {
    std::string solver;
    std::string field;
    std::uint64_t window{};
    std::uint32_t iteration{};
    double norm{};
    std::uint32_t flags{};
    std::string code;
    std::uint32_t error_code{};
};
struct ResidualBatchPoint {
    std::string solver, field;
    std::uint32_t iteration{};
    double norm{};
    std::uint32_t flags{}, linear_iterations{}, window_iterations{};
    bool operator==(const ResidualBatchPoint&) const = default;
};
struct ResidualBatch {
    std::uint64_t window{};
    double time{};
    std::vector<ResidualBatchPoint> points;
    std::uint32_t dropped_intermediate{};
    bool operator==(const ResidualBatch&) const = default;
};
struct LogLine {
    std::int32_t level{};
    std::int32_t rank{};
    std::string message;
    std::string code;
    std::uint32_t error_code{};
};
struct State {
    enum class Value : std::uint8_t {
        unspecified = 0,
        starting = 1,
        running = 2,
        paused = 3,
        completed = 4,
        failed = 5
    };
    Value value{Value::starting};
    std::uint64_t window{};
    double time{};
};
struct OutputAvailable {
    std::uint64_t window{};
    double time{};
    std::string path;
    std::string checksum;
};
struct ControlAck {
    std::uint64_t cmd_seq{};
    std::string client_request_id;
    std::uint32_t status{};
    std::uint64_t effective_step{};
    std::string message;
    double effective_time{};
    bool operator==(const ControlAck&) const = default;
};
struct KernelExit {
    std::int32_t exit_code{};
    std::uint64_t last_completed_window{};
    std::string reason;
    bool operator==(const KernelExit&) const = default;
};

using EventPayload = std::variant<KernelHello,
                                  Progress,
                                  Residual,
                                  LogLine,
                                  State,
                                  OutputAvailable,
                                  ResidualBatch,
                                  ControlAck,
                                  KernelExit>;
struct Event {
    std::uint64_t seq{};
    std::int64_t wall_time_unix_ns{};
    EventPayload payload;
};

// Frozen v1 layout: each record is "CAELOG1\0", le32 body length (max 64 MiB),
// le32 CRC-32 (reflected polynomial 0xEDB88320, init/final XOR 0xFFFFFFFF,
// covering the body), then the body: le64 seq, le64 signed wall_ns, uint8 kind
// equal to the RunEvent oneof field number, then fields in proto field order.
// Numeric fields are little-endian, strings have le32 byte lengths, repeated
// fields have le32 element counts; Plugin contains key then version strings.
// Reads are inclusive by seq and stop at an incomplete or CRC-mismatched tail.
// The writer flushes without fsync: an accepted MVP tradeoff. A crash may lose
// flushed but non-durable records; reopening truncates a torn tail. A dropped
// tail is first copied to <path>.corrupt-<offset>, or <path>.corrupt-<offset>-<n>
// if that name already exists. The library does not rotate or delete these
// files; the operator is responsible for cleaning them up.
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
    std::unique_ptr<Impl> impl_;
};

class EventLogReader {
public:
    explicit EventLogReader(std::filesystem::path path);
    // A cursor is an offset in this log, advanced only past validated records.
    // Invalid boundaries fall back to zero. Use with increasing seq when polling.
    std::vector<Event> read_from(std::uint64_t seq, std::uint64_t* next_offset = nullptr) const;

private:
    std::filesystem::path path_;
    mutable std::set<std::uint64_t> boundaries_{0};
};

} // namespace chipcae::io
