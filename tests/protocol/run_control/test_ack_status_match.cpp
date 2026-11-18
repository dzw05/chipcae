#include "test_support.hpp"

#include <algorithm>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <type_traits>

using namespace runctl_test;
namespace control_ack = chipcae::coupling::control_ack;

static_assert(control_ack::unspecified == 0);
static_assert(control_ack::applied == 1);
static_assert(control_ack::noop == 2);
static_assert(control_ack::rejected == 3);
static_assert(std::is_same_v<decltype(control_ack::unspecified), const std::uint32_t>);
static_assert(std::is_same_v<decltype(control_ack::applied), const std::uint32_t>);
static_assert(std::is_same_v<decltype(control_ack::noop), const std::uint32_t>);
static_assert(std::is_same_v<decltype(control_ack::rejected), const std::uint32_t>);

namespace {
std::string read_source(const std::filesystem::path& relative) {
    std::ifstream input(std::filesystem::path(CHIPCAE_SOURCE_DIR) / relative);
    require(input.is_open(), relative.string() + " must be readable; never skip");
    const std::string source{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};
    require(!input.bad(), relative.string() + " read failed");
    return source;
}

std::string without_comments(const std::string& source, bool mask_strings = false) {
    // Keep quoted strings intact by default, including comment markers inside them.
    // Mask tokens with spaces to preserve offsets into the original source.
    const std::regex token(R"re("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|//[^\n]*|/\*[\s\S]*?\*/)re");
    std::string result;
    std::size_t cursor = 0;
    for (auto match = std::sregex_iterator(source.begin(), source.end(), token);
         match != std::sregex_iterator();
         ++match) {
        const auto position = static_cast<std::size_t>(match->position());
        result.append(source, cursor, position - cursor);
        const auto text = match->str();
        result += text.front() == '/' || mask_strings ? std::string(text.size(), ' ') : text;
        cursor = position + text.size();
    }
    result.append(source, cursor, std::string::npos);
    return result;
}

std::string block_body(const std::string& source, const std::regex& opening, const char* name) {
    const auto masked = without_comments(source, /*mask_strings=*/true);
    std::smatch match;
    require(std::regex_search(masked, match, opening), std::string(name) + " must exist");
    const auto begin = static_cast<std::size_t>(match.position() + match.length());
    std::size_t depth = 1;
    for (auto cursor = begin; cursor < masked.size(); ++cursor) {
        if (masked[cursor] == '{')
            ++depth;
        else if (masked[cursor] == '}' && --depth == 0)
            return source.substr(begin, cursor - begin);
    }
    throw std::runtime_error(std::string(name) + " has no closing brace");
}

std::map<std::string, std::uint32_t> parse_status(const std::string& source) {
    const auto message =
        block_body(source, std::regex(R"(\bmessage\s+ControlAck\s*\{)"), "ControlAck message");
    const auto body =
        block_body(message, std::regex(R"(\benum\s+Status\s*\{)"), "ControlAck.Status enum");
    const std::regex entry_pattern(R"(\s*([A-Za-z_][A-Za-z_0-9]*)\s*=\s*([0-9]+)\s*;)");
    const std::map<std::string, std::uint32_t> expected{
        {"STATUS_UNSPECIFIED", control_ack::unspecified},
        {"APPLIED", control_ack::applied},
        {"NOOP", control_ack::noop},
        {"REJECTED", control_ack::rejected}};
    std::map<std::string, std::uint32_t> actual;
    std::set<std::string> names;
    std::size_t count = 0;
    auto cursor = body.cbegin();
    while (cursor != body.cend()) {
        std::match_results<std::string::const_iterator> entry;
        if (!std::regex_search(cursor,
                               body.cend(),
                               entry,
                               entry_pattern,
                               std::regex_constants::match_continuous)) {
            require(std::all_of(cursor,
                                body.cend(),
                                [](char value) {
                                    return value == ' ' || value == '\t' || value == '\r' ||
                                           value == '\n';
                                }),
                    "malformed ControlAck.Status declaration");
            break;
        }
        ++count;
        const auto name = entry[1].str();
        const auto wanted = expected.find(name);
        require(wanted != expected.end(), "unexpected proto status " + name);
        require(names.insert(name).second, "duplicate proto status " + name);
        require(std::stoull(entry[2].str()) == wanted->second, "proto status value " + name);
        actual.emplace(name, wanted->second);
        cursor = entry[0].second;
    }
    require(count == 4 && names.size() == 4, "exactly four unique proto status declarations");
    require(actual == expected, "all ControlAck.Status names and values must match");
    return actual;
}
} // namespace

int main() {
    Suite suite;
    for (const auto& [name, declaration] : {
             std::pair{"block matching ignores double-quoted closing braces",
                       R"proto(option (note) = "}";)proto"},
             std::pair{"block matching ignores double-quoted opening braces",
                       R"proto(option (note) = "{";)proto"},
             std::pair{"block matching ignores single-quoted closing braces",
                       R"proto(option (note) = '}';)proto"},
             std::pair{"block matching ignores single-quoted opening braces",
                       R"proto(option (note) = '{';)proto"},
             std::pair{"block matching ignores line-comment closing braces", "// }"},
             std::pair{"block matching ignores line-comment opening braces", "// {"},
             std::pair{"block matching ignores block-comment closing braces", "/* } */"},
             std::pair{"block matching ignores block-comment opening braces", "/* { */"},
             std::pair{"block matching handles escaped double quotes and comment markers",
                       R"proto(option (note) = "escaped \" } // /*";)proto"},
             std::pair{"block matching handles escaped single quotes and comment markers",
                       R"proto(option (note) = 'escaped \' { // /*';)proto"},
         }) {
        suite.run(name, [&] {
            const auto body = std::string("\n") + declaration + "\nmessage Nested {}\n";
            const auto source = "message ControlAck {" + body + "}\nmessage After {}\n";
            require(block_body(source,
                               std::regex(R"(\bmessage\s+ControlAck\s*\{)"),
                               "ControlAck message") == body,
                    "return the complete original body, including nested blocks");
        });
    }
    suite.run("block matching ignores declarations in strings and comments", [] {
        const std::string source = R"proto(
// message ControlAck { line comment }
/* message ControlAck { block comment } */
option (note) = "message ControlAck { double-quoted string }";
option (note) = 'message ControlAck { single-quoted string }';
message ControlAck { uint64 cmd_seq = 1; }
)proto";
        require(block_body(source,
                           std::regex(R"(\bmessage\s+ControlAck\s*\{)"),
                           "ControlAck message") == " uint64 cmd_seq = 1; ",
                "only an unquoted, uncommented declaration opens a block");
    });
    suite.run("four named acknowledgment statuses match frozen proto twice", [] {
        const auto source = without_comments(read_source("protocol/chipcae/run/v1/run.proto"));
        const auto first = parse_status(source);
        const auto second = parse_status(source);
        require(first == second, "repeated proto parsing must be identical");
    });
    suite.run("dispatch uses named statuses instead of numeric assignments or comparisons", [] {
        const auto source = without_comments(read_source("coupling/src/run_control.cpp"));
        const std::regex numeric_status(
            R"(\bstatus\s*(?:==|!=|<=|>=|<|>|=(?!=))\s*[123]\b|\b[123]\s*(?:==|!=|<=|>=|<|>)\s*status\b|\bpaused_\s*\?\s*[12]\s*:\s*[12]\b)");
        std::string literals;
        for (auto match = std::sregex_iterator(source.begin(), source.end(), numeric_status);
             match != std::sregex_iterator();
             ++match)
            literals += " [" + match->str() + "]";
        require(literals.empty(), "dispatch must use control_ack constants; found" + literals);
    });
    suite.run("pause duplicate pause and resume emit complete named acknowledgments in order", [] {
        TempDirectory dir;
        append(dir.file(), 1, "PAUSE");
        append(dir.file(), 2, "PAUSE");
        append(dir.file(), 3, "RESUME");
        ControlChannel channel(dir.file());
        Events events;
        RunControl control(channel, {}, collect(events), no_wait());
        const WindowMark mark{7, 1.25};
        require(control.at_boundary(mark) == BoundaryDecision{}, "pause/resume empty decision");
        require(!control.paused(), "resume clears pause");
        expect_events(events,
                      {ack(1, control_ack::applied, mark),
                       state(State::Value::paused, mark),
                       ack(2, control_ack::noop, mark),
                       ack(3, control_ack::applied, mark),
                       state(State::Value::running, mark)});
        for (const auto& event : events) {
            if (const auto* acknowledgment = std::get_if<ControlAck>(&event))
                require(acknowledgment->status != control_ack::unspecified,
                        "dispatch must never emit unspecified status");
        }
    });
    return suite.result();
}
