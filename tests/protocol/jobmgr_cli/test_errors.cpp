#include "test_support.hpp"

#if CHIPCAE_JOBMGR_LOCAL
using namespace jobmgr_cli_test;

int main() {
    Suite suite;
    suite.test("invalid request UUID is a library error", [] {
        Fixture fixture;
        const auto result = cli(fixture.submit_args("not-a-uuid"));
        exit_code(result, 1);
        require(!result.err.empty(), "invalid UUID needs stderr diagnostic");
    });
    for (const auto* command : {"get", "wait", "cancel"}) {
        suite.test(std::string("unknown run ") + command, [&] {
            Fixture fixture;
            const auto result = fixture.command({command, "--run-id", std::string(unknown_id)});
            exit_code(result, 1);
            require(!result.err.empty(), "unknown run needs stderr diagnostic");
        });
    }
    const std::vector<std::vector<std::string>> invalid{
        {"list"},
        {"submit",
         "--case",
         "case.json",
         "--profile",
         "profile.json",
         "--request-id",
         std::string(request_id)},
        {"--root"},
        {"--root", "runs"},
        {"--root", "runs", "submit"},
        {"--root", "runs", "submit", "--case", "case.json"},
        {"--root", "runs", "submit", "--case", "case.json", "--profile", "profile.json"},
        {"--root",
         "runs",
         "submit",
         "--profile",
         "profile.json",
         "--request-id",
         std::string(request_id)},
        {"--root",
         "runs",
         "submit",
         "--case",
         "case.json",
         "--request-id",
         std::string(request_id)},
        {"--root", "runs", "get"},
        {"--root", "runs", "wait"},
        {"--root", "runs", "cancel"},
        {"--root", "runs", "get", "--run-id"},
        {"--root", "runs", "wait", "--run-id", std::string(unknown_id), "--timeout-ms"},
        {"--root", "runs", "cancel", "--run-id", std::string(unknown_id), "--grace-ms"},
        {"--root", "runs", "Submit"},
        {"--Root", "runs", "list"},
        {"--root", "runs", "list", "--unknown", "value"},
        {"--root", "runs", "list", "extra"},
        {"--root", "runs", "--root", "other", "list"},
        {"--root",
         "runs",
         "get",
         "--run-id",
         std::string(unknown_id),
         "--run-id",
         std::string(unknown_id)},
        {"--root", "runs", "get", "--run-id", std::string(unknown_id), "--solver", "unused"},
        {"--root", "runs", "list", "--timeout-ms", "0"},
        {"--root", "runs", "poll", "--grace-ms", "0"}};
    for (const auto& arguments : invalid) {
        std::string name = "usage error";
        for (const auto& arg : arguments)
            name += " [" + arg + "]";
        suite.test(name, [&] { usage_error(arguments); });
    }
    for (const auto* number :
         {"", "abc", "1ms", "1.5", "--1", "9223372036854775808", "-9223372036854775809"}) {
        for (const bool wait : {true, false}) {
            suite.test(std::string(wait ? "timeout " : "grace ") + number, [&] {
                Fixture fixture;
                usage_error({"--root",
                             fixture.root.string(),
                             wait ? "wait" : "cancel",
                             "--run-id",
                             std::string(unknown_id),
                             wait ? "--timeout-ms" : "--grace-ms",
                             number});
            });
        }
    }
    suite.test(
        "returned failed Job is API success; nonpositive wait and terminal cancel preserve it", [] {
            Fixture fixture;
            fs::remove(fixture.solver); // A missing executable produces a real terminal LAUNCH Job.
            const auto submitted = fixture.submit();
            require(submitted.state() == jobs::JOB_STATE_FAILED &&
                        submitted.failure().category() == jobs::FAILURE_CATEGORY_LAUNCH,
                    "missing solver must return a LAUNCH failure Job");
            for (const auto& options : {std::vector<std::string>{},
                                        {"--timeout-ms", "0"},
                                        {"--timeout-ms", "-1"},
                                        {"--timeout-ms", "-9223372036854775808"}}) {
                std::vector<std::string> arguments{"wait", "--run-id", submitted.run_id()};
                arguments.insert(arguments.end(), options.begin(), options.end());
                const auto job = text_message<jobs::Job>(fixture.command(arguments));
                require(job.run_id() == submitted.run_id() && job.state() == submitted.state() &&
                            job.failure().SerializeAsString() ==
                                submitted.failure().SerializeAsString(),
                        "nonpositive wait changed terminal Job");
            }
            for (const auto& options : {std::vector<std::string>{},
                                        {"--grace-ms", "-1"},
                                        {"--grace-ms", "1234"},
                                        {"--grace-ms", "9223372036854775807"},
                                        {"--grace-ms", "-9223372036854775808"}}) {
                std::vector<std::string> arguments{"cancel", "--run-id", submitted.run_id()};
                arguments.insert(arguments.end(), options.begin(), options.end());
                const auto job = text_message<jobs::Job>(fixture.command(arguments));
                require(job.run_id() == submitted.run_id() && job.state() == submitted.state() &&
                            job.failure().SerializeAsString() ==
                                submitted.failure().SerializeAsString(),
                        "cancel changed terminal Job");
            }
        });
    return suite.result();
}
#else
int main() {
    return 77;
}
#endif
