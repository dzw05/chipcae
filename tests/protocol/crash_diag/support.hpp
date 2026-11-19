#pragma once

#include "chipcae/core/crash_diag.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <stdlib.h>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace crashdiag_test {
using chipcae::core::CrashDiag;
using chipcae::core::CrashError;
namespace fs = std::filesystem;

inline void check(bool condition, std::string_view why) {
    if (!condition)
        throw std::runtime_error(std::string(why));
}

template <class Exception, class Function>
void expect_throw(Function function, const char* why) {
    bool caught = false;
    try {
        function();
    } catch (const Exception&) {
        caught = true;
    }
    check(caught, why);
}

struct TempDir {
    fs::path path;
    TempDir() {
        auto name = (fs::temp_directory_path() / "chipcae-crashdiag-XXXXXX").string();
        std::vector<char> buffer(name.begin(), name.end());
        buffer.push_back('\0');
        const char* result = mkdtemp(buffer.data());
        check(result != nullptr, "mkdtemp failed");
        path = result;
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

inline std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(input.is_open(), "expected diagnostic file is missing: " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

inline fs::path diagnostic(const fs::path& dir, int rank, std::string_view suffix) {
    return dir / "diag" / ("rank-" + std::to_string(rank) + std::string(suffix));
}

inline void empty_signal(const fs::path& dir, int rank) {
    const auto path = diagnostic(dir, rank, ".signal");
    check(!fs::exists(path) || fs::file_size(path) == 0, "normal path wrote signal evidence");
}

template <class Function>
int run(Function function) {
    try {
        function();
        CrashDiag::finalize();
        return 0;
    } catch (const std::exception& error) {
        CrashDiag::finalize();
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

// Small independent parser: retain numeric tokens until their precise type is known.
// JSON object order is retained so both the frozen key set and order are checked.
struct Value {
    bool is_string{};
    std::string text;
};
struct Json {
    std::map<std::string, Value> values;
    std::vector<std::string> keys;
    const std::string& string(const std::string& key) const {
        const auto& value = values.at(key);
        check(value.is_string, "expected JSON string");
        return value.text;
    }
    template <class Number>
    Number number(const std::string& key) const {
        const auto& value = values.at(key);
        check(!value.is_string, "expected JSON number");
        Number result{};
        const auto end = value.text.data() + value.text.size();
        const auto parsed = std::from_chars(value.text.data(), end, result);
        check(parsed.ec == std::errc{} && parsed.ptr == end, "invalid or out of range number");
        return result;
    }
};

class Parser {
    std::string_view input;
    std::size_t pos{};
    char take() {
        check(pos < input.size(), "truncated JSON");
        return input[pos++];
    }
    void whitespace() {
        while (pos < input.size() && (input[pos] == ' ' || input[pos] == '\n' ||
                                      input[pos] == '\r' || input[pos] == '\t'))
            ++pos;
    }
    void token(char expected) {
        whitespace();
        check(take() == expected, "unexpected JSON token");
    }
    unsigned hex4() {
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = take();
            unsigned digit = 0;
            if (c >= '0' && c <= '9')
                digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                digit = static_cast<unsigned>(c - 'a') + 10;
            else if (c >= 'A' && c <= 'F')
                digit = static_cast<unsigned>(c - 'A') + 10;
            else
                throw std::runtime_error("invalid unicode escape");
            value = value * 16 + digit;
        }
        return value;
    }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 63)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
            out.push_back(static_cast<char>(0x80 | (cp & 63)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 63)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
            out.push_back(static_cast<char>(0x80 | (cp & 63)));
        }
    }
    std::string string() {
        token('"');
        std::string out;
        for (;;) {
            const char c = take();
            if (c == '"')
                return out;
            check(static_cast<unsigned char>(c) >= 0x20, "bare JSON control byte");
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            switch (take()) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                unsigned cp = hex4();
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    check(take() == '\\' && take() == 'u', "missing low surrogate");
                    const unsigned low = hex4();
                    check(low >= 0xdc00 && low <= 0xdfff, "invalid low surrogate");
                    cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                } else {
                    check(cp < 0xdc00 || cp > 0xdfff, "unpaired low surrogate");
                }
                utf8(out, cp);
                break;
            }
            default:
                throw std::runtime_error("invalid JSON escape");
            }
        }
    }
    std::string number() {
        const auto start = pos;
        if (pos < input.size() && input[pos] == '-')
            ++pos;
        check(pos < input.size(), "truncated number");
        if (input[pos] == '0')
            ++pos;
        else {
            check(input[pos] >= '1' && input[pos] <= '9', "invalid JSON number");
            digits();
        }
        if (pos < input.size() && input[pos] == '.') {
            ++pos;
            digits();
        }
        if (pos < input.size() && (input[pos] == 'e' || input[pos] == 'E')) {
            ++pos;
            if (pos < input.size() && (input[pos] == '+' || input[pos] == '-'))
                ++pos;
            digits();
        }
        return std::string(input.substr(start, pos - start));
    }
    void digits() {
        const auto start = pos;
        while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9')
            ++pos;
        check(pos != start, "number requires digits");
    }

public:
    explicit Parser(std::string_view text) : input(text) {}
    Json parse() {
        Json result;
        token('{');
        for (;;) {
            const auto key = string();
            token(':');
            whitespace();
            check(pos < input.size(), "missing JSON value");
            const bool quoted = input[pos] == '"';
            Value value{quoted, quoted ? string() : number()};
            check(result.values.emplace(key, std::move(value)).second, "duplicate JSON key");
            result.keys.push_back(key);
            whitespace();
            const char separator = take();
            if (separator == '}')
                break;
            check(separator == ',', "invalid JSON separator");
        }
        whitespace();
        check(pos == input.size(), "trailing JSON content");
        const std::vector<std::string> required{"schema",
                                                "rank",
                                                "category",
                                                "code",
                                                "error_code",
                                                "module",
                                                "window",
                                                "time",
                                                "message"};
        check(result.keys == required, "diagnostic key set/order differs from contract");
        check(std::isfinite(result.number<double>("time")), "nonfinite JSON time");
        return result;
    }
};

inline Json parse(const fs::path& path) {
    return Parser(read(path)).parse();
}
inline void expect_json(
    const Json& json, int rank, const CrashError& error, std::uint64_t window, double time) {
    check(json.string("schema") == "chipcae.diag.rank/0.1", "schema mismatch");
    check(json.number<int>("rank") == rank, "rank mismatch");
    check(json.string("category") == error.category, "category mismatch");
    check(json.string("code") == error.code, "code mismatch");
    check(json.number<std::uint32_t>("error_code") == error.error_code, "error_code mismatch");
    check(json.string("module") == error.module, "module mismatch");
    check(json.number<std::uint64_t>("window") == window, "window mismatch");
    // The frozen serialization contract requires exact round trips, not numeric tolerance.
    check(json.number<double>("time") == time, "time did not round trip exactly");
    check(json.string("message") == error.message, "message byte mismatch");
}

inline int wait_child(pid_t child) {
    check(child > 0, "failed to start signal child");
    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited == -1 && errno == EINTR);
    check(waited == child, "waitpid failed");
    return status;
}
inline void disable_core_dumps() {
    const rlimit limit{0, 0};
    check(::setrlimit(RLIMIT_CORE, &limit) == 0, "setrlimit failed");
}
inline void fatal_status(int status, int sig) {
    check(WIFEXITED(status) && WEXITSTATUS(status) == 128 + sig,
          "handler must terminate with _exit(128 + sig) (not return or re-raise)");
}
inline void signal_evidence(const fs::path& dir, int rank, std::uint64_t window, int sig) {
    const auto expected =
        "window=" + std::to_string(window) + " signal=" + std::to_string(sig) + "\n";
    check(read(diagnostic(dir, rank, ".signal")) == expected, "signal bytes differ from contract");
    check(!fs::exists(diagnostic(dir, rank, ".json")), "signal handler wrote JSON");
}
} // namespace crashdiag_test
