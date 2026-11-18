#pragma once

#include "chipcae/io/control_file.hpp"
#include "chipcae/io/event_log.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace chipcae::coupling {
struct WindowMark {
    std::uint64_t window{};
    double time{};
    bool operator==(const WindowMark&) const = default;
};

// TEST-FIRST STUBS: signature-only scaffolding; no control-channel behavior yet.
class ControlChannel {
public:
    explicit ControlChannel(std::filesystem::path, std::function<void(const std::string&)> = {}) {}
    std::vector<io::ControlCommand> poll(const WindowMark&) { return {}; }
    std::uint64_t last_sequence() const noexcept { return 0; }
};

inline io::ControlAck
make_control_ack(const io::ControlCommand&, std::uint32_t, const WindowMark&, std::string = {}) {
    return {};
}

namespace detail {
inline std::vector<std::uint8_t> encode_commands(const std::vector<io::ControlCommand>&) {
    return {};
}
inline std::vector<io::ControlCommand> decode_commands(std::span<const std::uint8_t>) {
    return {};
}
} // namespace detail
} // namespace chipcae::coupling
