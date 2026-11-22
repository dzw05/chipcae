#include "test_support.hpp"

#if CHIPCAE_JOBMGR_LOCAL
using namespace jobmgr_cli_test;

int main() {
    Suite suite;
    for (const auto* alias : {"--help", "-h"}) {
        suite.test(alias, [&] {
            const auto result = cli({alias});
            exit_code(result, 0);
            for (const auto* command : {"submit", "list", "get", "wait", "cancel", "poll"})
                require(result.out.find(command) != std::string::npos,
                        std::string("help missing command ") + command + ": " + result.out);
            require(result.out.find("[--wait [--timeout-ms <n>]]") != std::string::npos,
                    "help missing optional submit wait syntax");
        });
    }
    for (const auto* alias : {"--version", "-v"}) {
        suite.test(alias, [&] {
            const auto result = cli({alias});
            exit_code(result, 0);
            require(result.out.starts_with("chipcae-jobmgr "), "version prefix missing");
            require(std::count(result.out.begin(), result.out.end(), '\n') == 1,
                    "version must occupy one line");
        });
    }
    suite.test("no arguments", [] { usage_error({}); });
    suite.test("unknown command", [] { usage_error({"--root", "unused", "unknown"}); });
    return suite.result();
}
#else
int main() {
    return 77;
}
#endif
