#include "chipcae/coupling/control_channel.hpp"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <variant>

int main() {
    const chipcae::io::ControlCommand command{5, "req-5", chipcae::io::ControlAction::stop};
    const chipcae::io::ControlAck expected{5, "req-5", 1, 7, "ok", 0.35};
    const auto ack = chipcae::coupling::make_control_ack(command, 1, {7, 0.35}, "ok");
    if (!(ack == expected))
        throw std::runtime_error("ack must copy command, status, message and completed window");
    const auto path =
        std::filesystem::temp_directory_path() /
        ("ctlch-ack-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".bin");
    {
        chipcae::io::EventLogWriter writer(path);
        writer.append({1, 0, ack});
    }
    const auto events = chipcae::io::EventLogReader(path).read_from(1);
    if (events.size() != 1 || !std::holds_alternative<chipcae::io::ControlAck>(events[0].payload) ||
        !(std::get<chipcae::io::ControlAck>(events[0].payload) == expected))
        throw std::runtime_error("event-log ack round trip");
    std::filesystem::remove(path);
}
