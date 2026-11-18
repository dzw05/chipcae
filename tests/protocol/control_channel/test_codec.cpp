#include "chipcae/coupling/control_channel.hpp"

#include <array>
#include <bit>
#include <limits>
#include <stdexcept>

using chipcae::io::ControlCommand;
using namespace chipcae::coupling::detail;
namespace {
void expect(bool ok, const char* why) {
    if (!ok)
        throw std::runtime_error(why);
}
void round_trip(const std::vector<ControlCommand>& commands) {
    const auto actual = decode_commands(encode_commands(commands));
    expect(actual.size() == commands.size(), "round-trip count");
    for (std::size_t i = 0; i < commands.size(); ++i) {
        const auto& a = actual[i];
        const auto& b = commands[i];
        expect(a.cmd_seq == b.cmd_seq && a.client_request_id == b.client_request_id &&
                   a.action == b.action && a.has_interval_steps == b.has_interval_steps &&
                   a.output_interval_steps == b.output_interval_steps &&
                   a.has_interval_time == b.has_interval_time &&
                   std::bit_cast<std::uint64_t>(a.output_interval_time) ==
                       std::bit_cast<std::uint64_t>(b.output_interval_time),
               "round-trip fields and double bits");
    }
}
void malformed(const std::vector<std::uint8_t>& bytes) {
    bool rejected = false;
    try {
        (void)decode_commands(bytes);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    expect(rejected, "malformed bytes must throw runtime_error");
}
} // namespace
int main() {
    round_trip({});
    const ControlCommand single{2, "ab", chipcae::io::ControlAction::stop};
    round_trip({single});
    std::vector<ControlCommand> commands;
    std::string utf8;
    for (int i = 0; i < 2048; ++i)
        utf8 += "\xc3\xa9";
    for (int i = 0; i < 6; ++i)
        commands.push_back(
            {i == 5 ? std::numeric_limits<std::uint64_t>::max() : static_cast<std::uint64_t>(i),
             i == 0 ? "" : utf8,
             static_cast<chipcae::io::ControlAction>(i),
             i % 2 == 0,
             std::numeric_limits<std::uint64_t>::max(),
             true,
             i % 2 == 0 ? -0.0 : 1e-300});
    round_trip(commands);
    constexpr std::array<std::uint8_t, 41> golden{1, 0, 0, 0, 0, 0, 0,    0,    2, 0, 0, 0, 0, 0,
                                                  0, 0, 2, 0, 0, 0, 0x61, 0x62, 2, 0, 0, 0, 0, 0,
                                                  0, 0, 0, 0, 0, 0, 0,    0,    0, 0, 0, 0, 0};
    const std::vector<std::uint8_t> expected(golden.begin(), golden.end());
    expect(encode_commands({single}) == expected, "41-byte little-endian golden");
    auto bytes = expected;
    bytes.pop_back();
    malformed(bytes);
    bytes = expected;
    bytes.push_back(0);
    malformed(bytes);
    bytes = expected;
    bytes[22] = 6;
    malformed(bytes);
    bytes = expected;
    bytes[23] = 2;
    malformed(bytes);
    bytes = expected;
    bytes[32] = 2;
    malformed(bytes);
    malformed({});
    expect(decode_commands(std::array<std::uint8_t, 8>{}).empty(), "zero count decodes empty");
}
