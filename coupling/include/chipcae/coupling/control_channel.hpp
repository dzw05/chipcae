#pragma once

#include "chipcae/io/control_file.hpp"
#include "chipcae/io/event_log.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace chipcae::coupling {
struct WindowMark {
    std::uint64_t window{};
    double time{};
    bool operator==(const WindowMark&) const = default;
};

/// Collective over MPI_COMM_WORLD when MPI is active; only rank 0 accesses the file.
class ControlChannel {
public:
    explicit ControlChannel(std::filesystem::path control_file,
                            std::function<void(const std::string&)> warning_sink = {});
    /// Poll after the completed window. Equal windows are allowed; rollback is rejected.
    std::vector<io::ControlCommand> poll(const WindowMark& completed);
    /// Largest accepted command sequence, initially zero.
    std::uint64_t last_sequence() const noexcept;

private:
    std::unique_ptr<io::ControlFileReader> reader_;
    std::uint64_t last_sequence_{};
    std::uint64_t previous_window_{};
};

/// Copy command identity and map the completed window/time to the effective step/time.
io::ControlAck make_control_ack(const io::ControlCommand& command,
                                std::uint32_t status,
                                const WindowMark& completed,
                                std::string message = {});

namespace detail {
/// Internal little-endian wire format: count, then sequence, length-prefixed ID,
/// action, interval-steps flag/value and interval-time flag/IEEE-754 bits.
std::vector<std::uint8_t> encode_commands(const std::vector<io::ControlCommand>& commands);
/// Reject truncated data, invalid action/boolean bytes and trailing data.
std::vector<io::ControlCommand> decode_commands(std::span<const std::uint8_t> bytes);
} // namespace detail
} // namespace chipcae::coupling
