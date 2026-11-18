#include "chipcae/coupling/control_channel.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {
void expect(bool ok, const char* why) {
    if (!ok)
        throw std::runtime_error(why);
}
} // namespace
int main() {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("ctlch-serial-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(dir);
    const auto path = dir / "control.jsonl";
    int warnings = 0;
    chipcae::coupling::ControlChannel channel(path, [&](const std::string&) { ++warnings; });
    expect(channel.last_sequence() == 0, "initial sequence");
    expect(channel.poll({0, 0.0}).empty(), "missing file returns no commands");
    expect(channel.last_sequence() == 0 && warnings == 0 && !std::filesystem::exists(path),
           "missing file must not warn or create file");
    auto append = [&](const std::string& text) {
        std::ofstream out(path, std::ios::app);
        out << text;
        expect(static_cast<bool>(out), "append fixture");
    };
    append("{\"cmd_seq\":\"1\",\"action\":\"PAUSE\"}\n"
           "{\"cmd_seq\":\"2\",\"action\":\"RESUME\"}\n"
           "{\"cmd_seq\":\"3\",\"action\":\"STOP\"");
    const auto first = channel.poll({1, 0.1});
    expect(first.size() == 2, "two complete commands must be delivered");
    expect(first[0].cmd_seq == 1 && first[0].action == chipcae::io::ControlAction::pause &&
               first[1].cmd_seq == 2 && first[1].action == chipcae::io::ControlAction::resume,
           "PAUSE and RESUME order and sequences");
    expect(channel.last_sequence() == 2 && warnings == 0, "partial line must remain unread");
    append("}\n");
    const auto second = channel.poll({1, 0.1});
    expect(second.size() == 1 && second[0].cmd_seq == 3 &&
               second[0].action == chipcae::io::ControlAction::stop,
           "same-window partial completion");
    append("{\"cmd_seq\":\"2\",\"action\":\"STOP\"}\n"
           "{\"cmd_seq\":\"4\",\"action\":\"stop\"}\n");
    expect(channel.poll({2, 0.2}).empty(), "duplicate and lowercase action ignored");
    expect(warnings == 1 && channel.last_sequence() == 3,
           "exact warning count and accepted sequence");
    bool rejected = false;
    try {
        (void)channel.poll({1, 0.1});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    expect(rejected, "window rollback must throw invalid_argument");
    const auto failure_path = dir / "failure.jsonl";
    {
        std::ofstream out(failure_path);
        out << "{\"cmd_seq\":\"7\",\"action\":\"PAUSE\"}\n"
               "{\"cmd_seq\":\"8\",\"action\":\"invalid\"}\n";
    }
    chipcae::coupling::ControlChannel failing(failure_path, [](const std::string&) {
        throw std::runtime_error("injected warning failure");
    });
    bool caught = false;
    try {
        (void)failing.poll({1, 0.1});
    } catch (const std::runtime_error&) {
        caught = true;
    }
    expect(caught && failing.last_sequence() == 7, "failure must preserve consumed sequence");
    expect(failing.poll({0, 0.0}).empty(),
           "failed poll must not advance window or replay commands");
    std::filesystem::remove_all(dir);
}
