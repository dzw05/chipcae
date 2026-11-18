#include "support.hpp"

#include <algorithm>
#include <initializer_list>
#include <regex>
#include <spawn.h>

extern char** environ;
using namespace crashdiag_test;

namespace {
// Erase comments and literals without changing offsets; braces/calls inside them
// cannot accidentally enlarge the inspected function or satisfy its whitelist.
std::string code_only(std::string source) {
    enum class Mode { code, line, block, string, character } mode = Mode::code;
    for (std::size_t i = 0; i < source.size(); ++i) {
        const char c = source[i];
        const char next = i + 1 < source.size() ? source[i + 1] : '\0';
        if (mode == Mode::code) {
            if (c == '/' && (next == '/' || next == '*')) {
                mode = next == '/' ? Mode::line : Mode::block;
                source[i] = source[i + 1] = ' ';
                ++i;
            } else if (c == '"' || c == '\'') {
                mode = c == '"' ? Mode::string : Mode::character;
                source[i] = ' ';
            }
        } else if (mode == Mode::line) {
            if (c == '\n')
                mode = Mode::code;
            else
                source[i] = ' ';
        } else if (mode == Mode::block) {
            source[i] = ' ';
            if (c == '*' && next == '/') {
                source[++i] = ' ';
                mode = Mode::code;
            }
        } else {
            source[i] = ' ';
            if (c == '\\' && i + 1 < source.size())
                source[++i] = ' ';
            else if ((mode == Mode::string && c == '"') || (mode == Mode::character && c == '\''))
                mode = Mode::code;
        }
    }
    return source;
}

std::string body(const std::string& source, const std::regex& signature) {
    std::smatch match;
    check(std::regex_search(source, match, signature), "required signal-path function is missing");
    const auto start = static_cast<std::size_t>(match.position() + match.length());
    int depth = 1;
    for (auto pos = start; pos < source.size(); ++pos) {
        if (source[pos] == '{')
            ++depth;
        if (source[pos] == '}' && --depth == 0)
            return source.substr(start, pos - start);
    }
    throw std::runtime_error("unterminated signal-path function");
}

void rg_forbidden(const fs::path& file) {
    std::string pattern =
        R"(\b(new|malloc|calloc|realloc|free|printf|fprintf|sprintf|snprintf|throw|mutex|on_error|flush|fsync)\b|std\s*::\s*(string|filesystem|cout|cerr)\b|\.(lock|unlock)\s*\()";
    auto name = file.string();
    std::array<char*, 6> args{const_cast<char*>("rg"),
                              const_cast<char*>("-n"),
                              const_cast<char*>("-e"),
                              pattern.data(),
                              name.data(),
                              nullptr};
    pid_t child = -1;
    check(::posix_spawnp(&child, "rg", nullptr, nullptr, args.data(), environ) == 0,
          "could not run required rg signal-safety inspection");
    const int status = wait_child(child);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 1,
          "rg found forbidden signal-path operations or inspection failed");
}

void check_calls(const std::string& code, std::initializer_list<std::string_view> allowed) {
    const std::regex call(R"(\b((?:[A-Za-z_]\w*\s*(?:::|\.)\s*)*[A-Za-z_]\w*)\s*\()");
    for (auto it = std::sregex_iterator(code.begin(), code.end(), call);
         it != std::sregex_iterator{};
         ++it) {
        const auto name = std::regex_replace((*it)[1].str(), std::regex(R"(\s+)"), "");
        if (name == "if" || name == "while" || name == "for" || name == "switch" ||
            name == "sizeof")
            continue;
        check(std::find(allowed.begin(), allowed.end(), name) != allowed.end(),
              "signal path calls a function outside its whitelist: " + name);
    }
}
} // namespace

int main() {
    return run([] {
        const auto source =
            code_only(read(fs::path(CHIPCAE_SOURCE_DIR) / "core/src/crash_diag.cpp"));
        const auto handler =
            body(source, std::regex(R"(\bCrashDiag\s*::\s*on_signal\s*\(\s*int\s+\w+\s*\)\s*\{)"));
        const auto helper =
            body(source,
                 std::regex(R"(\bstatic\s+void\s+write_signal_line\s*\(\s*int\s+sig\s*\)\s*\{)"));
        TempDir tmp;
        const auto handler_file = tmp.path / "handler.cpp";
        const auto helper_file = tmp.path / "helper.cpp";
        std::ofstream(handler_file) << handler;
        std::ofstream(helper_file) << helper;
        rg_forbidden(handler_file);
        rg_forbidden(helper_file);
        check(handler.find("write_signal_line") != std::string::npos,
              "on_signal must use the inspected signal writer");
        // Metadata is allowed only on the known, always-lock-free atomic objects.
        const auto check_handler = [](const std::string& code) {
            check_calls(code,
                        {"handling.test_and_set", "published.load", "write_signal_line", "_exit"});
        };
        check_handler("if (!handling.test_and_set()) { published.load(); write_signal_line(sig); } "
                      "::_exit(128 + sig);");
        for (const auto* forbidden : {"::raise(sig);",
                                      "::kill(1, sig);",
                                      "std::terminate();",
                                      "uninspected_helper(sig);",
                                      "other.load();"})
            expect_throw<std::runtime_error>([&] { check_handler(forbidden); },
                                             "handler whitelist accepted a forbidden call");
        check_handler(handler);
        check(std::regex_search(handler, std::regex(R"(\b_exit\s*\()")),
              "on_signal never calls _exit");
        check_calls(helper, {"write", "_exit"});
        check(std::regex_search(helper, std::regex(R"(\bwrite\s*\()")),
              "signal writer never calls write");
    });
}
