#pragma once

#include "chipcae/coupling/residual_decimator.hpp"

#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace resdec_test {
using chipcae::coupling::ResidualBatchSink;
using chipcae::coupling::ResidualDecimator;
using chipcae::coupling::ResidualDecimatorOptions;
using chipcae::coupling::ResidualRecord;
using chipcae::coupling::SpecialKind;
using chipcae::coupling::WindowSummary;
using chipcae::io::ResidualBatch;
using chipcae::io::ResidualBatchPoint;
namespace flag = chipcae::coupling::residual_flag;
using Batches = std::vector<ResidualBatch>;

inline void require(bool condition, const std::string& why) {
    if (!condition)
        throw std::runtime_error(why);
}

template <typename Function>
void expect_invalid_argument(Function action, const std::string& why) {
    try {
        action();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(why + ": expected std::invalid_argument");
}

inline ResidualBatchSink collect(Batches& batches) {
    return [&batches](const ResidualBatch& batch) { batches.push_back(batch); };
}

inline void expect_batches(const Batches& actual, const Batches& expected) {
    require(actual.size() == expected.size(),
            "batch count: expected " + std::to_string(expected.size()) + ", got " +
                std::to_string(actual.size()));
    for (std::size_t i = 0; i < expected.size(); ++i)
        require(actual[i] == expected[i], "batch " + std::to_string(i) + ": full equality");
}

// The contract preserves norm bits, including NaN payloads. Compare every other field too.
inline void expect_batch_bits(const ResidualBatch& actual, const ResidualBatch& expected) {
    require(actual.window == expected.window, "batch window");
    require(actual.time == expected.time, "batch time");
    require(actual.dropped_intermediate == expected.dropped_intermediate, "batch dropped count");
    require(actual.points.size() == expected.points.size(), "batch point count");
    for (std::size_t i = 0; i < expected.points.size(); ++i) {
        const auto& got = actual.points[i];
        const auto& want = expected.points[i];
        const auto context = "point " + std::to_string(i);
        require(got.solver == want.solver, context + ": solver");
        require(got.field == want.field, context + ": field");
        require(got.iteration == want.iteration, context + ": iteration");
        require(std::bit_cast<std::uint64_t>(got.norm) == std::bit_cast<std::uint64_t>(want.norm),
                context + ": norm bits");
        require(got.flags == want.flags, context + ": flags");
        require(got.linear_iterations == want.linear_iterations, context + ": linear iterations");
        require(got.window_iterations == want.window_iterations, context + ": window iterations");
    }
}

class TempDirectory {
public:
    TempDirectory() {
        const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            path_ = std::filesystem::temp_directory_path() /
                    ("resdec-" + std::to_string(seed) + "-" + std::to_string(attempt));
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
    std::filesystem::path file(std::string_view name = "events.bin") const { return path_ / name; }

private:
    std::filesystem::path path_;
};

class Suite {
public:
    template <typename Function>
    void run(std::string_view name, Function test) {
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
} // namespace resdec_test
