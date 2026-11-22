#pragma once

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <stdlib.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace jobmgr_test {
namespace fs = std::filesystem;

inline void require(bool condition, std::string_view why) {
    if (!condition)
        throw std::runtime_error(std::string(why));
}
inline void near(double actual, double expected) {
    require(std::abs(actual - expected) <= 1e-10 + 5.5e-6 * std::abs(expected),
            "numeric field differs from expected value");
}
template <class F>
void throws_message(F&& action, std::string_view expected) {
    try {
        action();
    } catch (const std::exception& e) {
        require(e.what() == expected,
                "expected exception '" + std::string(expected) + "', got '" + e.what() + "'");
        return;
    }
    throw std::runtime_error("expected exception '" + std::string(expected) + "', none thrown");
}
inline std::string read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(stream.good(), "cannot read fixture/output " + path.string());
    return {std::istreambuf_iterator<char>(stream), {}};
}
inline void write(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    require(!stream.fail(), "cannot write test fixture " + path.string());
}
class TempDirectory {
public:
    TempDirectory() {
        auto pattern = (fs::temp_directory_path() / "chipcae-jobmgr-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        const auto result = mkdtemp(buffer.data());
        require(result != nullptr, "mkdtemp failed");
        path = result;
    }
    ~TempDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    fs::path path;
};
class Suite {
public:
    template <class F>
    void test(std::string_view name, F&& body) {
        try {
            body();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& e) {
            ++failures_;
            std::cerr << "FAIL: " << name << ": " << e.what() << '\n';
        }
    }
    int result() const { return failures_ == 0 ? EXIT_SUCCESS : EXIT_FAILURE; }

private:
    int failures_ = 0;
};
} // namespace jobmgr_test
