#include "chipcae/io/control_file.hpp"

#include <fstream>
#include <regex>
#include <stdexcept>
#include <utility>

namespace chipcae::io {
namespace {
std::string string_field(const std::string& line, const char* name) {
    const std::regex re(std::string("\\\"") + name + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"");
    std::smatch m;
    return std::regex_search(line, m, re) ? m[1].str() : std::string{};
}
std::uint64_t integer_field(const std::string& line, const char* name) {
    const std::regex re(std::string("\\\"") + name + "\\\"\\s*:\\s*(?:\\\"(\\d+)\\\"|(\\d+))");
    std::smatch m;
    if (!std::regex_search(line, m, re))
        throw std::runtime_error(std::string("control command missing ") + name);
    return std::stoull(m[m[1].matched ? 1 : 2].str());
}
} // namespace
ControlFileReader::ControlFileReader(std::filesystem::path path,
                                     std::function<void(const std::string&)> warning_sink)
    : path_(std::move(path)), warning_sink_(std::move(warning_sink)) {}
std::vector<ControlCommand> ControlFileReader::read_available() {
    std::vector<ControlCommand> result;
    std::ifstream in(path_, std::ios::binary);
    if (!in)
        return result;
    in.seekg(static_cast<std::streamoff>(offset_));
    std::string bytes((std::istreambuf_iterator<char>(in)), {});
    std::size_t start = 0;
    while (true) {
        const auto newline = bytes.find('\n', start);
        if (newline == std::string::npos)
            break;
        std::string line = bytes.substr(start, newline - start);
        start = newline + 1;
        offset_ += line.size() + 1;
        if (line.empty())
            continue;
        try {
            ControlCommand c;
            c.cmd_seq = integer_field(line, "cmd_seq");
            if (c.cmd_seq == 0 || c.cmd_seq <= last_sequence_)
                continue;
            c.client_request_id = string_field(line, "client_request_id");
            const auto action = string_field(line, "action");
            if (action == "PAUSE")
                c.action = ControlAction::pause;
            else if (action == "RESUME")
                c.action = ControlAction::resume;
            else if (action == "STOP")
                c.action = ControlAction::stop;
            else if (action == "SET_OUTPUT_INTERVAL")
                c.action = ControlAction::set_output_interval;
            else if (action == "WRITE_OUTPUT_NOW")
                c.action = ControlAction::write_output_now;
            else if (action == "CHECKPOINT_AND_STOP")
                c.action = ControlAction::checkpoint_and_stop;
            else
                throw std::runtime_error("unknown control action");
            std::smatch m;
            if (std::regex_search(
                    line,
                    m,
                    std::regex(
                        "\\\"output_interval_steps\\\"\\s*:\\s*(?:\\\"(\\d+)\\\"|(\\d+))"))) {
                c.has_interval_steps = true;
                c.output_interval_steps = std::stoull(m[m[1].matched ? 1 : 2].str());
            }
            if (std::regex_search(
                    line,
                    m,
                    std::regex("\\\"output_interval_time\\\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]"
                               "+)?(?:[eE][+-]?[0-9]+)?)"))) {
                c.has_interval_time = true;
                c.output_interval_time = std::stod(m[1].str());
            }
            last_sequence_ = c.cmd_seq;
            result.push_back(std::move(c));
        } catch (const std::exception& error) {
            if (warning_sink_)
                warning_sink_(std::string("invalid control.jsonl line skipped: ") + error.what());
        }
    }
    return result;
}
} // namespace chipcae::io
