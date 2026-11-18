#include "test_support.hpp"

#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>

using namespace resdec_test;

static_assert(flag::none == 0);
static_assert(flag::window_first == 1);
static_assert(flag::window_last == 2);
static_assert(flag::not_converged == 4);
static_assert(flag::diverged == 8);
static_assert(flag::cutback == 16);
static_assert(flag::max_iter == 32);
static_assert(flag::linear_summary == 64);
static_assert(flag::interface == 128);
static_assert(static_cast<std::uint32_t>(SpecialKind::NotConverged) == flag::not_converged);
static_assert(static_cast<std::uint32_t>(SpecialKind::Diverged) == flag::diverged);
static_assert(static_cast<std::uint32_t>(SpecialKind::Cutback) == flag::cutback);
static_assert(static_cast<std::uint32_t>(SpecialKind::MaxIter) == flag::max_iter);
static_assert(ResidualDecimatorOptions{}.max_intermediate == 16);

int main() {
    Suite suite;
    suite.run("all nine residual flags match frozen proto enum", [] {
        const auto path = std::filesystem::path(CHIPCAE_SOURCE_DIR) / "protocol" / "chipcae" /
                          "run" / "v1" / "run.proto";
        std::ifstream input(path);
        require(input.is_open(), "run.proto must be readable; never skip");
        const std::string source{std::istreambuf_iterator<char>(input),
                                 std::istreambuf_iterator<char>()};
        const std::regex enum_pattern(R"(enum\s+ResidualFlag\s*\{([^}]*)\})");
        std::smatch enumeration;
        require(std::regex_search(source, enumeration, enum_pattern),
                "ResidualFlag enum must exist");
        const auto body = enumeration[1].str();
        const std::regex entry_pattern(R"(\b(RESIDUAL_FLAG_[A-Z_]+)\s*=\s*([0-9]+)\s*;)");
        const std::map<std::string, std::uint32_t> expected{
            {"RESIDUAL_FLAG_NONE", flag::none},
            {"RESIDUAL_FLAG_WINDOW_FIRST", flag::window_first},
            {"RESIDUAL_FLAG_WINDOW_LAST", flag::window_last},
            {"RESIDUAL_FLAG_NOT_CONVERGED", flag::not_converged},
            {"RESIDUAL_FLAG_DIVERGED", flag::diverged},
            {"RESIDUAL_FLAG_CUTBACK", flag::cutback},
            {"RESIDUAL_FLAG_MAX_ITER", flag::max_iter},
            {"RESIDUAL_FLAG_LINEAR_SUMMARY", flag::linear_summary},
            {"RESIDUAL_FLAG_INTERFACE", flag::interface}};
        std::set<std::string> names;
        std::size_t count = 0;
        for (auto entry = std::sregex_iterator(body.begin(), body.end(), entry_pattern);
             entry != std::sregex_iterator();
             ++entry) {
            ++count;
            const auto name = (*entry)[1].str();
            const auto wanted = expected.find(name);
            require(wanted != expected.end(), "unexpected proto flag " + name);
            require(names.insert(name).second, "duplicate proto flag " + name);
            require(std::stoull((*entry)[2].str()) == wanted->second, "proto flag value " + name);
        }
        require(count == 9 && names.size() == expected.size(),
                "exactly nine matching proto entries");
    });
    return suite.result();
}
