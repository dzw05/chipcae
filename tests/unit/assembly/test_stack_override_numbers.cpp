#include "stack_override.hpp"

#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
void check(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}
struct Fixture {
    std::filesystem::path path = "stack_override_numbers.json";
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
    auto parse(std::string_view token) const {
        {
            std::ofstream output(path);
            output
                << R"({"schema":"chipcae.stack_override/0.1","layers":[{"name":"core","thickness_m":)"
                << token << "}]}";
            check(bool(output), "write stack override fixture");
        }
        return chipcae::importer::read_stack_override(path);
    }
};
} // namespace

int main() {
    try {
        Fixture fixture;
        struct Valid {
            std::string_view token;
            double expected;
        };
        for (const auto& [token, expected] :
             {Valid{"1", 1},
              Valid{" 1", 1},
              Valid{"1.5", 1.5},
              Valid{"2e3", 2000},
              Valid{"0.00028123456789", 0.00028123456789},
              Valid{"5e-324", std::numeric_limits<double>::denorm_min()},
              Valid{"2.2250738585072012e-308", std::numeric_limits<double>::min()}}) {
            const auto parsed = fixture.parse(token);
            check(bool(parsed), token);
            check(std::bit_cast<std::uint64_t>(parsed.value().thicknesses.at("core")) ==
                      std::bit_cast<std::uint64_t>(expected),
                  "override must preserve the exact double value");
        }
        for (std::string_view token : {"+1",
                                       ".5",
                                       "5.",
                                       "-0.5",
                                       "0",
                                       "-0",
                                       "nan",
                                       "inf",
                                       "1x",
                                       "1e",
                                       "0x10",
                                       "1e999",
                                       "1e-324"}) {
            const auto parsed = fixture.parse(token);
            check(!parsed, token);
            check(parsed.error().message.starts_with("IMPORT_REJECTED: "),
                  "override must retain the import error path");
            if (token == "1e999" || token == "1e-324")
                check(parsed.error().message.starts_with(
                          "IMPORT_REJECTED: thickness_m must be finite and positive at byte "),
                      "range errors must retain their diagnostic");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
