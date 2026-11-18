#include "support.hpp"

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <regex>

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

void check_forbidden(const std::string& code) {
    const std::regex pattern(
        R"(\b(new|malloc|calloc|realloc|free|printf|fprintf|sprintf|snprintf|throw|mutex|on_error|flush|fsync)\b|std\s*::\s*(string|filesystem|cout|cerr)\b|\.(lock|unlock)\s*\()");
    check(!std::regex_search(code, pattern), "found forbidden signal-path operations");
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
        // Source inspection must work even when no external tools are on PATH.
        check(::setenv("PATH", "", 1) == 0, "could not isolate inspection from external tools");
        const auto source =
            code_only(read(fs::path(CHIPCAE_SOURCE_DIR) / "core/src/crash_diag.cpp"));
        const auto handler =
            body(source, std::regex(R"(\bCrashDiag\s*::\s*on_signal\s*\(\s*int\s+\w+\s*\)\s*\{)"));
        const auto helper =
            body(source,
                 std::regex(R"(\bstatic\s+void\s+write_signal_line\s*\(\s*int\s+sig\s*\)\s*\{)"));
        check_forbidden("const char* bytes = snapshot.lines[index]; ::write(fd, bytes, length);");
        for (const auto* forbidden :
             {"new int",     "malloc(1)",   "calloc(1, 1)",    "realloc(ptr, 1)",
              "free(ptr)",   "printf()",    "fprintf()",       "sprintf()",
              "snprintf()",  "throw error", "mutex m",         "on_error(error)",
              "flush()",     "fsync(fd)",   "std :: string s", "std :: filesystem :: path p",
              "std :: cout", "std :: cerr", "m.lock ()",       "m.unlock ()"})
            expect_throw<std::runtime_error>([&] { check_forbidden(forbidden); },
                                             "inspection accepted a forbidden operation");
        check_forbidden(handler);
        check_forbidden(helper);
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
