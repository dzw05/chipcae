#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace chipcae::io {
enum class ControlAction {
    pause,
    resume,
    stop,
    set_output_interval,
    write_output_now,
    checkpoint_and_stop
};
struct ControlCommand {
    std::uint64_t cmd_seq{};
    std::string client_request_id;
    ControlAction action{};
    bool has_interval_steps{};
    std::uint64_t output_interval_steps{};
    bool has_interval_time{};
    double output_interval_time{};
};

// Incrementally reads complete proto3-JSON command lines. A trailing partial line
// remains unread until a subsequent call observes its newline.
class ControlFileReader {
public:
    explicit ControlFileReader(std::filesystem::path path);
    std::vector<ControlCommand> read_available();
    std::uint64_t last_sequence() const noexcept { return last_sequence_; }

private:
    std::filesystem::path path_;
    std::uintmax_t offset_{};
    std::uint64_t last_sequence_{};
};
} // namespace chipcae::io
