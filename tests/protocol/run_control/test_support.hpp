#pragma once

#include "chipcae/coupling/run_control.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace runctl_test {
using namespace std::chrono_literals;
using chipcae::coupling::BoundaryDecision;
using chipcae::coupling::ControlCapabilities;
using chipcae::coupling::ControlChannel;
using chipcae::coupling::RunControl;
using chipcae::coupling::RunControlOptions;
using chipcae::coupling::WindowMark;
using chipcae::io::ControlAck;
using chipcae::io::EventPayload;
using chipcae::io::State;
using Events = std::vector<EventPayload>;

inline void require(bool condition, const std::string& why) {
    if (!condition)
        throw std::runtime_error(why);
}

class TempDirectory {
public:
    TempDirectory() {
        const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            path_ = std::filesystem::temp_directory_path() /
                    ("runctl-" + std::to_string(seed) + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(path_))
                return;
        }
        throw std::runtime_error("could not create unique test directory");
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    std::filesystem::path file(std::string_view name = "control.jsonl") const {
        return path_ / name;
    }

private:
    std::filesystem::path path_;
};

inline void append(const std::filesystem::path& path,
                   std::uint64_t seq,
                   std::string_view action,
                   std::string_view parameters = {}) {
    std::ofstream out;
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out.open(path, std::ios::app);
    out << "{\"cmd_seq\":\"" << seq << "\",\"client_request_id\":\"request-" << seq
        << "\",\"action\":\"" << action << '"' << parameters << "}\n";
    out.close();
}

// Expected values come directly from the contract, never from make_control_ack().
inline EventPayload
ack(std::uint64_t seq, std::uint32_t status, WindowMark mark, std::string message = {}) {
    return ControlAck{
        seq, "request-" + std::to_string(seq), status, mark.window, std::move(message), mark.time};
}
inline EventPayload state(State::Value value, WindowMark mark) {
    return State{value, mark.window, mark.time};
}
inline void expect_events(const Events& actual, const Events& expected) {
    require(actual.size() == expected.size(),
            "event count: expected " + std::to_string(expected.size()) + ", got " +
                std::to_string(actual.size()));
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto context = "event " + std::to_string(i);
        require(actual[i].index() == expected[i].index(), context + ": variant index");
        if (const auto* wanted = std::get_if<ControlAck>(&expected[i])) {
            require(std::get<ControlAck>(actual[i]) == *wanted, context + ": full ControlAck");
        } else if (const auto* wanted_state = std::get_if<State>(&expected[i])) {
            const auto& got = std::get<State>(actual[i]);
            require(got.value == wanted_state->value && got.window == wanted_state->window &&
                        got.time == wanted_state->time,
                    context + ": State value/window/time");
        } else {
            throw std::runtime_error("unexpected expected payload kind");
        }
    }
}

inline std::function<void(EventPayload)> collect(Events& events) {
    return [&events](EventPayload event) { events.push_back(std::move(event)); };
}
inline RunControlOptions no_wait() {
    RunControlOptions options;
    options.sleep = [](auto) { throw std::runtime_error("unexpected sleep"); };
    return options;
}

class Suite {
public:
    template <typename Function>
    void run(const char* name, Function test) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures_;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    int result() const { return failures_ == 0 ? 0 : 1; }

private:
    int failures_{};
};
} // namespace runctl_test
